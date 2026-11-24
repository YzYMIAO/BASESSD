/* ======================================================================
 * main.c — 启动入口与裸机主循环
 *
 * 职责：全系统初始化顺序编排 + superloop 驱动源。
 * 调用链：
 *   main() → pc_sample_init → sys_init → timer_timx_int_init → cli_init
 *          → log sink 绑定 → stack_guard_init → 上电报告
 *          → fc_init → fc_nand_init → fc_boot_arm → platform_register
 *          → ssd_ftl_boot_all（含 ssd_ftl_boot / ssd_controller_init）
 *          → usb_msc_init → usb_device_init → timer_scheduler_init
 *          → ftl_timer_tasks_start
 *          → while(1) { stack_guard_check; cli_service_poll; ssd_runtime_pump; }
 * 关键约定：
 *   - 初始化是**严格 if/else-if 级联**：前一步失败会整段跳过后面的，
 *     因此 fc_init 失败时 NAND 根本不会被初始化。
 *   - FC/NAND/FTL 的推进**全部在非 ISR 上下文**：没有任何中断调用
 *     fc_service / ftl_service，superloop 是唯一驱动源。
 *   - NAND 启动命令（RESET/READ_ID）在 fc_nand_init 内绕过命令队列，
 *     直连 FC 的同步 UOP 引擎。
 * 生命周期：ssd_ftl_boot_all 成功（controller 进入 READY）后，主循环由
 *          fc_service 切换到 ftl_service（后者内部会先跑 fc_service）。
 * 文件结构：日志 sink 绑定 → FTL 实例存储 → ssd_ftl_boot(_all) → main()。
 * 流程图见 docs/design/fc_nand_design.md §22（图 15）/ §23（图 18）。
 * ====================================================================== */

#include "cli.h"
#include "controller.h"
#include "delay.h"
#include "fault.h"
#include "fc_interface.h"
#include "ftl_interface.h"
#include "lifetime.h" /* 终身计数：boot_count / PFR / 上电起点 */
#include "log.h"
#include "pc_sample.h" /* 上一轮 PC 采样环的启动快照与提示 */
#include "platform.h"
#include "runtime.h"
#include "stack_guard.h"
#include "stats.h"
#include "stm32h7xx_hal.h"
#include "sys.h"
#include "timer.h"
#include "usart.h"
#include "usb.h"
#include "usb_msc.h"
#include "watchdog.h"

#include <string.h>

/* 全部构建档当前均为 0：健康域尚未覆盖所有最长路径。长同步操作已有
 * 显式喂狗点 storage_wdg_kick()（ftl_sync_submit_wait 的自旋泵），启用前仍需上板
 * 验证满盘 GC / format 等路径不会触发 30 s 复位。 */
#define SSD_WATCHDOG_ENABLE 0U

/* TIM6 是 timer 调度器的 1 ms 心跳：APB1=100 MHz 且 APB1 分频=2 ⇒ 内核时钟
 * 200 MHz，arr=1000-1、psc=200-1。系统毫秒时基由 SysTick/HAL tick 提供。 */
#define SSD_TIM6_ARR (1000U - 1U)

#define SSD_TIM6_PSC (200U - 1U)

/*
 * STM32H743 烧录自检入口。
 * Reset_Handler 已完成 C 运行时初始化；HAL_Init() 配置 SysTick 为 1 ms 节拍。
 * 项目毫秒/微秒时基由 ARM 核心 SysTick 提供；TIM6 只驱动 timer 任务表的调度
 * 心跳（sys_init 配置完时钟树后立即启动）。
 */

/* 主循环耗时采样：每 32 轮采样一次（每轮读 µs 时基 + 除法太贵）。 */
#define SYS_STATS_TIME_SAMPLE_MASK (31U)

#define SYS_STATS_OVER_1MS_US (1000U)

