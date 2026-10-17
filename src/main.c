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
#include "common/stack_guard.h"
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
#include "nand/nand_fc_interface.h"
#include "platform.h"
#include "ssd_compiler.h"
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
 * 上电报告（boot_report_*）：把 main() 里的上电打印按分区封装成函数，
 * main() 只留调用与初始化级联，不内联打印细节。
 * 约定：这些函数只做打印 / 只读查询，**不做任何初始化**——初始化仍留在
 * main() 的 if/else-if 级联里，保证打印顺序与初始化成败一致。
 * ══════════════════════════════════════════════════════════════════ */

/* 开机横幅（裸行，编译期常量）+ 复位原因/上次崩溃记录。
 * 必须在 sink 绑定之后调用：fault_report_boot 走异步日志，且 .noinit 记录
 * 可跨软复位保留（VBAT 未供电时 BKPSRAM 不可用）。 */
static void boot_report_banner(void)
{
    (void)uart_log_write(g_boot_banner, sizeof(g_boot_banner) - 1U);
    fault_report_boot();
}

/* system 分区：系统身份（CPU/时钟 / Cache / MPU 区域数）+ 时钟树（全部 HAL 实读）。 */
static void boot_report_system(void)
{
    sys_clock_tree_t clk;

    boot_section("system");
    LOGI("[SYS ] CPU=%luMHz HCLK=%luMHz ICache=%s DCache=%s MPU=%lu\r\n",
         (unsigned long)(delay_port_core_clock_hz() / 1000000U),
         (unsigned long)(delay_port_hclk_hz() / 1000000U),
         (get_icache_sta() != 0U) ? "on" : "off",
         (get_dcache_sta() != 0U) ? "on" : "off",
         (unsigned long)sys_mpu_region_count());
    /* 时钟树：APB1/2/3/4 相等时压成一个 pclk，不等则四个全列。 */
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
             (unsigned long)(clk.uart_hz / 1000000U));
    }
}

/* controller 分区：FC 池 / 拓扑 / 数据通路配置汇总（器件就绪后打印）。 */
static void boot_report_controller(void)
{
    boot_section("controller");
    fc_boot_report();
    LOGI("[FC  ] raw path ready\r\n");
}

/* console 分区：交互入口就绪提示。 */
static void boot_report_console(void)
{
    boot_section("console");
    LOGI("[SYS ] console ready, type 'help' for commands\r\n");
}

/* ══════════════════════════════════════════════════════════════════
 * FTL 实例（v0.37 上线；P2-5 起**自持表**）
 *
 * 表所有权（P2-5）：BST / free / op / spare / closed 五个大数组**不再由本文件声明**，
 *   而是内含在 ftl_context_t 里（`ftl->tables`，见 ftl/allocator.h）——表归 FTL 本体
 *   所有，本文件只提供实例存储。与主流同构（OpenSSD 全局数组 / SPDK `spdk_ftl_dev`
 *   内含表）。好处：内存预算**单点可见**（下面的 _Static_assert 看 g_ftl + 共享 cache）。
 *
 * SRAM 布局（阶段 C：描述符 + 内存域散列）：
 *   AXI_RAM（.bss）：g_ftl 描述符（~14.7KB/实例）+ g_l2p_cache（共享热 cache）
 *                    + CLI 写缓冲 + 其它 .bss
 *   SRAM1+SRAM2（.sram12_data）：g_ftl_tables[FTL_DIM_TOTAL]（BST/池/closed）
 *   SRAM3（.sram3_data）：g_ftl_cp_job[] + g_ftl_journal[]
 *   SRAM4（.sram4_data）：g_ftl_md[]（Metadata Directory）
 *
 * 四个 `.sram*_data` 数组是 **NOLOAD**（启动不清零）⇒ ftl_init 显式 memset。
 * N=2：AXI ~197KB / SRAM12 247,792B / SRAM3 21,256B / SRAM4 49,320B，均装得下。
 * ══════════════════════════════════════════════════════════════════ */
static ftl_context_t g_ftl[CH_NUM][CE_NUM][LUN_NUM]; /* 描述符 → AXI_RAM（.bss） */

/* 共享 L2P cache：同一 (ch,ce) 下所有 LUN 共用一份（阶段 B）。
 * 设备级初始化一次，由 ssd_l2p_owner_of_region 把全局 region 解析到 owner LUN。 */
