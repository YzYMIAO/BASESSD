/* ======================================================================
 * main.c — 启动入口与裸机主循环
 *
 * 职责：全系统初始化顺序编排 + superloop 驱动源。
 * 调用链：
 *   main() → sys_init → ssd_log 绑定 → fc_init → nand_init
 *          → nand_dispatch_set_armed(1) → stm32_platform_register
 *          → ssd_ftl_boot_all（含 ssd_ftl_boot / ssd_controller_init）
 *          → while(1) { cli_service_poll; ftl_service | fc_service;
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
#include "controller/cli.h"
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
#include "fault.h"
#include "mpu.h"
#include "nand/nand_dispatch.h"
#include "nand/nand_mt29f4g08abadawp.h"
#include "platform.h"
#include "stm32h7xx_hal.h"
#include "sys.h"
#include "usart.h"
#include "watchdog.h"

#include <string.h>

/* 每次需要确认板上 ELF 是否更新时递增；仅用于开发阶段 UART 诊断。 */
#define SSD_FIRMWARE_DEBUG_REVISION 24
#define SSD_WATCHDOG_ENABLE         0U

/* 编译期字符串化：把版本号宏拼进开机横幅。 */
#define SSD_STR_(x) #x
#define SSD_STR(x)  SSD_STR_(x)

/* 开机横幅（编译期常量；构建日期/时间由编译器注入 __DATE__ / __TIME__）。 */
static const char g_boot_banner[] = "=========== soft_ssd rev=" SSD_STR(SSD_FIRMWARE_DEBUG_REVISION) " built " __DATE__ " " __TIME__ " ===========\r\n";

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

/*
 * 分区标题（裸行，不带 [INF] 前缀）：把开机日志按
 * system / nand / controller / ftl / console 分区，便于扫读。
 */
static void boot_section(const char *name)
{
    (void)uart_log_write("--- ", 4U);
    (void)uart_log_write(name, (uint32_t)strlen(name));
    (void)uart_log_write(" ---\r\n", 6U);
}

/* ══════════════════════════════════════════════════════════════════
 * FTL 实例（v0.37 上线；P2-5 起**自持表**）
 *
 * 表所有权（P2-5）：BST / free / op / spare / closed 五个大数组**不再由本文件声明**，
 *   而是内含在 ftl_context_t 里（`ftl->tables`，见 ftl/allocator.h）——表归 FTL 本体
 *   所有，本文件只提供实例存储。与主流同构（OpenSSD 全局数组 / SPDK `spdk_ftl_dev`
 *   内含表）。好处：内存预算**单点可见**（下面的 _Static_assert 只看 g_ftl 一项）。
 *
 * SRAM 布局：
 *   g_ftl（l2p cache 128KB + journal/md + 自持表 ~112KB + 页打包写缓冲 4KB）→ AXI_RAM（.bss）
 *   AXI_RAM 实占 ≈ 287KB + 命令/描述符/缓冲池 ~19KB，仍在 384KB 内。
 *   （v0.60 删 g_valid_map 64KB：有效性走 DRAM-less approach ③，逐 CW META 反查 L2P；
 *     v0.66 加 wbuf 4KB/实例：页打包写缓冲，见 ftl_wbuf_t。）
 * ══════════════════════════════════════════════════════════════════ */
static ftl_context_t g_ftl[CH_NUM][CE_NUM][LUN_NUM]; /* ≈287KB → AXI_RAM（.bss） */
ssd_controller_t     g_ctrl;                         /* controller 路由表（收 g_ftl 三维指针）；非 static：
                                                      * UART CLI（cli.c）经 extern 引用，见 controller/cli.h */
static int g_ftl_ready;                              /* 主循环守卫：全部实例 boot 成功才切 ftl_service */

/* ── 编译期内存预算（AXI_RAM）──
 * g_ftl 随 CH_NUM×CE_NUM×LUN_NUM 线性放大，而 AXI_RAM 是硬上限
 * （linker/stm32h743xx_flash.ld: __AXI_RAM_SIZE = 0x60000 = 384KB）。
 * 当前 1 个维度用 ~287KB，**再加一个实例就装不下**。把它变成编译错误，
 * 而不是运行期 HardFault 或静默的 .bss 溢出。提高维度前必须先重新规划内存。 */