/* 开机横幅（编译期常量；构建日期/时间由编译器注入 __DATE__ / __TIME__）。 */
static const char g_boot_banner[] = "=========== BASESSD built " __DATE__ " " __TIME__ " ===========\r\n";

#if (SSD_WATCHDOG_ENABLE != 0U)
/*
 * 当前板级自检阶段只监督 Scheduler。产品启动 Host/NAND/FTL/GC/DMA 后，应把对应
 * bit 加入 required_domains，并在每个状态机的“确认前进”位置调用 report_progress()。
 * 当前板级阶段只启用 IWDG。按典型 LSI=32 kHz 计算，/256 与 Reload=3749 的
 * 名义超时约为 30 s：T=(3749+1)*256/32000。LSI 有频偏，产品量产前应根据
 * 数据手册的频率范围验证最短/最长复位时间，而不应把它当作精确的 30 s 定时器。
 */
static const storage_wdg_cfg_s g_storage_wdg_config = {
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

/* ══════════════════════════════════════════════════════════════════
 * FTL 实例
 *
 * workspace 所有权在 FTL 模块内部：每块由拥有它的模块定义并选定内存域
 * （allocator→SRAM12、checkpoint→SRAM3、metadata→SRAM4、journal/wbuf→DTCM、
 * migrate/rr/L2P cache→AXI）。本文件只持有 controller 路由表，通过
 * ftl_instance_get() 取不透明句柄，不引用任何 FTL 内部类型。
 *
 * 各域容量由 linker ASSERT 兜底（.dtcm_data / .sram12_data / .sram3_data /
 * .sram4_data 与 AXI_RAM 区域溢出均为链接错误）。
 * ══════════════════════════════════════════════════════════════════ */

ssd_ctl_s g_ctrl; /* controller 路由表（按 compact die 号收 FTL 实例指针）；非 static：

                   * UART CLI（cli.c）经 extern 引用，见 controller/cli.h */

/* die → 实例指针表（ssd_controller_init 的输入；实例句柄由 FTL 提供）。 */
static ftl_ctx_s *g_ftl_ptrs[FTL_DIE_MAX];

// 本次 boot 是否有任一 die 走了 PFR；lifetime_boot 在全部 die 恢复后统一调用一次。
static BOOL s_boot_pfr_any;

/*
 * 分区标题（裸行，不带 [INF] 前缀）：把开机日志按
 * system / nand / controller / ftl / console 分区，便于扫读。
 */
#if SSD_STATS_ENABLE
typedef struct SYS_LOOP_STATISTICS_S
{
    DWORD rounds;
    DWORD progress_rounds;
    DWORD idle_streak;
    DWORD idle_streak_max;
    DWORD over_1ms; /* 采样轮中耗时 > 1 ms 的次数 */
    DWORD max_us;   /* 采样轮的最大耗时（高水位 gauge） */
} sys_stats_loop_s;

static sys_stats_loop_s s_stats_loop;

#    define SYS_LOOP_STATS_LIST(X)                   \
        X(rounds, STATS_U32, STATS_COUNTER)          \
        X(progress_rounds, STATS_U32, STATS_COUNTER) \
        X(idle_streak, STATS_U32, STATS_GAUGE)       \
        X(idle_streak_max, STATS_U32, STATS_GAUGE)   \
        X(over_1ms, STATS_U32, STATS_COUNTER)        \
        X(max_us, STATS_U32, STATS_GAUGE)

#    define SYS_LOOP_STATS_NAME(field, width, kind) \
        {STATS_NAME_STR(field), STATS_OFFSET(sys_stats_loop_s, field), width, kind},
static const stats_name_s s_stats_loop_names[] = {
    SYS_LOOP_STATS_LIST(SYS_LOOP_STATS_NAME)};
#    undef SYS_LOOP_STATS_NAME
#    define SYS_LOOP_STATS_CHECK(field, width, kind) STATS_CHECK_FIELD(sys_stats_loop_s, field, width);
SYS_LOOP_STATS_LIST(SYS_LOOP_STATS_CHECK)
#    undef SYS_LOOP_STATS_CHECK
#endif /* SSD_STATS_ENABLE */

static void boot_section(const char *name)
{
    (void)uart_log_write("--- ", sizeof("--- ") - 1U);
    (void)uart_log_write(name, strlen(name));
    (void)uart_log_write(" ---\r\n", sizeof(" ---\r\n") - 1U);
}

/* controller 分区：FC 池 / 拓扑 / 数据通路配置汇总（器件就绪后打印）。 */
static void boot_report_controller(void)
{
    boot_section("controller");
    fc_boot_report();
}

/* console 分区：交互入口就绪提示。 */
static void boot_report_console(void)
{
    boot_section("console");
    LOGI("[SYS ] console ready, type 'help' for commands\r\n");
}

/* ── 主循环统计（模块自持；docs/design/common/statistics_design.md）──
 * 空转判据：每轮比较 FC/FTL 的进展签名，签名不变即记一次 idle。
 * 统计专用对象/表/签名在 SSD_STATS_ENABLE=0 时整体不编译。 */
#if SSD_STATS_ENABLE
/**
 * @brief 主循环进展签名（FC 完成/派发 + FTL 完成派发/host 下发）
 *
 * @return 进展签名
 */
static DWORD sys_stats_progress(void)
{
    return fc_stats_progress() + ftl_op_stats_progress();
}
#endif /* SSD_STATS_ENABLE */

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

    /* 上一轮若留有 PC 采样环，提示一条；详细序列/排行由 CLI `pc` 打印。 */
    if (pc_sample_prev_valid())
    {
        const DWORD pc_count = pc_sample_count(true);
        const DWORD pc_rate  = pc_sample_rate(true);
        const DWORD span_ms  = pc_count * pc_rate;

        LOGI("[PC  ] previous-run PC samples available (%lu samples, rate=%lu, span=%lums): type 'pc' to dump\r\n", pc_count, pc_rate, span_ms);
    }
}

