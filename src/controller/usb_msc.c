/*
 * usb_msc.c — USB Mass Storage Class（BOT）块设备前端
 *
 * 职责：TinyUSB MSC class 回调 + SCSI → controller 适配。
 *   READ(10)  → ssd_controller_submit(SSD_REQUEST_READ)  → USB IN 数据
 *   WRITE(10) → ssd_controller_submit(SSD_REQUEST_WRITE) → 数据已在 USB 缓冲
 *   SYNCHRONIZE CACHE(10) → SSD_REQUEST_FLUSH（ftl_flush，全实例）
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
 *       USB 复位/拔出会在请求仍在飞时复用它（BOT 重新收 CBW）。因此读写都先
 *       memcpy 到本模块的静态 bounce buffer，FTL 只持有 bounce buffer 的指针，
 *       USB 侧缓冲可在任意时刻被协议栈回收。
 */

#include "usb_msc.h"

#include "cli.h"
#include "controller.h"
#include "device/dcd.h" /* DCD_EVENT_*：tusb.h 不导出该枚举 */
#include "ftl.h"
#include "log.h"
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
_Static_assert(FTL_LBA_MAX <= UINT32_MAX, "SCSI block count must fit 32 bits");

/* USB MSC sector 与 FTL 映射单元必须一一对应；任一侧改变都会破坏 LBA 语义。 */
_Static_assert(USB_MSC_BLOCK_SIZE == FTL_IU_BYTES, "MSC sector must equal FTL mapping unit");

/* ─────────────────────────── 在飞请求 ─────────────────────────── */

typedef enum
{
    USB_MSC_IO_IDLE = 0,
    /* host busy gate（本仓自拟：同一时刻只允许一个 host 前端提交 controller） */
    USB_MSC_IO_WAIT_GATE, /* 参数已收到，等 CLI 释放 host busy gate 后提交 */
    USB_MSC_IO_IN_FLIGHT  /* 已提交 controller，等 FTL 完成 */
} usb_msc_io_state_t;

typedef struct
{
    /* state / discard 会被 USB ISR（tud_event_hook_cb）改写，必须 volatile（H16）。 */
    volatile usb_msc_io_state_t state;
    bool                        is_write;
    volatile bool               discard; /* 主机已复位/拔出：完成后丢弃结果，不回写 USB 缓冲 */
    uint32_t                    lba;
    uint32_t                    offset;
    uint32_t                    length;
    void                       *usb_buffer; /* TinyUSB 端点缓冲；ASYNC 期间协议栈不会复用 */
    ssd_request_t               request;
} usb_msc_io_t;

static usb_msc_io_t s_msc_io;

/* FTL 只认这一块缓冲；USB 端点缓冲随时可能被协议栈复用，不能直接交给 FTL。
 * 该缓冲只被 CPU 读写（DWC2 走 PIO），放普通 .bss 即可；4B 对齐满足 FTL 的字访问。 */
static uint8_t s_msc_bounce[CFG_TUD_MSC_EP_BUFSIZE] __attribute__((aligned(4)));

/* CLI 与 USB 共用 host busy gate（本仓自拟）：任一前端有未完成请求时，另一个不提交。 */
bool usb_msc_host_busy(void)
{
    return s_msc_io.state != USB_MSC_IO_IDLE;
}

void usb_msc_init(void)
{
    /* 在 usb_device_init() 之前调用，USB ISR 尚未使能；可整体清零。 */
    memset(&s_msc_io, 0, sizeof(s_msc_io));
}

static bool usb_msc_ready(void)
{
    return g_ctrl.state == SSD_CONTROLLER_READY;
}

static bool usb_msc_io_matches(bool is_write, uint32_t lba, uint32_t offset, uint32_t length)
{
    return (s_msc_io.is_write == is_write) && (s_msc_io.lba == lba) &&
           (s_msc_io.offset == offset) && (s_msc_io.length == length);
}

/* 把 s_msc_io 里已登记的请求提交给 controller。
 * 长度/对齐/范围已由 usb_msc_transfer() 在 host 入口校验（H2）；这里不再复检。 */
