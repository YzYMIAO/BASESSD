/*
 * usb_msc.c — USB Mass Storage Class（BOT）块设备前端
 *
 * 职责：TinyUSB MSC class 回调 + SCSI → controller 适配。
 *   READ(10)  → ssd_controller_submit(SSD_REQUEST_READ)  → USB IN 数据
 *   WRITE(10) → ssd_controller_submit(SSD_REQUEST_WRITE) → 数据已在 USB 缓冲
 *   SYNCHRONIZE CACHE(10)/(16) → SSD_REQUEST_FLUSH（异步：提交即返回 ASYNC，
 *                            完成走 usb_msc_task；FTL 侧是 ftl_flush_begin/step job）
 *   UNMAP       → 逐 range 的 SSD_REQUEST_TRIM（异步；全部 range 完成才回 CSW）
 *   TEST UNIT READY / READ CAPACITY / INQUIRY / START STOP 由 TinyUSB 调本文件的回调。
 *
 * 并发模型（为什么回调不阻塞）：
 *   tud_task()/MSC 回调运行在主循环（usb_device_task），但 FTL 的完成需要
 *   ftl_service_pump()+ftl_service() 继续推进。若在回调里自旋等待，就会在
 *   主循环里再套一层主循环，吞掉 CLI/看门狗/后台任务。因此采用 TinyUSB 的
 *   异步模型：
 *     ① 回调提交请求后返回 TUD_MSC_RET_ASYNC（协议栈保持 pending_io=true）；
 *     ② 主循环每轮先跑 FTL 服务，再由 usb_msc_task() 检查 complete_flag；
 *     ③ 完成时回填 READ 数据并调用 tud_msc_async_io_done()，由随后的
 *        usb_device_task() 发出数据/CSW。
 *   ⚠ 不要改成 TUD_MSC_RET_BUSY：TinyUSB 会在同一个 tud_task() 调用里立即
 *     重入回调，单线程下 FTL 得不到推进机会（饿死）。
 *   BOT 本身串行（CBW → data → CSW），同一时刻只有这一条 host 命令。
 *
 * 缓冲：回调收到的 buffer 是 TinyUSB 的 MSC 端点缓冲，生命周期只到回调返回；
 *       USB 复位/拔出会在请求仍在飞时复用它（BOT 重新收 CBW）。因此写路径先
 *       memcpy 到**槽自己的 bounce**（读路径由 FTL 直接写 bounce、完成时拷回端点
 *       缓冲），FTL 只持有 bounce 的指针。数据路径有 2 个槽（见 usb_msc_slot_s）：
 *       写需要 1 chunk lookahead（缓存程序序列尾页由下一 chunk 的首条命令确认），
 *       读不需要；另有 1 个无 bounce 的控制槽（FLUSH/UNMAP），保证两个写槽都在飞时
 *       紧接的 SYNCHRONIZE CACHE / UNMAP 仍能排队（见 USB_MSC_SLOT_NUM）。
 */

#include "usb_msc.h"

#include "cli.h"
#include "controller.h"
#include "delay.h" /* SysTick µs 分段计时（submit → complete） */
#include "ftl_capacity.h"
#include "log.h"
#include "ssd_compiler.h"
#include "tusb.h"

#include <string.h>

/* SCSI SYNCHRONIZE CACHE(10)/(16)；TinyUSB 的 scsi_cmd_type_t 未列出这两个命令。
 * 两者都没有 data stage，走同一条异步 FLUSH 路径；某些主机（含部分 Windows/FS
 * 工具链）用 16 字节 CDB 版本。 */
#define USB_SCSI_SYNCHRONIZE_CACHE_10 0x35U

#define USB_SCSI_SYNCHRONIZE_CACHE_16 0x91U

#define USB_SCSI_UNMAP 0x42U

#define USB_SCSI_MODE_SELECT_6 0x15U

#define USB_SCSI_MODE_SELECT_10 0x55U

/* SCSI additional sense code / qualifier（SPC-4）。 */
#define USB_SCSI_ASC_WRITE_ERROR (0x0CU)

#define USB_SCSI_ASC_UNRECOVERED_READ_ERROR (0x11U)

#define USB_SCSI_ASC_LBA_OUT_OF_RANGE (0x21U)

#define USB_SCSI_ASC_INVALID_FIELD_IN_CDB (0x24U)

#define USB_SCSI_ASC_INVALID_FIELD_IN_PARAM_LIST (0x26U)

#define USB_SCSI_ASC_NO_INFORMATION (0x00U)

#define USB_SCSI_ASCQ_NONE (0x00U)

/* MODE SENSE page codes / lengths（SPC-4）：page = code + 长度字节 + 数据。 */
#define USB_SCSI_MODE_PAGE_CACHING (0x08U)

#define USB_SCSI_MODE_PAGE_IE (0x1CU)

#define USB_SCSI_MODE_PAGE_ALL (0x3FU)

#define USB_SCSI_MODE_PAGE_CACHING_DATA_LEN (18U)

#define USB_SCSI_MODE_PAGE_IE_DATA_LEN (10U)

#define USB_SCSI_MODE_PAGE_CACHING_LEN (2U + USB_SCSI_MODE_PAGE_CACHING_DATA_LEN)

#define USB_SCSI_MODE_PAGE_IE_LEN (2U + USB_SCSI_MODE_PAGE_IE_DATA_LEN)

/* SCSI UNMAP 参数表（SBC-4）：8B 头 + N×16B block descriptor；
 * descriptor 内 LBA 占 8B（高/低各 4B），NLB 在偏移 8。 */
#define USB_MSC_UNMAP_HDR_SIZE (8U)

#define USB_MSC_UNMAP_DESC_SIZE (16U)

#define USB_MSC_UNMAP_DESC_NLB_OFFSET (8U)

/* WR chunk 直方图的 4 KiB 桶边界。 */
#define USB_MSC_HIST_4K (4096U)

/* 单 LUN 设备（SCSI LUN 字段恒为 0）。 */
#define USB_MSC_LUN (0U)

/* SCSI UNMAP 参数表最多收多少个 range；超过拒绝（ILLEGAL REQUEST）。
 * 16 × 16B = 256B，足够覆盖文件系统的常见 discard 批量。 */
#define USB_MSC_UNMAP_MAX (16U)

#define USB_MSC_EP_OUT 0x01U

#define USB_MSC_EP_IN 0x81U

#define USB_MSC_EP_SIZE 64U /* 本仓选定的 FS bulk 端点包长（TinyUSB MSC 描述符参数） */

#define USB_MSC_POWER_MA 500U /* 总线供电声明；实际功耗以板级实测为准 */

#define USB_MSC_LANGID_EN_US 0x0409U

/* 设备描述符标识：VID 沿用 ST 开发板 VID，PID 为本仓选定的开发用值（未申请量产分配）。 */
#define USB_MSC_VID 0x0483U

#define USB_MSC_PID 0x5720U

#define USB_MSC_BCD_USB 0x0200U /* bcdUSB：USB 2.0 */

#define USB_MSC_BCD_DEVICE 0x0100U /* bcdDevice：本仓设备版本 1.00 */

/* SCSI READ CAPACITY 的块计数是 32 位；越界会让主机看到错误容量。 */
_Static_assert(FTL_LBA_MAX_AT_MAX <= UINT32_MAX, "SCSI block count must fit 32 bits");

/* USB MSC sector 与 FTL 映射单元必须一一对应；任一侧改变都会破坏 LBA 语义。 */
_Static_assert(USB_MSC_BLOCK_SIZE == FTL_IU_BYTES, "MSC sector must equal FTL mapping unit");

/* ─────────────────────────── 在飞请求 ─────────────────────────── */

/* 0/1 = 数据槽（各带一块 16 KiB bounce）；2 = 控制槽（FLUSH / UNMAP，无数据缓冲）。
 * 控制槽单独一个是为了：两个写 chunk 都在飞（volatile write cache 的 lookahead）时，
 * 主机紧跟的 SYNCHRONIZE CACHE / UNMAP 仍能排队并异步完成，而不是被 host busy 门拒掉。 */
#define USB_MSC_SLOT_NUM (3U)

/* 最后一个槽是控制槽：FLUSH / UNMAP 共用（BOT 串行下同一时刻只有一条控制命令），
 * 无 bounce。 */
#define USB_MSC_SLOT_CONTROL (USB_MSC_SLOT_NUM - 1U)

/* ─────────────────────────── USB 描述符 ─────────────────────────── */

#define USB_MSC_STRING_COUNT 3U

#define USB_MSC_STRING_MAX_CHARS 31U /* 描述符缓冲 32 项：长度字 + 最多 31 个 UTF-16 单元 */

#define USB_MSC_CONFIG_LEN (TUD_CONFIG_DESC_LEN + TUD_MSC_DESC_LEN)

typedef enum USB_MSC_SLOT_STATE_E
{
    USB_MSC_SLOT_FREE = 0,
    /* host busy gate（本仓自拟：同一时刻只允许一个 host 前端提交 controller） */
    USB_MSC_SLOT_WAIT_GATE, /* 数据/参数已收到，等 CLI 释放 host busy gate 后提交 */
    USB_MSC_SLOT_IN_FLIGHT  /* 已提交 controller，等 FTL 完成 */
} usb_msc_slot_state_e;

/*
 * 一个在飞 chunk。写路径是 **volatile write cache** 语义（与主流 SSD 一致）：
 *   · 所有写 chunk（含末 chunk）**提交即回流控回执**，主机可以立刻发下一段/下一条
 *     命令——设备 cache 序列因此可以跨 host 命令连续（FTL 只在 Journal 顺序页满 /
 *     block 头 / 空闲 / LBA 不连续时主动收尾）；
 *   · 槽位保留到 FTL 完成（bounce 缓冲在 FTL 消费完之前不能被复用）；
 *   · 写 chunk 之后的失败**不在本条命令上报**：置 deferred error，由下一条
 *     READ/WRITE/FLUSH 命令的 CSW 报告一次（本仓用"下一条命令失败"近似 SCSI 的
 *     deferred error；承载错误的那条命令会设置 MEDIUM ERROR/WRITE ERROR sense）；
 *   · 两个数据槽都占用时写回调返回 TUD_MSC_RET_BUSY（数据留在端点缓冲，协议栈重投）。
 * 读路径不需要 lookahead：每 chunk 完成即回执，下一 chunk 才会来。
 */