#define BOARD_AXI_RAM_SIZE   (384U * 1024U) /* 对齐 __AXI_RAM_SIZE */
#define FTL_AXI_RAM_RESERVED (20U * 1024U)  /* 命令池/描述符池/缓冲池等（实测 ~19KB）*/

_Static_assert(sizeof(g_ftl) + FTL_AXI_RAM_RESERVED <= BOARD_AXI_RAM_SIZE,
               "FTL 实例数组超出 AXI_RAM：提高 CH/CE/LUN 前必须重新规划内存");

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
    ftl_context_t    *ftl = &g_ftl[ch][ce][lun];
    ftl_allocator_t  *alloc;
    ftl_boot_result_t result;
    uint16_t          gblock;
    uint16_t          free_n = 0U;
    uint16_t          good_n = 0U;

    if (ftl_init(ftl, (ftl_topology_t){.ch = ch, .ce = ce, .lun = lun}) != FTL_OK)
    {
        LOGE("[FTL ] init failed ch=%u ce=%u lun=%u\r\n", ch, ce, lun);
        return -1;
    }

    alloc = &ftl->alloc;

    /* 介质布局版本判据：介质不是当前版本（旧格式 / 空片）⇒ 全片格式化。
     * 放在 ftl_init 之后、free pool 回填之前——bbt_init 已先把出厂坏块标记读进 BST，
     * 且此时回填尚未把未擦的旧块当空白块放进 free pool。格式化后写当前版本 Checkpoint
     * 标记 ⇒ 下次开机判据命中，只发生一次。 */
    if (ftl_media_needs_format(ftl) != 0)
    {
        LOGW("[FTL ] media layout mismatch -> format (erase all blocks)\r\n");
        if (ftl_media_format(ftl) != FTL_OK)
        {
            LOGE("[FTL ] media format failed ch=%u ce=%u lun=%u\r\n", ch, ce, lun);
            return -1;
        }
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

        if (ftl_block_is_bad(&alloc->blocks[gblock]))
        {
            continue;
        }
        if (ftl_is_metadata_block(&ftl->metadata_dir, ftl_gblock_plane(gblock), ftl_gblock_block(gblock)))
        {
            continue;
        }

        good_n++;

        /* ① 前 50 个好块入 Spare Pool（坏块替换池） */
        spare_idx = alloc->spare_count;
        if (spare_idx < FTL_SPARE_POOL_SIZE)
        {
            alloc->spare_blocks[spare_idx] = gblock;
            alloc->spare_count++;
            continue;
        }

        /* ② 前 10% 好块入 OP 池（GC 缓冲池） */
        op_idx = alloc->op_count;
        if (op_idx < (uint16_t)((uint16_t)(FTL_BLOCK_TOTAL / 2U) * FTL_OP_PERCENT / 100U))
        {
            alloc->op_blocks[op_idx] = gblock;
            alloc->op_count++;
            continue;
        }

        allocator_free_block(alloc, gblock);
        free_n++;
    }

    /* 总好块数（spare + op + free）——wear_leveling 的「低擦除区占比」判据靠它定分母
     * （见 allocator.h 声明语义）。allocator_init 置 0，此处按出厂扫描结果置定。 */
    alloc->total_blocks = good_n;

    LOGI("[FTL ] instance ch=%u ce=%u lun=%u free=%u op=%u spare=%u\r\n",
         ch,
         ce,
         lun,
         (unsigned)free_n,
         (unsigned)alloc->op_count,
         (unsigned)alloc->spare_count);

    if (ftl_boot_recovery(ftl, &result) != FTL_OK)
    {
        LOGE("[FTL ] boot recovery failed ch=%u ce=%u lun=%u\r\n", ch, ce, lun);
        return -1;
    }
    LOGI("[FTL ] boot type=%d cp_seq=%lu journal_entries=%lu\r\n",
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
        LOGE("[FTL ] boot failed, fallback to fc_service\r\n");
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
        LOGE("[FTL ] controller init failed: %d\r\n", ctrl_ret);
        return;
    }

    /* ⑤ 主循环切换点：此后走 ftl_service（内部先跑 fc_service） */
    g_ftl_ready = 1;
    /* %lu 而非 %llu：newlib-nano 不支持 long long 格式化，见 usart.c 的 _Static_assert。 */
    LOGI("[FTL ] controller ready, lba_max=%lu (%luMiB)\r\n",
         (unsigned long)FTL_LBA_MAX,
         (unsigned long)((unsigned long)FTL_LBA_MAX * FC_BUFFER_USER_SIZE / (1024UL * 1024UL)));
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
    uint8_t          nand_init_result;
    sys_clock_tree_t clk;

    sys_init();
    cli_init(); /* 预填 CLI 固定 pattern buffer（.sram2_data NOLOAD） */
    log_set_sink(stm32_uart_log_sink);
    log_set_flush_sink(stm32_uart_log_flush_sink);
    log_set_level(LOG_INFO);
    LOGD("debug logging enabled\r\n");
    /* 开机横幅（裸行）+ 系统身份（时钟 / Cache / MPU 区域数）。 */
    (void)uart_log_write(g_boot_banner, sizeof(g_boot_banner) - 1U);
    /*
     * 复位原因 + 上次崩溃记录。必须在 sink 绑定之后调用：它走异步日志，
     * 且 .noinit 记录可跨软复位保留（VBAT 未供电时 BKPSRAM 不可用）。
     */
    fault_report_boot();
    boot_section("system");
    LOGI("[SYS ] CPU=%luMHz HCLK=%luMHz ICache=%s DCache=%s MPU=%lu\r\n",
         (unsigned long)(delay_port_core_clock_hz() / 1000000U),
         (unsigned long)(delay_port_hclk_hz() / 1000000U),
         (get_icache_sta() != 0U) ? "on" : "off",
         (get_dcache_sta() != 0U) ? "on" : "off",
         (unsigned long)sys_mpu_region_count());
    /* 时钟树：全部 HAL 实读。APB1/2/3/4 相等时压成一个 pclk，不等则四个全列。 */
    sys_clock_tree_get(&clk);
    if ((clk.pclk1_hz == clk.pclk2_hz) && (clk.pclk2_hz == clk.pclk3_hz) && (clk.pclk3_hz == clk.pclk4_hz))
    {
        LOGI("[SYS ] clk sys=%lu hclk=%lu pclk=%lu fmc=%lu qspi=%lu uart=%lu\r\n",
             (unsigned long)(clk.sysclk_hz / 1000000U),
             (unsigned long)(clk.hclk_hz / 1000000U),
             (unsigned long)(clk.pclk1_hz / 1000000U),
             (unsigned long)(clk.fmc_hz / 1000000U),
             (unsigned long)(clk.qspi_hz / 1000000U),
             (unsigned long)(clk.uart_hz / 1000000U));
    }
    else
    {
        LOGI("[SYS ] clk sys=%lu hclk=%lu pclk1/2/3/4=%lu/%lu/%lu/%lu fmc=%lu qspi=%lu uart=%lu\r\n",
             (unsigned long)(clk.sysclk_hz / 1000000U),
             (unsigned long)(clk.hclk_hz / 1000000U),
             (unsigned long)(clk.pclk1_hz / 1000000U),
             (unsigned long)(clk.pclk2_hz / 1000000U),
             (unsigned long)(clk.pclk3_hz / 1000000U),
             (unsigned long)(clk.pclk4_hz / 1000000U),
             (unsigned long)(clk.fmc_hz / 1000000U),
             (unsigned long)(clk.qspi_hz / 1000000U),
             (unsigned long)(clk.uart_hz / 1000000U));
    }

    /* ── 全系统初始化顺序（严格 if/else-if 级联：前一步失败会整段跳过后面的）──
     *   ① fc_init()            —— FC 控制面：三池 + SQ/HSQ/CQ + 调度器 + CRC。
     *                             内部会调 nand_dispatch_init()（纯 memset，不碰硬件），
     *                             所以 FC 可以先于 NAND 硬件初始化。
     *   ② nand_init()          —— NAND 芯片：HAL/FMC + R/B# EXTI + per-(ch,ce,lun) 后端
     *                             + RESET + READ_ID 校验。其 boot 命令直接走
     *                             nand_engine_execute，绕过 fc / nand_dispatch 命令队列。
     *   ③ set_armed(1)         —— 放行 nand_dispatch_submit / submit_chain
     *                             （此前一律返回 INVALID_ARGUMENT）。
     *   ④ platform_register()  —— 注册 time_us，供 FTL 的 T3 定时触发（CP/GC）使用。
     *   ⑤ ssd_ftl_boot_all()   —— FTL 全实例 boot + controller 绑定 → 置 g_ftl_ready=1。
     *   详见 docs/design/fc_nand_design.md §22「初始化全流程」。 */
    if (fc_init() != FC_STATUS_OK)
    {
        LOGE("fc init failed\r\n");
    }
    else
    {
        boot_section("nand");
        nand_init_result = nand_init();
        if (nand_init_result != NAND_RESULT_OK)
        {
            LOGE("nand init failed: 0x%02X\r\n", (unsigned int)nand_init_result);
        }
        else
        {
            nand_dispatch_set_armed(1U);
            /* controller 汇总在器件就绪后打印：pools / 拓扑 / 数据通路配置。 */
            boot_section("controller");
            fc_boot_report();
            LOGI("[FC  ] raw path ready\r\n");
            /* 平台 time_us 注册：FTL 的 T3 定时触发（CP 10s / GC 500ms）依赖它。
             * 必须早于首次 ftl_service（boot 即首次），失败仅打日志不阻塞启动。 */
            LOGE_IF(stm32_platform_register() != 0, "platform register failed; T3 timers disabled\r\n");
            boot_section("ftl");
            ssd_ftl_boot_all();
        }
    }
    /* APB1=100 MHz and APB1 prescaler=2, so TIM6 kernel clock is 200 MHz. */
    btim_timx_int_init(1000 - 1, 200 - 1);
    boot_section("console");
    LOGI("[SYS ] console ready, type 'help' for commands\r\n");