/* system 分区：系统身份（CPU/时钟 / Cache / MPU 区域数）+ 时钟树（全部 HAL 实读）。 */
static void boot_report_system(void)
{
    boot_section("system");

    const DWORD cpu_mhz  = delay_port_core_clock_hz() / 1000000U;
    const DWORD hclk_mhz = delay_port_hclk_hz() / 1000000U;
    const char *icache   = (get_icache_sta() != 0U) ? "on" : "off";
    const char *dcache   = (get_dcache_sta() != 0U) ? "on" : "off";
    const DWORD mpu_num  = sys_mpu_region_count();

    LOGI("[SYS ] CPU=%luMHz HCLK=%luMHz ICache=%s DCache=%s MPU=%lu\r\n", cpu_mhz, hclk_mhz, icache, dcache, mpu_num);

    sys_clock_tree_s clk;

    sys_clock_tree_get(&clk);

    const DWORD sys_mhz   = clk.sysclk_hz / 1000000U;
    const DWORD tree_hclk = clk.hclk_hz / 1000000U;
    const DWORD pclk1_mhz = clk.pclk1_hz / 1000000U;
    const DWORD pclk2_mhz = clk.pclk2_hz / 1000000U;
    const DWORD pclk3_mhz = clk.pclk3_hz / 1000000U;
    const DWORD pclk4_mhz = clk.pclk4_hz / 1000000U;
    const DWORD fmc_mhz   = clk.fmc_hz / 1000000U;
    const DWORD qspi_mhz  = clk.qspi_hz / 1000000U;
    const DWORD uart_mhz  = clk.uart_hz / 1000000U;

    /* 时钟树：APB1/2/3/4 相等时压成一个 pclk，不等则四个全列。 */
    if ((clk.pclk1_hz == clk.pclk2_hz) && (clk.pclk2_hz == clk.pclk3_hz) && (clk.pclk3_hz == clk.pclk4_hz))
    {
        LOGI("[SYS ] clk sys=%lu hclk=%lu pclk=%lu fmc=%lu qspi=%lu uart=%lu\r\n", sys_mhz, tree_hclk, pclk1_mhz, fmc_mhz, qspi_mhz, uart_mhz);
    }
    else
    {
        LOGI("[SYS ] clk sys=%lu hclk=%lu pclk1/2/3/4=%lu/%lu/%lu/%lu fmc=%lu qspi=%lu uart=%lu\r\n", sys_mhz, tree_hclk, pclk1_mhz, pclk2_mhz, pclk3_mhz, pclk4_mhz, fmc_mhz, qspi_mhz, uart_mhz);
    }
}