typedef struct USB_MSC_SLOT_S
{
    /* 状态机只在主循环（usb_msc_task / 前端回调）推进；会话代次也在 TinyUSB 的
     * mount/umount 回调（task 上下文）推进。没有 ISR 共享 ⇒ state / session_gen
     * 都无需 volatile（H16）。 */
    usb_msc_slot_state_e state;
    DWORD                session_gen; /* 本槽所属的 USB 会话代次（分配时快照） */
    BOOL                 is_write;
    BOOL                 is_flush;                        /* SYNCHRONIZE CACHE：无数据阶段 */
    BOOL                 is_unmap;                        /* SCSI UNMAP：多 range，逐段 TRIM */
    BOOL                 ack_sent;                        /* 写 chunk 的流控回执已发（提交即回执） */
    BOOL                 fua;                             /* WRITE(10) FUA：完成前不回执 */
    BOOL                 unmap_flush_done;                /* UNMAP 先做一次内部 FLUSH 屏障 */
    BOOL                 unmap_submitted;                 /* 当前 range 的 TRIM 已提交 */
    BYTE                 unmap_index;                     /* 当前 range 下标 */
    BYTE                 unmap_count;                     /* 本命令 range 数（≤ USB_MSC_UNMAP_MAX） */
    DWORD                unmap_lba[USB_MSC_UNMAP_MAX];    /* range 起始 LBA */
    DWORD                unmap_blocks[USB_MSC_UNMAP_MAX]; /* range 块数 */
    DWORD                start_ms;                        /* 本 host 命令到达时刻（请求级 deadline） */
    DWORD                order;                           /* 到达序：完成按序处理（前 chunk 错误先记录） */
    DWORD                lba;                             /* 本 chunk 起始 LBA */
    DWORD                offset;                          /* 命令内字节偏移（TinyUSB 分块） */
    DWORD                length;                          /* 本 chunk 字节数 */
    void                *usb_buffer;                      /* TinyUSB 端点缓冲（读回写目标；写只读它一次） */
    DWORD                submit_us;                       /* submit 起点（SysTick µs），完成时算设备侧墙钟 */
    ssd_host_entry_s    *entry;                           /* HIL host queue entry（tag/owner/request） */
    BYTE                *bounce;                          /* 指向本槽的 16 KiB 缓冲（见 s_bounce*） */
} usb_msc_slot_s;

/* WRITE(10) 数据相分块直方图（取证）。
 *
 * TinyUSB 按 CFG_TUD_MSC_EP_BUFSIZE 分块回调，桶值就是本次数据相长度：
 * 大量 <=512B 说明主机按单扇区下发（BOT 往返主导）；<=4K 说明每块 4 KB；
 * <=16K 说明 16 KB 端点缓冲生效（每块一次 FTL 异步完成往返）。只在 `info`
 * 时打印，运行期不输出，不影响时序。 */
_Static_assert(CFG_TUD_MSC_EP_BUFSIZE == 16384U, "histogram label <=16K depends on EP bufsize");

typedef struct USB_MSC_WR_HIST_S
{
    DWORD calls;    /* 回调次数 = 数据相分块数 */
    DWORD bytes;    /* 累计字节 */
    DWORD le_block; /* <= 512 B */
    DWORD le_4k;    /* (512, 4096] B */
    DWORD le_epbuf; /* (4096, 16384] B */
    DWORD gt_epbuf; /* > 16384 B（当前配置下不应出现） */
} usb_msc_wr_hist_s;

/* submit → complete 的设备侧墙钟（SysTick µs）。用来区分：
 *   · 十几 ms：设备处理本身；
 *   · 只有几 ms：剩余时间在 USB/BOT 主机侧等待，不是 CPU/FTL。
 * 只在 `info` 打印，运行期零输出。 */
typedef struct USB_MSC_LAT_S
{
    DWORD calls;
    QWORD total_us;
    DWORD max_us;
    DWORD last_us;
} usb_msc_lat_s;

static usb_msc_slot_s s_slots[USB_MSC_SLOT_NUM];

/* 当前 SCSI 命令的 CBW 信息（tud_msc_cbw_info_cb 填写）。它只用于给 FTL 计算
 * 「本 chunk 之后同一命令还剩多少 LBA」（cache 序列 END 的落点）；序列本身由 FTL
 * 的顺序流检测器判定，host 传输段边界不构成断点。0 = 未知（按单 chunk 处理）。 */
static DWORD s_cmd_lba;

static DWORD s_cmd_blocks;

static BOOL s_cmd_fua; /* 当前 WRITE(10) 的 FUA 位（READ 恒 false） */

static DWORD s_order_next; /* 下一条到达 chunk 的序 */

/* USB 会话代次（session generation）：
 *   · `tud_mount_cb` / `tud_umount_cb`（TinyUSB task 上下文）各推进一次；
 *   · `usb_msc_task` / `usb_msc_slot_alloc` 消费代次，统一退役 WAIT_GATE 槽、
 *     撤销 stale FLUSH、给 in-flight 数据/UNMAP 槽打 stale 标记。
 * 为什么以 mount/umount 为界：USB bus reset 必然导致重新枚举与 SET_CONFIGURATION，
 * `tud_mount_cb` 在 class 命令之前执行；unplug 走 `tud_umount_cb`。因此所有 slot
 * 状态转移都发生在 task 上下文，ISR 不参与应用状态机（H16 / H18）。
 * 代次不等即「旧会话」；`s_session_gen_seen` 只由主循环写。 */
static DWORD s_session_gen;

static DWORD s_session_gen_seen; /* 主循环已应用到的代次 */

/* 已提前回执的写 chunk 后来失败（volatile write cache）：留到下一条命令上报一次。 */
static BOOL s_deferred_write_error;

static usb_msc_wr_hist_s s_wr_hist;

static usb_msc_lat_s s_lat;

static const char *const s_string_desc[USB_MSC_STRING_COUNT] = {
    "BASESSD",
    "NAND U-Disk",
    "SSD0001",
};

static const tusb_desc_device_t s_device_desc = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = USB_MSC_BCD_USB,
    .bDeviceClass       = 0x00, /* class 在 interface 上定义（MSC） */
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = USB_MSC_VID,
    .idProduct          = USB_MSC_PID,
    .bcdDevice          = USB_MSC_BCD_DEVICE,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01,
};

static const BYTE s_config_desc[] = {
    /* config, itf count, string idx, total len, attributes, power(2mA) */
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, USB_MSC_CONFIG_LEN, 0x00, USB_MSC_POWER_MA),
    /* itf, string idx, EP out, EP in, EP size */
    TUD_MSC_DESCRIPTOR(0, 0, USB_MSC_EP_OUT, USB_MSC_EP_IN, USB_MSC_EP_SIZE),
};

/* 两个 chunk 的 bounce：0 号在 AXI .bss，1 号放 DTCM。
 * 两块都只被 CPU 读写（DWC2 走 PIO，不经 DMA）⇒ DTCM 可用且最快；
 * SRAM4/SRAM12 在 UT 镜像里已被 FTL 表占满。 */
static BYTE s_bounce0[CFG_TUD_MSC_EP_BUFSIZE] __attribute__((aligned(4)));

static BYTE s_bounce1[CFG_TUD_MSC_EP_BUFSIZE] DTCM_DATA __attribute__((aligned(4)));

/* 消费会话代次（定义在 slot 辅助之后；deferred 错误入口需要先调用它）。 */
static void usb_msc_session_apply(void);

/**
 * @brief Controller 是否已 READY
 *
 * @return true = 可接受 host 命令
 */
static BOOL usb_msc_ready(void)
{
    return g_ctrl.state == SSD_CONTROLLER_READY;
}

/**
 * @brief 取槽的 host queue request（entry 必须已分配）
 *
 * @param slot 槽
 * @return request 指针
 */
static ssd_req_s *usb_msc_slot_request(usb_msc_slot_s *slot)
{
    ASSERT(slot->entry != NULL);
    return &slot->entry->request;
}

/**
 * @brief 归还槽的 host queue entry 并置 FREE
 *
 * @param slot 槽
 */
static void usb_msc_slot_release(usb_msc_slot_s *slot)
{
    if (slot->entry != NULL)
    {
        ssd_host_queue_free(&g_ctrl.host_queue, slot->entry);
        slot->entry = NULL;
    }
    slot->state = USB_MSC_SLOT_FREE;
}

/**
 * @brief 两个数据槽是否都空闲
 *
 * @return true = 都 FREE
 */
static BOOL usb_msc_data_slots_idle(void)
{
    for (BYTE i = 0U; i < USB_MSC_SLOT_CONTROL; i++)
    {
        if (s_slots[i].state != USB_MSC_SLOT_FREE)
        {
            return false;
        }
    }
    return true;
}

/**
 * @brief 读取 Big-Endian 16-bit
 *
 * @param p 字节指针
 * @return 16-bit 值
 */
static WORD usb_msc_be16(const BYTE *p)
{
    return (WORD)(((WORD)p[0] << 8) | (WORD)p[1]);
}

/**
 * @brief 读取 Big-Endian 32-bit
 *
 * @param p 字节指针
 * @return 32-bit 值
 */
static DWORD usb_msc_be32(const BYTE *p)
{
    return ((DWORD)p[0] << 24) | ((DWORD)p[1] << 16) | ((DWORD)p[2] << 8) | (DWORD)p[3];
}

/**
 * @brief 以 MEDIUM ERROR 结束 UNMAP 并回执
 *
 * @param slot 槽
 * @param asc ASC（0x0C = write error）
 */
