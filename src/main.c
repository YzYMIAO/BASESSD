
#include "btim.h"
#include "common/ssd_crash.h"
#include "common/ssd_log.h"
#include "controller/controller.h"
#include "delay.h"
#include "fc/fc.h"
#include "ftl/allocator.h"
#include "ftl/bst.h"
#include "ftl/completion_table.h"
#include "ftl/ftl.h"
#include "ftl/metadata.h"
#include "ftl/power_failure_recovery.h"
// #include "led.h"
#include "mpu.h"
#include "nand/nand_dispatch.h"
#include "nand/nand_mt29f4g08abadawp.h"
#if defined(FC_NAND_TEST_ENABLE)
/* TODO: 待重构为 munit UT 框架后移除 */
/* #include "test_fc_nand.h" */
#endif
#include "platform/stm32h743/platform_ops.h"
#include "stm32h7xx_hal.h"
#include "sys.h"
#include "usart.h"
#include "watchdog.h"

#include <string.h>

/* 每次需要确认板上 ELF 是否更新时递增；仅用于开发阶段 UART 诊断。 */
#define SSD_FIRMWARE_DEBUG_REVISION 24U
#define SSD_WATCHDOG_ENABLE         0U

#if (SSD_WATCHDOG_ENABLE != 0U)
/*
 * 当前板级自检阶段只监督 Scheduler。产品启动 Host/NAND/FTL/GC/DMA 后，应把对应
 * bit 加入 required_domains，并在每个状态机的“确认前进”位置调用 report_progress()。
 * 当前板级阶段只启用 IWDG。按典型 LSI=32 kHz 计算，/256 与 Reload=3749 的
 * 名义超时约为 30 s：T=(3749+1)*256/32000。LSI 有频偏，产品量产前应根据
 * 数据手册的频率范围验证最短/最长复位时间，而不应把它当作精确的 30 s 定时器。
 */
static const storage_wdg_config_t g_storage_wdg_config = {
    .iwdg_prescaler      = IWDG_PRESCALER_256,
    .iwdg_reload         = 3749U,
    .required_domains    = STORAGE_WDG_DOMAIN_MASK(STORAGE_WDG_DOMAIN_SCHEDULER),
    .domain_timeout_ms   = {[STORAGE_WDG_DOMAIN_SCHEDULER] = 100U},
    .enable_wwdg         = false,
    .wwdg_prescaler      = 0U,
    .wwdg_counter        = 0U,
    .wwdg_window         = 0U,
    .wwdg_refresh_min_ms = 0U,
    .wwdg_refresh_max_ms = 0U,
};
#endif

/* Common logging stays transport-neutral; this BSP binds it to the non-blocking UART queue. */
static int stm32_uart_log_sink(const char *message, uint32_t length)
{
    return uart_log_write(message, length);
}

static int stm32_uart_log_flush_sink(uint32_t timeout_ms)
{
    return uart_log_flush(timeout_ms);
}

/* ══════════════════════════════════════════════════════════════════
 * FTL 实例多维表（v0.37 — main.c FTL 上线）
 *
 * SRAM 布局（方案 B 修正，已与设计文档确认）：
 *   g_ftl    含 l2p cache 128KB + journal/md → AXI_RAM（约 131.5KB）
 *   g_blocks BST 224KB 连续 → AXI_RAM（保持 params.blocks 线性索引契约，
 *            拆段破坏 allocator 的 blocks[gblock] 索引；SRAM1 仅 128KB 放不下）
 *   g_free_blocks + g_closed_sb_list → SRAM2（.sram2_data NOLOAD，linker 已有）
 *
 *   AXI_RAM ≈355.5KB（剩 11.5KB）、SRAM2 ≈24KB（剩 104KB），不溢出。
 * ══════════════════════════════════════════════════════════════════ */