static ftl_topo_s ssd_ftl_die_topo(BYTE die)
{
    ftl_topo_s topo;

    topo.ch  = die_2_ch(die);
    topo.ce  = die_2_ce(die);
    topo.lun = die_2_lun(die);

    return topo;
}

/**
 * @brief 初始化单个 die 的 FTL 实例
 *
 * 顺序：ftl_instance_init（装配自持 workspace + ftl_init，内部含 bbt_init 标坏块
 *       → 遍历 BST 非坏块回填 free pool，alc_init 不填 free pool、bbt 只标坏块）
 *       → 格式判定 / 格式化 → ftl_boot_recover → 日志。
 *
 * @param die 紧凑 die 号（g_die_table 给出对应 (ch,ce,lun)）
 * @return FTL_OK 成功；FTL_ERROR 初始化 / 介质判定 / 格式化 / 恢复失败
 */
static ftl_status_e ssd_ftl_boot(BYTE die)
{
    ftl_ctx_s *ftl = ftl_instance_get(die);

    const ftl_topo_s topo = ssd_ftl_die_topo(die);
    const BYTE       ch   = topo.ch;
    const BYTE       ce   = topo.ce;
    const BYTE       lun  = topo.lun;

    /* workspace 由 FTL 模块自持；本函数只触发装配（含 owner 登记与 debug 契约自检）。 */
    if (ftl_instance_init(die) != FTL_OK)
    {
        LOGE("[FTL ] init failed ch=%u ce=%u lun=%u\r\n", ch, ce, lun);
        return FTL_ERROR;
    }

    /* 坏块落点统计（开机基线，**早于格式判定**）：boot 可能因介质读不通而中止，
     * 先打一行保证失败时也能看到 BAD 分布。 */
    ftl_boot_log_bad_areas(ftl);

    /* 介质布局版本判据：介质不是当前版本（旧格式 / 空片）⇒ 全片格式化。
     * 放在 ftl_init 之后、free pool 回填之前——bbt_init 已把出厂坏块标记读进 BST
     * （**只在空片时读**；用过的介质上那些字节是 FTL 数据不是标记，bbt_init 会拒绝重扫，
     * 见 bbt.c:bbt_medium_has_ftl_data），且此时回填尚未把未擦的旧块当空白块放进
     * free pool。格式化后写当前版本 Checkpoint 标记 ⇒ 下次开机判据命中，只发生一次。 */
    BOOL need_format   = false;
    BOOL format_verify = false;

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
    if (ftl_boot_check_format(ftl, &need_format) != FTL_OK)
    {
        LOGE("[FTL ] CP media unreadable — refuse to format ch=%u ce=%u lun=%u\r\n", ch, ce, lun);
        return FTL_ERROR;
    }
#endif

    if (need_format)
    {
        LOGW("[FTL ] media blank / layout mismatch -> format\r\n");
        if (ftl_boot_format(ftl, format_verify) != FTL_OK)
        {
            LOGE("[FTL ] media format failed ch=%u ce=%u lun=%u\r\n", ch, ce, lun);
            return FTL_ERROR;
        }
    }

    /* 池回填 / full metadata rebuild / 块元数据快照恢复全部在 ftl_boot_recover 内完成：
     *   · 块元数据快照有效 ⇒ 直接按快照恢复 Free/OP/Spare（不做几何 carve）；
     *   · 无快照 / 快照损坏 ⇒ full metadata rebuild → carve。
     * ⚠ **full metadata rebuild 必须先于 carve**（full metadata rebuild 认领写过的块，carve 只把 state==NONE 的块入池）；
     *   carve 也**不能**在块元数据快照恢复之前做——否则池条目会重复。 */
    ftl_boot_report_s result;

    if (ftl_boot_recover(ftl, &result) != FTL_OK)
    {
        LOGE("[FTL ] boot recovery failed ch=%u ce=%u lun=%u\r\n", ch, ce, lun);
        return FTL_ERROR;
    }

    ftl_boot_log_usage(ftl);

    const char *boot_bit = result.scoped ? "scoped" : ((result.blocks_restored != 0U) ? "restored" : "fallback");

    LOGI("[FTL ] boot type=%d cp_seq=%lu journal_entries=%lu bit=%s\r\n", result.boot_type, result.cp_sequence, result.journal_entries, boot_bit);

    if (result.boot_type == FTL_BOOT_POWER_FAILURE)
    {
        s_boot_pfr_any = true;
    }

    return FTL_OK;
}