static void usb_msc_unmap_fail(usb_msc_slot_s *slot, BYTE asc)
{
    (void)tud_msc_set_sense(USB_MSC_LUN, SCSI_SENSE_MEDIUM_ERROR, asc, USB_SCSI_ASCQ_NONE);
    usb_msc_slot_release(slot);
    (void)tud_msc_async_io_done(TUD_MSC_RET_ERROR, false);
}

/**
 * @brief 写请求是否允许启动（读/FLUSH/UNMAP 不受 admission 限制）
 *
 * @param slot 已填好 lba / is_write 的槽
 * @return true = 允许提交；false = 保持 WAIT_GATE 稍后重试
 */
static BOOL usb_msc_slot_write_admit(const usb_msc_slot_s *slot)
{
    return (slot->is_write == false) || (ssd_controller_host_write_admit(&g_ctrl, slot->lba) != false);
}

/**
 * @brief 填 MODE SENSE page 0x1C（Informational Exceptions Control），返回字节数。
 *
 * 全部为 0：不主动上报 IE（DEXCPT=0 / LOGERR=0 / MRIE=0）。
 */
static DWORD usb_msc_mode_page_ie(BYTE *page)
{
    page[0] = USB_SCSI_MODE_PAGE_IE;
    page[1] = USB_SCSI_MODE_PAGE_IE_DATA_LEN;
    memset(&page[2], 0, USB_SCSI_MODE_PAGE_IE_DATA_LEN);
    return USB_SCSI_MODE_PAGE_IE_LEN;
}

/**
 * @brief 取走 deferred 写错误（有则本次命令失败并带准确 sense）。
 *
 * 在承载错误的那条命令上设置 MEDIUM ERROR / WRITE ERROR (0x0C/0x00)；
 * TinyUSB 侧补丁：应用设置过 sense 时，不用默认 NOT READY/0x3A 覆盖。
 *
 * @param lun SCSI LUN（0）
 * @return true = 上一条已回执的写有失败；本次命令应失败（并已清除标志）
 *         false = 无 deferred 错误
 */
static BOOL usb_msc_take_deferred_error(BYTE lun)
{
    /* 先消费会话代次：新会话必须在读 deferred 之前清掉旧会话的错误。 */
    usb_msc_session_apply();

    /* FTL 侧的迟到结论优先：早回执后写组在页编程完成时就报 OK，
     * FAIL_CACHE 要晚一条命令才到 ⇒ 那个结论只存在于 FTL 的粘性错误里。 */
    for (BYTE die = 0U; die < DIE_MAX; die++)
    {
        if (ftl_take_deferred_error(g_ctrl.ftl[die]) == true)
        {
            (void)tud_msc_set_sense(lun, SCSI_SENSE_MEDIUM_ERROR, USB_SCSI_ASC_WRITE_ERROR, USB_SCSI_ASCQ_NONE);
            LOGW("[USB ] deferred write error (FTL) — fail the next command\r\n");
            return true;
        }
    }

    if (s_deferred_write_error == false)
    {
        return false;
    }

    s_deferred_write_error = false;
    (void)tud_msc_set_sense(lun, SCSI_SENSE_MEDIUM_ERROR, USB_SCSI_ASC_WRITE_ERROR, USB_SCSI_ASCQ_NONE);
    LOGW("[USB ] deferred write error — fail the next command (volatile write cache)\r\n");
    return true;
}

/**
 * @brief 记录一次 submit→complete 的延迟
 *
 * @param start_us 起始时刻（µs）
 */
static void usb_msc_note_latency(DWORD start_us)
{
    const DWORD us = delay_time_since_us(start_us);

    s_lat.calls++;
    s_lat.total_us += us;
    s_lat.last_us = us;

    if (us > s_lat.max_us)
    {
        s_lat.max_us = us;
    }
}

/**
 * @brief 更新 WRITE(10) 数据相分块直方图
 *
 * @param bufsize 数据相长度（字节）
 */
static void usb_msc_note_write(DWORD bufsize)
{
    s_wr_hist.calls++;
    s_wr_hist.bytes += bufsize;

    if (bufsize <= USB_MSC_BLOCK_SIZE)
    {
        s_wr_hist.le_block++;
    }
    else if (bufsize <= USB_MSC_HIST_4K)
    {
        s_wr_hist.le_4k++;
    }
    else if (bufsize <= CFG_TUD_MSC_EP_BUFSIZE)
    {
        s_wr_hist.le_epbuf++;
    }
    else
    {
        s_wr_hist.gt_epbuf++;
    }
}

/**
 * @brief 查找可复用的 WAIT_GATE 数据槽（事件重放）
 *
 * @param is_write true = WRITE
 * @param lba 起始 LBA
 * @param offset 字节偏移（本仓恒 0）
 * @param length 数据相长度（字节）
 * @return 匹配的槽；NULL = 无
 */
static usb_msc_slot_s *usb_msc_slot_match(BOOL is_write, DWORD lba, DWORD offset, DWORD length)
{
    for (BYTE i = 0U; i < USB_MSC_SLOT_NUM; i++)
    {
        usb_msc_slot_s *slot = &s_slots[i];

        if ((slot->state == USB_MSC_SLOT_WAIT_GATE) && (slot->is_flush == false) && (slot->is_unmap == false) && (slot->is_write == is_write) && (slot->lba == lba) && (slot->offset == offset) && (slot->length == length))
        {
            return slot;
        }
    }

    return NULL;
}

/**
 * @brief 消费会话代次：在主循环上下文退役 stale 槽
 *
 * 代次由 tud_mount_cb / tud_umount_cb（TinyUSB task 上下文）推进；本函数把
 * 旧会话落到 slot 状态机：
 *   · WAIT_GATE：尚未提交，直接归还（无 controller 请求）。会话结束时 mscd_reset
 *     已清/将清 pending_io，**不能**在这里配 async_io_done，否则可能误配给新会话
 *     的新命令；
 *   · IN_FLIGHT FLUSH：撤销 controller 的 flush job 后立即归还（控制槽不长期占用）；
 *   · IN_FLIGHT 数据 / UNMAP：保留到 FTL 完成；完成路径按 session_gen 判 stale 并丢弃。
 */
static void usb_msc_session_apply(void)
{
    const DWORD gen = s_session_gen;

    if (gen == s_session_gen_seen)
    {
        return;
    }
    s_session_gen_seen = gen;

    /* 新会话不继承旧会话的 deferred error：否则重新枚举后的第一条命令会被
     * 上一个 host 会话的失败误伤。FTL 的粘性错误是设备级状态，也在这里清。 */
    s_deferred_write_error = false;
    for (BYTE die = 0U; die < DIE_MAX; die++)
    {
        (void)ftl_take_deferred_error(g_ctrl.ftl[die]);
    }

    for (BYTE i = 0U; i < USB_MSC_SLOT_NUM; i++)
    {
        usb_msc_slot_s *slot = &s_slots[i];

        if (slot->state == USB_MSC_SLOT_FREE)
        {
            continue;
        }

        if (slot->state == USB_MSC_SLOT_WAIT_GATE)
        {
            usb_msc_slot_release(slot);
            continue;
        }

        if ((slot->is_flush != false) && (g_ctrl.flush_req == usb_msc_slot_request(slot)))
        {
            /* 旧会话的 FLUSH：撤销 job 后立即释放；in-flight 数据槽不能取消，
             * 等 FTL 完成。 */
            ssd_controller_cancel_flush(&g_ctrl);
            usb_msc_slot_release(slot);
        }
    }
}

/**
 * @brief 认领空闲槽
 *
 * WAIT_GATE 槽属于当前正在等 gate 的命令，BOT 串行下不会同时出现第二条命令；
 * 因此这里只认领 FREE 槽，绝不复用 WAIT_GATE（避免丢弃一条已返回 ASYNC 的命令）。
 *
 * @param need_bounce 是否需要带 bounce 的数据槽
 * @return 槽指针；NULL = 无可用
 */
static usb_msc_slot_s *usb_msc_slot_alloc(BOOL need_bounce)
{
    /* 先消费会话代次：新命令绝不能复用旧会话的槽。 */
    usb_msc_session_apply();

    /* FLUSH/UNMAP 优先用控制槽，避免占用数据槽。 */
    if ((need_bounce == false) && (s_slots[USB_MSC_SLOT_CONTROL].state == USB_MSC_SLOT_FREE))
    {
        usb_msc_slot_s *slot = &s_slots[USB_MSC_SLOT_CONTROL];

        ASSERT(slot->entry == NULL);
        slot->session_gen = s_session_gen;
        return slot;
    }

    for (BYTE i = 0U; i < USB_MSC_SLOT_NUM; i++)
    {
        usb_msc_slot_s *slot = &s_slots[i];

        if ((slot->state == USB_MSC_SLOT_FREE) && ((need_bounce == false) || (slot->bounce != NULL)))
        {
            ASSERT(slot->entry == NULL);
            slot->session_gen = s_session_gen;
            return slot;
        }
    }

    return NULL;
}

/**
 * @brief 把 controller 拒绝映射成 SCSI sense
 *
 * @param st   controller 返回状态（非 OK / BUSY）
 * @param slot 槽（决定 MEDIUM ERROR 的 ASC：写/FLUSH 0x0C，读 0x11）
 */
static void usb_msc_note_submit_error(ssd_status_e st, const usb_msc_slot_s *slot)
{
    switch (st)
    {
        case SSD_STATUS_INVALID_ARGUMENT:
        case SSD_STATUS_NOT_SUPPORTED:
            (void)tud_msc_set_sense(USB_MSC_LUN, SCSI_SENSE_ILLEGAL_REQUEST, USB_SCSI_ASC_INVALID_FIELD_IN_CDB, USB_SCSI_ASCQ_NONE); /* invalid field in CDB */
            break;
        case SSD_STATUS_TIMEOUT:
            (void)tud_msc_set_sense(USB_MSC_LUN, SCSI_SENSE_ABORTED_COMMAND, USB_SCSI_ASC_NO_INFORMATION, USB_SCSI_ASCQ_NONE);
            break;
        case SSD_STATUS_NO_SPACE:
        case SSD_STATUS_IO_ERROR:
        default:
            (void)tud_msc_set_sense(USB_MSC_LUN, SCSI_SENSE_MEDIUM_ERROR, (slot->is_write != false) || (slot->is_flush != false) ? USB_SCSI_ASC_WRITE_ERROR : USB_SCSI_ASC_UNRECOVERED_READ_ERROR, USB_SCSI_ASCQ_NONE);
            break;
    }
}