static ftl_context_t g_ftl[CH_NUM][CE_NUM][LUN_NUM];                       /* 131.5KB → AXI_RAM */
static ftl_block_t   g_blocks[CH_NUM][CE_NUM][LUN_NUM][FTL_BLOCK_TOTAL];   /* 56B×4096=224KB → AXI_RAM */
static uint16_t      g_free_blocks[CH_NUM][CE_NUM][LUN_NUM][FTL_BLOCK_TOTAL] /* 8KB → SRAM2 */
    __attribute__((section(".sram2_data")));
static uint16_t      g_op_blocks[CH_NUM][CE_NUM][LUN_NUM][FTL_BLOCK_TOTAL] /* OP 预留池，容量不动 → SRAM2 */
    __attribute__((section(".sram2_data")));
static uint16_t      g_spare_blocks[CH_NUM][CE_NUM][LUN_NUM][FTL_SPARE_POOL_SIZE] /* 坏块替换池 → SRAM2 */
    __attribute__((section(".sram2_data")));
static super_block_t g_closed_sb_list[CH_NUM][CE_NUM][LUN_NUM][SUPER_BLOCK_LIST_MAX] /* 16KB → SRAM2 */
    __attribute__((section(".sram2_data")));
static ssd_controller_t g_ctrl;   /* controller 路由表（收 g_ftl 三维指针） */
static int              g_ftl_ready; /* 主循环守卫：全部实例 boot 成功才切 ftl_service */

/* 指针形式的三维路由表（类型匹配 ssd_controller_init 形参）。
 * 编译器不认可 static 数组直接退化为匹配指针数组参数（GCC 10.3 起
 * -Wincompatible-pointer-types 恒报），此处显式构造。 */
static ftl_context_t *g_ftl_ptrs[CH_NUM][CE_NUM][LUN_NUM];

/*
 * ssd_ftl_boot - 初始化单个 (ch,ce,lun) FTL 实例
 *
 * 顺序：ftl_init（内部含 bbt_init 标坏块）→ 遍历 BST 非坏块回填 free pool
 *       （allocator_init 不填 free pool、bbt 只标坏块——不回填则首次写失败）
 *       → ftl_boot_recovery → 日志。
 *
 * @param ch/ce/lun 本实例绑定拓扑
 * @return 0 成功，负值失败
 */
