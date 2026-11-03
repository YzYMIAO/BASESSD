/*
 * usb_msc.c — USB Mass Storage Class（BOT）块设备前端
 *
 * 职责：TinyUSB MSC class 回调 + SCSI → controller 适配。
 *   READ(10)  → ssd_controller_submit(SSD_REQUEST_READ)  → USB IN 数据
 *   WRITE(10) → ssd_controller_submit(SSD_REQUEST_WRITE) → 数据已在 USB 缓冲
 *   SYNCHRONIZE CACHE(10) → SSD_REQUEST_FLUSH（异步：提交即返回 ASYNC，完成走
 *                            usb_msc_task；FTL 侧是 ftl_flush_begin/step job）
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
 *       缓冲），FTL 只持有 bounce 的指针。读/写各有 2 个槽（见 usb_msc_slot_t）：
 *       写需要 1 chunk lookahead（缓存程序序列尾页由下一 chunk 的首条命令确认），
 *       读不需要。
 */

#include "usb_msc.h"

#include "cli.h"
#include "controller.h"
#include "delay.h"      /* SysTick µs 分段计时（submit → complete） */
#include "device/dcd.h" /* DCD_EVENT_*：tusb.h 不导出该枚举 */
#include "ftl.h"
#include "log.h"
#include "ssd_compiler.h"
#include "tusb.h"

#include <string.h>

/* SCSI SYNCHRONIZE CACHE(10)；TinyUSB 的 scsi_cmd_type_t 未列出该命令。 */
#define USB_SCSI_SYNCHRONIZE_CACHE_10 0x35U

#define USB_MSC_EP_OUT       0x01U
#define USB_MSC_EP_IN        0x81U
#define USB_MSC_EP_SIZE      64U  /* 本仓选定的 FS bulk 端点包长（TinyUSB MSC 描述符参数） */
#define USB_MSC_POWER_MA     500U /* 总线供电声明；实际功耗以板级实测为准 */
#define USB_MSC_LANGID_EN_US 0x0409U

/* 设备描述符标识：VID 沿用 ST 开发板 VID，PID 为本仓选定的开发用值（未申请量产分配）。 */
#define USB_MSC_VID        0x0483U
#define USB_MSC_PID        0x5720U
#define USB_MSC_BCD_USB    0x0200U /* bcdUSB：USB 2.0 */
#define USB_MSC_BCD_DEVICE 0x0100U /* bcdDevice：本仓设备版本 1.00 */

/* SCSI READ CAPACITY 的块计数是 32 位；越界会让主机看到错误容量。 */
_Static_assert(FTL_LBA_MAX_AT_MAX <= UINT32_MAX, "SCSI block count must fit 32 bits");

/* USB MSC sector 与 FTL 映射单元必须一一对应；任一侧改变都会破坏 LBA 语义。 */
_Static_assert(USB_MSC_BLOCK_SIZE == FTL_IU_BYTES, "MSC sector must equal FTL mapping unit");

/* ─────────────────────────── 在飞请求 ─────────────────────────── */

#define USB_MSC_SLOT_NUM (2U)

typedef enum
{
    USB_MSC_SLOT_FREE = 0,
    /* host busy gate（本仓自拟：同一时刻只允许一个 host 前端提交 controller） */
    USB_MSC_SLOT_WAIT_GATE, /* 数据/参数已收到，等 CLI 释放 host busy gate 后提交 */
    USB_MSC_SLOT_IN_FLIGHT  /* 已提交 controller，等 FTL 完成 */
} usb_msc_slot_state_t;