static bool usb_msc_submit(void)
{
    /* WRITE：先把 USB 端点缓冲拷进 bounce；READ：FTL 直接写 bounce，
     * 完成后再拷回端点缓冲。两种方向 FTL 都不持有 USB 侧指针。 */
    if (s_msc_io.is_write)
    {
        memcpy(s_msc_bounce, s_msc_io.usb_buffer, s_msc_io.length);
    }

    ssd_request_t *request = &s_msc_io.request;

    memset(request, 0, sizeof(*request));

    const uint32_t blocks = s_msc_io.length / USB_MSC_BLOCK_SIZE;

    request->opcode      = s_msc_io.is_write ? SSD_REQUEST_WRITE : SSD_REQUEST_READ;
    request->start_lba   = s_msc_io.lba;
    request->block_count = blocks;
    request->buffer      = s_msc_bounce;
    request->buffer_size = s_msc_io.length;

    if (ssd_controller_submit(&g_ctrl, request) != SSD_STATUS_OK)
    {
        return false;
    }

    s_msc_io.state = USB_MSC_IO_IN_FLIGHT;
    return true;
}

/*
 * READ10/WRITE10 的公共入口。
 *
 * 返回 TUD_MSC_RET_ASYNC（已接管）或 TUD_MSC_RET_ERROR（参数/状态不允许）。
 * ⚠ 不能返回 TUD_MSC_RET_BUSY：TinyUSB 会在 tud_task() 循环里立即重入回调，
 *    单线程主循环下 FTL 永远得不到推进机会（饿死）。
 */
static int32_t usb_msc_transfer(bool is_write, uint32_t lba, uint32_t offset, void *buffer, uint32_t length)
{
    if (s_msc_io.state != USB_MSC_IO_IDLE)
    {
        if (usb_msc_io_matches(is_write, lba, offset, length))
        {
            /* 同一命令的重入（协议栈事件重放）：已在处理，等 usb_msc_task 收尾。 */
            return TUD_MSC_RET_ASYNC;
        }

        if (s_msc_io.state == USB_MSC_IO_IN_FLIGHT)
        {
            /* 旧请求已提交，无法撤销；不能把旧数据/状态交给新命令。 */
            return TUD_MSC_RET_ERROR;
        }

        /* WAIT_GATE：旧命令还没提交，可以安全丢弃，换成当前命令。 */
        s_msc_io.state = USB_MSC_IO_IDLE;
    }

    /* offset 只在端点缓冲小于 block 时非零；本仓 4096B ≥ 512B，恒为 0。
     * length 是主机数据阶段长度，先做边界检查再交给内部（H29/H31）。 */
    if ((offset != 0U) || (length == 0U) || (length > sizeof(s_msc_bounce)) ||
        ((length % USB_MSC_BLOCK_SIZE) != 0U))
    {
        return TUD_MSC_RET_ERROR;
    }

    const uint32_t blocks = length / USB_MSC_BLOCK_SIZE;

    if (!ssd_controller_lba_range_ok(lba, blocks))
    {
        return TUD_MSC_RET_ERROR;
    }

    /* controller 未就绪：主机应先靠 TEST UNIT READY 等待，不应下发读写。 */
    if (!usb_msc_ready())
    {
        return TUD_MSC_RET_ERROR;
    }

    s_msc_io.is_write   = is_write;
    s_msc_io.lba        = lba;
    s_msc_io.offset     = offset;
    s_msc_io.length     = length;
    s_msc_io.usb_buffer = buffer;
    s_msc_io.state      = USB_MSC_IO_WAIT_GATE;

    if (!cli_host_busy())
    {
        if (!usb_msc_submit())
        {
            s_msc_io.state = USB_MSC_IO_IDLE;
            return TUD_MSC_RET_ERROR;
        }
    }

    return TUD_MSC_RET_ASYNC;
}

