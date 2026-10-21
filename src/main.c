/* ======================================================================
 * main.c — 启动入口与裸机主循环
 *
 * 职责：全系统初始化顺序编排 + superloop 驱动源。
 * 调用链：
 *   main() → sys_init → ssd_log 绑定 → fc_init → nand_init
 *          → nand_dispatch_set_armed(1) → stm32_platform_register
 *          → ssd_ftl_boot_all（含 ssd_ftl_boot / ssd_controller_init）
 *          → usb_msc_init → usb_device_init
 *          → while(1) { cli_service_poll; ftl_service | fc_service;
 *                       usb_msc_task; usb_device_task; btim_scheduler_poll; }
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

#include "allocator.h"
#include "bst.h"
#include "btim.h"
#include "cli.h"
#include "completion_table.h"
#include "controller.h"
#include "delay.h"
#include "fc.h"
#include "ftl.h"
#include "log.h"
#include "metadata.h"
#include "power_failure_recovery.h"
#include "stack_guard.h"
// #include "led.h"
#include "fault.h"
#include "mpu.h"
#include "nand_dispatch.h"
#include "nand_fc_interface.h"
#include "platform.h"
#include "ssd_compiler.h"
#include "stm32h7xx_hal.h"
#include "sys.h"
#include "usart.h"
#include "usb.h"
#include "usb_msc.h"
#include "watchdog.h"

#include <string.h>

/* 每次需要确认板上 ELF 是否更新时递增；仅用于开发阶段 UART 诊断。 */
#define SSD_FIRMWARE_DEBUG_REVISION 33
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
static DWORD stm32_uart_log_sink(const char *message, DWORD length)
{
    return uart_log_write(message, length);
}