/*
 * 一个在飞 chunk。写路径是 **volatile write cache** 语义（与主流 SSD 一致）：
 *   · 所有写 chunk（含末 chunk）**提交即回流控回执**，主机可以立刻发下一段/下一条
 *     命令——设备 cache 序列因此可以跨 host 命令连续（FTL 只在 Journal 顺序页满 /
 *     block 头 / 空闲 / LBA 不连续时主动收尾）；
 *   · 槽位保留到 FTL 完成（bounce 缓冲在 FTL 消费完之前不能被复用）；
 *   · 写 chunk 之后的失败**不在本条命令上报**：置 deferred error，由下一条
 *     READ/WRITE/FLUSH 命令的 CSW 报告一次（本仓用"下一条命令失败"近似 SCSI 的
 *     deferred sense；TinyUSB 不支持自定义 sense data）；
 *   · 两个槽都占用时写回调返回 TUD_MSC_RET_BUSY（数据留在端点缓冲，协议栈重投）。
 * 读路径不需要 lookahead：每 chunk 完成即回执，下一 chunk 才会来。
 */
typedef struct
{
    /* state / discard 会被 USB ISR（tud_event_hook_cb）改写，必须 volatile（H16）。 */
    volatile usb_msc_slot_state_t state;
    volatile bool                 discard; /* 主机已复位/拔出：完成后丢弃结果 */
    bool                          is_write;
    bool                          is_flush; /* SYNCHRONIZE CACHE：无数据阶段 */
    bool                          ack_sent; /* 写 chunk 的流控回执已发（提交即回执） */
    DWORD                         order;    /* 到达序：完成按序处理（前 chunk 错误先记录） */
    DWORD                         lba;
    DWORD                         offset;
    DWORD                         length;
    void                         *usb_buffer; /* TinyUSB 端点缓冲（读回写目标；写只读它一次） */
    DWORD                         submit_us;  /* submit 起点（SysTick µs），完成时算设备侧墙钟 */
    ssd_request_t                 request;
    BYTE                         *bounce; /* 指向本槽的 16 KiB 缓冲（见 s_bounce*） */
} usb_msc_slot_t;

static usb_msc_slot_t s_slots[USB_MSC_SLOT_NUM];

/* 两个 chunk 的 bounce：0 号留在 AXI .bss（与旧单缓冲一致），1 号放 DTCM。
 * 两块都只被 CPU 读写（DWC2 走 PIO，不经 DMA）⇒ DTCM 可用且最快；
 * SRAM4/SRAM12 在 UT 镜像里已被 FTL 表占满。 */
static BYTE s_bounce0[CFG_TUD_MSC_EP_BUFSIZE] __attribute__((aligned(4)));
static BYTE s_bounce1[CFG_TUD_MSC_EP_BUFSIZE] DTCM_DATA __attribute__((aligned(4)));

/* 当前 SCSI 命令的 CBW 信息（tud_msc_cbw_info_cb 填写）。它只用于给 FTL 计算
 * 「本 chunk 之后同一命令还剩多少 LBA」（cache 序列 END 的落点）；序列本身由 FTL
 * 的顺序流检测器判定，host 传输段边界不构成断点。0 = 未知（按单 chunk 处理）。 */
static DWORD s_cmd_lba;
static DWORD s_cmd_blocks;

static DWORD s_order_next; /* 下一条到达 chunk 的序 */

/* 已提前回执的写 chunk 后来失败（volatile write cache）：留到下一条命令上报一次。 */
static bool s_deferred_write_error;

/**
 * @brief 取走 deferred 写错误（有则本次命令失败）。
 *
 * @return true = 上一条已回执的写有失败；本次命令应失败（并已清除标志）
 *         false = 无 deferred 错误
 */
static bool usb_msc_take_deferred_error(void)
{
    /* FTL 侧的迟到结论优先（RISK-CACHE-27）：早回执后写组在页编程完成时就报 OK，
     * FAIL_CACHE 要晚一条命令才到 ⇒ 那个结论只存在于 FTL 的粘性错误里。 */
    for (ftl_die_id_t die = 0U; die < DIE_MAX; die++)
    {
        if (ftl_take_deferred_error(g_ctrl.ftl[die]) == true)
        {
            LOGW("[USB ] deferred write error (FTL) — fail the next command\r\n");
            return true;
        }
    }

    if (s_deferred_write_error == false)
    {
        return false;
    }

    s_deferred_write_error = false;
    LOGW("[USB ] deferred write error — fail the next command (volatile write cache)\r\n");
    return true;
}