/**
 * @brief 把槽请求提交给 controller（分配 host entry）
 *
 * @param slot 槽
 * @return SSD_STATUS_OK    已受理
 *         SSD_STATUS_BUSY  host queue 满 / controller 忙（瞬态，调用者可保留
 *                          WAIT_GATE 重试）
 *         其他             controller 拒绝（调用者应映射 sense 并失败）
 */
static ssd_status_e usb_msc_slot_submit(usb_msc_slot_s *slot)
{
    DWORD start_us = 0U;

    if (slot->entry == NULL)
    {
        slot->entry = ssd_host_queue_alloc(&g_ctrl.host_queue, SSD_HOST_OWNER_USB);
        if (slot->entry == NULL)
        {
            return SSD_STATUS_BUSY; /* host queue 满 */
        }
    }

    (void)delay_time_now_us(&start_us);

    ssd_req_s *request = usb_msc_slot_request(slot);

    memset(request, 0, sizeof(*request));

    if (slot->is_flush)
    {
        /* SYNCHRONIZE CACHE：无数据阶段，不碰 bounce。 */
        request->opcode = SSD_REQUEST_FLUSH;
    }
    else
    {
        const DWORD blocks = slot->length / USB_MSC_BLOCK_SIZE;

        request->opcode      = slot->is_write ? SSD_REQUEST_WRITE : SSD_REQUEST_READ;
        request->start_lba   = slot->lba;
        request->block_count = blocks;
        request->buffer      = slot->bounce;
        request->buffer_size = slot->length;
        /* 同一 SCSI 命令在本 chunk 之后还剩多少 LBA：FTL 只用它判断 cache 序列
         * END 的落点（器件要求 END 在最后一个数据页）。CBW 未知（0）⇒ 按单 chunk
         * 语义（本段是命令末尾）。传输段本身的边界不是 FTL 的序列边界。 */
        if (s_cmd_blocks != 0U)
        {
            const QWORD consumed = (QWORD)(slot->lba - s_cmd_lba) + blocks;

            request->io.cmd_remaining = (s_cmd_blocks > consumed) ? (DWORD)(s_cmd_blocks - consumed) : 0U;
        }
    }

    const ssd_status_e st = ssd_controller_submit(&g_ctrl, request);

    if (st != SSD_STATUS_OK)
    {
        return st;
    }

    slot->submit_us = start_us;
    slot->state     = USB_MSC_SLOT_IN_FLIGHT;
    return SSD_STATUS_OK;
}

/**
 * @brief 标记 host 命令超时并回执一次
 *
 * 只解开 host 侧；slot/entry 由调用者按 `ssd_controller_request_reclaimable()`
 * 判断何时可释放（FTL / controller 可能仍引用 request）。
 *
 * @param slot 槽
 */
static void usb_msc_slot_timeout(usb_msc_slot_s *slot)
{
    ssd_req_s *request = (slot->entry != NULL) ? usb_msc_slot_request(slot) : NULL;

    if (request != NULL)
    {
        ssd_request_mark_timeout(request);

        /* READ/WRITE：中止本请求在所有 FTL 实例上的组；在飞命令完成后组仍会
         * 回调（HIL timed_out 分支只递减 sub_remaining）。 */
        if ((request->opcode == SSD_REQUEST_READ) || (request->opcode == SSD_REQUEST_WRITE))
        {
            ftl_abort_request(request);
        }

        /* FLUSH 可直接撤销；UNMAP 的内部 FLUSH 同理。TRIM 由 controller
         * 在下一轮 service 里 ftl_trim_abort。 */
        const BOOL flush_in_flight = (slot->is_flush != false) || ((slot->is_unmap != false) && (slot->unmap_submitted != false) && (slot->unmap_flush_done == false));

        if ((flush_in_flight != false) && (g_ctrl.flush_req == request))
        {
            ssd_controller_cancel_flush(&g_ctrl);
        }
    }

    /* 读 / FUA / FLUSH / UNMAP 还没回执：回一次 ABORTED COMMAND；
     * 非 FUA 写已提前回执，host 侧不再发错误（迟到失败走 deferred）。 */
    if (slot->ack_sent == false)
    {
        (void)tud_msc_set_sense(USB_MSC_LUN, SCSI_SENSE_ABORTED_COMMAND, USB_SCSI_ASC_NO_INFORMATION, USB_SCSI_ASCQ_NONE);
        (void)tud_msc_async_io_done(TUD_MSC_RET_ERROR, false);
        slot->ack_sent = true;
    }

    LOGW("[USB ] request timeout op=%s lba=%lu len=%lu\r\n", (slot->is_flush != false) ? "flush" : ((slot->is_unmap != false) ? "unmap" : (slot->is_write ? "write" : "read")), slot->lba, slot->length);
}

/* ── SCSI UNMAP（多 range 逻辑擦除）──
 *
 * UNMAP 的参数表是 Big-Endian：2B UNMAP 数据长度 + 2B 块描述符长度 + 4B 保留 +
 * N × (8B LBA + 4B 块数 + 4B 保留)，即每个 block descriptor 16B（SBC-4）。
 * HIL 一次 TRIM 只接一个连续 range，因此控制槽保存最多 USB_MSC_UNMAP_MAX 个
 * range，由 usb_msc_task 逐段推进；全部完成才回 CSW，保证 UNMAP 与后续写在
 * BOT 上保持顺序（不能提前回执后再慢慢擦）。
 */

/**
 * @brief 取 UNMAP 专用控制槽
 *
 * @return 控制槽；NULL = 被占用
 */
static usb_msc_slot_s *usb_msc_slot_alloc_unmap(void)
{
    usb_msc_session_apply();

    usb_msc_slot_s *slot = &s_slots[USB_MSC_SLOT_CONTROL];

    if (slot->state != USB_MSC_SLOT_FREE)
    {
        return NULL;
    }

    ASSERT(slot->entry == NULL);
    slot->session_gen = s_session_gen;
    return slot;
}

/**
 * @brief UNMAP 屏障：提交内部 FLUSH
 *
 * @param slot 槽
 * @return true = 已提交/暂缓（BUSY）
 */
static BOOL usb_msc_unmap_submit_flush(usb_msc_slot_s *slot)
{
    ssd_req_s *request = usb_msc_slot_request(slot);

    memset(request, 0, sizeof(*request));
    request->opcode = SSD_REQUEST_FLUSH;

    const ssd_status_e st = ssd_controller_submit(&g_ctrl, request);

    if (st == SSD_STATUS_BUSY)
    {
        slot->unmap_submitted = false;
        return true;
    }
    if (st != SSD_STATUS_OK)
    {
        slot->unmap_submitted = false;
        return false;
    }
    slot->unmap_submitted = true;
    return true;
}

/**
 * @brief 提交下一个 TRIM range
 *
 * @param slot 槽
 * @return true = 已提交/等待数据槽空闲
 */
static BOOL usb_msc_unmap_submit_next(usb_msc_slot_s *slot)
{
    if (slot->unmap_index >= slot->unmap_count)
    {
        return true;
    }

    /* 先等所有数据槽（在飞写）落定：写路径提前回执，UNMAP 必须在它们确认映射
     * 之后才能开始 TRIM，否则顺序反转。 */
    if (!usb_msc_data_slots_idle())
    {
        slot->unmap_submitted = false;
        return true;
    }

    ssd_req_s *request = usb_msc_slot_request(slot);

    memset(request, 0, sizeof(*request));
    request->opcode      = SSD_REQUEST_TRIM;
    request->start_lba   = slot->unmap_lba[slot->unmap_index];
    request->block_count = slot->unmap_blocks[slot->unmap_index];

    const ssd_status_e st = ssd_controller_submit(&g_ctrl, request);

    if (st == SSD_STATUS_BUSY)
    {
        slot->unmap_submitted = false;
        return true; /* 控制器 TRIM 单飞行：下一轮重试 */
    }
    if (st != SSD_STATUS_OK)
    {
        slot->unmap_submitted = false;
        return false;
    }
    slot->unmap_submitted = true;
    return true;
}

/**
 * @brief 启动 UNMAP：分配 entry 并提交 FLUSH 屏障
 *
 * @param slot 槽
 * @return true = 已启动
 */
static BOOL usb_msc_unmap_start(usb_msc_slot_s *slot)
{
    if (slot->entry == NULL)
    {
        slot->entry = ssd_host_queue_alloc(&g_ctrl.host_queue, SSD_HOST_OWNER_USB);
        if (slot->entry == NULL)
        {
            return false;
        }
    }

    slot->state            = USB_MSC_SLOT_IN_FLIGHT;
    slot->unmap_index      = 0U;
    slot->unmap_flush_done = false;
    slot->unmap_submitted  = false;

    /* UNMAP 是「丢弃」语义，但必须在本命令之前已回执的写**确认映射之后**才能
     * 生效。先借 controller 的异步 FLUSH 做一道屏障（ftl_flush_step 会等
     * wr_inflight / WRITE_CONFIRM），再逐 range TRIM；否则早回执写可能在 TRIM
     * 之后才确认，数据会复活。 */
    return usb_msc_unmap_submit_flush(slot);
}

/**
 * @brief 推进 UNMAP：FLUSH → 逐 range TRIM → 回执
 *
 * @param slot 槽
 */