/* 完整初始化 FTL：全实例 boot + controller 绑定（controller READY 才切 ftl_service）
 *
 *   ① 拓扑建表 + 全局静态复位 + g_ftl_ptrs 预绑 + ftl_device_prepare ——
 *      **必须早于逐实例 boot**，理由见函数内注释。
 *   ② 逐 die boot —— 任一失败则 boot_ok=false，整体回退 fc_service。
 *   ③ ssd_controller_init(&g_ctrl, g_ftl_ptrs) —— 对外 controller 就绪（READY）。
 *   ④ 主循环按 controller READY 切换到 ftl_service。
 */
static void ssd_ftl_boot_all(void)
{
    BOOL boot_ok = true;

    s_boot_pfr_any = false;

    /* ① 拓扑建表：板级扫描结果（g_nand_targets，armed 之前已填）→ 紧凑 die 表。
     * 必须在逐实例 boot 之前；纯 RAM，不碰硬件。
     * L2P/容量按运行时 DIE_MAX；die 表只含实际存在的 target。 */
    if (!ftl_topology_build(g_nand_targets))
    {
        LOGE("[FTL ] topology build failed\r\n");
        return;
    }

    ftl_global_init();

    /* ①.5 设备级一次初始化（全局完成表 + 实例路由 + 每 (ch,ce) 共享 L2P cache）。
     * 必须早于逐实例 boot：恢复期的 full metadata rebuild 会抽完成表，路由为 NULL
     * 时完成无法派发，job 永久卡死。 */
    for (BYTE die = 0U; die < DIE_MAX; die++)
    {
        ftl_ctx_s *ftl = ftl_instance_get(die);

        g_ftl_ptrs[die] = ftl;

        /* 预绑 controller 的实例指针：boot 失败时 ssd_controller_init 不会执行，
         * 但 CLI 的 `bbt --erase` 仍需要实例才能擦除旧/坏介质。
         * boot 成功后 ssd_controller_init 会重新校验并绑定同一组指针。 */
        g_ctrl.ftl[die] = ftl;
    }
    ftl_device_prepare();

    /* ② 逐 die boot */
    for (BYTE die = 0U; die < DIE_MAX; die++)
    {
        if (ssd_ftl_boot(die) != FTL_OK)
        {
            boot_ok = false;
        }
    }

    if (!boot_ok)
    {
        /* 实例未必 init 完成 ⇒ 撤销路由，恢复「路由未设」语义（此时主循环退回
         * fc_service，无人 drain 完成表，派发不会发生）。 */
        ftl_route_clear();
        LOGE("[FTL ] boot failed, fallback to fc_service\r\n");
        return;
    }

    /* 全部实例的 lifetime 块已加载 ⇒ 统一计一次 boot / PFR，启动上电计时。 */
    lifetime_boot(s_boot_pfr_any);

    /* ②.5 路由 ↔ die 的往返自检（owner 解析的根基）：
     * topo_2_die(route(die)) 必须等于 die；公式漂移会让每次 cache 操作找不到 owner。
     * 必须在逐实例 ftl_instance_init 之后做（topo 那时才有值），且 release 也拒绝启动
     * （H14 判据 9 的同类：静默错路由比停机更贵）。 */
    if (ftl_route_verify() == false)
    {
        ftl_route_clear();
        LOGE("[FTL ] route/die verify failed, fallback to fc_service\r\n");
        return;
    }

    /* ③ controller 就绪 */
    const ssd_status_e ctrl_ret = ssd_controller_init(&g_ctrl, g_ftl_ptrs);
    if (ctrl_ret != SSD_STATUS_OK)
    {
        ftl_route_clear();
        LOGE("[FTL ] controller init failed: %d; fallback to fc_service\r\n", ctrl_ret);
        return;
    }

    /* ③.5 主机可见容量：按实际坏块扣减（本仓自拟的容量折中）。
     * 必须在 USB 枚举之前设置——main 继续往下才初始化 USB 设备栈。 */
    g_ctrl.host_lba_max = ftl_host_lba_max();

    /* ③.6 空白介质补默认 MBR：boot auto-format 与 CLI format 都走这里。
     * 必须在 recover 回填 free/op/spare 池、host_lba_max 定稿之后；
     * LBA0 已有 55AA 时保持用户分区不动。 */
    if (ssd_controller_ensure_default_mbr(&g_ctrl) != SSD_STATUS_OK)
    {
        LOGE("[HIL ] default MBR ensure failed — host may see partitionless media\r\n");
    }

    /* ④ ssd_controller_init 已把状态置 READY —— 主循环据此切换 ftl_service。 */
    const DWORD lba_max      = g_ctrl.host_lba_max;
    const QWORD capacity_mib = ((QWORD)lba_max * FTL_IU_BYTES) / (1024UL * 1024UL);

    LOGI("[FTL ] controller ready, lba_max=%lu (%lluMiB)\r\n", lba_max, capacity_mib);
}