static l2p_cache_t g_l2p_cache[CH_NUM][CE_NUM];

/* per-instance 大数组（阶段 C）：由 ftl_storage_t 绑定到描述符。 */
static ftl_tables_t         g_ftl_tables[FTL_DIM_TOTAL] SRAM12_DATA;
static cp_job_t             g_ftl_cp_job[FTL_DIM_TOTAL] SRAM3_DATA;
static l2p_journal_t        g_ftl_journal[FTL_DIM_TOTAL] SRAM3_DATA;
static metadata_dir_cache_t g_ftl_md[FTL_DIM_TOTAL] SRAM4_DATA;

ssd_controller_t g_ctrl; /* controller 路由表（收 g_ftl 三维指针）；非 static：
                          * UART CLI（cli.c）经 extern 引用，见 controller/cli.h */
static int g_ftl_ready;  /* 主循环守卫：全部实例 boot 成功才切 ftl_service */

/* ── 编译期内存预算（各域）──
 * AXI_RAM 是硬上限（linker: __AXI_RAM_SIZE = 0x60000 = 384KB）；
 * D2/D3 SRAM 的容量与 linker 的 SRAM12/SRAM3/SRAM4 区域一一对应。 */
#define BOARD_AXI_RAM_SIZE   (384U * 1024U) /* 对齐 __AXI_RAM_SIZE */
#define BOARD_SRAM12_SIZE    (256U * 1024U) /* SRAM1+SRAM2 合并区域 */
#define BOARD_SRAM3_SIZE     (32U * 1024U)
#define BOARD_SRAM4_SIZE     (64U * 1024U)
#define FTL_AXI_RAM_RESERVED (20U * 1024U) /* 命令池/描述符池/缓冲池等（实测 ~19KB）*/

/* g_wr_buf 从 .sram2_data 迁入 AXI .bss（SRAM12 留给 tables），显式计入预算。 */
#define CLI_AXI_RAM_RESERVED (16U * 1024U)

_Static_assert(sizeof(g_ftl) + sizeof(g_l2p_cache) + FTL_AXI_RAM_RESERVED + CLI_AXI_RAM_RESERVED <= BOARD_AXI_RAM_SIZE,
               "FTL 描述符/共享 cache/CLI 缓冲超出 AXI_RAM");
_Static_assert(sizeof(g_ftl_tables) <= BOARD_SRAM12_SIZE, "FTL tables 超出 SRAM12（SRAM1+SRAM2）");
_Static_assert(sizeof(g_ftl_cp_job) + sizeof(g_ftl_journal) <= BOARD_SRAM3_SIZE, "cp_job/journal 超出 SRAM3");
_Static_assert(sizeof(g_ftl_md) <= BOARD_SRAM4_SIZE, "metadata_dir 超出 SRAM4");

/* 指针形式的三维路由表（类型匹配 ssd_controller_init 形参）。
 * 编译器不认可 static 数组直接退化为匹配指针数组参数（GCC 10.3 起
 * -Wincompatible-pointer-types 恒报），此处显式构造。 */
static ftl_context_t *g_ftl_ptrs[CH_NUM][CE_NUM][LUN_NUM];

/*
 * ssd_l2p_owner_of_region - 全局 L2P region → owner FTL 实例
 *
 * 共享 cache 的 load/flush/commit 用它找 owner die。region→(ch,ce,lun) 的分解
 * 复用 completion_table_route_region（与 lba_route_* 同一套公式，避免第二套坐标）。
 */
static struct ftl_context *ssd_l2p_owner_of_region(void *ctx, uint32_t global_region)
{
    (void)ctx;