static void usb_msc_unmap_step(usb_msc_slot_s *slot)
{
    const BOOL session_stale = (slot->session_gen != s_session_gen);

    /* 会话结束/未就绪：内部 FLUSH 可立即撤销并释放；已提交的 TRIM 必须等
     * controller 清掉 trim_req（提前释放会让 trim_req 悬垂）。
     * 不要在会话结束后调 async_io_done（TinyUSB 的 mscd_reset 已清 pending_io）。 */
    if ((session_stale != false) || !tud_ready())
    {
        if ((session_stale != false) && (slot->unmap_submitted != false) && (slot->unmap_flush_done == false) && (g_ctrl.flush_req == usb_msc_slot_request(slot)))
        {
            ssd_controller_cancel_flush(&g_ctrl);
            usb_msc_slot_release(slot);
            return;
        }

        if ((slot->unmap_submitted == false) || (ssd_controller_is_complete(usb_msc_slot_request(slot)) != 0U))
        {
            usb_msc_slot_release(slot);
        }
        return;
    }

    ssd_req_s *request = usb_msc_slot_request(slot);

    if ((request->timed_out == false) && (slot->start_ms != 0U) && (ssd_request_expired(slot->start_ms, delay_now_ms()) != false))
    {
        usb_msc_slot_timeout(slot);
    }
    else if ((request->timed_out != false) && (slot->ack_sent == false))
    {
        usb_msc_slot_timeout(slot);
    }

    if (request->timed_out != false)
    {
        /* host 回执已发；内部 FLUSH 已撤销。TRIM 仍可能在飞（trim_req 指向本请求），
         * 等 controller 清掉后再释放，避免 trim_req 悬垂。 */
        if (ssd_controller_request_reclaimable(&g_ctrl, request) != false)
        {
            usb_msc_slot_release(slot);
        }
        return;
    }

    if (slot->unmap_submitted == false)
    {
        const BOOL ok = (slot->unmap_flush_done == false) ? usb_msc_unmap_submit_flush(slot) : usb_msc_unmap_submit_next(slot);

        if (!ok)
        {
            usb_msc_unmap_fail(slot, USB_SCSI_ASC_WRITE_ERROR); /* WRITE ERROR */
        }
        return;
    }

    if (ssd_controller_is_complete(usb_msc_slot_request(slot)) == 0U)
    {
        return;
    }

    if (usb_msc_slot_request(slot)->completion_status != SSD_STATUS_OK)
    {
        usb_msc_unmap_fail(slot, USB_SCSI_ASC_WRITE_ERROR);
        return;
    }

    /* 屏障 FLUSH 完成：下一轮开始逐 range TRIM。 */
    if (slot->unmap_flush_done == false)
    {
        slot->unmap_flush_done = true;
        slot->unmap_submitted  = false;
        return;
    }

    slot->unmap_index++;
    slot->unmap_submitted = false;

    if (slot->unmap_index >= slot->unmap_count)
    {
        usb_msc_slot_release(slot);
        (void)tud_msc_async_io_done(0, false);
        return;
    }

    if (!usb_msc_unmap_submit_next(slot))
    {
        usb_msc_unmap_fail(slot, USB_SCSI_ASC_WRITE_ERROR);
    }
}

/**
 * @brief 解析 UNMAP 参数表并接管命令
 *
 * @param lun 逻辑单元（本仓恒 0）
 * @param param UNMAP 参数表
 * @param length 数据相长度（字节）
 * @return TUD_MSC_RET_ASYNC / ERROR（空表返回 0）
 */
static int32_t usb_msc_unmap_parse(BYTE lun, const BYTE *param, WORD length)
{
    if ((lun != USB_MSC_LUN) || (param == NULL) || (length < USB_MSC_UNMAP_HDR_SIZE))
    {
        return TUD_MSC_RET_ERROR;
    }

    const WORD data_len = usb_msc_be16(param);
    const WORD desc_len = usb_msc_be16(param + 2U);

    /* UNMAP block descriptor（SBC-4）：8B LBA + 4B NLB + 4B reserved = 16B。 */
    if ((data_len < USB_MSC_UNMAP_HDR_SIZE) || (data_len > length) || (desc_len != (WORD)(data_len - USB_MSC_UNMAP_HDR_SIZE)) || ((desc_len % USB_MSC_UNMAP_DESC_SIZE) != 0U))
    {
        (void)tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, USB_SCSI_ASC_INVALID_FIELD_IN_PARAM_LIST, USB_SCSI_ASCQ_NONE); /* invalid field in parameter list */
        return TUD_MSC_RET_ERROR;
    }

    const DWORD count = (DWORD)desc_len / USB_MSC_UNMAP_DESC_SIZE;

    if (count > USB_MSC_UNMAP_MAX)
    {
        (void)tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, USB_SCSI_ASC_INVALID_FIELD_IN_PARAM_LIST, USB_SCSI_ASCQ_NONE);
        return TUD_MSC_RET_ERROR;
    }

    /* 先校验并统计有效 descriptor：NLB == 0 是规范允许的 no-op，跳过。
     * 全部 no-op 时按成功返回，不占用控制槽。 */
    DWORD stored = 0U;

    for (DWORD i = 0U; i < count; i++)
    {
        const BYTE *desc   = param + USB_MSC_UNMAP_HDR_SIZE + (i * USB_MSC_UNMAP_DESC_SIZE);
        const DWORD lba_hi = usb_msc_be32(desc);
        const DWORD lba_lo = usb_msc_be32(desc + 4U);
        const DWORD blocks = usb_msc_be32(desc + USB_MSC_UNMAP_DESC_NLB_OFFSET);

        if (blocks == 0U)
        {
            continue;
        }
        /* 本仓 host LBA 是 32 位：高 32 位非零必然越界。 */
        if ((lba_hi != 0U) || !ssd_controller_lba_range_ok(&g_ctrl, lba_lo, blocks))
        {
            (void)tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, USB_SCSI_ASC_LBA_OUT_OF_RANGE, USB_SCSI_ASCQ_NONE); /* LBA out of range */
            return TUD_MSC_RET_ERROR;
        }
        stored++;
    }

    if (stored == 0U)
    {
        return 0; /* 空参数表 / 全部 no-op：成功，无操作 */
    }
    if (!usb_msc_ready())
    {
        return TUD_MSC_RET_ERROR;
    }

    usb_msc_slot_s *slot = usb_msc_slot_alloc_unmap();

    if (slot == NULL)
    {
        return TUD_MSC_RET_ERROR;
    }

    slot->start_ms = delay_now_ms();

    DWORD store = 0U;

    for (DWORD i = 0U; i < count; i++)
    {
        const BYTE *desc   = param + USB_MSC_UNMAP_HDR_SIZE + (i * USB_MSC_UNMAP_DESC_SIZE);
        const DWORD lba_lo = usb_msc_be32(desc + 4U);
        const DWORD blocks = usb_msc_be32(desc + USB_MSC_UNMAP_DESC_NLB_OFFSET);

        if (blocks == 0U)
        {
            continue;
        }
        slot->unmap_lba[store]    = lba_lo;
        slot->unmap_blocks[store] = blocks;
        store++;
    }

    slot->is_write         = false;
    slot->is_flush         = false;
    slot->is_unmap         = true;
    slot->order            = s_order_next++;
    slot->lba              = 0U;
    slot->offset           = 0U;
    slot->length           = 0U;
    slot->usb_buffer       = NULL;
    slot->ack_sent         = false;
    slot->unmap_count      = (BYTE)stored;
    slot->unmap_index      = 0U;
    slot->unmap_flush_done = false;
    slot->unmap_submitted  = false;
    slot->state            = USB_MSC_SLOT_WAIT_GATE;

    if (!cli_host_busy())
    {
        if (!usb_msc_unmap_start(slot))
        {
            (void)tud_msc_set_sense(lun, SCSI_SENSE_MEDIUM_ERROR, USB_SCSI_ASC_WRITE_ERROR, USB_SCSI_ASCQ_NONE);
            usb_msc_slot_release(slot);
            return TUD_MSC_RET_ERROR;
        }
    }

    return TUD_MSC_RET_ASYNC;
}

/**
 * @brief READ10/WRITE10 公共入口：校验 + 槽分配 + 提交
 *
 * @param is_write true = WRITE
 * @param lba 起始 LBA
 * @param offset 字节偏移（本仓恒 0）
 * @param buffer USB 端点缓冲
 * @param length 数据相长度（字节）
 * @return TUD_MSC_RET_ASYNC / BUSY / ERROR
 */
