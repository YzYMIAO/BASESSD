/* ======================================================================
 * main.c — 启动入口与裸机主循环
 *
 * 职责：全系统初始化顺序编排 + superloop 驱动源。
 * 调用链：
 *   main() → sys_init → ssd_log 绑定 → fc_init → nand_init
 *          → nand_dispatch_set_device_ready(1) → stm32_platform_register
 *          → ssd_ftl_boot_all（含 ssd_ftl_boot / ssd_controller_init）
 *          → while(1) { uart_service_poll; ftl_service | fc_service;
 *                       btim_scheduler_poll; }
 * 关键约定：
 *   - 初始化是**严格 if/else-if 级联**：前一步失败会整段跳过后面的，
 *     因此 fc_init 失败时 NAND 根本不会被初始化。
 *   - FC/NAND/FTL 的推进**全部在非 ISR 上下文**：没有任何中断调用
 *     fc_service / ftl_service，superloop 是唯一驱动源。
 *   - NAND 启动命令（RESET/READ_ID）在 nand_init 内绕过命令队列，
 *     直连 nand_engine_execute。
 * 生命周期：ssd_ftl_boot_all 成功 → g_ftl_ready=1，主循环由 fc_service
 *          切换到 ftl_service（后者内部会先跑 fc_service）。
 * 文件结构：日志 sink 绑定 → FTL 实例存储 → ssd_ftl_boot(_all) → main()。
 * 流程图见 docs/design/fc_nand_design.md §22（图 14）/ §23（图 18）。
 * ====================================================================== */

#include "btim.h"
#include "common/log.h"
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
#include "platform/stm32h743/fault.h"
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
static ftl_context_t g_ftl[CH_NUM][CE_NUM][LUN_NUM];                         /* 131.5KB → AXI_RAM */
static ftl_block_t   g_blocks[CH_NUM][CE_NUM][LUN_NUM][FTL_BLOCK_TOTAL];     /* 56B×4096=224KB → AXI_RAM */
static uint16_t      g_free_blocks[CH_NUM][CE_NUM][LUN_NUM][FTL_BLOCK_TOTAL] /* 8KB → SRAM2 */
    __attribute__((section(".sram2_data")));
static uint16_t g_op_blocks[CH_NUM][CE_NUM][LUN_NUM][FTL_BLOCK_TOTAL] /* OP 预留池，容量不动 → SRAM2 */
    __attribute__((section(".sram2_data")));
static uint16_t g_spare_blocks[CH_NUM][CE_NUM][LUN_NUM][FTL_SPARE_POOL_SIZE] /* 坏块替换池 → SRAM2 */
    __attribute__((section(".sram2_data")));
static super_block_t g_closed_sb_list[CH_NUM][CE_NUM][LUN_NUM][SUPER_BLOCK_LIST_MAX] /* 16KB → SRAM2 */
    __attribute__((section(".sram2_data")));
ssd_controller_t g_ctrl; /* controller 路由表（收 g_ftl 三维指针）；非 static：
                          * UART CLI（usart.c）经 extern 引用，见 usart.h */
static int g_ftl_ready;  /* 主循环守卫：全部实例 boot 成功才切 ftl_service */

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
    params.topo           = (ftl_topology_t){.ch = ch, .ce = ce, .lun = lun};
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
        LOGE("[FTL] init failed ch=%u ce=%u lun=%u\r\n", ch, ce, lun);
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
    LOGI("[FTL] instance ch=%u ce=%u lun=%u free=%u op=%u spare=%u\r\n",
         ch,
         ce,
         lun,
         (unsigned)free_n,
         (unsigned)g_ftl[ch][ce][lun].alloc.op_count,
         (unsigned)g_ftl[ch][ce][lun].alloc.spare_count);

    if (ftl_boot_recovery(&g_ftl[ch][ce][lun], &result) != FTL_OK)
    {
        LOGE("[FTL] boot recovery failed ch=%u ce=%u lun=%u\r\n", ch, ce, lun);
        return -1;
    }
    LOGI("[FTL] boot type=%d cp_seq=%lu journal_entries=%lu\r\n",
         (int)result.boot_type,
         (unsigned long)result.cp_sequence,
         (unsigned long)result.journal_entries);

    return 0;
}

/* 完整初始化 FTL：全实例 boot + controller 绑定（boot_ok 才置 g_ftl_ready）
 *
 *   ① 逐 (ch,ce,lun) boot —— 任一失败则 boot_ok=0，整体回退 fc_service。
 *   ② 建 g_ftl_ptrs 三维指针表 —— 精确类型匹配 ssd_controller_init 形参。
 *   ③ completion_table_set_route(g_ftl_ptrs) —— 全局完成表的实例路由。
 *   ④ ssd_controller_init(&g_ctrl, g_ftl_ptrs) —— 对外 controller 就绪。
 *   ⑤ g_ftl_ready = 1 —— 主循环切换到 ftl_service。
 */