/* 【本仓补丁】TinyUSB CBW 信息钩子：命令开始时记录总 block_count / 起始 LBA。
 * 应用侧强符号，覆盖 msc_device.c 的 weak 默认实现。 */
void tud_msc_cbw_info_cb(uint8_t lun, uint32_t lba, uint16_t block_count)
{
    if (lun == 0U)
    {
        s_cmd_lba    = lba;
        s_cmd_blocks = block_count;
        /* ⚠ 不清 deferred 写错误：它必须留给下一条能承载状态的命令上报。 */
    }
}

/* WRITE(10) 数据相分块直方图（RISK-NAND-04 取证）。
 *
 * TinyUSB 按 CFG_TUD_MSC_EP_BUFSIZE 分块回调，桶值就是本次数据相长度：
 * 大量 <=512B 说明主机按单扇区下发（BOT 往返主导）；<=4K 说明每块 4 KB；
 * <=16K 说明 16 KB 端点缓冲生效（每块一次 FTL 异步完成往返）。只在 `info`
 * 时打印，运行期不输出，不影响时序。 */
_Static_assert(CFG_TUD_MSC_EP_BUFSIZE == 16384U, "histogram label <=16K depends on EP bufsize");

typedef struct
{
    DWORD calls;    /* 回调次数 = 数据相分块数 */
    DWORD bytes;    /* 累计字节 */
    DWORD le_block; /* <= 512 B */
    DWORD le_4k;    /* (512, 4096] B */
    DWORD le_epbuf; /* (4096, 16384] B */
    DWORD gt_epbuf; /* > 16384 B（当前配置下不应出现） */
} usb_msc_wr_hist_t;

static usb_msc_wr_hist_t s_wr_hist;

/* submit → complete 的设备侧墙钟（SysTick µs）。用来区分：
 *   · 十几 ms：设备处理本身；
 *   · 只有几 ms：剩余时间在 USB/BOT 主机侧等待，不是 CPU/FTL。
 * 只在 `info` 打印，运行期零输出。 */
typedef struct
{
    DWORD calls;
    QWORD total_us;
    DWORD max_us;
    DWORD last_us;
} usb_msc_lat_t;

static usb_msc_lat_t s_lat;

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