static int32_t usb_msc_transfer(BOOL is_write, DWORD lba, DWORD offset, void *buffer, DWORD length)
{
    if (usb_msc_slot_match(is_write, lba, offset, length) != NULL)
    {
        return TUD_MSC_RET_ASYNC; /* 同一命令的重入（协议栈事件重放） */
    }

    /* offset 只在端点缓冲小于 block 时非零；本仓 16384B ≥ 512B，恒为 0。
     * length 是主机数据阶段长度，先做边界检查再交给内部（H29/H31）。 */
    if ((offset != 0U) || (length == 0U) || (length > (DWORD)CFG_TUD_MSC_EP_BUFSIZE) || ((length % USB_MSC_BLOCK_SIZE) != 0U))
    {
        /* 参数非法 ≠ 介质不存在：报 ILLEGAL REQUEST / invalid field (0x24)。 */
        (void)tud_msc_set_sense(USB_MSC_LUN, SCSI_SENSE_ILLEGAL_REQUEST, USB_SCSI_ASC_INVALID_FIELD_IN_CDB, USB_SCSI_ASCQ_NONE);
        return TUD_MSC_RET_ERROR;
    }

    const DWORD blocks = length / USB_MSC_BLOCK_SIZE;

    if (!ssd_controller_lba_range_ok(&g_ctrl, lba, blocks))
    {
        /* LBA 越界：ILLEGAL REQUEST / LBA out of range (0x21)，与 UNMAP 同口径。 */
        (void)tud_msc_set_sense(USB_MSC_LUN, SCSI_SENSE_ILLEGAL_REQUEST, USB_SCSI_ASC_LBA_OUT_OF_RANGE, USB_SCSI_ASCQ_NONE);
        return TUD_MSC_RET_ERROR;
    }

    /* controller 未就绪：主机应先靠 TEST UNIT READY 等待，不应下发读写。 */
    if (!usb_msc_ready())
    {
        return TUD_MSC_RET_ERROR;
    }

    usb_msc_slot_s *slot = usb_msc_slot_alloc(true);

    if (slot == NULL)
    {
        /* 两个数据槽都在飞（典型是两个写 chunk 的 lookahead）⇒ 回 BUSY 让协议栈重投。
         * 读路径也回 BUSY：TinyUSB 的 proc_read_io_data 支持该返回值（伪完成重投）；
         * 回 ERROR 会让「写两 chunk 后立刻读」的主机看到一次无谓的 I/O error。 */
        return TUD_MSC_RET_BUSY;
    }

    slot->start_ms   = delay_now_ms();
    slot->is_write   = is_write;
    slot->is_flush   = false;
    slot->is_unmap   = false;
    slot->fua        = (is_write == true) ? s_cmd_fua : false;
    slot->order      = s_order_next++;
    slot->lba        = lba;
    slot->offset     = offset;
    slot->length     = length;
    slot->usb_buffer = buffer;
    slot->ack_sent   = false;

    if (is_write)
    {
        /* USB 端点缓冲生命周期只到回调返回：先拷进本槽 bounce（FTL 只持有 bounce）。
         * 读路径由 FTL 直接写 bounce，完成时再拷回端点缓冲。 */
        memcpy(slot->bounce, buffer, length);
    }

    slot->state = USB_MSC_SLOT_WAIT_GATE;

    if (!cli_host_busy())
    {
        if (usb_msc_slot_write_admit(slot) == false)
        {
            return TUD_MSC_RET_ASYNC; /* 保持 WAIT_GATE，usb_msc_task 下一轮重试 */
        }

        const ssd_status_e st = usb_msc_slot_submit(slot);

        if (st != SSD_STATUS_OK)
        {
            if (st != SSD_STATUS_BUSY)
            {
                usb_msc_note_submit_error(st, slot);
            }

            usb_msc_slot_release(slot);

            // 瞬态（host queue 满）交给协议栈重投；其余按 sense 失败。
            return (st == SSD_STATUS_BUSY) ? TUD_MSC_RET_BUSY : TUD_MSC_RET_ERROR;
        }
    }

    return TUD_MSC_RET_ASYNC;
}

/**
 * @brief 完成一个槽：回执 / 回收 entry / 记录 deferred
 *
 * @param slot 槽
 */
static void usb_msc_slot_complete(usb_msc_slot_s *slot)
{
    // 写 chunk 的"设备侧延迟"在**回执点**记录（volatile write cache：完成可能远晚于
    // 回执）；读 / flush 在完成点记录。
    if (slot->is_write == false)
    {
        usb_msc_note_latency(slot->submit_us);
    }

    const BOOL ok = (usb_msc_slot_request(slot)->completion_status == SSD_STATUS_OK);

    /* 主机已复位/拔出/挂起：TinyUSB 可能已把端点缓冲用于新的 CBW/枚举，
     * 此时既不能回写数据，也不能再依赖 pending_io。 */
    const BOOL session_stale = (slot->session_gen != s_session_gen);

    if ((session_stale != false) || !tud_ready())
    {
        /* 只有 FLUSH 槽才需要撤销 controller 的 flush job；数据槽被丢弃时不能
         * 顺手取消别人（可能是总线复位后新命令）的 FLUSH。 */
        if ((session_stale != false) && (slot->is_flush != false) && (g_ctrl.flush_req == usb_msc_slot_request(slot)))
        {
            ssd_controller_cancel_flush(&g_ctrl);
        }

        /* 会话结束由 TinyUSB 的 mscd_reset 清 pending_io：不能再配对
         * async_io_done，否则可能误配给新会话的新命令。只有「未就绪但会话未变」
         * （挂起）才需要与之前的 ASYNC 返回配对。 */
        const BOOL ack_owed = (session_stale == false) && (slot->ack_sent == false);

        usb_msc_slot_release(slot);

        if (ack_owed)
        {
            (void)tud_msc_async_io_done(TUD_MSC_RET_ERROR, false);
        }
        return;
    }

    if (slot->is_write)
    {
        if (slot->fua)
        {
            /* FUA：提交时没有回执，这里才是回执点（FTL 组完成 = program + 映射确认）。
             * 失败直接 CSW failed，不置 deferred——主机还没收到成功。 */
            usb_msc_note_latency(slot->submit_us);
            if (ok == false)
            {
                (void)tud_msc_set_sense(USB_MSC_LUN, SCSI_SENSE_MEDIUM_ERROR, USB_SCSI_ASC_WRITE_ERROR, USB_SCSI_ASCQ_NONE);
            }

            const DWORD length = slot->length;

            usb_msc_slot_release(slot);
            (void)tud_msc_async_io_done((ok == true) ? (int32_t)length : TUD_MSC_RET_ERROR, false);
            return;
        }

        // 非 FUA：提交即回执（volatile write cache）：本槽的完成只回收缓冲；
        // 失败延后到**下一条命令**上报（本命令的 CSW 早已发出）。
        if (ok == false)
        {
            s_deferred_write_error = true;
            LOGE("[USB ] write chunk failed after ack lba=%lu len=%lu — deferred\r\n", slot->lba, slot->length);
        }

        usb_msc_slot_release(slot);
        return;
    }

    if ((ok == true) && (slot->is_flush == false))
    {
        memcpy(slot->usb_buffer, slot->bounce, slot->length);
    }
    else if (ok == false)
    {
        /* 写失败已在上面分支处理（FUA 直接 sense / 非 FUA deferred）。这里只剩
         * READ 与 FLUSH：不设 sense 时 TinyUSB 会默认报 NOT READY/0x3A
         * （MEDIUM NOT PRESENT），主机会误判介质不存在。 */
        (void)tud_msc_set_sense(USB_MSC_LUN, SCSI_SENSE_MEDIUM_ERROR, (slot->is_flush != false) ? USB_SCSI_ASC_WRITE_ERROR : USB_SCSI_ASC_UNRECOVERED_READ_ERROR, USB_SCSI_ASCQ_NONE);
    }

    /* FLUSH 无数据阶段：成功回执为 0（TinyUSB 侧已按命令类型区分 0 的语义）。 */
    const int32_t transfer_result = (ok == true) ? (slot->is_flush ? 0 : (int32_t)slot->length) : TUD_MSC_RET_ERROR;

    usb_msc_slot_release(slot);
    (void)tud_msc_async_io_done(transfer_result, false);
}

/**
 * @brief 填 MODE SENSE page 0x08（Caching），返回字节数。
 *
 * WCE=0：对主机宣称无 write cache。主机若看到 WCE=1，会在格式化前用 MODE SELECT
 * 设置缓存页；本仓只接受、不应用 mode parameter list，保持 WCE=0 让主机走简单路径
 * （与“不提供 caching page”的既有行为等效）。
 */
static DWORD usb_msc_mode_page_caching(BYTE *page)
{
    page[0] = USB_SCSI_MODE_PAGE_CACHING;
    page[1] = USB_SCSI_MODE_PAGE_CACHING_DATA_LEN;
    memset(&page[2], 0, USB_SCSI_MODE_PAGE_CACHING_DATA_LEN);
    page[2] = 0x00U; /* WCE = 0 */
    return USB_SCSI_MODE_PAGE_CACHING_LEN;
}

/**
 * @brief 记录 CBW 的起始 LBA / 块数 / FUA（TinyUSB 弱钩子）
 *
 * @param lun 逻辑单元（本仓恒 0）
 * @param lba 起始 LBA
 * @param block_count 块数
 * @param fua WRITE(10) FUA 位
 */
void tud_msc_cbw_info_cb(uint8_t lun, uint32_t lba, uint16_t block_count, BOOL fua)
{
    if (lun == USB_MSC_LUN)
    {
        s_cmd_lba    = lba;
        s_cmd_blocks = block_count;
        s_cmd_fua    = fua;
        /* ⚠ 不清 deferred 写错误：它必须留给下一条能承载状态的命令上报。 */
    }
}

/**
 * @brief 打印 USB 写分块与延迟统计（诊断）
 */
void usb_msc_io_stats_print(void)
{
    LOGI("[USB ] wr chunk calls=%lu bytes=%lu <=512=%lu <=4K=%lu <=16K=%lu >16K=%lu\r\n", s_wr_hist.calls, s_wr_hist.bytes, s_wr_hist.le_block, s_wr_hist.le_4k, s_wr_hist.le_epbuf, s_wr_hist.gt_epbuf);

    {
        const DWORD avg_us = (s_lat.calls == 0U) ? 0U : (DWORD)(s_lat.total_us / s_lat.calls);

        LOGI("[USB ] latency n=%lu avg=%luus max=%luus last=%luus total=%llums\r\n", s_lat.calls, avg_us, s_lat.max_us, s_lat.last_us, s_lat.total_us / 1000U);
    }
}

/**
 * @brief 是否有 USB 槽尚未回到 FREE
 *
 * @return true = 任一槽 WAIT_GATE/IN_FLIGHT
 */
BOOL usb_msc_host_busy(void)
{
    for (BYTE i = 0U; i < USB_MSC_SLOT_NUM; i++)
    {
        if (s_slots[i].state != USB_MSC_SLOT_FREE)
        {
            return true;
        }
    }

    return false;
}

/**
 * @brief 清零 USB MSC 槽状态（host queue 由 controller 初始化）
 *
 */
void usb_msc_init(void)
{
    /* 在 usb_device_init() 之前调用，USB ISR 尚未使能；可整体清零。
     * 2 号槽是控制槽（FLUSH/UNMAP）：bounce 保持 NULL。 */
    memset(s_slots, 0, sizeof(s_slots));
    s_slots[0].bounce      = s_bounce0;
    s_slots[1].bounce      = s_bounce1;
    s_cmd_lba              = 0U;
    s_cmd_blocks           = 0U;
    s_cmd_fua              = false;
    s_order_next           = 0U;
    s_deferred_write_error = false;
    s_session_gen          = 0U;
    s_session_gen_seen     = 0U;
}

/**
 * @brief 主循环推进在飞槽：提交、FUA/写回执、UNMAP、完成回收
 *
 */