void usb_msc_task(void)
{
    if (s_msc_io.state == USB_MSC_IO_WAIT_GATE)
    {
        if (!tud_ready())
        {
            /* 等待期间主机复位/拔出：命令作废。仍要回一次 async_io_done 与
             * 之前的 TUD_MSC_RET_ASYNC 配对（pending_io 已清时它返回 false）。 */
            s_msc_io.state = USB_MSC_IO_IDLE;
            (void)tud_msc_async_io_done(TUD_MSC_RET_ERROR, false);
            return;
        }

        if (cli_host_busy())
        {
            return;
        }

        if (!usb_msc_submit())
        {
            s_msc_io.state = USB_MSC_IO_IDLE;
            /* false = 命令已被协议栈作废（pending_io 已清），无需处理。 */
            (void)tud_msc_async_io_done(TUD_MSC_RET_ERROR, false);
        }
        return;
    }

    if (s_msc_io.state != USB_MSC_IO_IN_FLIGHT)
    {
        return;
    }

    if (!ssd_controller_is_complete(&s_msc_io.request))
    {
        return;
    }

    const bool ok = (s_msc_io.request.completion_status == SSD_STATUS_OK);

    /* 主机已复位/拔出/挂起：TinyUSB 可能已把端点缓冲用于新的 CBW/枚举，
     * 此时既不能回写数据，也不能再调用 async_io_done。 */
    if (s_msc_io.discard || !tud_ready())
    {
        s_msc_io.discard = false;
        s_msc_io.state   = USB_MSC_IO_IDLE;
        /* 与之前的 ASYNC 返回配对；主机已复位时 pending_io 已清，调用无副作用。 */
        (void)tud_msc_async_io_done(TUD_MSC_RET_ERROR, false);
        return;
    }

    if (ok && !s_msc_io.is_write)
    {
        memcpy(s_msc_io.usb_buffer, s_msc_bounce, s_msc_io.length);
    }

    const int32_t transfer_result = ok ? (int32_t)s_msc_io.length : TUD_MSC_RET_ERROR;

    s_msc_io.state = USB_MSC_IO_IDLE;
    /* false = 主机已复位、命令已作废（pending_io 被清），无需处理。 */
    (void)tud_msc_async_io_done(transfer_result, false);
}

/* ─────────────────────────── MSC 回调 ─────────────────────────── */

int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize)
{
    /* 单 LUN 设备：lun 是主机输入，越界直接拒绝（H29/H31）。 */
    if (lun != 0U)
    {
        return TUD_MSC_RET_ERROR;
    }

    return usb_msc_transfer(false, lba, offset, buffer, bufsize);
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize)
{
    if (lun != 0U)
    {
        return TUD_MSC_RET_ERROR;
    }

    return usb_msc_transfer(true, lba, offset, buffer, bufsize);
}

void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8], uint8_t product_id[16], uint8_t product_rev[4])
{
    /* 回调参数是 8/16/4 字节定长数组（见 msc_device.h）；字符串字面量长度
     * 小于目标，直接 memcpy 会越界读，所以显式补空格到定长。 */
    static const uint8_t vendor_id_src[8]   = {'S', 'O', 'F', 'T', 'S', 'S', 'D', ' '};
    static const uint8_t product_id_src[16] = {'N', 'A', 'N', 'D', ' ', 'U', '-', 'D', 'I', 'S', 'K', ' ', ' ', ' ', ' ', ' '};
    static const uint8_t product_rev_src[4] = {'1', '.', '0', '0'};

    if (lun != 0U)
    {
        return;
    }

    memcpy(vendor_id, vendor_id_src, sizeof(vendor_id_src));
    memcpy(product_id, product_id_src, sizeof(product_id_src));
    memcpy(product_rev, product_rev_src, sizeof(product_rev_src));
}

bool tud_msc_test_unit_ready_cb(uint8_t lun)
{
    return (lun == 0U) && usb_msc_ready();
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count, uint16_t *block_size)
{
    if (lun != 0U)
    {
        *block_count = 0U;
        *block_size  = 0U;
        return;
    }

    *block_count = (uint32_t)FTL_LBA_MAX;
    *block_size  = (uint16_t)USB_MSC_BLOCK_SIZE;
}

bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start, bool load_eject)
{
    (void)power_condition;
    (void)start;
    (void)load_eject;

    return lun == 0U;
}

bool tud_msc_is_writable_cb(uint8_t lun)
{
    return lun == 0U;
}

/*
 * SCSI 命令的兜底分支（TinyUSB 的内建命令不会到这里）。
 *
 * SYNCHRONIZE CACHE(10)：PC 在 fsync/sync/卸载时下发，要求把已确认的写数据
 * 落盘。这里转成 controller 的 FLUSH（L2P journal 尾页 + cache 写回）。
 * ⚠ ftl_flush 是 best-effort，不是掉电屏障语义；详见设计文档 §9。
 */