    return completion_table_route_region(global_region);
}

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
    ftl_context_t       *ftl = &g_ftl[ch][ce][lun];
    ftl_allocator_t     *alloc;
    ftl_boot_result_t    result;
    const ftl_topology_t topo    = {.ch = ch, .ce = ce, .lun = lun};
    const uint32_t       dim     = ftl_topo_dim(topo);
    const ftl_storage_t  storage = {
         .tables       = &g_ftl_tables[dim],
         .metadata_dir = &g_ftl_md[dim],
         .journal      = &g_ftl_journal[dim],
         .cp_job       = &g_ftl_cp_job[dim],
    };

    ASSERT(dim < FTL_DIM_TOTAL);

    if (ftl_init(ftl, topo, &g_l2p_cache[ch][ce], &storage) != FTL_OK)
    {
        LOGE("[FTL ] init failed ch=%u ce=%u lun=%u\r\n", ch, ce, lun);
        return -1;
    }

    alloc = &ftl->alloc;

    /* 介质布局版本判据：介质不是当前版本（旧格式 / 空片）⇒ 全片格式化。
     * 放在 ftl_init 之后、free pool 回填之前——bbt_init 已把出厂坏块标记读进 BST
     * （**只在空片时读**；用过的介质上那些字节是 FTL 数据不是标记，bbt_init 会拒绝重扫，
     * 见 bbt.c:bbt_medium_has_ftl_data），且此时回填尚未把未擦的旧块当空白块放进
     * free pool。格式化后写当前版本 Checkpoint 标记 ⇒ 下次开机判据命中，只发生一次。 */
    if (ftl_media_needs_format(ftl))
    {
        LOGW("[FTL ] media layout mismatch -> format (erase all blocks)\r\n");
        if (ftl_media_format(ftl) != FTL_OK)
        {
            LOGE("[FTL ] media format failed ch=%u ce=%u lun=%u\r\n", ch, ce, lun);
            return -1;
        }
    }

    /* 池回填 / F2 / 块账快照恢复全部在 ftl_boot_recovery 内完成：
     *   · 块账快照有效 ⇒ 直接按快照恢复 Free/OP/Spare（不再几何 carve）；
     *   · 无快照 / 快照损坏 ⇒ carve + F2。
     * ⚠ carve 必须先于 F2（F2 要把 written 块逐出 Free Pool），且**不能**在
     *   块账快照恢复之前做——否则池条目会重复。 */
    if (ftl_boot_recovery(ftl, &result) != FTL_OK)
    {
        LOGE("[FTL ] boot recovery failed ch=%u ce=%u lun=%u\r\n", ch, ce, lun);
        return -1;
    }

    LOGI("[FTL ] instance ch=%u ce=%u lun=%u free=%u op=%u spare=%u\r\n",
         ch,
         ce,
         lun,
         (unsigned)allocator_free_count(alloc),
         (unsigned)allocator_op_total(alloc),
         (unsigned)allocator_spare_total(alloc));

    LOGI("[FTL ] boot type=%d cp_seq=%lu journal_entries=%lu bit=%s\r\n",
         (int)result.boot_type,
         (unsigned long)result.cp_sequence,
         (unsigned long)result.journal_entries,
         (result.blocks_restored != 0U) ? "restored" : "fallback");

    return 0;
}

/* 完整初始化 FTL：全实例 boot + controller 绑定（boot_ok 才置 g_ftl_ready）
 *
 *   ① 建 g_ftl_ptrs 三维指针表 + completion_table_set_route —— **必须早于逐实例 boot**，
 *      理由见函数内注释（boot 期 F2 重建会经 l2p_cache_read_sync 抽完成表）。
 *   ② 逐 (ch,ce,lun) boot —— 任一失败则 boot_ok=0，整体回退 fc_service。
 *   ③ ssd_controller_init(&g_ctrl, g_ftl_ptrs) —— 对外 controller 就绪。
 *   ④ g_ftl_ready = 1 —— 主循环切换到 ftl_service。
 */