void usb_msc_task(void)
{
    DWORD done_mask = 0U;

    /* 先消费会话代次：退役旧会话的 WAIT_GATE / stale FLUSH。
     * 之后本函数看到的所有槽都属于当前会话。 */
    usb_msc_session_apply();

    /* 按到达序处理槽位：先回收先到达的槽、按序记录 deferred 错误。回执已在提交时发出，
     * 这里的顺序只影响缓冲回收与错误归因，不影响 CSW 时机（volatile write cache）。 */
    for (BYTE n = 0U; n < USB_MSC_SLOT_NUM; n++)
    {
        usb_msc_slot_s *slot = NULL;

        for (BYTE i = 0U; i < USB_MSC_SLOT_NUM; i++)
        {
            usb_msc_slot_s *cand = &s_slots[i];

            if ((cand->state == USB_MSC_SLOT_FREE) || ((done_mask & (1UL << i)) != 0U))
            {
                continue;
            }
            if ((slot == NULL) || (cand->order < slot->order))
            {
                slot = cand;
            }
        }

        if (slot == NULL)
        {
            break;
        }
        done_mask |= (1UL << (BYTE)(slot - s_slots));

        if (slot->state == USB_MSC_SLOT_WAIT_GATE)
        {
            if ((slot->start_ms != 0U) && (ssd_request_expired(slot->start_ms, delay_now_ms()) != false))
            {
                usb_msc_slot_timeout(slot);

                if ((slot->entry == NULL) || (ssd_controller_request_reclaimable(&g_ctrl, usb_msc_slot_request(slot)) != false))
                {
                    usb_msc_slot_release(slot);
                }
                continue;
            }

            if (!tud_ready())
            {
                /* 挂起（会话未变）：命令作废，并配对之前的 ASYNC 返回。
                 * 会话结束的 WAIT_GATE 已在 apply 里退役，不会走到这里。 */
                usb_msc_slot_release(slot);
                (void)tud_msc_async_io_done(TUD_MSC_RET_ERROR, false);
                continue;
            }

            if (cli_host_busy())
            {
                continue;
            }

            if (usb_msc_slot_write_admit(slot) == false)
            {
                continue; /* 写 admission 暂缓；读/FLUSH/UNMAP 不受影响 */
            }

            if (slot->is_unmap)
            {
                if (!usb_msc_unmap_start(slot))
                {
                    usb_msc_unmap_fail(slot, USB_SCSI_ASC_WRITE_ERROR);
                    continue;
                }
            }
            else
            {
                const ssd_status_e st = usb_msc_slot_submit(slot);

                if (st == SSD_STATUS_BUSY)
                {
                    continue; /* 瞬态：保留 WAIT_GATE，下一轮重试 */
                }
                if (st != SSD_STATUS_OK)
                {
                    usb_msc_note_submit_error(st, slot);
                    usb_msc_slot_release(slot);
                    (void)tud_msc_async_io_done(TUD_MSC_RET_ERROR, false);
                    continue;
                }
            }
        }

        /* UNMAP 是逐 range 的异步 job：完成前不进入下面的通用完成路径。 */
        if (slot->is_unmap)
        {
            usb_msc_unmap_step(slot);
            continue;
        }

        ssd_req_s *request  = usb_msc_slot_request(slot);
        const BOOL complete = (ssd_controller_is_complete(request) != 0U);

        /* 请求级 deadline：只对未完成、未超时的槽判超时。 */
        if ((complete == false) && (request->timed_out == false) && (slot->start_ms != 0U) && (ssd_request_expired(slot->start_ms, delay_now_ms()) != false))
        {
            usb_msc_slot_timeout(slot);
        }
        else if ((request->timed_out != false) && (slot->ack_sent == false))
        {
            /* controller 可能先于 USB task 判超时（FLUSH/TRIM）：补一次 host 回执。 */
            usb_msc_slot_timeout(slot);
        }

        if (request->timed_out != false)
        {
            if (ssd_controller_request_reclaimable(&g_ctrl, request) != false)
            {
                /* 迟到的非 FUA 写失败仍要在下一条命令上报，避免静默丢错误。
                 * TIMEOUT 是 abort 的组状态，不是介质失败，不能报成 deferred。 */
                if ((slot->is_write != false) && (slot->is_flush == false) && (slot->fua == false) && (request->completion_status != SSD_STATUS_OK) && (request->completion_status != SSD_STATUS_TIMEOUT))
                {
                    s_deferred_write_error = true;
                }

                usb_msc_slot_release(slot);
            }
            continue;
        }

        /* 写 chunk（含末 chunk）：提交后立即回流控回执（volatile write cache）。
         * 设备 cache 序列因此可以跨 host 命令连续；槽位的 FTL 完成只用于回收缓冲
         * 与 deferred error 记录。
         * ⚠ 旧会话的槽不能回执：async_io_done 是单飞行配对，复位后可能误配给
         * 新会话正在等完成的命令。 */
        const BOOL session_stale = (slot->session_gen != s_session_gen);

        if ((session_stale == false) && (slot->is_write == true) && (slot->is_flush == false) && (slot->ack_sent == false) && (slot->fua == false))
        {
            slot->ack_sent = true;
            usb_msc_note_latency(slot->submit_us); /* 主机可见延迟 = submit → 回执 */
            (void)tud_msc_async_io_done((int32_t)slot->length, false);
        }

        if (complete == false)
        {
            continue;
        }

        usb_msc_slot_complete(slot);
    }
}

/* ─────────────────────────── MSC 回调 ─────────────────────────── */

/**
 * @brief TinyUSB READ(10) 回调
 *
 * @param lun 逻辑单元（本仓恒 0）
 * @param lba 起始 LBA
 * @param offset 字节偏移（本仓恒 0）
 * @param buffer USB 端点缓冲
 * @param bufsize 数据相长度（字节）
 * @return TUD_MSC_RET_ASYNC / BUSY / ERROR
 */
int32_t tud_msc_read10_cb(BYTE lun, DWORD lba, DWORD offset, void *buffer, DWORD bufsize)
{
    /* 单 LUN 设备：lun 是主机输入，越界直接拒绝（H29/H31）。 */
    if (lun != USB_MSC_LUN)
    {
        return TUD_MSC_RET_ERROR;
    }

    // deferred 写错误在**下一条能承载状态的命令**上报一次（近似 SCSI deferred sense）。
    if (usb_msc_take_deferred_error(lun) == true)
    {
        return TUD_MSC_RET_ERROR;
    }

    return usb_msc_transfer(false, lba, offset, buffer, bufsize);
}

/**
 * @brief TinyUSB WRITE(10) 回调（FUA 来自 CBW 钩子）
 *
 * @param lun 逻辑单元（本仓恒 0）
 * @param lba 起始 LBA
 * @param offset 字节偏移（本仓恒 0）
 * @param buffer USB 端点缓冲
 * @param bufsize 数据相长度（字节）
 * @return TUD_MSC_RET_ASYNC / BUSY / ERROR
 */
int32_t tud_msc_write10_cb(BYTE lun, DWORD lba, DWORD offset, BYTE *buffer, DWORD bufsize)
{
    if (lun != USB_MSC_LUN)
    {
        return TUD_MSC_RET_ERROR;
    }

    if (usb_msc_take_deferred_error(lun) == true)
    {
        return TUD_MSC_RET_ERROR;
    }

    usb_msc_note_write(bufsize);

    return usb_msc_transfer(true, lba, offset, buffer, bufsize);
}

/**
 * @brief TinyUSB INQUIRY 回调：填厂商/产品/版本串
 *
 * @param lun 逻辑单元（本仓恒 0）
 * @param vendor_id 8 字节厂商串
 * @param product_id 16 字节产品串
 * @param product_rev 4 字节版本串
 */
void tud_msc_inquiry_cb(BYTE lun, BYTE vendor_id[8], BYTE product_id[16], BYTE product_rev[4])
{
    /* 回调参数是 8/16/4 字节定长数组（见 msc_device.h）；字符串字面量长度
     * 小于目标，直接 memcpy 会越界读，所以显式补空格到定长。 */
    static const BYTE vendor_id_src[8]   = {'S', 'O', 'F', 'T', 'S', 'S', 'D', ' '};
    static const BYTE product_id_src[16] = {'N', 'A', 'N', 'D', ' ', 'U', '-', 'D', 'I', 'S', 'K', ' ', ' ', ' ', ' ', ' '};
    static const BYTE product_rev_src[4] = {'1', '.', '0', '0'};

    if (lun != USB_MSC_LUN)
    {
        return;
    }

    memcpy(vendor_id, vendor_id_src, sizeof(vendor_id_src));
    memcpy(product_id, product_id_src, sizeof(product_id_src));
    memcpy(product_rev, product_rev_src, sizeof(product_rev_src));
}

/**
 * @brief TinyUSB TEST UNIT READY 回调
 *
 * @param lun 逻辑单元（本仓恒 0）
 * @return true = Controller READY
 */
BOOL tud_msc_test_unit_ready_cb(BYTE lun)
{
    return (lun == USB_MSC_LUN) && usb_msc_ready();
}

/**
 * @brief TinyUSB READ CAPACITY 回调：填容量/块大小
 *
 * @param lun 逻辑单元（本仓恒 0）
 * @param block_count 块数
 * @param block_size 块大小出参
 */
void tud_msc_capacity_cb(BYTE lun, DWORD *block_count, WORD *block_size)
{
    if (lun != USB_MSC_LUN)
    {
        *block_count = 0U;
        *block_size  = 0U;
        return;
    }

    *block_count = ssd_controller_host_lba_max(&g_ctrl);
    *block_size  = USB_MSC_BLOCK_SIZE;
}

