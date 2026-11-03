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
 *                       usb_msc_task; usb_device_task; timer_scheduler_poll; }
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
#include "cli.h"
#include "controller.h"
#include "delay.h"
#include "fc.h"
#include "ftl.h"
#include "ftl_op.h"
#include "log.h"
#include "metadata.h"
#include "power_failure_recovery.h"
#include "stack_guard.h"
#include "timer.h"
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
    LOGI("[SYS ] CPU=%luMHz HCLK=%luMHz ICache=%s DCache=%s MPU=%lu\r\n", (delay_port_core_clock_hz() / 1000000U), (delay_port_hclk_hz() / 1000000U), (get_icache_sta() != 0U) ? "on" : "off", (get_dcache_sta() != 0U) ? "on" : "off", sys_mpu_region_count());
    /* 时钟树：APB1/2/3/4 相等时压成一个 pclk，不等则四个全列。 */
    sys_clock_tree_get(&clk);
    if ((clk.pclk1_hz == clk.pclk2_hz) && (clk.pclk2_hz == clk.pclk3_hz) && (clk.pclk3_hz == clk.pclk4_hz))
    {
        LOGI("[SYS ] clk sys=%lu hclk=%lu pclk=%lu fmc=%lu qspi=%lu uart=%lu\r\n", (clk.sysclk_hz / 1000000U), (clk.hclk_hz / 1000000U), (clk.pclk1_hz / 1000000U), (clk.fmc_hz / 1000000U), (clk.qspi_hz / 1000000U), (clk.uart_hz / 1000000U));
    }
    else
    {
        LOGI("[SYS ] clk sys=%lu hclk=%lu pclk1/2/3/4=%lu/%lu/%lu/%lu fmc=%lu qspi=%lu uart=%lu\r\n", (clk.sysclk_hz / 1000000U), (clk.hclk_hz / 1000000U), (clk.pclk1_hz / 1000000U), (clk.pclk2_hz / 1000000U), (clk.pclk3_hz / 1000000U), (clk.pclk4_hz / 1000000U), (clk.fmc_hz / 1000000U), (clk.qspi_hz / 1000000U), (clk.uart_hz / 1000000U));
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
 *   每张表一个对象 ⇒ 可以单独换内存域。下面的 _Static_assert 只核对本文件可见的
 *   FTL 对象；FC 命令池等跨文件 DTCM 对象的完整容量由 linker 的 `.dtcm_data` ASSERT 核对。
 *
 * SRAM 布局（阶段 C：描述符 + 内存域散列）：
 *   DTCM（.dtcm_data）：g_ftl 描述符 + g_ftl_wbuf + g_ftl_journal
 *                       + FC 命令池 + NAND dispatch 队列 + BCH 静态表
 *                       （CPU-only 热数据；DMA 不可访问，故 FC buffer/
 *                       NAND page buffer 不进来）
 *   AXI_RAM（.bss）：g_ftl_migrate / g_ftl_rr_page_scratch（装配挂载块）
 *                    + g_l2p_cache（共享热 cache）+ CLI 写缓冲 + 其它 .bss
 *   AXI_NOCACHE（.noncacheable_buffer）：FC buffer pool / return staging /
 *                    NAND 页缓冲（页缓冲放 DTCM 会破坏 multi-plane program，
 *                    板端已实测）
 *   SRAM1+SRAM2（.sram12_data）：g_ftl_blocks / g_ftl_free_blocks / g_ftl_op_blocks /
 *                                g_ftl_spare_blocks / g_ftl_closed_sb_list（各自独立对象）
 *   SRAM3（.sram3_data）：g_ftl_checkpoint_handle[]
 *   SRAM4（.sram4_data）：g_ftl_md 33,752 B（与 BCH 交换；TCM 之前本就在此域）
 *
 * `.dtcm_data` / `.sram*_data` 都是 **NOLOAD**（启动不清零）⇒ ftl_init 显式 memset。
 * 装配挂载块：wbuf 移 DTCM，migrate/rr 仍在 AXI .bss；ftl_init 统一 memset。
 * 单 die：`.dtcm_data` 67,832B / ITCM 61,296B（release）、62,064B（debug）；
 * BCH 静态表在 DTCM，NAND 页缓冲在 AXI_NOCACHE。release 额外模块按对象路径
 * `release/obj/` 匹配进 ITCM，debug 不匹配（见 linker/stm32h743xx_flash.ld）。
 * 完整容量由 linker ASSERT 核对。栈预留按历史实测 5,448B + 余量取 20KB。
 * ══════════════════════════════════════════════════════════════════ */
/* DTCM 热数据定义在 ftl.c；链接脚本按整个目标文件放 .dtcm_data。 */
extern ftl_ctx_t g_ftl[FTL_DIE_MAX];

/* 共享 L2P cache：同一 (ch,ce) 下所有 LUN 共用一份（阶段 B）。
 * 设备级初始化一次，由 ssd_l2p_owner_of_region 把全局 region 解析到 owner LUN。 */
static l2p_cache_t g_l2p_cache[CH_NUM][CE_NUM];

/* per-instance 大数组（阶段 C）：由 ftl_storage_t 绑定到描述符。
 * 五张表**各自独立定义**，每张可单独换 section/内存域；当前全部落 SRAM12，
 * 将来要挪某一张只改该行的 section 宏，不动别的表。 */
static bst_block_t    g_ftl_blocks[FTL_DIE_MAX][FTL_BLOCK_TOTAL] SRAM12_DATA;
static WORD           g_ftl_free_blocks[FTL_DIE_MAX][FTL_BLOCK_TOTAL] SRAM12_DATA;
static WORD           g_ftl_op_blocks[FTL_DIE_MAX][PLANE_NUM][FTL_OP_POOL_CAP] SRAM12_DATA;
static WORD           g_ftl_spare_blocks[FTL_DIE_MAX][PLANE_NUM][FTL_SPARE_POOL_PER_PLANE] SRAM12_DATA;
static super_block_t  g_ftl_closed_sb_list[FTL_DIE_MAX][SUPER_BLOCK_LIST_MAX] SRAM12_DATA;
static cp_handle_t    g_ftl_checkpoint_handle[FTL_DIE_MAX] SRAM3_DATA;
extern l2p_jrn_t      g_ftl_journal[FTL_DIE_MAX];
extern md_dir_cache_t g_ftl_md[FTL_DIE_MAX];

/* 装配挂载：三块大状态挂 storage 指针；wbuf 落 DTCM，migrate/rr 留 AXI。
 * 挂指针**不减少总 RAM**，只让 ftl_ctx_t 变小、放置可控、handle 形态统一。 */
static mig_handle_t g_ftl_migrate[FTL_DIE_MAX];
extern wb_handle_t  g_ftl_wbuf[FTL_DIE_MAX];
static BYTE         g_ftl_rr_page_scratch[FTL_DIE_MAX][FTL_READ_USER_SIZE];

ssd_controller_t g_ctrl; /* controller 路由表（按 compact die 号收 g_ftl 指针）；非 static：
                          * UART CLI（cli.c）经 extern 引用，见 controller/cli.h */
static int g_ftl_ready;  /* 主循环守卫：全部实例 boot 成功才切 ftl_service */

/* ── 编译期内存预算（各域）──
 * AXI_RAM 是硬上限（linker: __AXI_RAM_SIZE = 0x60000 = 384KB）；
 * D2/D3 SRAM 的容量与 linker 的 SRAM12/SRAM3/SRAM4 区域一一对应。 */
#define BOARD_AXI_RAM_SIZE       (384U * 1024U) /* 对齐 __AXI_RAM_SIZE */
#define BOARD_SRAM12_SIZE        (256U * 1024U) /* SRAM1+SRAM2 合并区域 */
#define BOARD_SRAM3_SIZE         (32U * 1024U)
#define BOARD_SRAM4_SIZE         (64U * 1024U)
#define BOARD_DTCM_SIZE          (128U * 1024U)
#define BOARD_DTCM_STACK_RESERVE (20U * 1024U) /* 历史实测 5,448 B（v0.75）；当前版本待复测 */
#define FTL_AXI_RAM_RESERVED     (20U * 1024U) /* 非 FTL 自有对象的保守 AXI .bss 预留 */

/* g_wr_buf 从 .sram2_data 迁入 AXI .bss（SRAM12 留给 tables），显式计入预算。 */
#define CLI_AXI_RAM_RESERVED (16U * 1024U)

_Static_assert(sizeof(g_ftl_migrate) + sizeof(g_ftl_rr_page_scratch) + sizeof(g_l2p_cache) + FTL_AXI_RAM_RESERVED + CLI_AXI_RAM_RESERVED <= BOARD_AXI_RAM_SIZE, "FTL 挂载块/共享 cache/CLI 缓冲超出 AXI_RAM");
/* FTL 自有 DTCM 对象；fc.o / fc_bch_engine.o / nand_dispatch.o 的 DTCM 对象在
 * 其它 TU，其总量不在此断言内，由 linker 的
 * `.dtcm_data + .noinit + 20KB <= DTCM` 兜底。 */
_Static_assert(sizeof(g_ftl) + sizeof(g_ftl_wbuf) + sizeof(g_ftl_journal) + BOARD_DTCM_STACK_RESERVE <= BOARD_DTCM_SIZE, "DTCM 热数据 + stack/heap 预留超出 DTCM");
_Static_assert(sizeof(g_ftl_md) <= BOARD_SRAM4_SIZE, "metadata_dir 超出 SRAM4");
_Static_assert(sizeof(g_ftl_blocks) + sizeof(g_ftl_free_blocks) + sizeof(g_ftl_op_blocks) + sizeof(g_ftl_spare_blocks) + sizeof(g_ftl_closed_sb_list) <= BOARD_SRAM12_SIZE, "FTL tables 超出 SRAM12（SRAM1+SRAM2）");
_Static_assert(sizeof(g_ftl_checkpoint_handle) <= BOARD_SRAM3_SIZE, "cp_handle 超出 SRAM3");

/* die → 实例指针表（类型匹配 ssd_controller_init / ftl_op_route_set 形参）。
 * 编译器不认可 static 数组直接退化为匹配指针数组参数（GCC 10.3 起
 * -Wincompatible-pointer-types 恒报），此处显式构造。 */
static ftl_ctx_t *g_ftl_ptrs[FTL_DIE_MAX];

/* die → FTL 实例：g_ftl 与路由表都按 compact die 号索引。 */
static ftl_ctx_t *ssd_ftl_die_get(BYTE die)
{
    ASSERT(die < DIE_MAX);
    return &g_ftl[die];
}

static ftl_topo_t ssd_ftl_die_topo(BYTE die)
{
    ftl_topo_t topo;

    topo.ch  = die_2_ch(die);
    topo.ce  = die_2_ce(die);
    topo.lun = die_2_lun(die);

    return topo;
}

/* 共享 L2P cache 的 owner 端口表（docs §4.7）：每 die 一份，只暴露
 * {die, metadata_dir, alloc, topo}，不把整个 ftl_context 交给 l2p_cache。
 * 由 ssd_ftl_boot 在 ftl_init 之后填充；生命周期覆盖全程。 */
static l2p_owner_t g_l2p_owner[FTL_DIE_MAX];

/*
 * ssd_l2p_owner_of_region - 全局 L2P region → owner FTL 实例
 *
 * 共享 cache 的 load/flush/commit 用它找 owner die。die 坐标按
 * global_region % DIE_MAX 计算（与 lba_2_die 同一公式，避免第二套坐标）。
 */
static const l2p_owner_t *ssd_l2p_owner_of_region(void *ctx, DWORD global_region)
{
    (void)ctx;

    ASSERT(DIE_MAX != 0U);

    const BYTE die = (BYTE)(global_region % DIE_MAX);

    ASSERT(die < DIE_MAX);
    ASSERT(g_l2p_owner[die].metadata_dir != NULL);

    return &g_l2p_owner[die];
}

/*
 * ssd_ftl_boot - 初始化单个 die 的 FTL 实例
 *
 * 顺序：ftl_init（内部含 bbt_init 标坏块）→ 遍历 BST 非坏块回填 free pool
 *       （alc_init 不填 free pool、bbt 只标坏块——不回填则首次写失败）
 *       → pfr_boot_recovery → 日志。
 *
 * @param die 紧凑 die 号（g_die_table 给出对应 (ch,ce,lun)）
 * @return 0 成功，负值失败
 */
static ftl_status_e ssd_ftl_boot(BYTE die)
{
    ASSERT(die < DIE_MAX);

    ftl_ctx_t        *ftl = &g_ftl[die];
    alc_t            *alloc;
    pfr_boot_result_t result;

    ftl_topo_t topo;
    topo.ch  = die_2_ch(die);
    topo.ce  = die_2_ce(die);
    topo.lun = die_2_lun(die);

    const BYTE ch  = topo.ch;
    const BYTE ce  = topo.ce;
    const BYTE lun = topo.lun;

    ftl_table_refs_t refs;
    refs.blocks         = g_ftl_blocks[die];
    refs.free_blocks    = g_ftl_free_blocks[die];
    refs.op_blocks      = g_ftl_op_blocks[die];
    refs.spare_blocks   = g_ftl_spare_blocks[die];
    refs.closed_sb_list = g_ftl_closed_sb_list[die];

    ftl_storage_t storage;
    storage.table_refs      = &refs;
    storage.metadata_dir    = &g_ftl_md[die];
    storage.journal         = &g_ftl_journal[die];
    storage.cp_handle       = &g_ftl_checkpoint_handle[die];
    storage.migrate         = &g_ftl_migrate[die];
    storage.wbuf            = &g_ftl_wbuf[die];
    storage.rr_page_scratch = g_ftl_rr_page_scratch[die];

    if (ftl_init(ftl, topo, &g_l2p_cache[ch][ce], &storage) != FTL_OK)
    {
        LOGE("[FTL ] init failed ch=%u ce=%u lun=%u\r\n", ch, ce, lun);
        return FTL_ERROR;
    }

    alloc = &ftl->alloc;

    /* L2P owner 端口：必须在任何 cache 访问（含下面的格式化/恢复）之前就绪。 */
    l2p_owner_t owner;

    owner.die          = die;
    owner.metadata_dir = ftl->metadata_dir;
    owner.alloc        = &ftl->alloc;
    owner.topo         = ftl->topo;
    owner.journal      = ftl->journal; /* WAL 水位闸门（RISK-L2P-09） */
    g_l2p_owner[die]   = owner;

    /* debug-only：初始化契约自检。必须在 owner 登记之后、任何 cache 访问之前——
     * 校验全部句柄/端口绑定、topology 副本与 owner/route 身份。
     * release 下连调用都不生成，保证零运行期开销。 */
#if !defined(NDEBUG)
    ftl_contract_audit(ftl);
#endif

    /* 坏块落点统计（开机基线，**早于格式判定**）：boot 可能因介质读不通而中止，
     * 先打一行保证失败时也能看到 BAD 分布。 */
    pfr_boot_log_bad_areas(ftl);

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
    if (pfr_media_check_format(ftl, &need_format) != FTL_OK)
    {
        LOGE("[FTL ] CP media unreadable — refuse to format ch=%u ce=%u lun=%u\r\n", ch, ce, lun);
        return FTL_ERROR;
    }
#endif

    if (need_format)
    {
        LOGW("[FTL ] media blank / layout mismatch -> format\r\n");
        if (pfr_media_format(ftl, format_verify) != FTL_OK)
        {
            LOGE("[FTL ] media format failed ch=%u ce=%u lun=%u\r\n", ch, ce, lun);
            return FTL_ERROR;
        }
    }

    /* 池回填 / full metadata rebuild / 块元数据快照恢复全部在 pfr_boot_recovery 内完成：
     *   · 块元数据快照有效 ⇒ 直接按快照恢复 Free/OP/Spare（不再几何 carve）；
     *   · 无快照 / 快照损坏 ⇒ full metadata rebuild → carve。
     * ⚠ **full metadata rebuild 必须先于 carve**（full metadata rebuild 认领写过的块，carve 只把 state==NONE 的块入池）；
     *   carve 也**不能**在块元数据快照恢复之前做——否则池条目会重复。 */
    if (pfr_boot_recovery(ftl, &result) != FTL_OK)
    {
        LOGE("[FTL ] boot recovery failed ch=%u ce=%u lun=%u\r\n", ch, ce, lun);
        return FTL_ERROR;
    }

    LOGI("[FTL ] instance ch=%hhu ce=%hhu lun=%hhu free=%hu op=%hu spare=%hu\r\n", ch, ce, lun, alloc->free_count, alc_pool_sum_counts(alloc->op_count, PLANE_NUM), alc_pool_sum_counts(alloc->spare_count, PLANE_NUM));

    LOGI("[FTL ] boot type=%d cp_seq=%lu journal_entries=%lu bit=%s\r\n", (int)result.boot_type, result.cp_sequence, result.journal_entries, result.scoped ? "scoped" : ((result.blocks_restored != 0U) ? "restored" : "fallback"));

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

    for (BYTE die = 0U; die < DIE_MAX; die++)
    {
        const ftl_ctx_t *ftl              = ssd_ftl_die_get(die);
        const ftl_topo_t topo             = ssd_ftl_die_topo(die);
        DWORD            plane_usable_min = UINT32_MAX;

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

        LOGI(LOG_TAG_CAP "ch=%hhu ce=%hhu lun=%hhu plane_usable_min=%lu instance_blocks=%lu\r\n", topo.ch, topo.ce, topo.lun, plane_usable_min, instance_blocks);

        if (instance_blocks < cap_blocks)
        {
            cap_blocks = instance_blocks;
        }
    }

    if (cap_blocks == UINT32_MAX)
    {
        cap_blocks = 0U;
    }

    QWORD lba = (QWORD)cap_blocks * PAGES_PER_BLOCK * CW_NUM * DIE_MAX;

    if (lba > FTL_LBA_MAX)
    {
        lba = FTL_LBA_MAX;
    }
    if (lba == 0U)
    {
        lba = 1U; /* 极端坏块：保留至少 1 个 LBA，避免 READ CAPACITY 为 0 */
    }

    LOGI(LOG_TAG_CAP "host_data_blocks=%lu lba_max=%llu (%lluMiB)\r\n", cap_blocks, lba, (lba * FTL_IU_BYTES) / (1024UL * 1024UL));

    return (DWORD)lba;
}

/* --------------------------------------------------------------------
 * ssd_ftl_stat_task - 周期性 [STAT] 一行摘要（timer 表每 10 s 调用）
 *
 *   只保留块分配/坏块/搬移状态；op/spare/cp/jrn/wr/dng/rrab/mode 等低频计数
 *   改由 `info` / `diag` 按需查看，避免长期占用 UART。
 *   逐事件日志只保留首次故障与状态变化（见 docs/design/logging_design.md）。
 * -------------------------------------------------------------------- */
static void ssd_ftl_stat_task(void *context, DWORD now_ms)
{
    (void)context;
    (void)now_ms; /* TIM6 只负责调度；t= 是系统时间，读 SysTick。 */

    if (g_ftl_ready == 0)
    {
        return;
    }

    const DWORD uptime_ms = delay_now_ms();

    for (BYTE die = 0U; die < DIE_MAX; die++)
    {
        const ftl_ctx_t *ftl  = ssd_ftl_die_get(die);
        const ftl_topo_t topo = ssd_ftl_die_topo(die);

        LOGI(LOG_TAG_STAT "ch=%hhu t=%lus free=%hu bad=%hu mig=%u/%u mv=%lu nop=%lu\r\n", topo.ch, (uptime_ms / 1000U), ftl->alloc.free_count, ftl->alloc.bad_count, ((ftl->mig_handle->active != 0U) ? 1U : 0U), ((ftl->mig_handle->paused != 0U) ? 1U : 0U), ftl->mig_handle->moved_count, ftl->mig_handle->noop_count);
    }
}

/*
 * 后台周期节拍：只置 flag / 补充配额，不跑业务。
 *   - L2P 写回节拍（L2P_FLUSH_INTERVAL_MS）
 *   - CP T3 到期判定（10 s，SysTick 比较；到期只置 cp->t3_due）
 *   - BBT 起新 job 节拍（BBT_FLUSH_INTERVAL_MS）
 * 真正的执行仍由 bg_scheduler 在 ftl_service 的固定位置完成；must-run 与在飞推进
 * 不受这些 flag 影响。
 */
_Static_assert(BG_TOKEN_INTERVAL_MS == L2P_FLUSH_INTERVAL_MS, "后台节拍与 token refill 周期必须一致");

static void ssd_ftl_bg_tick_task(void *context, DWORD now_ms)
{
    (void)context;
    (void)now_ms; /* TIM6 只负责唤醒；时间比较读 SysTick。 */

    if (g_ftl_ready == 0U)
    {
        return;
    }

    static DWORD s_bbt_last_ms;
    const DWORD  now     = delay_now_ms();
    const bool   bbt_due = (s_bbt_last_ms == 0U) || ((now - s_bbt_last_ms) >= BBT_FLUSH_INTERVAL_MS);

    if (bbt_due != false)
    {
        s_bbt_last_ms = now;
    }

    for (BYTE die = 0U; die < DIE_MAX; die++)
    {
        ftl_ctx_t *ftl = ssd_ftl_die_get(die);

        bg_scheduler_refill(&ftl->bg);
        ftl->l2p_flush_due = 1U;
        cp_timer_poll(ftl->cp_handle);

        if (bbt_due != false)
        {
            ftl->alloc.bbt.flush_due = 1U;
        }
    }
}

/* 完整初始化 FTL：全实例 boot + controller 绑定（boot_ok 才置 g_ftl_ready）
 *
 *   ① 建 g_ftl_ptrs die 指针表 + ftl_op_route_set —— **必须早于逐实例 boot**，
 *      理由见函数内注释（boot 期 full metadata rebuild会经 l2p_read_sync 抽完成表）。
 *   ② 逐 die boot —— 任一失败则 boot_ok=0，整体回退 fc_service。
 *   ③ ssd_controller_init(&g_ctrl, g_ftl_ptrs) —— 对外 controller 就绪。
 *   ④ g_ftl_ready = 1 —— 主循环切换到 ftl_service。
 */
static void ssd_ftl_boot_all(void)
{
    BYTE         ch;
    BYTE         ce;
    int          boot_ok = 1;
    ssd_status_e ctrl_ret;

    /* ① 全局完成旁表 + 路由表：清一次、填一次、设一次——**全部必须早于逐实例 boot**。
     *    - ftl_op_init() 清的是**全局**资源（旁表 + 路由表），只做一次。
     *      ⚠ 它原在 ftl_init 里（逐实例调用）⇒ 实例 1 的 init 会抹掉实例 0 在飞的
     *      ctx 登记与已设好的路由表；已移到此处。
     *    - g_ftl_ptrs 只是对静态数组 g_ftl 取址，不依赖 ftl_init。
     *    - 为何路由不能等 boot 之后再设：`ssd_ftl_boot` 内部的 `pfr_boot_recovery`
     *      （full metadata rebuild）会经 `l2p_read_sync` → `l2p_sync_pump` → `ftl_service_pump()`
     *      抽完成表。此时若 route 仍为 NULL，`ftl_op_dispatch` 解析出的 ftl 为 NULL，
     *      服务函数只 release 不推进状态 ⇒ job 永久卡死
     *      （现象：[L2P] read_sync timeout + load_inflight 永久不减）。
     *    命令能在完成时拿到非 NULL 实例，前提是该实例的 ftl_init 已跑过——boot 循环
     *    逐实例推进，任一时刻在飞的命令只可能属于已 boot 的实例。 */
    /* ① 拓扑建表：板级扫描结果（g_nand_targets，armed 之前已填）→ 紧凑 die 表。
     * 必须在逐实例 boot 之前；纯 RAM，不碰硬件。
     * Phase 2b：L2P/容量按运行时 DIE_MAX；稀疏拓扑允许部分 (ch,ce,lun) 槽为 NULL。 */
    if (!ftl_topology_build(g_nand_targets))
    {
        LOGE("[FTL ] topology build failed\r\n");
        return;
    }

    ftl_global_init();
    ftl_op_init();
    memset(g_ftl_ptrs, 0, sizeof(g_ftl_ptrs)); /* 稀疏拓扑：缺失 die 保持 NULL */
    for (BYTE die = 0U; die < DIE_MAX; die++)
    {
        ftl_ctx_t *ftl = &g_ftl[die];

        g_ftl_ptrs[die] = ftl;

        /* 预绑 controller 的实例指针：boot 失败时 ssd_controller_init 不会执行，
         * 但 CLI 的 `bbt --erase` 仍需要实例才能擦除旧/坏介质。
         * boot 成功后 ssd_controller_init 会重新校验并绑定同一组指针。 */
        g_ctrl.ftl[die] = ftl;
    }
    ftl_op_route_set(g_ftl_ptrs);

    /* ①.5 共享 L2P cache：每个 (ch,ce) 一份，**设备级只初始化一次**。
     * 必须早于逐实例 boot（pfr_boot_recovery 的 full metadata rebuild / journal 回放就会访问 cache）；
     * 且绝不能放进逐实例 ftl_init（第二个实例会清掉第一个实例的 line）。 */
    for (ch = 0U; ch < CH_NUM; ch++)
    {
        for (ce = 0U; ce < CE_NUM; ce++)
        {
            l2p_init(&g_l2p_cache[ch][ce], ch, ce, ssd_l2p_owner_of_region, NULL);
        }
    }

    /* ② 逐 die boot */
    for (BYTE die = 0U; die < DIE_MAX; die++)
    {
        if (ssd_ftl_boot(die) != FTL_OK)
        {
            boot_ok = 0;
        }
    }

    if (!boot_ok)
    {
        /* 实例未必 init 完成 ⇒ 撤销路由，恢复「路由未设」语义（此时主循环退回
         * fc_service，无人 drain 完成表，派发不会发生）。 */
        ftl_op_route_set(NULL);
        LOGE("[FTL ] boot failed, fallback to fc_service\r\n");
        return;
    }

    /* ②.5 路由 ↔ die 的往返自检（owner 解析的根基）：
     * topo_2_die(route(die)) 必须等于 die；公式漂移会让每次 cache 操作找不到 owner。
     * 必须在逐实例 ftl_init 之后做（topo 那时才有值），且 release 也拒绝启动
     * （H14 判据 9 的同类：静默错路由比停机更贵）。 */
    for (BYTE die = 0U; die < DIE_MAX; die++)
    {
        const ftl_ctx_t *owner = ftl_op_route_get_die(die);

        if ((owner == NULL) || (topo_2_die(owner->topo) != die))
        {
            LOGE("[FTL ] route/die mismatch die=%hhu\r\n", die);
            ftl_op_route_set(NULL);
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
    LOGI("[FTL ] controller ready, lba_max=%lu (%lluMiB)\r\n", g_ctrl.host_lba_max, ((QWORD)g_ctrl.host_lba_max * FTL_IU_BYTES) / (1024UL * 1024UL));
}

/* newlib calls this hook from __libc_init_array when no CRT provides it. */
void _init(void)
{
}

/*
 * STM32H743 烧录自检入口。
 * Reset_Handler 已完成 C 运行时初始化；HAL_Init() 配置 SysTick 为 1 ms 节拍。
 * 项目毫秒/微秒时基由 ARM 核心 SysTick 提供；TIM6 只驱动 timer 任务表的调度
 * 心跳（sys_init 配置完时钟树后立即启动）。
 */

int main(void)
{
    BYTE nand_init_result;

    sys_init();

    /*
     * TIM6 是 timer 调度器的毫秒心跳（watchdog / [STAT] / FTL 周期评估），
     * 必须在任务注册与首次 poll 之前运行。系统毫秒时基由 SysTick/HAL tick
     * 提供，与 TIM6 无关；TIM6 晚启动只会推迟调度器任务的首次到期。
     * APB1=100 MHz 且 APB1 分频=2 ⇒ TIM6 内核时钟 200 MHz；
     * arr=1000-1、psc=200-1 ⇒ 1 ms 周期中断。
     */
    timer_timx_int_init(1000 - 1, 200 - 1);

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
                    LOGW(LOG_TAG_NAND "high-speed enable failed: 0x%02hhX — keep low speed\r\n", high_speed_result);
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

    boot_report_console();

    /* 任务表与 watchdog 是否启用无关：[STAT] 等平台任务也要注册。 */
    timer_scheduler_init();
#if (SSD_WATCHDOG_ENABLE != 0U)
    if (!storage_wdg_init(&g_storage_wdg_config))
    {
        LOGE("watchdog supervisor init failed\r\n");
    }
    else
    {
        LOGI("[SYS ] timer task table ready, IWDG nominal=30s, period=100ms\r\n");
    }
#else
    LOGI("[SYS ] timer task table ready, IWDG disabled for debug\r\n");
#endif

    DWORD stat_task_id;
    if (!timer_scheduler_add(ssd_ftl_stat_task, NULL, 10000U, 10000U, &stat_task_id))
    {
        LOGE("[SYS ] stat task add failed\r\n");
    }

    /* 后台周期节拍：L2P 写回 / CP T3 / BBT 起新 job 的 flag，10 ms 一次。 */
    DWORD bg_tick_task_id;
    if (!timer_scheduler_add(ssd_ftl_bg_tick_task, NULL, 0U, L2P_FLUSH_INTERVAL_MS, &bg_tick_task_id))
    {
        LOGE("[SYS ] bg tick task add failed\r\n");
    }

    /* FTL 周期求值（GC/WL/predict）：每实例一个 timer 任务，ctx = mig_handle。 */
    if (g_ftl_ready != 0)
    {
        for (BYTE die = 0U; die < DIE_MAX; die++)
        {
            ftl_ctx_t *ftl = ssd_ftl_die_get(die);

            if (!mig_timer_register(ftl->mig_handle))
            {
                const ftl_topo_t topo = ssd_ftl_die_topo(die);

                LOGE("[SYS ] migrate timer register failed ch=%hhu ce=%hhu lun=%hhu\r\n", topo.ch, topo.ce, topo.lun);
            }
        }
    }

    cli_service_poll();
    /* ── 主循环（裸机 superloop，无中断参与）──
     *   g_ftl_ready == 0：只跑 fc_service()，保持 FC/NAND 裸路径活着（可跑 CLI 自检命令）。
     *   g_ftl_ready == 1：ftl_service_pump() 一次（全局 NAND 泵 + 抽干完成表），
     *                     再对**每个**实例调 ftl_service()（host 补发 / 后台调度）。
     *                     ⚠ 必须遍历全部实例：只跑 [0][0][0] 会让其余实例的
     *                       journal / checkpoint / bbt / GC 永不推进（序 4）。
     *   cli_service_poll()    —— CLI 解析（含 `test` 单元测试入口）。
     *   timer_scheduler_poll() —— 1ms 任务表（看门狗喂狗等）。
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
            /* ① 全局泵：NAND 推进 + 完成表抽干（实例无关，每轮一次） */
            ftl_service_pump();

            /* ② 逐 die 后台：host 补发 + 后台调度 */
            for (BYTE die = 0U; die < DIE_MAX; die++)
            {
                ftl_service(ssd_ftl_die_get(die));
            }
        }
        else
        {
            (void)fc_service();
        }

        /* Controller 自己的异步命令（FLUSH / TRIM）：每轮推进 + 完成回执。
         * 必须在 USB 任务之前——USB 前端同一轮就能看到 complete_flag。 */
        ssd_controller_service(&g_ctrl);

        /* USB：先让 MSC 前端检查在飞请求（提交/回填），再让协议栈处理事件
         * （tud_msc_async_io_done 会在这里把数据/CSW 发出去）。两者都非阻塞。 */
        usb_msc_task();
        usb_device_task();

        timer_scheduler_poll();
    }
}