int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16], void *buffer, uint16_t bufsize)
{
    (void)buffer;
    (void)bufsize;

    if ((lun != 0U) || (scsi_cmd[0] != USB_SCSI_SYNCHRONIZE_CACHE_10))
    {
        /* 未知命令一律 ILLEGAL REQUEST；打印 opcode 便于定位主机实际下发什么
         * （Windows format/diskpart 会用到 VERIFY / SYNCHRONIZE CACHE(16) 等）。 */
        LOGW("[USB ] unsupported SCSI opcode=0x%02X lun=%u\r\n",
             (unsigned)scsi_cmd[0],
             (unsigned)lun);
        return TUD_MSC_RET_ERROR;
    }

    if (!usb_msc_ready() || usb_msc_host_busy() || cli_host_busy())
    {
        return TUD_MSC_RET_ERROR;
    }

    ssd_request_t request;

    memset(&request, 0, sizeof(request));
    request.opcode = SSD_REQUEST_FLUSH;

    const int status = ssd_controller_submit(&g_ctrl, &request);

    if (status != SSD_STATUS_OK)
    {
        return TUD_MSC_RET_ERROR;
    }

    return 0; /* 无数据阶段 */
}

void tud_msc_read10_complete_cb(uint8_t lun)
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
void tud_event_hook_cb(uint8_t rhport, uint32_t eventid, bool in_isr)
{
    (void)rhport;
    (void)in_isr;

    if ((eventid != DCD_EVENT_BUS_RESET_START) && (eventid != DCD_EVENT_BUS_RESET_END) &&
        (eventid != DCD_EVENT_UNPLUGGED))
    {
        return;
    }

    if (s_msc_io.state == USB_MSC_IO_IN_FLIGHT)
    {
        s_msc_io.discard = true;
    }
    else if (s_msc_io.state == USB_MSC_IO_WAIT_GATE)
    {
        s_msc_io.state = USB_MSC_IO_IDLE;
    }
}

void tud_msc_write10_complete_cb(uint8_t lun)
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

static const uint8_t s_config_desc[] = {
    /* config, itf count, string idx, total len, attributes, power(2mA) */
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, USB_MSC_CONFIG_LEN, 0x00, USB_MSC_POWER_MA),
    /* itf, string idx, EP out, EP in, EP size */
    TUD_MSC_DESCRIPTOR(0, 0, USB_MSC_EP_OUT, USB_MSC_EP_IN, USB_MSC_EP_SIZE),
};

uint8_t const *tud_descriptor_device_cb(void)
{
    return (uint8_t const *)&s_device_desc;
}

uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;
    return s_config_desc;
}

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    static uint16_t desc_str[1U + USB_MSC_STRING_MAX_CHARS];
    uint8_t         char_count;

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

        char_count = (uint8_t)strlen(str);
        if (char_count > USB_MSC_STRING_MAX_CHARS)
        {
            char_count = USB_MSC_STRING_MAX_CHARS;
        }

        for (uint8_t i = 0U; i < char_count; i++)
        {
            desc_str[1U + i] = (uint16_t)(uint8_t)str[i];
        }
    }

    /* bLength = 2 字节头 + 每个 UTF-16 单元 2 字节；bDescriptorType 在高字节。 */
    const uint16_t desc_len = (uint16_t)(2U * char_count + 2U);

    desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | desc_len);
    return desc_str;
}

/* ─────────────────────────── 挂载诊断 ─────────────────────────── */

void tud_mount_cb(void)
{
    /* 容量按 SCSI 坐标打印：sector 数 × 每 sector 字节。 */
    const uint32_t block_count = (uint32_t)FTL_LBA_MAX;

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
    if (s_msc_io.state == USB_MSC_IO_WAIT_GATE)
    {
        s_msc_io.state = USB_MSC_IO_IDLE;
    }
    else if (s_msc_io.state == USB_MSC_IO_IN_FLIGHT)
    {
        s_msc_io.discard = true;
    }

    LOGI("[USB ] unmounted\r\n");
}