static void usb_msc_note_write(DWORD bufsize)
{
    s_wr_hist.calls++;
    s_wr_hist.bytes += bufsize;

    if (bufsize <= USB_MSC_BLOCK_SIZE)
    {
        s_wr_hist.le_block++;
    }
    else if (bufsize <= 4096U)
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

void usb_msc_io_stats_print(void)
{
    LOGI("[USB ] wr chunk calls=%lu bytes=%lu <=512=%lu <=4K=%lu <=16K=%lu >16K=%lu\r\n", s_wr_hist.calls, s_wr_hist.bytes, s_wr_hist.le_block, s_wr_hist.le_4k, s_wr_hist.le_epbuf, s_wr_hist.gt_epbuf);

    {
        const DWORD avg_us = (s_lat.calls == 0U) ? 0U : (DWORD)(s_lat.total_us / s_lat.calls);

        LOGI("[USB ] latency n=%lu avg=%luus max=%luus last=%luus total=%llums\r\n", s_lat.calls, avg_us, s_lat.max_us, s_lat.last_us, s_lat.total_us / 1000U);
    }
}

/* CLI 与 USB 共用 host busy gate（本仓自拟）：任一槽有在飞请求时，CLI 不提交。 */
bool usb_msc_host_busy(void)
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

void usb_msc_init(void)
{
    /* 在 usb_device_init() 之前调用，USB ISR 尚未使能；可整体清零。 */
    memset(s_slots, 0, sizeof(s_slots));
    s_slots[0].bounce      = s_bounce0;
    s_slots[1].bounce      = s_bounce1;
    s_cmd_lba              = 0U;
    s_cmd_blocks           = 0U;
    s_order_next           = 0U;
    s_deferred_write_error = false;
}

static bool usb_msc_ready(void)
{
    return g_ctrl.state == SSD_CONTROLLER_READY;
}

/* 同一请求的协议栈重放（事件重放）：已在某个槽里 ⇒ 返回 ASYNC 等 usb_msc_task 收尾。 */
static usb_msc_slot_t *usb_msc_slot_match(bool is_write, DWORD lba, DWORD offset, DWORD length)
{
    for (BYTE i = 0U; i < USB_MSC_SLOT_NUM; i++)
    {
        usb_msc_slot_t *slot = &s_slots[i];

        if ((slot->state != USB_MSC_SLOT_FREE) && (slot->is_write == is_write) && (slot->lba == lba) && (slot->offset == offset) && (slot->length == length))
        {
            return slot;
        }
    }

    return NULL;
}

/* 认领空闲槽；没有空闲槽时，可回收一个**尚未提交**的 WAIT_GATE 槽
 * （旧命令已被协议栈作废，与旧实现的「WAIT_GATE 可安全丢弃」一致）。 */
static usb_msc_slot_t *usb_msc_slot_alloc(bool is_write, DWORD lba)
{
    for (BYTE i = 0U; i < USB_MSC_SLOT_NUM; i++)
    {
        if (s_slots[i].state == USB_MSC_SLOT_FREE)
        {
            return &s_slots[i];
        }
    }

    for (BYTE i = 0U; i < USB_MSC_SLOT_NUM; i++)
    {
        usb_msc_slot_t *slot = &s_slots[i];

        if ((slot->state == USB_MSC_SLOT_WAIT_GATE) && ((slot->is_write != is_write) || (slot->lba != lba)))
        {
            // bounce 是 init 时绑定的固定缓冲（0 号 AXI、1 号 DTCM）：清零槽时必须保留，
            // 否则后续 memcpy 写 NULL。
            BYTE *bounce = slot->bounce;

            memset(slot, 0, sizeof(*slot));
            slot->bounce = bounce;
            return slot;
        }
    }

    return NULL;
}

/* 把槽里已登记的请求提交给 controller。
 * 长度/对齐/范围已由 usb_msc_transfer() 在 host 入口校验（H2）；这里不再复检。 */
static bool usb_msc_slot_submit(usb_msc_slot_t *slot)
{
    DWORD start_us = 0U;

    (void)delay_time_now_us(&start_us);

    ssd_request_t *request = &slot->request;

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

    if (ssd_controller_submit(&g_ctrl, request) != SSD_STATUS_OK)
    {
        return false;
    }

    slot->submit_us = start_us;
    slot->state     = USB_MSC_SLOT_IN_FLIGHT;
    return true;
}

/*
 * READ10/WRITE10 的公共入口。
 *
 * 返回 TUD_MSC_RET_ASYNC（已接管）或 TUD_MSC_RET_ERROR（参数/状态不允许）。
 * 两个槽都在飞时，写路径返回 TUD_MSC_RET_BUSY（0）：TinyUSB 把未消费的数据留在
 * 端点缓冲并重投。主循环在 usb_device_task 之前先跑 ftl_service，且 `tud_task` 单次
 * 最多处理 CFG_TUD_TASK_EVENTS_PER_RUN（16）个事件 ⇒ 重投不会把 FTL 饿死。
 */
static int32_t usb_msc_transfer(bool is_write, DWORD lba, DWORD offset, void *buffer, DWORD length)
{
    if (usb_msc_slot_match(is_write, lba, offset, length) != NULL)
    {
        return TUD_MSC_RET_ASYNC; /* 同一命令的重入（协议栈事件重放） */
    }

    /* offset 只在端点缓冲小于 block 时非零；本仓 16384B ≥ 512B，恒为 0。
     * length 是主机数据阶段长度，先做边界检查再交给内部（H29/H31）。 */
    if ((offset != 0U) || (length == 0U) || (length > (DWORD)CFG_TUD_MSC_EP_BUFSIZE) || ((length % USB_MSC_BLOCK_SIZE) != 0U))
    {
        return TUD_MSC_RET_ERROR;
    }

    const DWORD blocks = length / USB_MSC_BLOCK_SIZE;

    if (!ssd_controller_lba_range_ok(&g_ctrl, lba, blocks))
    {
        return TUD_MSC_RET_ERROR;
    }

    /* controller 未就绪：主机应先靠 TEST UNIT READY 等待，不应下发读写。 */
    if (!usb_msc_ready())
    {
        return TUD_MSC_RET_ERROR;
    }

    usb_msc_slot_t *slot = usb_msc_slot_alloc(is_write, lba);

    if (slot == NULL)
    {
        /* 两个槽都在飞：写路径回 BUSY 让协议栈重投；读路径没有 lookahead ⇒ 状态错误。 */
        return is_write ? (int32_t)TUD_MSC_RET_BUSY : TUD_MSC_RET_ERROR;
    }

    slot->is_write   = is_write;
    slot->is_flush   = false;
    slot->order      = s_order_next++;
    slot->lba        = lba;
    slot->offset     = offset;
    slot->length     = length;
    slot->usb_buffer = buffer;
    slot->discard    = false;
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
        if (!usb_msc_slot_submit(slot))
        {
            slot->state = USB_MSC_SLOT_FREE;
            return TUD_MSC_RET_ERROR;
        }
    }

    return TUD_MSC_RET_ASYNC;
}