/* CRT 未提供时，newlib 的 __libc_init_array 会调用这个钩子。 */
void _init(void)
{
}

/**
 * @brief 推进一次后台服务：FTL/FC → controller → USB → timer。
 *
 * @note 主循环和需要同步等待完成的 CLI 命令共用本函数；顺序必须保持
 *       controller 在 USB 之前，USB 前端同一轮才能看到 complete_flag。
 * @note 不解析 CLI、不调用 cli_service_poll —— 避免命令 handler 重入解析器。
 */
void ssd_runtime_pump(void)
{
#if SSD_STATS_ENABLE
    STATS_INC(s_stats_loop.rounds);
    const DWORD progress_before = sys_stats_progress();
    BOOL        time_sample     = ((s_stats_loop.rounds & SYS_STATS_TIME_SAMPLE_MASK) == 0U) ? true : false;
    DWORD       time_start_us   = 0U;

    if ((time_sample != false) && (delay_time_now_us(&time_start_us) == false))
    {
        time_sample = false;
    }
#endif

    /* controller READY ⇔ FTL 全部实例 boot + 绑定完成（唯一就绪判据） */
    if (g_ctrl.state == SSD_CONTROLLER_READY)
    {
        /* ① 全局泵：NAND 推进 + 完成表抽干（实例无关，每轮一次） */
        ftl_service_pump();

        /* ② 逐 die 后台：host 补发 + 后台调度 */
        for (BYTE die = 0U; die < DIE_MAX; die++)
        {
            ftl_ctx_s *ftl = ftl_instance_get(die);

            ftl_service(ftl);
        }
    }
    else
    {
        (void)fc_service();
    }

    /* Controller 自己的异步命令（FLUSH / TRIM）：每轮推进 + 完成回执。
     * 必须在 USB 任务之前——USB 前端同一轮就能看到 complete_flag。 */
    ssd_controller_service(&g_ctrl);

    /* USB：先让 MSC 前端检查在飞请求（提交/回填），再让协议栈处理事件。 */
    usb_msc_task();
    usb_device_task();

    timer_scheduler_poll();

#if SSD_STATS_ENABLE
    if (time_sample != false)
    {
        DWORD time_end_us = 0U;

        if (delay_time_now_us(&time_end_us) != false)
        {
            const DWORD elapsed = time_end_us - time_start_us;

            if (elapsed > s_stats_loop.max_us)
            {
                s_stats_loop.max_us = elapsed;
            }
            if (elapsed > SYS_STATS_OVER_1MS_US)
            {
                STATS_INC(s_stats_loop.over_1ms);
            }
        }
    }

    const DWORD progress_after = sys_stats_progress();

    if (progress_after != progress_before)
    {
        STATS_INC(s_stats_loop.progress_rounds);
        s_stats_loop.idle_streak = 0U;
    }
    else
    {
        STATS_INC(s_stats_loop.idle_streak);
        if (s_stats_loop.idle_streak > s_stats_loop.idle_streak_max)
        {
            s_stats_loop.idle_streak_max = s_stats_loop.idle_streak;
        }
    }
#endif
}