static bool stm32_uart_log_flush_sink(DWORD timeout_ms)
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
    (void)uart_log_write(name, strlen(name));
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
         (delay_port_core_clock_hz() / 1000000U),
         (delay_port_hclk_hz() / 1000000U),
         (get_icache_sta() != 0U) ? "on" : "off",
         (get_dcache_sta() != 0U) ? "on" : "off",
         sys_mpu_region_count());
    /* 时钟树：APB1/2/3/4 相等时压成一个 pclk，不等则四个全列。 */
    sys_clock_tree_get(&clk);
    if ((clk.pclk1_hz == clk.pclk2_hz) && (clk.pclk2_hz == clk.pclk3_hz) && (clk.pclk3_hz == clk.pclk4_hz))
    {
        LOGI("[SYS ] clk sys=%lu hclk=%lu pclk=%lu fmc=%lu qspi=%lu uart=%lu\r\n",
             (clk.sysclk_hz / 1000000U),
             (clk.hclk_hz / 1000000U),
             (clk.pclk1_hz / 1000000U),
             (clk.fmc_hz / 1000000U),
             (clk.qspi_hz / 1000000U),
             (clk.uart_hz / 1000000U));
    }
    else
    {
        LOGI("[SYS ] clk sys=%lu hclk=%lu pclk1/2/3/4=%lu/%lu/%lu/%lu fmc=%lu qspi=%lu uart=%lu\r\n",
             (clk.sysclk_hz / 1000000U),
             (clk.hclk_hz / 1000000U),
             (clk.pclk1_hz / 1000000U),
             (clk.pclk2_hz / 1000000U),
             (clk.pclk3_hz / 1000000U),
             (clk.pclk4_hz / 1000000U),
             (clk.fmc_hz / 1000000U),
             (clk.uart_hz / 1000000U));
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
 * 表所有权：BST / free / op / spare / closed 五个大数组由**本文件各自独立定义**，
 *   并决定落哪个域；FTL 只收 `ftl_table_refs_t`（五个指针，见 ftl/allocator.h）。
 *   每张表一个对象 ⇒ 可以单独换内存域；内存预算由下面的 _Static_assert 单点核对。
 *
 * SRAM 布局（阶段 C：描述符 + 内存域散列）：
 *   AXI_RAM（.bss）：g_ftl 描述符 + g_ftl_migrate / g_ftl_wbuf / g_ftl_rr_page_scratch
 *                    （Phase 1.5 挂载块）+ g_l2p_cache（共享热 cache）
 *                    + CLI 写缓冲 + 其它 .bss
 *   SRAM1+SRAM2（.sram12_data）：g_ftl_blocks / g_ftl_free_blocks / g_ftl_op_blocks /
 *                                g_ftl_spare_blocks / g_ftl_closed_sb_list（各自独立对象）
 *   SRAM3（.sram3_data）：g_ftl_checkpoint_handle[] + g_ftl_journal[]
 *   SRAM4（.sram4_data）：g_ftl_md[]（Metadata Directory）
 *
 * 四个 `.sram*_data` 数组是 **NOLOAD**（启动不清零）⇒ ftl_init 显式 memset。
 * Phase 1.5 的三个挂载块在 AXI .bss（启动清零），ftl_init 仍显式 memset（统一语义）。
 * N=2：AXI ~197KB / SRAM12 247,792B / SRAM3 21,256B / SRAM4 49,320B，均装得下。
 * ══════════════════════════════════════════════════════════════════ */
static ftl_ctx_t g_ftl[CH_NUM][CE_NUM][LUN_NUM]; /* 描述符 → AXI_RAM（.bss） */

/* 共享 L2P cache：同一 (ch,ce) 下所有 LUN 共用一份（阶段 B）。
 * 设备级初始化一次，由 ssd_l2p_owner_of_region 把全局 region 解析到 owner LUN。 */
static l2p_cache_t g_l2p_cache[CH_NUM][CE_NUM];

/* per-instance 大数组（阶段 C）：由 ftl_storage_t 绑定到描述符。
 * 五张表**各自独立定义**，每张可单独换 section/内存域；当前全部落 SRAM12，
 * 将来要挪某一张只改该行的 section 宏，不动别的表。 */
static bst_block_t    g_ftl_blocks[FTL_DIM_TOTAL][FTL_BLOCK_TOTAL] SRAM12_DATA;
static WORD           g_ftl_free_blocks[FTL_DIM_TOTAL][FTL_BLOCK_TOTAL] SRAM12_DATA;
static WORD           g_ftl_op_blocks[FTL_DIM_TOTAL][PLANE_NUM][FTL_OP_POOL_CAP] SRAM12_DATA;
static WORD           g_ftl_spare_blocks[FTL_DIM_TOTAL][PLANE_NUM][FTL_SPARE_POOL_PER_PLANE] SRAM12_DATA;
static super_block_t  g_ftl_closed_sb_list[FTL_DIM_TOTAL][SUPER_BLOCK_LIST_MAX] SRAM12_DATA;
static cp_handle_t    g_ftl_checkpoint_handle[FTL_DIM_TOTAL] SRAM3_DATA;
static l2p_jrn_t      g_ftl_journal[FTL_DIM_TOTAL] SRAM3_DATA;
static md_dir_cache_t g_ftl_md[FTL_DIM_TOTAL] SRAM4_DATA;

/* Phase 1.5：三块大状态挂 storage 指针，仍落 AXI_RAM（默认 .bss）。
 * 挂指针**不减少总 RAM**，只让 ftl_ctx_t 变小、放置可控、handle 形态统一。 */
static mig_handle_t g_ftl_migrate[FTL_DIM_TOTAL];
static wb_handle_t  g_ftl_wbuf[FTL_DIM_TOTAL];
static BYTE         g_ftl_rr_page_scratch[FTL_DIM_TOTAL][FTL_READ_USER_SIZE];

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

_Static_assert(sizeof(g_ftl) + sizeof(g_ftl_migrate) + sizeof(g_ftl_wbuf) +
                       sizeof(g_ftl_rr_page_scratch) + sizeof(g_l2p_cache) +
                       FTL_AXI_RAM_RESERVED + CLI_AXI_RAM_RESERVED <=
                   BOARD_AXI_RAM_SIZE,
               "FTL 描述符/挂载块/共享 cache/CLI 缓冲超出 AXI_RAM");
_Static_assert(sizeof(g_ftl_blocks) + sizeof(g_ftl_free_blocks) + sizeof(g_ftl_op_blocks) +
                       sizeof(g_ftl_spare_blocks) + sizeof(g_ftl_closed_sb_list) <=
                   BOARD_SRAM12_SIZE,
               "FTL tables 超出 SRAM12（SRAM1+SRAM2）");
_Static_assert(sizeof(g_ftl_checkpoint_handle) + sizeof(g_ftl_journal) <= BOARD_SRAM3_SIZE, "cp_handle/journal 超出 SRAM3");
_Static_assert(sizeof(g_ftl_md) <= BOARD_SRAM4_SIZE, "metadata_dir 超出 SRAM4");

/* 指针形式的三维路由表（类型匹配 ssd_controller_init 形参）。
 * 编译器不认可 static 数组直接退化为匹配指针数组参数（GCC 10.3 起
 * -Wincompatible-pointer-types 恒报），此处显式构造。 */
static ftl_ctx_t *g_ftl_ptrs[CH_NUM][CE_NUM][LUN_NUM];

/* 共享 L2P cache 的 owner 端口表（docs §4.7）：每 dim 一份，只暴露
 * {dim, metadata_dir, alloc, topo}，不把整个 ftl_context 交给 l2p_cache。
 * 由 ssd_ftl_boot 在 ftl_init 之后填充；生命周期覆盖全程。 */
static l2p_owner_t g_l2p_owner[FTL_DIM_TOTAL];

/*
 * ssd_l2p_owner_of_region - 全局 L2P region → owner FTL 实例
 *
 * 共享 cache 的 load/flush/commit 用它找 owner die。region→(ch,ce,lun) 的分解
 * 复用 ct_get_route_region（与 lba_route_* 同一套公式，避免第二套坐标）。
 */
static const l2p_owner_t *ssd_l2p_owner_of_region(void *ctx, DWORD global_region)
{
    (void)ctx;

    const DWORD dim = global_region % FTL_DIM_TOTAL;

    ASSERT(dim < FTL_DIM_TOTAL);
    ASSERT(g_l2p_owner[dim].metadata_dir != NULL);

    return &g_l2p_owner[dim];
}

/*
 * ssd_ftl_boot - 初始化单个 (ch,ce,lun) FTL 实例
 *
 * 顺序：ftl_init（内部含 bbt_init 标坏块）→ 遍历 BST 非坏块回填 free pool
 *       （alc_init 不填 free pool、bbt 只标坏块——不回填则首次写失败）
 *       → ftl_boot_recovery → 日志。
 *
 * @param ch/ce/lun 本实例绑定拓扑
 * @return 0 成功，负值失败
 */
static ftl_status_e ssd_ftl_boot(BYTE ch, BYTE ce, BYTE lun)
{
    ftl_ctx_t        *ftl = &g_ftl[ch][ce][lun];
    alc_t            *alloc;
    pfr_boot_result_t result;

    ftl_topo_t topo;
    topo.ch  = ch;
    topo.ce  = ce;
    topo.lun = lun;

    const DWORD dim = ftl_topo_dim(topo);

    ftl_table_refs_t refs;
    refs.blocks         = g_ftl_blocks[dim];
    refs.free_blocks    = g_ftl_free_blocks[dim];
    refs.op_blocks      = g_ftl_op_blocks[dim];
    refs.spare_blocks   = g_ftl_spare_blocks[dim];
    refs.closed_sb_list = g_ftl_closed_sb_list[dim];

    ftl_storage_t storage;
    storage.table_refs      = &refs;
    storage.metadata_dir    = &g_ftl_md[dim];
    storage.journal         = &g_ftl_journal[dim];
    storage.cp_handle       = &g_ftl_checkpoint_handle[dim];
    storage.migrate         = &g_ftl_migrate[dim];
    storage.wbuf            = &g_ftl_wbuf[dim];
    storage.rr_page_scratch = g_ftl_rr_page_scratch[dim];

    ASSERT(dim < FTL_DIM_TOTAL);

    if (ftl_init(ftl, topo, &g_l2p_cache[ch][ce], &storage) != FTL_OK)
    {
        LOGE("[FTL ] init failed ch=%u ce=%u lun=%u\r\n", ch, ce, lun);
        return FTL_ERROR;
    }

    alloc = &ftl->alloc;

    /* L2P owner 端口：必须在任何 cache 访问（含下面的格式化/恢复）之前就绪。 */
    l2p_owner_t owner;

    owner.dim          = dim;
    owner.metadata_dir = ftl->metadata_dir;
    owner.alloc        = &ftl->alloc;
    owner.topo         = ftl->topo;
    g_l2p_owner[dim]   = owner;

    /* debug-only：初始化契约自检。必须在 owner 登记之后、任何 cache 访问之前——
     * 校验全部句柄/端口绑定、topology 副本与 owner/route 身份。
     * release 下连调用都不生成，保证零运行期开销。 */
#if !defined(NDEBUG)
    ftl_contract_audit(ftl);
#endif

    /* 坏块落点统计（开机基线，**早于格式判定**）：boot 可能因介质读不通而中止，
     * 先打一行保证失败时也能看到 BAD 分布。 */
    ftl_boot_log_bad_areas(ftl);

    /* 介质布局版本判据：介质不是当前版本（旧格式 / 空片）⇒ 全片格式化。
     * 放在 ftl_init 之后、free pool 回填之前——bbt_init 已把出厂坏块标记读进 BST
     * （**只在空片时读**；用过的介质上那些字节是 FTL 数据不是标记，bbt_init 会拒绝重扫，
     * 见 bbt.c:bbt_medium_has_ftl_data），且此时回填尚未把未擦的旧块当空白块放进
     * free pool。格式化后写当前版本 Checkpoint 标记 ⇒ 下次开机判据命中，只发生一次。 */
    bool need_format   = false;
    bool format_verify = false;

#if defined(FORCE_FORMAT_ON_BOOT)
    /* 开卡镜像（`make format-image`）：**每次启动**都擦全片并写初始元数据。
     * 用途：介质账本已坏、开机 full metadata rebuild 太慢，先用这张镜像开卡；
     *       开卡完成后必须换回正常镜像——这张镜像每次上电都会清数据。 */
    LOGW("[FTL ] FORCE_FORMAT_ON_BOOT: erase + re-init media (destructive, EVERY boot)\r\n");
    need_format = true;
#    if defined(FORCE_FORMAT_VERIFY)
    format_verify = true;
    LOGW("[FTL ] FORCE_FORMAT_ON_BOOT: full-chip write/read verify enabled\r\n");
#    endif
#else
    if (ftl_media_check_format(ftl, &need_format) != FTL_OK)
    {
        LOGE("[FTL ] CP media unreadable — refuse to format ch=%u ce=%u lun=%u\r\n", ch, ce, lun);
        return FTL_ERROR;
    }
#endif

    if (need_format)
    {
        LOGW("[FTL ] media blank / layout mismatch -> format\r\n");
        if (ftl_media_format(ftl, format_verify) != FTL_OK)
        {
            LOGE("[FTL ] media format failed ch=%u ce=%u lun=%u\r\n", ch, ce, lun);
            return FTL_ERROR;
        }
    }

    /* 池回填 / full metadata rebuild / 块元数据快照恢复全部在 ftl_boot_recovery 内完成：
     *   · 块元数据快照有效 ⇒ 直接按快照恢复 Free/OP/Spare（不再几何 carve）；
     *   · 无快照 / 快照损坏 ⇒ full metadata rebuild → carve。
     * ⚠ **full metadata rebuild 必须先于 carve**（full metadata rebuild 认领写过的块，carve 只把 state==NONE 的块入池）；
     *   carve 也**不能**在块元数据快照恢复之前做——否则池条目会重复。 */
    if (ftl_boot_recovery(ftl, &result) != FTL_OK)
    {
        LOGE("[FTL ] boot recovery failed ch=%u ce=%u lun=%u\r\n", ch, ce, lun);
        return FTL_ERROR;
    }

    LOGI("[FTL ] instance ch=%u ce=%u lun=%u free=%u op=%u spare=%u\r\n",
         ch,
         ce,
         lun,
         (unsigned)alloc->free_count,
         (unsigned)alc_get_op_total(alloc),
         (unsigned)alc_get_spare_total(alloc));

    LOGI("[FTL ] boot type=%d cp_seq=%lu journal_entries=%lu bit=%s\r\n",
         (int)result.boot_type,
         result.cp_sequence,
         result.journal_entries,
         result.scoped ? "scoped" : ((result.blocks_restored != 0U) ? "restored" : "fallback"));

    return 0;
}

/* 主机容量 = 编译期映射上限 − 实际坏块占用的 LBA。
 *
 * 本仓自拟的容量折中：主流量产 SSD 的容量在出厂固定，坏块由 OP 吸收、不在
 * 运行期缩容；本仓的二手片没有可信出厂坏块表，OP/Spare 也只会越用越少，
 * 容量口径（本仓自拟的动态折中；主流量产是固定容量 + OP 吸收坏块，见 RISK-CAP-03）：
 *   1. 每 plane 只统计**好**的、非固定元数据的块（`>= FTL_METADATA_AREA_END`）；
 *   2. 扣掉 OP 与 Spare 两个预留池的目标块数；
 *   3. **取 plane 最小值**再乘 PLANE_NUM —— Super Block 需要每 plane 一块，
 *      跨 plane 求和会超卖；
 *   4. **取实例最小值** —— LBA 按 region 路由，最紧的实例先耗尽，跨实例求和同样超卖。
 * 扣减粒度：每个物理块 = PAGES_PER_BLOCK × CW_NUM 个 512B LBA。
 * ⚠ 只在开机算一次：同一会话内 runtime 新增坏块由 Spare/OP 吸收，不改已枚举容量；
 *   下一次开机按当时的 BAD 集重算。
 */
static DWORD ssd_ftl_host_lba_max(void)
{
    DWORD cap_blocks = UINT32_MAX;

    for (BYTE ch = 0U; ch < CH_NUM; ch++)
    {
        for (BYTE ce = 0U; ce < CE_NUM; ce++)
        {
            for (BYTE lun = 0U; lun < LUN_NUM; lun++)
            {
                const ftl_ctx_t *ftl = g_ftl_ptrs[ch][ce][lun];

                if (ftl == NULL)
                {
                    continue;
                }

                DWORD plane_usable_min = UINT32_MAX;

                for (BYTE plane = 0U; plane < PLANE_NUM; plane++)
                {
                    DWORD good = 0U;

                    for (WORD block = FTL_METADATA_AREA_END; block < BLOCKS_PER_PLANE; block++)
                    {
                        const WORD g = ftl_gblock(plane, block);

                        if (!ftl_block_is_bad(&ftl->alloc.blocks[g]))
                        {
                            good++;
                        }
                    }

                    const DWORD reserved = FTL_OP_BLOCKS_PER_PLANE + FTL_SPARE_POOL_PER_PLANE;
                    const DWORD usable   = (good > reserved) ? (good - reserved) : 0U;

                    if (usable < plane_usable_min)
                    {
                        plane_usable_min = usable;
                    }
                }

                const DWORD instance_blocks = plane_usable_min * PLANE_NUM;

                LOGI("[CAP] ch=%u ce=%u lun=%u plane_usable_min=%lu instance_blocks=%lu\r\n",
                     (unsigned)ch,
                     (unsigned)ce,
                     (unsigned)lun,
                     plane_usable_min,
                     instance_blocks);

                if (instance_blocks < cap_blocks)
                {
                    cap_blocks = instance_blocks;
                }
            }
        }
    }

    if (cap_blocks == UINT32_MAX)
    {
        cap_blocks = 0U;
    }

    QWORD lba = (QWORD)cap_blocks * PAGES_PER_BLOCK * CW_NUM * FTL_DIM_TOTAL;

    if (lba > FTL_LBA_MAX)
    {
        lba = FTL_LBA_MAX;
    }
    if (lba == 0U)
    {
        lba = 1U; /* 极端坏块：保留至少 1 个 LBA，避免 READ CAPACITY 为 0 */
    }

    LOGI("[CAP] host_data_blocks=%lu lba_max=%lu (%luMiB)\r\n",
         cap_blocks,
         (unsigned long)lba,
         (unsigned long)((lba * FTL_IU_BYTES) / (1024UL * 1024UL)));

    return (DWORD)lba;
}

/* --------------------------------------------------------------------
 * ssd_ftl_log_status - 周期性 [STAT] 一行摘要（每 5 s）
 *
 *   把「计数类」信息从逐事件日志里挪出来：free/op/spare/bad、CP/JRN 水位、
 *   迁移状态、dangling/已退役块重读计数、当前 NAND timing mode。
 *   逐事件日志只保留首次故障与状态变化（见 docs/design/logging_design.md）。
 * -------------------------------------------------------------------- */
static void ssd_ftl_log_status(void)
{
    static DWORD last_ms;
    const DWORD  now = HAL_GetTick();

    if ((last_ms != 0U) && ((now - last_ms) < 5000U))
    {
        return;
    }
    last_ms = now;

    const BYTE mode = nand_get_timing_mode();

    for (BYTE ch = 0U; ch < CH_NUM; ch++)
    {
        for (BYTE ce = 0U; ce < CE_NUM; ce++)
        {
            for (BYTE lun = 0U; lun < LUN_NUM; lun++)
            {
                const ftl_ctx_t *ftl = g_ftl_ptrs[ch][ce][lun];

                if (ftl == NULL)
                {
                    continue;
                }

                LOGI("[STAT] ch=%u t=%lus mode=%u free=%u op=%u/%u spare=%u/%u bad=%u cp=%lu jrn=%lu wr=%lu mig=%u/%u dng=%lu rrab=%lu\r\n",
                     (unsigned)ch,
                     (unsigned long)(now / 1000U),
                     (unsigned)mode,
                     (unsigned)ftl->alloc.free_count,
                     (unsigned)alc_get_op_count(&ftl->alloc, 0U),
                     (unsigned)alc_get_op_count(&ftl->alloc, 1U),
                     (unsigned)alc_get_spare_count(&ftl->alloc, 0U),
                     (unsigned)alc_get_spare_count(&ftl->alloc, 1U),
                     (unsigned)ftl->alloc.bad_count,
                     ftl->cp_handle->cp_state.sequence,
                     (unsigned long)ftl->journal->handle.sequence_counter,
                     ftl->total_writes,
                     (unsigned)((ftl->mig_handle->active != 0U) ? 1U : 0U),
                     (unsigned)((ftl->mig_handle->paused != 0U) ? 1U : 0U),
                     ftl->dangling_total,
                     ftl->read_recovery_handle.already_bad_reads);
            }
        }
    }
}

/* 完整初始化 FTL：全实例 boot + controller 绑定（boot_ok 才置 g_ftl_ready）
 *
 *   ① 建 g_ftl_ptrs 三维指针表 + ct_set_route —— **必须早于逐实例 boot**，
 *      理由见函数内注释（boot 期 full metadata rebuild会经 l2p_read_sync 抽完成表）。
 *   ② 逐 (ch,ce,lun) boot —— 任一失败则 boot_ok=0，整体回退 fc_service。
 *   ③ ssd_controller_init(&g_ctrl, g_ftl_ptrs) —— 对外 controller 就绪。
 *   ④ g_ftl_ready = 1 —— 主循环切换到 ftl_service。
 */
static void ssd_ftl_boot_all(void)
{
    BYTE         ch;
    BYTE         ce;
    BYTE         lun;
    int          boot_ok = 1;
    ssd_status_e ctrl_ret;

    /* ① 全局完成旁表 + 路由表：清一次、填一次、设一次——**全部必须早于逐实例 boot**。
     *    - ct_init() 清的是**全局**资源（旁表 + 路由表），只做一次。
     *      ⚠ 它原在 ftl_init 里（逐实例调用）⇒ 实例 1 的 init 会抹掉实例 0 在飞的
     *      ctx 登记与已设好的路由表；已移到此处。
     *    - g_ftl_ptrs 只是对静态数组 g_ftl 取址，不依赖 ftl_init。
     *    - 为何路由不能等 boot 之后再设：`ssd_ftl_boot` 内部的 `ftl_boot_recovery`
     *      （full metadata rebuild）会经 `l2p_read_sync` → `l2p_sync_pump` → `ftl_service_pump()`
     *      抽完成表。此时若 route 仍为 NULL，`ftl_op_dispatch` 解析出的 ftl 为 NULL，
     *      服务函数只 release 不推进状态 ⇒ job 永久卡死
     *      （现象：[L2P] read_sync timeout + load_inflight 永久不减）。
     *    命令能在完成时拿到非 NULL 实例，前提是该实例的 ftl_init 已跑过——boot 循环
     *    逐实例推进，任一时刻在飞的命令只可能属于已 boot 的实例。 */
    ct_init();
    for (ch = 0U; ch < CH_NUM; ch++)
    {
        for (ce = 0U; ce < CE_NUM; ce++)
        {
            for (lun = 0U; lun < LUN_NUM; lun++)
            {
                g_ftl_ptrs[ch][ce][lun] = &g_ftl[ch][ce][lun];

                /* 预绑 controller 的实例指针：boot 失败时 ssd_controller_init 不会执行，
                 * 但 CLI 的 `bbt --erase` 仍需要实例才能擦除旧/坏介质。
                 * boot 成功后 ssd_controller_init 会重新校验并绑定同一组指针。 */
                g_ctrl.ftl[ch][ce][lun] = g_ftl_ptrs[ch][ce][lun];
            }
        }
    }
    ct_set_route(g_ftl_ptrs);

    /* ①.5 共享 L2P cache：每个 (ch,ce) 一份，**设备级只初始化一次**。
     * 必须早于逐实例 boot（ftl_boot_recovery 的 full metadata rebuild / journal 回放就会访问 cache）；
     * 且绝不能放进逐实例 ftl_init（第二个实例会清掉第一个实例的 line）。 */
    for (ch = 0U; ch < CH_NUM; ch++)
    {
        for (ce = 0U; ce < CE_NUM; ce++)
        {
            l2p_init(&g_l2p_cache[ch][ce], ch, ce, ssd_l2p_owner_of_region, NULL);
        }
    }

    /* ② 逐实例 boot */
    for (ch = 0U; ch < CH_NUM; ch++)
    {
        for (ce = 0U; ce < CE_NUM; ce++)
        {
            for (lun = 0U; lun < LUN_NUM; lun++)
            {
                if (ssd_ftl_boot(ch, ce, lun) != FTL_OK)
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
        ct_set_route(NULL);
        LOGE("[FTL ] boot failed, fallback to fc_service\r\n");
        return;
    }

    /* ②.5 路由 ↔ dim 的往返自检（owner 解析的根基）：
     * ftl_topo_dim(route(dim)) 必须等于 dim；公式漂移会让每次 cache 操作找不到 owner。
     * 必须在逐实例 ftl_init 之后做（topo 那时才有值），且 release 也拒绝启动
     * （H14 判据 9 的同类：静默错路由比停机更贵）。 */
    for (DWORD dim = 0U; dim < FTL_DIM_TOTAL; dim++)
    {
        const struct ftl_context *owner = ct_get_route_region(dim);

        if ((owner == NULL) || (ftl_topo_dim(owner->topo) != dim))
        {
            LOGE("[FTL ] route/dim mismatch dim=%lu\r\n", dim);
            ct_set_route(NULL);
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

    /* ③.5 主机可见容量：按实际坏块扣减（RISK-CAP-03，本仓自拟的容量折中）。
     * 必须在 USB 枚举之前设置——main 继续往下才初始化 USB 设备栈。 */
    g_ctrl.host_lba_max = ssd_ftl_host_lba_max();

    /* ④ 主循环切换点：此后走 ftl_service（内部先跑 fc_service） */
    g_ftl_ready = 1;
    /* %lu 而非 %llu：newlib-nano 不支持 long long 格式化，见 usart.c 的 _Static_assert。 */
    LOGI("[FTL ] controller ready, lba_max=%lu (%luMiB)\r\n",
         (unsigned long)g_ctrl.host_lba_max,
         (unsigned long)(((QWORD)g_ctrl.host_lba_max * FTL_IU_BYTES) / (1024UL * 1024UL)));
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
    BYTE nand_init_result;

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
     *   ⑥ nand_enable_high_speed() —— 全部初始化完成后才切 Mode 5；失败保持低速。
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

            /* ⑥ 高速模式：只有初始化（NAND + FTL boot/恢复）全部完成后才切换，
             *    保证上电路径始终跑在保守低速时序。失败仅告警，保持低速可用。 */
            if (g_ftl_ready != 0)
            {
                sys_clock_tree_t clk;

                sys_clock_tree_get(&clk);

                const BYTE high_speed_result = nand_enable_high_speed(clk.fmc_hz);

                if (high_speed_result != NAND_RESULT_OK)
                {
                    LOGW("[NAND] high-speed enable failed: 0x%02X — keep low speed\r\n",
                         (unsigned)high_speed_result);
                }
            }
        }
    }

    /* USB device：MSC 前端 + TinyUSB 协议栈。放在 FTL boot 之后 —— 主机枚举
     * 后第一条 TEST UNIT READY 就能反映 controller 是否 READY；boot 失败时
     * 设备仍然枚举，但 USB MSC 块设备不可用（而不是整块板子不再出现 USB 设备）。 */
    boot_section("usb");
    usb_msc_init();
    if (!usb_device_init())
    {
        LOGE("usb device init failed\r\n");
    }
    else
    {
        LOGI("[USB ] device stack ready (USB2_OTG_FS, full speed)\r\n");
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
     *                       journal / checkpoint / bbt / GC 永不推进（序 4）。
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
            BYTE ch;
            BYTE ce;
            BYTE lun;

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

            /* 计数类信息每 5 s 一行，替代逐事件刷屏。 */
            ssd_ftl_log_status();
        }
        else
        {
            (void)fc_service();
        }

        /* Controller 自己的异步命令（当前只有 FLUSH）：每轮推进 + 完成回执。
         * 必须在 USB 任务之前——USB 前端同一轮就能看到 complete_flag。 */
        ssd_controller_service(&g_ctrl);

        /* USB：先让 MSC 前端检查在飞请求（提交/回填），再让协议栈处理事件
         * （tud_msc_async_io_done 会在这里把数据/CSW 发出去）。两者都非阻塞。 */
        usb_msc_task();
        usb_device_task();

        btim_scheduler_poll();
    }
}