/* 完成一个槽：写 chunk 只回收缓冲并记录 deferred error（回执已在提交时发出）；
 * 读 / flush 负责回执（数据与 CSW）。 */
static void usb_msc_slot_complete(usb_msc_slot_t *slot)
{
    // 写 chunk 的"设备侧延迟"在**回执点**记录（volatile write cache：完成可能远晚于
    // 回执）；读 / flush 在完成点记录。
    if (slot->is_write == false)
    {
        usb_msc_note_latency(slot->submit_us);
    }

    const bool ok = (slot->request.completion_status == SSD_STATUS_OK);

    /* 主机已复位/拔出/挂起：TinyUSB 可能已把端点缓冲用于新的 CBW/枚举，
     * 此时既不能回写数据，也不能再依赖 pending_io。 */
    if ((slot->discard == true) || !tud_ready())
    {
        /* 放弃回执：避免 controller 迟到的 FLUSH 完成写回已被复用的 request。 */
        ssd_controller_cancel_flush(&g_ctrl);

        const bool ack_owed = (slot->ack_sent == false);

        slot->state = USB_MSC_SLOT_FREE;

        if (ack_owed)
        {
            /* 与之前的 ASYNC 返回配对；主机已复位时 pending_io 已清，调用无副作用。 */
            (void)tud_msc_async_io_done(TUD_MSC_RET_ERROR, false);
        }
        return;
    }

    if (slot->is_write)
    {
        // 写 chunk 一律提交即回执（volatile write cache）：本槽的完成只回收缓冲；
        // 失败延后到**下一条命令**上报（本命令的 CSW 早已发出）。
        if (ok == false)
        {
            s_deferred_write_error = true;
            LOGE("[USB ] write chunk failed after ack lba=%lu len=%lu — deferred\r\n", slot->lba, slot->length);
        }

        slot->state = USB_MSC_SLOT_FREE;
        return;
    }

    if ((ok == true) && (slot->is_flush == false))
    {
        memcpy(slot->usb_buffer, slot->bounce, slot->length);
    }

    /* FLUSH 无数据阶段：成功回执为 0（TinyUSB 侧已按命令类型区分 0 的语义）。 */
    const int32_t transfer_result = (ok == true) ? (slot->is_flush ? 0 : (int32_t)slot->length) : TUD_MSC_RET_ERROR;

    slot->state = USB_MSC_SLOT_FREE;
    (void)tud_msc_async_io_done(transfer_result, false);
}