static int ssd_ftl_boot(uint8_t ch, uint8_t ce, uint8_t lun)
{
    ftl_init_params_t params;
    ftl_boot_result_t result;
    uint16_t          gblock;
    uint16_t          free_n = 0U;

    memset(&params, 0, sizeof(params));
    params.topo           = (ftl_topology_t){ .ch = ch, .ce = ce, .lun = lun };
    params.blocks         = g_blocks[ch][ce][lun];
    params.free_blocks    = g_free_blocks[ch][ce][lun];
    params.free_count     = 0U;
    params.op_blocks      = g_op_blocks[ch][ce][lun];
    params.op_count       = 0U;
    params.spare_blocks   = g_spare_blocks[ch][ce][lun];
    params.spare_count    = 0U;
    params.closed_sb_list = g_closed_sb_list[ch][ce][lun];

    if (ftl_init(&g_ftl[ch][ce][lun], &params) != FTL_OK)
    {
        SSD_LOGE("[FTL] init failed ch=%u ce=%u lun=%u\r\n", ch, ce, lun);
        return -1;
    }

    /* free pool 回填：遍历 BST 非坏块（bad 位已由 bbt_init 标好）。
     * 固定/动态元数据块（L2P/anchor/BBT/checkpoint/journal）不进 free pool——
     * 否则开机后可能被 allocator 当作空白块复用（覆盖未擦除的半满元数据块）。
     * 用户数据 written 块仍回填（时序安全：boot_recovery 的 F2 重建会把它们拉出）。
     *
     * OP 10% + Spare Pool（容量不动语义，ftl_design §3.6/ADR-8）：
     *   先扣 FTL_SPARE_POOL_SIZE=50 入 Spare Pool（坏块替换池，只出不进，
     *   耗尽后容量自然下降），再按 FTL_OP_PERCENT=10% 扣入 OP 池（GC 缓冲池），
     *   其余才进 free pool。L2P region 数 / FTL_LBA_MAX 不变 → 用户容量 512MiB 不变。
     */
    for (gblock = 0U; gblock < FTL_BLOCK_TOTAL; gblock++)
    {
        uint16_t spare_idx;
        uint16_t op_idx;

        if (ftl_block_is_bad(&g_blocks[ch][ce][lun][gblock]))
        {
            continue;
        }
        if (ftl_is_metadata_block(&g_ftl[ch][ce][lun].metadata_dir,
                                  (uint16_t)(gblock % BLOCKS_PER_PLANE)))
        {
            continue;
        }

        /* ① 前 50 个好块入 Spare Pool（坏块替换池） */
        spare_idx = g_ftl[ch][ce][lun].alloc.spare_count;
        if (spare_idx < FTL_SPARE_POOL_SIZE)
        {
            g_spare_blocks[ch][ce][lun][spare_idx] = gblock;
            g_ftl[ch][ce][lun].alloc.spare_count++;
            continue;
        }

        /* ② 前 10% 好块入 OP 池（GC 缓冲池） */
        op_idx = g_ftl[ch][ce][lun].alloc.op_count;
        if (op_idx < (uint16_t)((uint16_t)(FTL_BLOCK_TOTAL / 2U) * FTL_OP_PERCENT / 100U))
        {
            g_op_blocks[ch][ce][lun][op_idx] = gblock;
            g_ftl[ch][ce][lun].alloc.op_count++;
            continue;
        }

        allocator_free_block(&g_ftl[ch][ce][lun].alloc, gblock);
        free_n++;
    }
    SSD_LOGI("[FTL] instance ch=%u ce=%u lun=%u free=%u op=%u spare=%u\r\n",
             ch, ce, lun,
             (unsigned)free_n,
             (unsigned)g_ftl[ch][ce][lun].alloc.op_count,
             (unsigned)g_ftl[ch][ce][lun].alloc.spare_count);

    if (ftl_boot_recovery(&g_ftl[ch][ce][lun], &result) != FTL_OK)
    {
        SSD_LOGE("[FTL] boot recovery failed ch=%u ce=%u lun=%u\r\n", ch, ce, lun);
        return -1;
    }
    SSD_LOGI("[FTL] boot type=%d cp_seq=%lu journal_entries=%lu\r\n",
             (int)result.boot_type,
             (unsigned long)result.cp_sequence,
             (unsigned long)result.journal_entries);

    return 0;
}

/* 完整初始化 FTL：全实例 boot + controller 绑定（boot_ok 才置 g_ftl_ready） */
static void ssd_ftl_boot_all(void)
{
    uint8_t ch;
    uint8_t ce;
    uint8_t lun;
    int     boot_ok  = 1;
    int     ctrl_ret;

    for (ch = 0U; ch < CH_NUM; ch++)
    {
        for (ce = 0U; ce < CE_NUM; ce++)
        {
            for (lun = 0U; lun < LUN_NUM; lun++)
            {
                if (ssd_ftl_boot(ch, ce, lun) != 0)
                {
                    boot_ok = 0;
                }
            }
        }
    }

    if (!boot_ok)
    {
        SSD_LOGE("[FTL] boot failed, fallback to fc_service\r\n");
        return;
    }

    /* 填 g_ftl_ptrs 三维指针表 → ssd_controller_init（类型精确匹配） */
    for (ch = 0U; ch < CH_NUM; ch++)
    {
        for (ce = 0U; ce < CE_NUM; ce++)
        {
            for (lun = 0U; lun < LUN_NUM; lun++)
            {
                g_ftl_ptrs[ch][ce][lun] = &g_ftl[ch][ce][lun];
            }
        }
    }

    /* F6：全局 completion 表的实例路由表 = 全部实例 boot 完成后的 g_ftl_ptrs。
     * 必须在所有 ftl_init 之后、首次 ftl_service 之前设置（路由表是全局的，
     * 补进哪个实例都行，一次设置覆盖全部）。 */
    completion_table_set_route(g_ftl_ptrs);

    ctrl_ret = ssd_controller_init(&g_ctrl, g_ftl_ptrs);
    if (ctrl_ret != SSD_STATUS_OK)
    {
        SSD_LOGE("[FTL] controller init failed: %d\r\n", ctrl_ret);
        return;
    }

    g_ftl_ready = 1;
    SSD_LOGI("[FTL] controller ready, lba_max=%llu\r\n",
             (unsigned long long)FTL_LBA_MAX);
}