/**
 * @brief MODE SENSE(6) 应用响应（TinyUSB 补丁）。
 *
 * Windows 的格式化流程会查询 caching（0x08）与 informational exceptions（0x1C）
 * mode page；TinyUSB 内建响应不含任何 page。这里按 SCSI 规范返回最小合法页：
 * WCE=0（无写缓存）、IE 全关。
 *
 * @note 不是「Windows 无法完成格式化」的根因：根因是主机对 partitionless 介质
 *       （超级软盘布局）的处理。
 *
 * @param lun 逻辑单元（本仓恒 0）
 * @param page 请求的 mode page（CDB[2] & 0x3F）
 * @param buffer 响应缓冲
 * @param bufsize 缓冲容量
 * @return >0 = 响应字节数；0 = 交给 TinyUSB 内建空响应
 */
int32_t tud_msc_mode_sense_cb(BYTE lun, BYTE page, BYTE *buffer, WORD bufsize)
{
    if ((lun != USB_MSC_LUN) || (buffer == NULL) || (bufsize < 4U))
    {
        return 0;
    }

    DWORD n = 0U;

    switch (page)
    {
        case USB_SCSI_MODE_PAGE_CACHING:
            n = usb_msc_mode_page_caching(&buffer[4]);
            break;
        case USB_SCSI_MODE_PAGE_IE:
            n = usb_msc_mode_page_ie(&buffer[4]);
            break;
        case USB_SCSI_MODE_PAGE_ALL:
            n += usb_msc_mode_page_caching(&buffer[4 + n]);
            n += usb_msc_mode_page_ie(&buffer[4 + n]);
            break;
        default:
            return 0;
    }

    if ((4U + n) > bufsize)
    {
        return 0;
    }

    buffer[0] = (BYTE)((4U + n) - 1U); /* mode data length = total - 1 */
    buffer[1] = 0U;                    /* medium type */
    buffer[2] = 0U;                    /* device-specific: not write protected */
    buffer[3] = 0U;                    /* block descriptor length */
    return (int32_t)(4U + n);
}

/**
 * @brief TinyUSB START STOP UNIT 回调
 *
 * @param lun 逻辑单元（本仓恒 0）
 * @param power_condition 电源条件
 * @param start true = 启动
 * @param load_eject true = 装载/弹出
 * @return true = 支持该 LUN
 */
BOOL tud_msc_start_stop_cb(BYTE lun, BYTE power_condition, BOOL start, BOOL load_eject)
{
    (void)power_condition;
    (void)start;
    (void)load_eject;

    return lun == USB_MSC_LUN;
}

/**
 * @brief TinyUSB 可写查询
 *
 * @param lun 逻辑单元（本仓恒 0）
 * @return true = 可写
 */
BOOL tud_msc_is_writable_cb(BYTE lun)
{
    return lun == USB_MSC_LUN;
}

/**
 * @brief SCSI 兜底：SYNCHRONIZE CACHE / UNMAP
 *
 * @param lun 逻辑单元（本仓恒 0）
 * @param scsi_cmd 参数
 * @param buffer USB 端点缓冲
 * @param bufsize 数据相长度（字节）
 * @return TUD_MSC_RET_ASYNC / ERROR
 */
int32_t tud_msc_scsi_cb(BYTE lun, BYTE const scsi_cmd[16], void *buffer, WORD bufsize)
{
    if (lun != USB_MSC_LUN)
    {
        return TUD_MSC_RET_ERROR;
    }

    if (scsi_cmd[0] == USB_SCSI_UNMAP)
    {
        if (usb_msc_take_deferred_error(lun) == true)
        {
            return TUD_MSC_RET_ERROR;
        }
        return usb_msc_unmap_parse(lun, (const BYTE *)buffer, bufsize);
    }

    /* MODE SELECT(6/10)：主机可能在格式化前设置 mode page。本仓没有可配置的
     * mode page（WCE 恒 0、无 IE），参数表接受并忽略；回 ILLEGAL REQUEST 只会
     * 让主机把设备当作不支持该命令。 */
    if ((scsi_cmd[0] == USB_SCSI_MODE_SELECT_6) || (scsi_cmd[0] == USB_SCSI_MODE_SELECT_10))
    {
        return 0;
    }

    if ((scsi_cmd[0] != USB_SCSI_SYNCHRONIZE_CACHE_10) && (scsi_cmd[0] != USB_SCSI_SYNCHRONIZE_CACHE_16))
    {
        /* 未知命令一律 ILLEGAL REQUEST；打印 opcode 便于定位主机实际下发什么
         * （Windows format/diskpart 会用到 VERIFY 等）。 */
        LOGW("[USB ] unsupported SCSI opcode=0x%02hhX lun=%hhu\r\n", scsi_cmd[0], lun);
        return TUD_MSC_RET_ERROR;
    }

    // deferred 写错误在 FLUSH 上同样要上报（它是持久性屏障；上一条已回执的写失败
    // 不能被吞掉）。
    if (usb_msc_take_deferred_error(lun) == true)
    {
        return TUD_MSC_RET_ERROR;
    }

    /* 不能再检查 usb_msc_host_busy()：写路径提交即回执后，主机可在最后一个写 chunk
     * 仍 IN_FLIGHT 时紧接 SYNCHRONIZE CACHE；这道门会把正常的 fsync 拒掉。
     * FLUSH 的有序性由 FTL 的 ftl_flush_step 保证（它等待 wr_inflight / WRITE_CONFIRM），
     * 这里只需要一个可用的 FLUSH 槽。 */
    if (!usb_msc_ready())
    {
        return TUD_MSC_RET_ERROR;
    }

    usb_msc_slot_s *slot = usb_msc_slot_alloc(false);

    if (slot == NULL)
    {
        return TUD_MSC_RET_ERROR;
    }

    slot->start_ms   = delay_now_ms();
    slot->is_write   = false;
    slot->is_flush   = true;
    slot->is_unmap   = false;
    slot->order      = s_order_next++;
    slot->lba        = 0U;
    slot->offset     = 0U;
    slot->length     = 0U;
    slot->usb_buffer = NULL;
    slot->ack_sent   = false;
    slot->state      = USB_MSC_SLOT_WAIT_GATE;

    if (cli_host_busy())
    {
        /* CLI 占用 host gate：同 READ/WRITE 先排队，由 usb_msc_task 提交。 */
    }
    else
    {
        const ssd_status_e st = usb_msc_slot_submit(slot);

        if (st == SSD_STATUS_BUSY)
        {
            /* 瞬态：保留 WAIT_GATE，usb_msc_task 下一轮重试。 */
        }
        else if (st != SSD_STATUS_OK)
        {
            usb_msc_note_submit_error(st, slot);
            usb_msc_slot_release(slot);
            return TUD_MSC_RET_ERROR;
        }
    }

    return TUD_MSC_RET_ASYNC;
}

/**
 * @brief READ(10) 完成回调（数据已在 task 交付）
 *
 * @param lun 逻辑单元（本仓恒 0）
 */
void tud_msc_read10_complete_cb(BYTE lun)
{
    /* READ 的数据与状态已在 usb_msc_task() 里交付；完成回调不需要额外动作。 */
    (void)lun;
}

/**
 * @brief WRITE(10) 完成回调（不做逐块 flush）
 *
 * @param lun 逻辑单元（本仓恒 0）
 */
void tud_msc_write10_complete_cb(BYTE lun)
{
    /* 不在这里 flush：每个 4KB 块都落盘会把 USB 写吞吐拖到 NAND 随机写水平。
     * 持久化由主机的 SYNCHRONIZE CACHE 驱动。 */
    (void)lun;
}

/**
 * @brief 返回设备描述符
 *
 * @return 描述符指针
 */
BYTE const *tud_descriptor_device_cb(void)
{
    return (BYTE const *)&s_device_desc;
}

/**
 * @brief 返回配置描述符
 *
 * @param index 描述符索引
 * @return 描述符指针
 */
BYTE const *tud_descriptor_configuration_cb(BYTE index)
{
    (void)index;
    return s_config_desc;
}

/**
 * @brief 返回字符串描述符
 *
 * @param index 描述符索引
 * @param langid 语言 ID
 * @return 描述符指针；NULL = 无此索引
 */
WORD const *tud_descriptor_string_cb(BYTE index, WORD langid)
{
    static WORD desc_str[1U + USB_MSC_STRING_MAX_CHARS];
    BYTE        char_count;

    (void)langid;

    if (index == 0U)
    {
        desc_str[1] = USB_MSC_LANGID_EN_US;
        char_count  = 1U;
    }
    else
    {
        if (index > USB_MSC_STRING_COUNT)
        {
            return NULL;
        }

        const char *str = s_string_desc[index - 1U];

        char_count = strlen(str);
        if (char_count > USB_MSC_STRING_MAX_CHARS)
        {
            char_count = USB_MSC_STRING_MAX_CHARS;
        }

        for (BYTE i = 0U; i < char_count; i++)
        {
            desc_str[1U + i] = (WORD)(BYTE)str[i];
        }
    }

    /* bLength = 2 字节头 + 每个 UTF-16 单元 2 字节；bDescriptorType 在高字节。 */
    const WORD desc_len = (2U * char_count + 2U);

    desc_str[0] = ((TUSB_DESC_STRING << 8) | desc_len);
    return desc_str;
}

/* ─────────────────────────── 挂载诊断 ─────────────────────────── */

/**
 * @brief 挂载回调：打印容量
 */
void tud_mount_cb(void)
{
    /* 新会话开始：旧会话的 slot 全部作废（bus reset 后必然重新枚举到这里，
     * 且发生在任何 class 命令之前）。 */
    s_session_gen++;

    /* 容量按 SCSI 坐标打印：sector 数 × 每 sector 字节。 */
    const DWORD block_count = ssd_controller_host_lba_max(&g_ctrl);

    LOGI("[USB ] mounted (MSC BOT, %lu sectors x %u bytes)\r\n", block_count, USB_MSC_BLOCK_SIZE);
}

/**
 * @brief 卸载回调：推进会话代次（收尾在 usb_msc_task）
 */
void tud_umount_cb(void)
{
    /* 会话结束：只推进代次；WAIT_GATE 退役、IN_FLIGHT 丢弃、stale FLUSH 撤销
     * 全部由 usb_msc_task 在主循环按代次完成。 */
    s_session_gen++;

    LOGI("[USB ] unmounted\r\n");
}