void usb_msc_task(void)
{
    DWORD done_mask = 0U;

    /* 按到达序处理槽位：先回收先到达的槽、按序记录 deferred 错误。回执已在提交时发出，
     * 这里的顺序只影响缓冲回收与错误归因，不影响 CSW 时机（volatile write cache）。 */
    for (BYTE n = 0U; n < USB_MSC_SLOT_NUM; n++)
    {
        usb_msc_slot_t *slot = NULL;

        for (BYTE i = 0U; i < USB_MSC_SLOT_NUM; i++)
        {
            usb_msc_slot_t *cand = &s_slots[i];

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
            if (!tud_ready())
            {
                /* 等待期间主机复位/拔出：命令作废。仍要回一次 async_io_done 与
                 * 之前的 TUD_MSC_RET_ASYNC 配对（pending_io 已清时它返回 false）。 */
                slot->state = USB_MSC_SLOT_FREE;
                (void)tud_msc_async_io_done(TUD_MSC_RET_ERROR, false);
                continue;
            }

            if (cli_host_busy())
            {
                continue;
            }

            if (!usb_msc_slot_submit(slot))
            {
                slot->state = USB_MSC_SLOT_FREE;
                (void)tud_msc_async_io_done(TUD_MSC_RET_ERROR, false);
                continue;
            }
        }

        /* 写 chunk（含末 chunk）：提交后立即回流控回执（volatile write cache）。
         * 设备 cache 序列因此可以跨 host 命令连续；槽位的 FTL 完成只用于回收缓冲
         * 与 deferred error 记录。 */
        if ((slot->is_write == true) && (slot->is_flush == false) && (slot->ack_sent == false))
        {
            slot->ack_sent = true;
            usb_msc_note_latency(slot->submit_us); /* 主机可见延迟 = submit → 回执 */
            (void)tud_msc_async_io_done((int32_t)slot->length, false);
        }

        if (!ssd_controller_is_complete(&slot->request))
        {
            continue;
        }

        usb_msc_slot_complete(slot);
    }
}

/* ─────────────────────────── MSC 回调 ─────────────────────────── */

int32_t tud_msc_read10_cb(BYTE lun, DWORD lba, DWORD offset, void *buffer, DWORD bufsize)
{
    /* 单 LUN 设备：lun 是主机输入，越界直接拒绝（H29/H31）。 */
    if (lun != 0U)
    {
        return TUD_MSC_RET_ERROR;
    }

    // deferred 写错误在**下一条能承载状态的命令**上报一次（近似 SCSI deferred sense）。
    if (usb_msc_take_deferred_error() == true)
    {
        return TUD_MSC_RET_ERROR;
    }

    return usb_msc_transfer(false, lba, offset, buffer, bufsize);
}

int32_t tud_msc_write10_cb(BYTE lun, DWORD lba, DWORD offset, BYTE *buffer, DWORD bufsize)
{
    if (lun != 0U)
    {
        return TUD_MSC_RET_ERROR;
    }

    if (usb_msc_take_deferred_error() == true)
    {
        return TUD_MSC_RET_ERROR;
    }

    usb_msc_note_write(bufsize);

    return usb_msc_transfer(true, lba, offset, buffer, bufsize);
}

void tud_msc_inquiry_cb(BYTE lun, BYTE vendor_id[8], BYTE product_id[16], BYTE product_rev[4])
{
    /* 回调参数是 8/16/4 字节定长数组（见 msc_device.h）；字符串字面量长度
     * 小于目标，直接 memcpy 会越界读，所以显式补空格到定长。 */
    static const BYTE vendor_id_src[8]   = {'S', 'O', 'F', 'T', 'S', 'S', 'D', ' '};
    static const BYTE product_id_src[16] = {'N', 'A', 'N', 'D', ' ', 'U', '-', 'D', 'I', 'S', 'K', ' ', ' ', ' ', ' ', ' '};
    static const BYTE product_rev_src[4] = {'1', '.', '0', '0'};

    if (lun != 0U)
    {
        return;
    }

    memcpy(vendor_id, vendor_id_src, sizeof(vendor_id_src));
    memcpy(product_id, product_id_src, sizeof(product_id_src));
    memcpy(product_rev, product_rev_src, sizeof(product_rev_src));
}