static void ssd_ftl_boot_all(void)
{
    uint8_t ch;
    uint8_t ce;
    uint8_t lun;
    int     boot_ok = 1;
    int     ctrl_ret;

    /* ① 全局完成旁表 + 路由表：清一次、填一次、设一次——**全部必须早于逐实例 boot**。
     *    - completion_table_init() 清的是**全局**资源（旁表 + 路由表），只做一次。
     *      ⚠ 它原在 ftl_init 里（逐实例调用）⇒ 实例 1 的 init 会抹掉实例 0 在飞的
     *      ctx 登记与已设好的路由表；已移到此处。
     *    - g_ftl_ptrs 只是对静态数组 g_ftl 取址，不依赖 ftl_init。
     *    - 为何路由不能等 boot 之后再设：`ssd_ftl_boot` 内部的 `ftl_boot_recovery`
     *      （F2 重建）会经 `l2p_cache_read_sync` → `l2p_sync_pump` → `ftl_service_pump()`
     *      抽完成表。此时若 route 仍为 NULL，`ftl_op_dispatch` 解析出的 ftl 为 NULL，
     *      服务函数只 release 不推进状态 ⇒ job 永久卡死
     *      （现象：[L2P] read_sync timeout + load_inflight 永久不减）。
     *    命令能在完成时拿到非 NULL 实例，前提是该实例的 ftl_init 已跑过——boot 循环
     *    逐实例推进，任一时刻在飞的命令只可能属于已 boot 的实例。 */
    completion_table_init();
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
    completion_table_set_route(g_ftl_ptrs);

    /* ①.5 共享 L2P cache：每个 (ch,ce) 一份，**设备级只初始化一次**。
     * 必须早于逐实例 boot（ftl_boot_recovery 的 F2/journal 回放就会访问 cache）；
     * 且绝不能放进逐实例 ftl_init（第二个实例会清掉第一个实例的 line）。 */
    for (ch = 0U; ch < CH_NUM; ch++)
    {
        for (ce = 0U; ce < CE_NUM; ce++)
        {
            l2p_cache_init(&g_l2p_cache[ch][ce], ch, ce, ssd_l2p_owner_of_region, NULL);
        }
    }

    /* ② 逐实例 boot */
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
        /* 实例未必 init 完成 ⇒ 撤销路由，恢复「路由未设」语义（此时主循环退回
         * fc_service，无人 drain 完成表，派发不会发生）。 */
        completion_table_set_route(NULL);
        LOGE("[FTL ] boot failed, fallback to fc_service\r\n");
        return;
    }

    /* ②.5 路由 ↔ dim 的往返自检（owner 解析的根基）：
     * ftl_topo_dim(route(dim)) 必须等于 dim；公式漂移会让每次 cache 操作找不到 owner。
     * 必须在逐实例 ftl_init 之后做（topo 那时才有值），且 release 也拒绝启动
     * （H14 判据 9 的同类：静默错路由比停机更贵）。 */
    for (uint32_t dim = 0U; dim < FTL_DIM_TOTAL; dim++)
    {
        const struct ftl_context *owner = completion_table_route_region(dim);

        if ((owner == NULL) || (ftl_topo_dim(owner->topo) != dim))
        {
            LOGE("[FTL ] route/dim mismatch dim=%lu\r\n", (unsigned long)dim);
            completion_table_set_route(NULL);
            return;
        }
    }

    /* ③ controller 就绪 */
    ctrl_ret = ssd_controller_init(&g_ctrl, g_ftl_ptrs);
    if (ctrl_ret != SSD_STATUS_OK)
    {
        LOGE("[FTL ] controller init failed: %d\r\n", ctrl_ret);
        return;
    }

    /* ④ 主循环切换点：此后走 ftl_service（内部先跑 fc_service） */
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
    uint8_t nand_init_result;

    sys_init();
    cli_init(); /* 预填 CLI 固定 pattern buffer（.sram2_data NOLOAD） */
    log_set_sink(stm32_uart_log_sink);
    log_set_flush_sink(stm32_uart_log_flush_sink);
    log_set_level(LOG_INFO);
    LOGD("debug logging enabled\r\n");
    /* 栈哨兵：把 [&end, 当前 SP) 整段填 pattern。必须**早**——越早，后面
     * （FTL boot / CLI test 这些深调用）用过的栈就越多地落在被守卫区内，
     * 周期校验才量得到真实峰值。此后由 superloop 每轮 stack_guard_check() 校验。 */
    stack_guard_init();
    boot_report_banner();
    boot_report_system();

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
            boot_report_controller();
            /* 平台 time_us 注册：FTL 的 T3 定时触发（CP 10s / GC 500ms）依赖它。
             * 必须早于首次 ftl_service（boot 即首次），失败仅打日志不阻塞启动。 */
            LOGE_IF(stm32_platform_register() != 0, "platform register failed; T3 timers disabled\r\n");
            boot_section("ftl");
            ssd_ftl_boot_all();
        }
    }
    /* APB1=100 MHz and APB1 prescaler=2, so TIM6 kernel clock is 200 MHz. */
    btim_timx_int_init(1000 - 1, 200 - 1);
    boot_report_console();
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
        /* 栈哨兵周期校验（每轮一次，MSP 无异常时开销 ~10 条指令）：
         * 查 MSP 是否仍在 DTCM 内 + 从上次最低点继续向下找被踩过的 pattern。
         * 放在循环**最前**：上一轮（含 CLI 命令 / FTL 后台）若踩了栈，这里先报。 */
        stack_guard_check();
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