#if 0
static void stm32_report_previous_crash(void)
{
    const ssd_crash_record_t *record = ssd_crash_record_get();

    if (record == NULL)
    {
        return;
    }

    SSD_LOGE("previous assert: %s (%s:%lu)\r\n",
             record->expression,
             record->file,
             (unsigned long)record->line);
    if (ssd_log_flush(1000U) == 0)
    {
        ssd_crash_record_clear();
    }
}
#endif
/* Newlib-nano calls this hook from __libc_init_array when no CRT provides it. */
void _init(void)
{
}

/*
 * STM32H743 烧录自检入口。
 * Reset_Handler 已完成 C 运行时初始化；HAL_Init() 配置 SysTick 为 1 ms 节拍。
 */
/*
void SysTick_Handler(void)
{
    HAL_IncTick();
}
*/

int main(void)
{
    uint8_t nand_init_result;

    sys_init();
    ssd_log_set_sink(stm32_uart_log_sink);
    ssd_log_set_flush_sink(stm32_uart_log_flush_sink);
    ssd_log_set_level(SSD_LOG_INFO);
    SSD_LOGD("debug logging enabled\r\n");
    /* VBAT 未供电时 BKPSRAM 无法保持；调试阶段暂不读取 Crash Record。 */
    /* stm32_report_previous_crash(); */
    SSD_LOGI("soft_ssd debug revision=%lu\r\n", (unsigned long)SSD_FIRMWARE_DEBUG_REVISION);
    if (fc_init() != FC_STATUS_OK)
    {
        SSD_LOGE("fc init failed\r\n");
    }
    else if ((nand_init_result = nand_init()) != NAND_RESULT_OK)
    {
        SSD_LOGE("nand init failed: 0x%02X\r\n", (unsigned int)nand_init_result);
    }
    else
    {
        nand_dispatch_set_device_ready(1U);
        SSD_LOGI("fc and nand raw path ready\r\n");
        /* 平台 time_us 注册：FTL 的 T3 定时触发（CP 10s / GC 500ms）依赖它。
         * 必须早于首次 ftl_service（boot 即首次），失败仅打日志不阻塞启动。 */
        if (stm32_platform_register() != 0)
        {
            SSD_LOGE("platform register failed; T3 timers disabled\r\n");
        }
        ssd_ftl_boot_all();
    }
    /* APB1=100 MHz and APB1 prescaler=2, so TIM6 kernel clock is 200 MHz. */
    btim_timx_int_init(1000 - 1, 200 - 1);
    SSD_LOGI("soft_ssd console ready\r\n");
    SSD_LOGI("type 'help' for commands\r\n");
#if (SSD_WATCHDOG_ENABLE != 0U)
    if (!storage_wdg_init(&g_storage_wdg_config))
    {
        SSD_LOGE("watchdog supervisor init failed\r\n");
    }
    else
    {
        /* 固定任务已在 btim.c 的 g_btim_tasks 中注册；这里只启动其运行态。 */
        btim_scheduler_init();
        SSD_LOGI("btim task table ready, IWDG nominal=30s, period=100ms\r\n");
    }
#else
    btim_scheduler_init();
    SSD_LOGI("btim task table ready, IWDG disabled for debug\r\n");
#endif
    uart_service_poll();
    while (1)
    {
        /* All command parsing and scheduled storage work remain outside ISR context. */
        uart_service_poll();
        if (g_ftl_ready)
        {
            /* FTL 主循环（Phase 1 内部已含 fc_service ≤4 次） */
            ftl_service(&g_ftl[0][0][0]);
        }
        else
        {
            (void)fc_service();
        }
        btim_scheduler_poll();
    }
}