int main(void)
{
    /* 必须先于 SysTick 启动：先把上一轮的采样环快照到 prev，再开始本轮采样，
     * 否则启动期间的采样会覆盖故障现场（pc_sample.h）。 */
    pc_sample_init();

    sys_init();

    /* TIM6 是 timer 调度器的毫秒心跳（watchdog / FTL 周期评估），
     * 必须在任务注册与首次 poll 之前运行；晚启动只会推迟任务的首次到期。 */
    timer_timx_int_init(SSD_TIM6_ARR, SSD_TIM6_PSC);

    cli_init(); /* 预填 CLI 固定 pattern buffer（.sram2_data NOLOAD） */
    log_set_sink(uart_log_write);
    log_set_flush_sink(uart_log_flush);
    log_set_level(LOG_INFO);
    /* 栈哨兵：把 [&end, 当前 SP) 整段填 pattern。必须**早**——越早，后面
     * （FTL boot / CLI test 这些深调用）用过的栈就越多地落在被守卫区内，
     * 周期校验才量得到真实峰值。此后由 superloop 每轮 stack_guard_check() 校验。 */
    stack_guard_init();
    boot_report_banner();
    boot_report_system();

    /* ── 全系统初始化顺序（② 起为 if/else-if 级联：前一步失败会整段跳过后面的）──
     *   ① fc_init()            —— FC 控制面：三池 + SQ/HSQ/CQ + 调度器 + CRC；无失败路径。
     *                             内部会调 fc_nand_dispatch_init()（纯 memset，不碰硬件），
     *                             所以 FC 可以先于 NAND 硬件初始化。
     *   ② fc_nand_init()       —— NAND 芯片：HAL/FMC + per-(ch,ce) 后端装配
     *                             + RESET + READ_ID 校验；R/B# 由 GPIO 轮询（无 EXTI）。
     *                             其 boot 命令直接走 FC 同步 UOP 引擎，绕过
     *                             fc / fc_nand_dispatch 命令队列。
     *   ③ set_armed(1)         —— 放行 fc_nand_dispatch_submit / submit_chain
     *                             （此前一律返回 INVALID_ARGUMENT）。
     *   ④ platform_register()  —— 注册 time_us，供 FTL 的 T3 定时触发（CP/GC）使用。
     *   ⑤ ssd_ftl_boot_all()   —— FTL 全实例 boot + controller 绑定 → controller READY。
     *   ⑥ fc_nand_enable_high_speed() —— 全部初始化完成后才切 Mode 5；失败保持低速。
     *   详见 docs/design/fc_nand_design.md §22「初始化全流程」。 */
    fc_init();

    boot_section("nand");
    const nand_result_e nand_init_result = fc_nand_init();
    if (nand_init_result != NAND_RESULT_OK)
    {
        LOGE(LOG_TAG_NAND "init failed: %d\r\n", nand_init_result);
    }
    else
    {
        fc_boot_arm();
        boot_report_controller();
        /* 平台 time_us 注册：FTL 的 T3 定时触发（CP 10s / GC 500ms）依赖它。
         * 必须早于首次 ftl_service（boot 即首次），失败仅打日志不阻塞启动。 */
        LOGE_IF(platform_register() != FUNC_STATUS_SUCCESS, "platform register failed; T3 timers disabled\r\n");
        boot_section("ftl");
        ssd_ftl_boot_all();

        /* ⑥ 高速模式：只有初始化（NAND + FTL boot/恢复）全部完成后才切换，
         *    保证上电路径始终跑在保守低速时序。失败仅告警；engine 会尝试回滚，
         *    但回滚失败不代表低速仍有效。 */
        if (g_ctrl.state == SSD_CONTROLLER_READY)
        {
            sys_clock_tree_s clk;

            sys_clock_tree_get(&clk);

            const nand_result_e high_speed_result = fc_nand_enable_high_speed(clk.fmc_hz);

            if (high_speed_result != NAND_RESULT_OK)
            {
                LOGW(LOG_TAG_NAND "high-speed enable failed: %d — rollback attempted; check NAND/FMC mode\r\n", high_speed_result);
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
        LOGI("[SYS ] IWDG nominal=30s, period=100ms\r\n");
    }
#else
    LOGI("[SYS ] IWDG disabled (SSD_WATCHDOG_ENABLE=0)\r\n");
#endif

    /* FTL 周期任务（后台节拍 / GC-WL 求值）：周期由 FTL 自定。
     * 失败回退 fc_service 时不注册。 */
    if (g_ctrl.state == SSD_CONTROLLER_READY)
    {
        ftl_timer_tasks_start();
    }

#if SSD_STATS_ENABLE
    (void)stats_register("sys.loop", STATS_INST_NONE, &s_stats_loop, s_stats_loop_names, STATS_ENTRY_COUNT(s_stats_loop_names));
#endif

    /* ── 主循环（裸机 superloop，无中断参与）──
     *   cli_service_poll() —— CLI 解析（含 `test` 单元测试入口）。
     *   ssd_runtime_pump() —— 后台服务统一入口，顺序与 CLI 同步命令共用：
     *     controller 非 READY：只跑 fc_service()，保持 FC/NAND 裸路径活着。
     *     controller READY：    ftl_service_pump() 一次（全局 NAND 泵 + 抽干完成表），
     *                           再对**每个**实例调 ftl_service()（host 补发 / 后台调度）。
     *                           ⚠ 必须遍历全部实例：只跑 [0][0][0] 会让其余实例的
     *                             journal / checkpoint / bbt / GC 永不推进。
     *     controller → USB → timer_scheduler_poll 的先后顺序见 ssd_runtime_pump()。
     *   所有命令解析与调度型存储工作都不在 ISR 上下文。 */
    while (1)
    {
        /* 栈哨兵周期校验（每轮一次，MSP 无异常时开销 ~10 条指令）：
         * 查 MSP 是否仍在 DTCM 内 + 从上次最低点继续向下找被踩过的 pattern。
         * 放在循环**最前**：上一轮（含 CLI 命令 / FTL 后台）若踩了栈，这里先报。 */
        stack_guard_check();
        cli_service_poll();
        ssd_runtime_pump();
    }
}