bool tud_msc_test_unit_ready_cb(BYTE lun)
{
    return (lun == 0U) && usb_msc_ready();
}

void tud_msc_capacity_cb(BYTE lun, DWORD *block_count, WORD *block_size)
{
    if (lun != 0U)
    {
        *block_count = 0U;
        *block_size  = 0U;
        return;
    }

    *block_count = ssd_controller_host_lba_max(&g_ctrl);
    *block_size  = USB_MSC_BLOCK_SIZE;
}

bool tud_msc_start_stop_cb(BYTE lun, BYTE power_condition, bool start, bool load_eject)
{
    (void)power_condition;
    (void)start;
    (void)load_eject;

    return lun == 0U;
}

bool tud_msc_is_writable_cb(BYTE lun)
{
    return lun == 0U;
}

/*
 * SCSI 命令的兜底分支（TinyUSB 的内建命令不会到这里）。
 *
 * SYNCHRONIZE CACHE(10)：PC 在 fsync/sync/卸载时下发，要求把已确认的写数据
 * 落盘。转成 controller 的 FLUSH，并**异步**完成：提交后立即返回
 * TUD_MSC_RET_ASYNC，由 usb_msc_task 轮询 complete_flag，完成时调
 * tud_msc_async_io_done()（RISK-USB-06：不能在 SCSI 回调里同步自旋——那段时间
 * 主循环无法抽 USB 事件）。
 * ⚠ FLUSH 是 best-effort，不是掉电屏障语义；详见设计文档 §9。
 */
int32_t tud_msc_scsi_cb(BYTE lun, BYTE const scsi_cmd[16], void *buffer, WORD bufsize)
{
    (void)buffer;
    (void)bufsize;

    if ((lun != 0U) || (scsi_cmd[0] != USB_SCSI_SYNCHRONIZE_CACHE_10))
    {
        /* 未知命令一律 ILLEGAL REQUEST；打印 opcode 便于定位主机实际下发什么
         * （Windows format/diskpart 会用到 VERIFY / SYNCHRONIZE CACHE(16) 等）。 */
        LOGW("[USB ] unsupported SCSI opcode=0x%02hhX lun=%hhu\r\n", scsi_cmd[0], lun);
        return TUD_MSC_RET_ERROR;
    }

    // deferred 写错误在 FLUSH 上同样要上报（它是持久性屏障；上一条已回执的写失败
    // 不能被吞掉）。
    if (usb_msc_take_deferred_error() == true)
    {
        return TUD_MSC_RET_ERROR;
    }

    /* BOT 串行：同一时刻只允许一条 host 命令在飞（与 READ/WRITE 的槽门一致）。 */
    if (!usb_msc_ready() || usb_msc_host_busy())
    {
        return TUD_MSC_RET_ERROR;
    }

    usb_msc_slot_t *slot = usb_msc_slot_alloc(false, 0U);

    if (slot == NULL)
    {
        return TUD_MSC_RET_ERROR;
    }

    slot->is_write   = false;
    slot->is_flush   = true;
    slot->order      = s_order_next++;
    slot->lba        = 0U;
    slot->offset     = 0U;
    slot->length     = 0U;
    slot->usb_buffer = NULL;
    slot->discard    = false;
    slot->ack_sent   = false;
    slot->state      = USB_MSC_SLOT_WAIT_GATE;

    if (cli_host_busy())
    {
        /* CLI 占用 host gate：同 READ/WRITE 先排队，由 usb_msc_task 提交。 */
    }
    else if (!usb_msc_slot_submit(slot))
    {
        slot->state = USB_MSC_SLOT_FREE;
        return TUD_MSC_RET_ERROR;
    }

    return TUD_MSC_RET_ASYNC;
}

void tud_msc_read10_complete_cb(BYTE lun)
{
    /* READ 的数据与状态已在 usb_msc_task() 里交付；完成回调不需要额外动作。 */
    (void)lun;
}