static void ssd_ftl_boot_all(void)
{
    uint8_t ch;
    uint8_t ce;
    uint8_t lun;
    int     boot_ok = 1;
    int     ctrl_ret;

    /* ① 逐实例 boot */
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
        LOGE("[FTL] boot failed, fallback to fc_service\r\n");
        return;
    }

    /* ② 填 g_ftl_ptrs 三维指针表 → ssd_controller_init（类型精确匹配） */
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

    /* ③ F6：全局 completion 表的实例路由表 = 全部实例 boot 完成后的 g_ftl_ptrs。
     * 必须在所有 ftl_init 之后、首次 ftl_service 之前设置（路由表是全局的，
     * 补进哪个实例都行，一次设置覆盖全部）。 */
    completion_table_set_route(g_ftl_ptrs);

    /* ④ controller 就绪 */
    ctrl_ret = ssd_controller_init(&g_ctrl, g_ftl_ptrs);
    if (ctrl_ret != SSD_STATUS_OK)
    {
        LOGE("[FTL] controller init failed: %d\r\n", ctrl_ret);
        return;
    }

    /* ⑤ 主循环切换点：此后走 ftl_service（内部先跑 fc_service） */
    g_ftl_ready = 1;
    /* %lu 而非 %llu：newlib-nano 不支持 long long 格式化，见 usart.c 的 _Static_assert。 */
    LOGI("[FTL] controller ready, lba_max=%lu\r\n",
         (unsigned long)FTL_LBA_MAX);
}

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
    log_set_sink(stm32_uart_log_sink);
    log_set_flush_sink(stm32_uart_log_flush_sink);
    log_set_level(LOG_INFO);
    LOGD("debug logging enabled\r\n");
    LOGI("soft_ssd debug revision=%lu\r\n", (unsigned long)SSD_FIRMWARE_DEBUG_REVISION);
    /*
     * 复位原因 + 上次崩溃记录。必须在 sink 绑定之后调用：它走异步日志，
     * 且 .noinit 记录可跨软复位保留（VBAT 未供电时 BKPSRAM 不可用）。
     */
    fault_report_boot();

    /* ── 全系统初始化顺序（严格 if/else-if 级联：前一步失败会整段跳过后面的）──
     *   ① fc_init()            —— FC 控制面：三池 + SQ/HSQ/CQ + 调度器 + CRC。
     *                             内部会调 nand_dispatch_init()（纯 memset，不碰硬件），
     *                             所以 FC 可以先于 NAND 硬件初始化。
     *   ② nand_init()          —— NAND 芯片：HAL/FMC + R/B# EXTI + per-(ch,ce,lun) 后端
     *                             + RESET + READ_ID 校验。其 boot 命令直接走
     *                             nand_engine_execute，绕过 fc / nand_dispatch 命令队列。
     *   ③ set_device_ready(1)  —— 放行 nand_dispatch_submit（此前一律返回 NOT_READY）。
     *   ④ platform_register()  —— 注册 time_us，供 FTL 的 T3 定时触发（CP/GC）使用。
     *   ⑤ ssd_ftl_boot_all()   —— FTL 全实例 boot + controller 绑定 → 置 g_ftl_ready=1。
     *   详见 docs/design/fc_nand_design.md §22「初始化全流程」。 */
    if (fc_init() != FC_STATUS_OK)
    {
        LOGE("fc init failed\r\n");
    }
    else if ((nand_init_result = nand_init()) != NAND_RESULT_OK)
    {
        LOGE("nand init failed: 0x%02X\r\n", (unsigned int)nand_init_result);
    }
    else
    {
        nand_dispatch_set_device_ready(1U);
        LOGI("fc and nand raw path ready\r\n");
        /* 平台 time_us 注册：FTL 的 T3 定时触发（CP 10s / GC 500ms）依赖它。
         * 必须早于首次 ftl_service（boot 即首次），失败仅打日志不阻塞启动。 */
        if (stm32_platform_register() != 0)
        {
            LOGE("platform register failed; T3 timers disabled\r\n");
        }
        ssd_ftl_boot_all();
    }
    /* APB1=100 MHz and APB1 prescaler=2, so TIM6 kernel clock is 200 MHz. */
    btim_timx_int_init(1000 - 1, 200 - 1);
    LOGI("soft_ssd console ready\r\n");
    LOGI("type 'help' for commands\r\n");
#if (SSD_WATCHDOG_ENABLE != 0U)
    if (!storage_wdg_init(&g_storage_wdg_config))
    {
        LOGE("watchdog supervisor init failed\r\n");
    }
    else
    {
        /* 固定任务已在 btim.c 的 g_btim_tasks 中注册；这里只启动其运行态。 */
        btim_scheduler_init();
        LOGI("btim task table ready, IWDG nominal=30s, period=100ms\r\n");
    }
#else
    btim_scheduler_init();
    LOGI("btim task table ready, IWDG disabled for debug\r\n");
#endif
    uart_service_poll();
    /* ── 主循环（裸机 superloop，无中断参与）──
     *   g_ftl_ready == 0：只跑 fc_service()，保持 FC/NAND 裸路径活着（可跑 CLI 自检命令）。
     *   g_ftl_ready == 1：改跑 ftl_service()，它内部会先调用 fc_service() ≤4 次
     *                     （NAND_SUBMIT_BUDGET）再 drain 完成表。
     *   uart_service_poll()   —— CLI 解析（含 `test` 单元测试入口）。
     *   btim_scheduler_poll() —— 1ms 任务表（看门狗喂狗等）。
     *   All command parsing and scheduled storage work remain outside ISR context. */
    while (1)
    {
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
