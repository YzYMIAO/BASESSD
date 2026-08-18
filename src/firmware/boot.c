#include "firmware/boot.h"

#include "platform/platform.h"
#include "platform/soc.h"

#include <stddef.h>

/* 将平台日志抽象连接到所选 SoC 的 UART 后端。 */
static void firmware_log_write(const char *message)
{
    soc_uart_write(message);
}

/*
 * 参考固件尚未提供动态内存分配器，因此 allocate/free 保持为空。
 * SSD 数据路径应优先使用链接期静态区域或专用内存池。
 */
static const ssd_platform_ops_t g_firmware_platform_ops = {
    .log_write       = firmware_log_write,
    .time_us         = soc_time_us,
    .memory_allocate = NULL,
    .memory_free     = NULL,
};

static void firmware_mark_placeholder(firmware_boot_context_t *context, firmware_placeholder_t placeholder)
{
    context->placeholder_mask |= (uint32_t)placeholder;
}

static int firmware_platform_init(firmware_boot_context_t *context)
{
    int status;

    context->stage = FIRMWARE_BOOT_STAGE_PLATFORM;

    /* UART 先于其他驱动初始化，便于后续阶段输出诊断信息。 */
    soc_uart_init();
    soc_uart_write("boot: platform initialization\n");

    /* Timer 为驱动超时、性能统计和调度提供统一时间基准。 */
    soc_timer_init();

    status = ssd_platform_register(&g_firmware_platform_ops);
    if (status != 0)
    {
        return SSD_STATUS_INVALID_ARGUMENT;
    }

    /* 在向量表和各模式栈准备完成后，初始化 SoC 中断控制器。 */
    soc_interrupt_init();

    /* 真实 BSP 应读取复位状态寄存器并区分冷启动、看门狗和异常复位。 */
    firmware_mark_placeholder(context, FIRMWARE_PLACEHOLDER_RESET_REASON);
    return SSD_STATUS_OK;
}

static int firmware_memory_init(firmware_boot_context_t *context)
{
    context->stage = FIRMWARE_BOOT_STAGE_MEMORY;
    soc_uart_write("boot: memory and protection initialization\n");

    /*
     * soc_memory_init() 已在 C 运行时建立后调用。参考 SoC 假定调试器或
     * 模拟器已经准备好 SRAM/DRAM；真实 BSP 必须完成 DDR PHY Training、
     * ECC 初始化和基础读写测试。
     */
    firmware_mark_placeholder(context, FIRMWARE_PLACEHOLDER_DDR);

    /*
     * MPU 和 Cache 依赖真实地址属性。后续应按 Code、SRAM、MMIO、DMA、
     * FTL Table 等区域配置，并在开启 Cache 前执行 invalidate、DSB、ISB。
     */
    firmware_mark_placeholder(context, FIRMWARE_PLACEHOLDER_MPU_CACHE);
    return SSD_STATUS_OK;
}

static int firmware_image_init(firmware_boot_context_t *context)
{
    context->stage = FIRMWARE_BOOT_STAGE_IMAGE;
    soc_uart_write("boot: firmware image validation\n");

    /*
     * 当前 ELF 由调试器或模拟器直接装载，没有 Boot ROM 镜像头协议。
     * 真实实现需要校验 magic、长度、加载地址、版本、CRC/Hash 和签名，
     * 必要时从 A/B 固件槽选择可启动镜像。
     */
    firmware_mark_placeholder(context, FIRMWARE_PLACEHOLDER_IMAGE_VERIFY);
    return SSD_STATUS_OK;
}

static int firmware_storage_init(firmware_boot_context_t *context)
{
    context->stage = FIRMWARE_BOOT_STAGE_STORAGE;
    soc_uart_write("boot: NAND and FTL recovery\n");

    /*
     * 尚无真实 NAND Controller、DMA、ECC 和 NAND ID，因此不能构造可靠的
     * nand_device_t。真实实现应先探测 Geometry 和坏块，再加载 Checkpoint、
     * 重放 Journal、恢复 L2P 与写指针，最后才允许 Controller 进入 Ready。
     */
    firmware_mark_placeholder(context, FIRMWARE_PLACEHOLDER_NAND);
    firmware_mark_placeholder(context, FIRMWARE_PLACEHOLDER_FTL_RECOVERY);
    return SSD_STATUS_OK;
}

static int firmware_host_init(firmware_boot_context_t *context)
{
    context->stage = FIRMWARE_BOOT_STAGE_HOST;
    soc_uart_write("boot: PCIe and NVMe initialization\n");

    /*
     * PCIe PHY、BAR、MSI/MSI-X 和 Doorbell 都依赖真实主控。未来只有在
     * FTL 恢复成功后才能设置 NVMe Controller Ready，避免主机过早下发 IO。
     */
    firmware_mark_placeholder(context, FIRMWARE_PLACEHOLDER_PCIE_NVME);
    return SSD_STATUS_OK;
}

void firmware_boot_context_init(firmware_boot_context_t *context)
{
    if (context == NULL)
    {
        return;
    }

    context->stage            = FIRMWARE_BOOT_STAGE_RESET;
    context->placeholder_mask = 0U;
    context->last_status      = SSD_STATUS_OK;
}

int firmware_boot_execute(firmware_boot_context_t *context)
{
    int status;

    if (context == NULL)
    {
        return SSD_STATUS_INVALID_ARGUMENT;
    }

    status = firmware_platform_init(context);
    if (status == SSD_STATUS_OK)
    {
        status = firmware_memory_init(context);
    }
    if (status == SSD_STATUS_OK)
    {
        status = firmware_image_init(context);
    }
    if (status == SSD_STATUS_OK)
    {
        status = firmware_storage_init(context);
    }
    if (status == SSD_STATUS_OK)
    {
        status = firmware_host_init(context);
    }

    context->last_status = status;
    if (status != SSD_STATUS_OK)
    {
        context->stage = FIRMWARE_BOOT_STAGE_FAILED;
    }
    else if (context->placeholder_mask != 0U)
    {
        /* 参考流程可以运行，但绝不能向真实 NVMe 主机报告 Controller Ready。 */
        context->stage = FIRMWARE_BOOT_STAGE_PLACEHOLDER_READY;
    }
    else
    {
        context->stage = FIRMWARE_BOOT_STAGE_READY;
    }
    return status;
}