/*
 * TinyUSB 事件钩子（可能在 ISR 里调用）：总线复位/拔出时把在飞请求标记为丢弃。
 *
 * 为什么需要：总线复位后 TinyUSB 会立刻把 MSC 端点缓冲用于新的枚举/CBW，
 * 而 FTL 里的旧请求还没完成。若等 tud_umount_cb()/tud_ready() 才发现，
 * 端点缓冲可能已被复用，回写 READ 数据会踩掉新 CBW。
 *
 * ⚠ ISR 上下文：只写标志，不打印、不等待（H21）。
 */
void tud_event_hook_cb(BYTE rhport, DWORD eventid, bool in_isr)
{
    (void)rhport;
    (void)in_isr;

    if ((eventid != DCD_EVENT_BUS_RESET_START) && (eventid != DCD_EVENT_BUS_RESET_END) && (eventid != DCD_EVENT_UNPLUGGED))
    {
        return;
    }

    for (BYTE i = 0U; i < USB_MSC_SLOT_NUM; i++)
    {
        if (s_slots[i].state == USB_MSC_SLOT_IN_FLIGHT)
        {
            s_slots[i].discard = true;
        }
        else if (s_slots[i].state == USB_MSC_SLOT_WAIT_GATE)
        {
            s_slots[i].state = USB_MSC_SLOT_FREE;
        }
    }
}

void tud_msc_write10_complete_cb(BYTE lun)
{
    /* 不在这里 flush：每个 4KB 块都落盘会把 USB 写吞吐拖到 NAND 随机写水平。
     * 持久化由主机的 SYNCHRONIZE CACHE 驱动。 */
    (void)lun;
}

/* ─────────────────────────── USB 描述符 ─────────────────────────── */

#define USB_MSC_STRING_COUNT     3U
#define USB_MSC_STRING_MAX_CHARS 31U /* 描述符缓冲 32 项：长度字 + 最多 31 个 UTF-16 单元 */

static const char *const s_string_desc[USB_MSC_STRING_COUNT] = {
    "soft_ssd",
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

#define USB_MSC_CONFIG_LEN (TUD_CONFIG_DESC_LEN + TUD_MSC_DESC_LEN)

static const BYTE s_config_desc[] = {
    /* config, itf count, string idx, total len, attributes, power(2mA) */
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, USB_MSC_CONFIG_LEN, 0x00, USB_MSC_POWER_MA),
    /* itf, string idx, EP out, EP in, EP size */
    TUD_MSC_DESCRIPTOR(0, 0, USB_MSC_EP_OUT, USB_MSC_EP_IN, USB_MSC_EP_SIZE),
};

BYTE const *tud_descriptor_device_cb(void)
{
    return (BYTE const *)&s_device_desc;
}

BYTE const *tud_descriptor_configuration_cb(BYTE index)
{
    (void)index;
    return s_config_desc;
}

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

void tud_mount_cb(void)
{
    /* 容量按 SCSI 坐标打印：sector 数 × 每 sector 字节。 */
    const DWORD block_count = ssd_controller_host_lba_max(&g_ctrl);

    LOGI("[USB ] mounted (MSC BOT, %lu sectors x %u bytes)\r\n", block_count, USB_MSC_BLOCK_SIZE);
}

void tud_umount_cb(void)
{
    /*
     * 主机拔出/复位：在飞请求仍由 FTL 完成。**不能直接清状态** ——
     *   - WAIT_GATE 尚未提交，可以安全丢弃；
     *   - IN_FLIGHT 的 `ssd_request_t` 仍被 FTL 完成回调引用，提前 memset
     *     会让下一次 submit 覆盖在飞请求的子请求计数；只能标记 discard，
     *     等 usb_msc_task() 看到完成后收尾。
     */
    for (BYTE i = 0U; i < USB_MSC_SLOT_NUM; i++)
    {
        if (s_slots[i].state == USB_MSC_SLOT_WAIT_GATE)
        {
            s_slots[i].state = USB_MSC_SLOT_FREE;
        }
        else if (s_slots[i].state == USB_MSC_SLOT_IN_FLIGHT)
        {
            s_slots[i].discard = true;
        }
    }

    LOGI("[USB ] unmounted\r\n");
}