#if (SSD_WATCHDOG_ENABLE != 0U)
    if (!storage_wdg_init(&g_storage_wdg_config))
    {
        LOGE("watchdog supervisor init failed\r\n");
    }
    else
    {
        /* 固定任务已在 btim.c 的 g_btim_tasks 中注册；这里只启动其运行态。 */
        btim_scheduler_init();
        LOGI("[SYS ] btim task table ready, IWDG nominal=30s, period=100ms\r\n");
    }
#else
    btim_scheduler_init();
    LOGI("[SYS ] btim task table ready, IWDG disabled for debug\r\n");
#endif
    cli_service_poll();
    /* ── 主循环（裸机 superloop，无中断参与）──
     *   g_ftl_ready == 0：只跑 fc_service()，保持 FC/NAND 裸路径活着（可跑 CLI 自检命令）。
     *   g_ftl_ready == 1：ftl_service_pump() 一次（全局 NAND 泵 + 抽干完成表），
     *                     再对**每个**实例调 ftl_service()（Phase 2.5/3 后台）。
     *                     ⚠ 必须遍历全部实例：只跑 [0][0][0] 会让其余实例的
     *                       journal / checkpoint / wear / bbt / GC 永不推进（序 4）。
     *   cli_service_poll()    —— CLI 解析（含 `test` 单元测试入口）。
     *   btim_scheduler_poll() —— 1ms 任务表（看门狗喂狗等）。
     *   All command parsing and scheduled storage work remain outside ISR context. */
    while (1)
    {
        cli_service_poll();
        if (g_ftl_ready)
        {
            uint8_t ch;
            uint8_t ce;
            uint8_t lun;

            /* ① 全局泵：NAND 推进 + 完成表抽干（实例无关，每轮一次） */
            ftl_service_pump();

            /* ② 逐实例后台：Phase 2.5 补发子操作 + Phase 3 三级后台 */
            for (ch = 0U; ch < CH_NUM; ch++)
            {
                for (ce = 0U; ce < CE_NUM; ce++)
                {
                    for (lun = 0U; lun < LUN_NUM; lun++)
                    {
                        ftl_service(&g_ftl[ch][ce][lun]);
                    }
                }
            }
        }
        else
        {
            (void)fc_service();
        }
        btim_scheduler_poll();
    }
}
