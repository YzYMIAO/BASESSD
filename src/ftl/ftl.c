/*
 * ftl.c — FTL 顶层（初始化 / 后台调度服务主循环 / 子操作下发 / 对外读写入口）
 *
 * 职责：
 *   FTL 的对外门面与主循环——ftl_init 装配各子模块；ftl_service 是**服务主循环**
 *   （NAND 泵 → 完成泵 → scheduler host 补发 → 工作量配额后台调度）；
 *   ftl_submit_read/write 把一条 host 请求拆成多个 FC 子操作下发；
 *   ftl_flush_begin/step / ftl_trim_begin/step 供 HIL 调用（均为异步推进）。
 *
 * 与子模块的分工：
 *   本文件             —— 编排：调度、拆分、完成聚合、模式切换；不解析介质格式。
 *   pending.c          —— 子操作登记 + 组完成聚合（一条 host 请求 = 一个 group）。
 *   ftl_op.c           —— request 表 + 实例路由 + 提交 + CQ 排空/派发（[pump] 完成泵）。
 *   其余模块（gc / wl / journal / checkpoint / bbt / l2p_cache）各司其职，
 *   见各自文件头。
 *
 * 服务主循环（全局泵 + 每实例服务）：
 *   [pump] 推进 NAND   fc_service × NAND_SUBMIT_BUDGET
 *   [pump] 完成泵      ftl_op_drain —— 按 request.op 派发到服务函数数组
 *   —— 以上在 ftl_service_pump（实例无关，每轮一次）；以下在 ftl_service（每实例）——
 *   [svc]  元数据候选池重算 / 写确认结算 / cache 序列确认 / host 补发 /
 *          序列收尾 / wbuf 空闲 flush / 后台调度（bg_policy_service）
 *   步骤清单的唯一定义见 include/ftl/ftl.h 的 ftl_service() 契约。
 *
 * 文件结构（与下方分区注释一一对应）：
 *   1. 内部辅助        写确认（ftl_confirm_write / ftl_confirm_service）/
 *                      ftl_host_io_write_ok / 写重试 / cache 写序列状态机
 *   2. host I/O 完成   四个完成分支 + ftl_host_io_callback
 *   3. cache program   host cache program 序列（早回执 + 已编程页表）
 *   4. padding END     写序列有序收尾（ftl_cache_pad_submit / callback）
 *   5. 对外接口        ftl_global_init / ftl_init
 *   6. 紧凑 die 表      ftl_topology_build（扫描结果的 FTL 调度坐标）
 *   7. 初始化契约自检  debug-only 的「初始化后不可变」绑定审计
 *   8. 服务主循环      ftl_service_pump / ftl_service
 *   9. 子操作下发      ftl_issue_subop / ftl_host_group_drain + ftl_submit_read / write
 *  10. 元数据落盘/TRIM ftl_flush_begin/step / ftl_trim_begin/step
 *
 * ⚠ 后台调度在 `ftl/bg_policy.c`：10 项任务表 / 档位 IDLE·NORMAL·BUSY → 预算规模 /
 *   mig_gc_evaluate（GC 统一触发求值）/ RR 巡检 / 后台调度入口 bg_policy_service；
 *   通用引擎也在同一文件。本文件不持有任何后台策略。
 *
 * ⚠ 页打包写缓冲在 `ftl/wbuf.c`（wbuf.h 定义 wb_handle_s），本文件只调用其接口。
 *
 * ⚠ 回收动作在 `ftl/migrate.c`：GC / WL / 预测性退役共用一台异步搬移 job
 *   （mig_gc_begin / mig_wl_begin / mig_predict_begin / mig_handle）。触发求值在
 *   `ftl/bg_policy.c`，写路径的按需 GC 在
 *   ftl_issue_subop；本文件不直接触发搬移。
 *
 * ⚠ 多实例：pending / groups / rr_last_scan_ms 均 per-instance；completion 全局
 *   但按命令 ch/ce/lun 路由。main.c 主循环：先 ftl_service_pump()（全局泵，每轮一次）
 *   抽干 NAND + 完成表，再**遍历全部实例**调 ftl_service() 逐实例推进 host 补发 / 后台调度——
 *   只跑 [0][0][0] 会让其余实例的 journal / checkpoint / bbt / GC 永不推进。
 */

#include "ftl.h"

#include "assertion.h"
#include "bbt.h"
#include "cache_batch.h"
#include "checkpoint.h"
#if defined(FC_NAND_TEST_ENABLE)
#    include "fault_inject.h" /* 测试镜像专属注入点；生产构建整段裁掉（宏隔离） */
#endif
#include "fc_interface.h"
#include "ftl_fc_interface.h"
#include "ftl_op.h"        /* ftl_op_submit / FTL_OP_* */
#include "lifetime_port.h" /* 终身计数接线（非热对象） */
#include "log.h"
#include "metadata_job.h"
#include "pending.h"
#include "power_failure_recovery.h"
#include "ssd_compiler.h"
#include "stats.h"
#include "timer.h"
#include "wbuf.h"

#include <string.h>

// FTL 每 I/O 热数据（实例描述符）：定义与内存域（DTCM）都归本模块。
// CPU-only：NAND/FC 的 DMA buffer 不在这里，DMA1/2 也访问不到 DTCM。
// `.dtcm_data` 是 NOLOAD ⇒ ftl_init 对每个实例显式 memset。
// 其余 workspace 由各自拥有的模块定义（wbuf/journal/allocator/...），
// 本文件只经 workspace_get() 取引用装配。
ftl_ctx_s g_ftl[FTL_DIE_MAX] DTCM_DATA;

// 公共 boot 入口的 once 守卫：重复初始化会清掉共享 cache / 重复注册 timer，
// 现象是静默停摆。本仓无运行期 re-init；若将来增加，必须同时复位这些守卫。
// ⚠ 本文件 .bss 落 DTCM NOLOAD ⇒ 上电/软复位都不清零，必须由 ftl_global_init 复位。
static BOOL s_device_prepared;

static BOOL s_timers_started;

static BOOL s_instance_initialized[FTL_DIE_MAX];

static DWORD s_bbt_last_ms; /* BBT 起新 job 的节拍时间戳（NOLOAD，同需复位） */

// 日志限流：同一类异常在短时间内重复出现时按间隔打一行；
// 时钟缺失时由 log_rate_due 按条数兜底。统一使用 log.h / timer.h 的
// log_ratelimit_s，本文件只保留一套限流类型（C9）。
static log_ratelimit_s s_seq_drain_stall_log; /* 序列 draining 兜底放锁（5 s） */

/* ══════════════════════════════════════════════════════════════
 * 紧凑 die 表（扫描结果的 FTL 调度坐标）
 *
 * 板级 target 扫描（armed 之前）只给出哪些 (ch,ce) target 存在、
 * logical_units 多少；这里给实际存在的 LUN 分配紧凑 die 号，并填两张逆映射表。
 * ══════════════════════════════════════════════════════════════ */
BYTE g_die_count;

BYTE g_chcelun_table[CH_NUM][CE_NUM][LUN_NUM];

ftl_topo_s g_die_table[FTL_DIE_MAX];

/**
 * @brief 后台周期节拍：补调度配额、置 L2P/BBT 到期标志
 *
 * 只置 flag，不跑业务；真正执行在 ftl_service 的固定位置。
 *
 * @param ftl     实例
 * @param bbt_due true = BBT 起新 job 的节拍到
 */
static void ftl_periodic_tick(ftl_ctx_s *ftl, BOOL bbt_due)
{
    bg_policy_refill(&ftl->bg);
    ftl->l2p_flush_due = 1U;

    if (bbt_due != false)
    {
        ftl->alloc.bbt.flush_due = 1U;
    }
}

/**
 * @brief 注册本实例的 GC/WL/预测性退役周期求值任务
 *
 * @param ftl 已初始化的实例
 * @return true = 注册成功；false = 定时器任务表满
 */
static BOOL ftl_mig_timer_register(ftl_ctx_s *ftl)
{
    return mig_timer_register(ftl->mig_handle);
}

/**
 * @brief 序列守卫：两件事 —— ① `draining` 的排空判定；② chunk 边界序列等下一 chunk 的
 *        空闲超时。
 *
 * @param ftl 实例。
 *
 * @note ① `draining`：所有者组的在飞子操作归零 ⇒ 清 `draining` 放开 LUN；超过
 *       FTL_CACHE_SEQ_DRAIN_STALL_MS 也强制清（命令卡死时不能把 LUN 永久锁死）。
 *       ② `active`：序列跨 host 传输段 / 跨 host 命令保持打开，等同一 LBA 流的下一段；
 *       所有者组已释放且超时无人接手 ⇒ 写序列置 `close_requested`（service 用 padding
 *       END 收尾，不丢提前回执页）；读序列直接释放（不发 LAST）。
 *       两者时钟缺失（now_ms == 0）时都不计时。
 */
static void SSD_CODE_FAST ftl_cache_seq_idle_guard(ftl_ctx_s *ftl)
{
    // 排空守卫：窗口已提交、命令仍在飞 ⇒ 门控继续关着，直到所有者组的在飞子操作
    // 归零（或 1 s 兜底：命令卡死时不能把 LUN 永久锁死；放锁时留 WRN 现场）。
    // 与 ftl_cache_seq_release 同口径：RR 托管的读子操作已脱钩，不算序列在飞。
    if (ftl->cache_stream.seq.draining == true)
    {
        const WORD gi        = ftl->cache_stream.seq.owner_gi;
        const BOOL in_flight = (gi < FTL_GROUP_MAX) && (ftl->pending.groups[gi].used == true) &&
                               (pend_group_has_op(&ftl->pending, gi, PENDING_OP_READ) == true);

        if (in_flight == false)
        {
            ftl->cache_stream.seq.draining = false;
            ftl->cache_stream.seq.idle_ms  = 0U;
            return;
        }

        const DWORD now_ms = platform_now_ms();

        if ((now_ms == 0U) || (ftl->cache_stream.seq.idle_ms == 0U))
        {
            if (now_ms != 0U)
            {
                ftl->cache_stream.seq.idle_ms = now_ms;
            }
            return;
        }

        if ((now_ms - ftl->cache_stream.seq.idle_ms) > FTL_CACHE_SEQ_DRAIN_STALL_MS)
        {
            // 兜底放锁是异常路径（命令 1s 没排空）：必须留现场，否则真卡死时无记录。
            LOGW_RL(&s_seq_drain_stall_log, LOG_TAG_FTL "seq drain force-clear gi=%hu outstanding=%hu age=%lums (LUN gate reopened)", gi, ftl->pending.groups[gi].outstanding, now_ms - ftl->cache_stream.seq.idle_ms);

            ftl->cache_stream.seq.draining = false;
            ftl->cache_stream.seq.idle_ms  = 0U;
        }
        return;
    }

    if (ftl->cache_stream.seq.active == false)
    {
        return;
    }

    // 所有者组仍在**下发**本序列的页 ⇒ 有进展，不计空闲。
    // 组已释放（读）或已把本 chunk 全部下发、在等下一 chunk 确认尾页（写）⇒ 开始计时。
    const BOOL owner_valid   = (ftl->cache_stream.seq.owner_gi < FTL_GROUP_MAX) && (ftl->pending.groups[ftl->cache_stream.seq.owner_gi].used == true);
    const BOOL owner_issuing = (owner_valid == true) && (ftl->pending.groups[ftl->cache_stream.seq.owner_gi].next < ftl->pending.groups[ftl->cache_stream.seq.owner_gi].total);

    if (owner_issuing == true)
    {
        ftl->cache_stream.seq.idle_ms = 0U; /* 所有者仍在推进：不计空闲 */
        return;
    }

    const DWORD now_ms = platform_now_ms();

    if ((now_ms == 0U) || (ftl->cache_stream.seq.idle_ms == 0U))
    {
        if (now_ms != 0U)
        {
            ftl->cache_stream.seq.idle_ms = now_ms;
        }
        return;
    }

    if ((now_ms - ftl->cache_stream.seq.idle_ms) > FTL_CACHE_SEQ_IDLE_MS)
    {
        if (ftl->cache_stream.seq.kind == FTL_CACHE_SEQ_WRITE)
        {
            // 写序列：没有新数据页可发 END ⇒ 请 service 用 padding END 有序收尾
            // （提前回执后最后一页未确认，不能 abort）。
            ftl->cache_stream.seq.close_requested = true;
        }
        else
        {
            ftl_cache_seq_release(ftl); /* 读序列放弃不发 LAST */
        }
    }
}

/**
 * @brief 后台周期节拍任务（每 L2P_FLUSH_INTERVAL_MS 一次）
 *
 * @param context 未使用
 * @param now_ms  未使用（时间由 platform_now_ms 提供）
 */
static void ftl_bg_tick_task(void *context, DWORD now_ms)
{
    (void)context;
    (void)now_ms;

    const DWORD now     = platform_now_ms();
    const BOOL  bbt_due = (s_bbt_last_ms == 0U) || ((now - s_bbt_last_ms) >= BBT_FLUSH_INTERVAL_MS);

    if (bbt_due != false)
    {
        s_bbt_last_ms = now;
    }

    for (BYTE die = 0U; die < DIE_MAX; die++)
    {
        ftl_periodic_tick(&g_ftl[die], bbt_due);
    }
}

/**
 * @brief 让出设备序列：写序列请求 padding 收尾，读序列直接放弃（不发 LAST）。
 *
 * @param ftl 实例
 *
 * @note 需要写 Journal/L2P 的前台控制路径（flush / trim）在入口调用：序列不退出，
 *       这些元数据提交会被门控，flush/trim 只能等空闲守卫。写序列不能 abort
 *       （提前回执页未确认），所以走 padding END。
 */
void SSD_CODE_FAST ftl_cache_seq_yield(ftl_ctx_s *ftl)
{
    if (ftl->cache_stream.seq.active == false)
    {
        return;
    }

    if (ftl->cache_stream.seq.kind == FTL_CACHE_SEQ_WRITE)
    {
        ftl->cache_stream.seq.close_requested = true;
    }
    else
    {
        ftl_cache_seq_release(ftl);
    }
}

/**
 * @brief 组被弃/失败时关闭它持有的 cache 序列。
 *
 * @param ftl 实例。
 * @param gi  组下标。
 *
 * @note 不关闭会让序列门控永久挡住该 LUN 的所有提交（门控只认 owner_submit）。
 */
void SSD_CODE_FAST ftl_cache_seq_close(ftl_ctx_s *ftl, WORD gi)
{
    if ((ftl->cache_stream.seq.active == true) && (ftl->cache_stream.seq.owner_gi == gi))
    {
        ftl_cache_seq_release(ftl);
    }
}

/**
 * @brief 复位 ftl.o 的文件级静态（NOLOAD 段不清零）
 *
 * 只调用一次，且必须在所有实例 ftl_init 之前。
 */
void ftl_global_init(void)
{
    // 公共 boot 入口的 once 守卫与后台节拍时间戳：NOLOAD 段随机残留会让启动被
    // 误判为「重复初始化」（直接拒绝）或让首次 BBT 节拍不确定。
    s_device_prepared = false;
    s_timers_started  = false;
    s_bbt_last_ms     = 0U;
    memset(s_instance_initialized, 0, sizeof(s_instance_initialized));

    // NOLOAD 段不清零：限流对象的 interval 必须在这里装好（last/suppressed 归零）。
    log_ratelimit_init(&s_seq_drain_stall_log, LOG_RATE_SLOW_INTERVAL_MS);
    ftl_flush_trim_init();
    ftl_host_io_init();
    ftl_confirm_init();
    ftl_cache_wr_init();
}

/**
 * @brief 由上电扫描结果构建紧凑 die 表
 *
 * 只给存在且 78h 探测有响应的 LUN 分配 die 号；g_chcelun_table 与
 * g_die_table 互为逆映射。
 *
 * @param targets 板级物理拓扑
 * @return true 至少一个 die 且往返自检通过；false 无 die / 越界
 */
BOOL ftl_topology_build(const nand_target_info_s targets[CH_NUM][CE_NUM])
{
    memset(g_chcelun_table, 0xFF, sizeof(g_chcelun_table));
    for (BYTE die = 0U; die < FTL_DIE_MAX; die++)
    {
        g_die_table[die].ch  = FTL_TOPO_FIELD_INVALID;
        g_die_table[die].ce  = FTL_TOPO_FIELD_INVALID;
        g_die_table[die].lun = FTL_TOPO_FIELD_INVALID;
    }
    g_die_count = 0U;

    /* 编号顺序必须与 topo_2_die 一致（ch 最快、lun 最慢）：
     * 满配矩形拓扑下 die == topo_2_die，避免出现第二套扁平坐标（C9）。
     * 稀疏拓扑时 die 紧凑。 */
    for (BYTE lun = 0U; lun < LUN_NUM; lun++)
    {
        for (BYTE ce = 0U; ce < CE_NUM; ce++)
        {
            for (BYTE ch = 0U; ch < CH_NUM; ch++)
            {
                if ((targets[ch][ce].present == 0U) || (lun >= targets[ch][ce].lun_count) || (targets[ch][ce].lun_present[lun] == 0U))
                {
                    continue;
                }

                if (g_die_count >= FTL_DIE_MAX)
                {
                    return false;
                }

                g_chcelun_table[ch][ce][lun] = g_die_count;
                g_die_table[g_die_count].ch  = ch;
                g_die_table[g_die_count].ce  = ce;
                g_die_table[g_die_count].lun = lun;
                g_die_count++;
            }
        }
    }

    if (g_die_count == 0U)
    {
        return false;
    }

    /* 往返自检：die → ftl_topo_s → (ch,ce,lun) → die。 */
    for (BYTE die = 0U; die < DIE_MAX; die++)
    {
        const BYTE ch  = die_2_ch(die);
        const BYTE ce  = die_2_ce(die);
        const BYTE lun = die_2_lun(die);

        if ((ch >= CH_NUM) || (ce >= CE_NUM) || (lun >= LUN_NUM) || (g_chcelun_table[ch][ce][lun] != die))
        {
            return false;
        }
    }

    /* 未分配的尾项必须保持无效（三个字段都 0xFF）。 */
    for (BYTE die = DIE_MAX; die < FTL_DIE_MAX; die++)
    {
        if ((g_die_table[die].ch != FTL_TOPO_FIELD_INVALID) || (g_die_table[die].ce != FTL_TOPO_FIELD_INVALID) || (g_die_table[die].lun != FTL_TOPO_FIELD_INVALID))
        {
            return false;
        }
    }

    return true;
}

/**
 * @brief 初始化 FTL 实例
 *
 * 只绑指针；storage 引用的 workspace 由各模块自持（ftl_instance_init 装配），
 * 本函数只对绑定到的表显式 memset。
 *
 * @param ftl     实例
 * @param topo    本实例路由
 * @param cache   共享 L2P cache
 * @param storage storage 数组/挂载块
 * @return FTL_OK；FTL_ERROR 元数据候选池构建失败
 */

SSD_CODE_FAST ftl_status_e ftl_init(ftl_ctx_s *ftl, ftl_topo_s topo, l2p_cache_s *cache, const ftl_storage_s *storage)
{
    ASSERT(ftl != NULL);
    ASSERT(cache != NULL);
    ASSERT(storage != NULL);
    ASSERT(storage->table_refs != NULL);
    ASSERT(storage->metadata_dir != NULL);
    ASSERT(storage->journal != NULL);
    ASSERT(storage->cp_handle != NULL);
    ASSERT(storage->migrate != NULL);
    ASSERT(storage->wbuf != NULL);
    ASSERT(storage->rr_page_scratch != NULL);

    // .dtcm_data 是 NOLOAD，启动不清零；描述符本身没有逐字段覆盖所有状态，
    // 必须在绑定前整表清零（与 storage 数组的 memset 同一纪律）。
    memset(ftl, 0, sizeof(*ftl));

    // ├─ PBA 字段视图自检（一次）：位域布局 implementation-defined，无法用 _Static_assert
    // 证明（见 ftl_config.h §三.2）。若本工具链的布局与 PBA_*_SHIFT 不一致，
    // 强转（*(DWORD *)&fields）会静默算错地址 ⇒ 在此硬失败。
    ASSERT(pba_fields_layout_matches());

    ftl->topo = topo;

    // 异步 TRIM job 初态（NOLOAD 段不清零，显式复位）。
    ftl->trim.active    = false;
    ftl->trim.lba       = 0U;
    ftl->trim.end       = 0U;
    ftl->trim.start_lba = 0U;
    ftl->trim.start_ms  = 0U;
    timer_deadline_clear(&ftl->trim.deadline);

    // ├─ 绑定 workspace（各模块自持，ftl_instance_init 按 die 装配）──
    // ⚠ .sram*_data 是 NOLOAD，启动代码不清零 ⇒ 下面必须逐表 memset。
    ftl->table_refs      = *storage->table_refs;
    ftl->metadata_dir    = storage->metadata_dir;
    ftl->journal         = storage->journal;
    ftl->cp_handle       = storage->cp_handle;
    ftl->mig_handle      = storage->migrate;
    ftl->wbuf            = storage->wbuf;
    ftl->rr_page_scratch = storage->rr_page_scratch;

    // ⚠ 这里**不**调 ftl_op_init()：request 表与实例路由表都是**全局**资源
    // （ftl_op.c 的 file-static），而本函数是**逐实例**调用的 ——
    // 在此清表会抹掉其它实例在飞的 ctx 登记，也会抹掉已设好的路由表。
    // 该函数由 ftl_device_prepare 在**所有实例 boot 之前**调用一次。
    // （完成派发不需要在此注册 handler：request 表里带 op，g_op_callbacks[] 是
    // 编译期常量数组，见 ftl_op.c；request 表只回答"这条命令的私有状态在哪"。）

    // ├─ Pending Request 表（per-instance） ──
    // 待入账项存本实例的 pending 表；推进由 ftl_service 的写确认服务负责。
    pend_init(&ftl->pending);

    // ├─ storage 数组显式清零（启动不碰 .sram*_data；装配挂载块同）──
    // 五张表：alc_init 逐块读 ftl_block_is_bad() ⇒ blocks 必须先清零；
    // free/op/spare/closed 的读者按 count 有界，逐表清零让 NOLOAD 语义确定。
    // metadata_dir：load/init 不覆盖全部字段 ⇒ 先整表清零。
    // journal：init 只重置部分 job 字段 ⇒ 先整表清零。
    // cp_handle：active 在 cp_job_begin 自己 memset 之前就会被读 ⇒ 必须先清零。
    // migrate / wbuf / rr_page_scratch：装配挂载块，与其余数组一致清零。
    // ⚠ 全部 memset 必须早于 md_load。
    memset(ftl->table_refs.blocks, 0, FTL_BLOCK_TOTAL * sizeof(*ftl->table_refs.blocks));
    memset(ftl->table_refs.free_blocks, 0, FTL_BLOCK_TOTAL * sizeof(*ftl->table_refs.free_blocks));
    memset(ftl->table_refs.op_blocks, 0, PLANE_NUM * sizeof(*ftl->table_refs.op_blocks));
    memset(ftl->table_refs.spare_blocks, 0, PLANE_NUM * sizeof(*ftl->table_refs.spare_blocks));
    memset(ftl->table_refs.closed_sb_list, 0, SUPER_BLOCK_LIST_MAX * sizeof(*ftl->table_refs.closed_sb_list));
    memset(ftl->metadata_dir, 0, sizeof(*ftl->metadata_dir));
    memset(ftl->journal, 0, sizeof(*ftl->journal));
    memset(ftl->cp_handle, 0, sizeof(*ftl->cp_handle));
    // 装配挂载块：与其余 storage 数组一致显式清零（未来若落 NOLOAD 段，
    // 启动代码不清零）。
    memset(ftl->mig_handle, 0, sizeof(*ftl->mig_handle));
    memset(ftl->wbuf, 0, sizeof(*ftl->wbuf));
    memset(ftl->rr_page_scratch, 0, FTL_READ_USER_SIZE);

    // ├─ 绑定共享 L2P Cache（设备级已初始化一次；metadata 依赖由 owner 解析）──
    ftl->cache = cache;

    // ├─ Allocator（各指针绑到 table_refs） ──
    alc_init(&ftl->alloc, ftl->topo, &ftl->table_refs);
    ftl->alloc.md_dir = ftl->metadata_dir; /* bbt_mark_bad 的借用日志块判定 */

    // ├─ 候选池存储绑定（metadata_dir 自持 md_pools_s；allocator 只持有指针）──
    // 必须在 bbt_init 之前：BBT 自举扫描/出厂扫描后要按 BAD 集建正式候选池。
    ftl->metadata_dir->topo  = ftl->topo;
    ftl->metadata_dir->alloc = &ftl->alloc;
    ftl->alloc.pools         = &ftl->metadata_dir->pools;

    // ├─ BBT：先自举扫描/出厂扫描，再用 BAD 集建元数据候选池 ──
    // 顺序是承重的：目录检查点/CP/Journal 的候选池都依赖 BBT 的 BAD 集；
    // 而 BBT 自身靠 record 的 magic/sequence 在「名义块 + shared spare」上自举。
    bbt_init(&ftl->alloc);

    if (md_pool_build_all(&ftl->alloc) == false)
    {
        LOGE("[FTL ] metadata candidate pool build failed — refuse boot\r\n");
        return FTL_ERROR;
    }

    // ├─ Journal ──
    // 持续擦除失败的环形块要标进 BBT；journal 只持有 allocator 指针，
    // 通过 allocator/bbt 的 owner API 查询与标坏，不直接改其内部字段。
    jrn_init(ftl->journal, ftl->topo, &ftl->alloc);

    // ├─ Metadata Directory（目录检查点候选池已就绪）──
    ftl->stats.total_writes   = 0U; /* 元数据 job 的 total_writes 指针在建目录前就有效 */
    ftl->stats.dangling_total = 0U;

    // cache_stream 的其余字段已由上面的整块 memset 清零；只有顺序流检测器的
    // 「无上一请求」哨兵不是 0（FTL_LBA_INVALID），必须显式写。
    ftl->cache_stream.rd.last_end = FTL_LBA_INVALID;
    ftl->cache_stream.wr.last_end = FTL_LBA_INVALID;
    {
        const ftl_status_e md_ret = md_load(ftl->metadata_dir, &ftl->alloc, &ftl->stats.total_writes);

        // 失败时仍建空 Directory（空片首启 / 格式化路径需要），但把失败状态留给
        // pfr_boot_recovery：介质若已有有效 Checkpoint，空 Directory = 映射整体丢失
        // ⇒ 必须拒绝启动。
        ftl->flags.md_load_failed = (md_ret != FTL_OK) ? 1U : 0U;

        if (md_ret != FTL_OK)
        {
            md_init(ftl->metadata_dir, &ftl->alloc, &ftl->stats.total_writes);
        }
    }

    // ├─ 搬移引擎（GC / WL / 预测性退役）：窄上下文 + 自持节拍 ──
    mig_init(ftl->mig_handle, &ftl->alloc, ftl->journal, ftl->cache, &ftl->stats.total_writes);

    // ├─ Read Recovery：窄上下文（L2P / allocator / 整页暂存 / pending 表）──
    rr_init(&ftl->read_recovery_handle, ftl->cache, &ftl->alloc, ftl->rr_page_scratch, &ftl->pending);

    // ├─ Wbuf：窄上下文（allocator / pending 表 / migrate handle）──
    wb_init(ftl->wbuf, &ftl->alloc, &ftl->pending, ftl->mig_handle);

    // ├─ Checkpoint：窄上下文（l2p / journal / metadata / allocator / topo）──
    // cp_init 内部整体 memset ⇒ 池游标 cursor_valid==0，首次 job 先扫池定位。
    cp_init(ftl->cp_handle, ftl->cache, ftl->journal, ftl->metadata_dir, &ftl->alloc);
    cp_bind_sched(ftl->cp_handle, &ftl->sched);

    // ├─ 统计 ──
    ftl_stats_register_modules(ftl);

    // ├─ 后台调度器任务表（10 项；must-run + interval/budget）──
    // 必须在所有在飞的字段就绪后装载（适配器只依赖实例字段与 kick，无额外依赖）。
    bg_policy_init(&ftl->bg);

    // request scheduler：L2P/host 请求调度。item workspace 与其它模块一样自持。
    ftl_sched_init(&ftl->sched, topo_2_die(topo));

    ftl_stats_register_sched(ftl);

    return FTL_OK;
}

/**
 * @brief 取紧凑 die 号对应的 FTL 实例（不透明句柄）
 *
 * @param die 紧凑 die 号（< DIE_MAX；未分配的尾项不得访问）
 * @return 实例指针（非空）
 */
ftl_ctx_s *ftl_instance_get(BYTE die)
{
    ASSERT(die < DIE_MAX);
    return &g_ftl[die];
}

/**
 * @brief 设备级一次初始化：全局完成表 + 实例路由 + 每 (ch,ce) 共享 L2P cache
 *
 * 调用顺序：ftl_topology_build → ftl_global_init → 本函数 → 逐实例 init。
 * 重复调用会被拒绝（debug ASSERT；release 记日志后返回，不重复清 cache）。
 * 路由必须早于实例 boot：恢复期的 full metadata rebuild 会抽完成表，路由为 NULL
 * 时完成无法派发，job 永久卡死。
 */
void ftl_device_prepare(void)
{
    ASSERT(!s_device_prepared);
    if (s_device_prepared != false)
    {
        LOGE("[FTL ] device_prepare called twice; ignored\r\n");
        return;
    }
    s_device_prepared = true;

    ftl_ctx_s *route[FTL_DIE_MAX];

    memset(route, 0, sizeof(route));
    for (BYTE die = 0U; die < DIE_MAX; die++)
    {
        route[die] = ftl_instance_get(die);
    }

    ftl_op_init();
    ftl_op_route_set(route);
    l2p_device_init();
}

/**
 * @brief 初始化一个 FTL 实例的 workspace 与模块状态
 *
 * workspace 由各模块自持（本函数只取引用装配），调用者不需要知道任何内部类型。
 * 每个 die 只能成功初始化一次；重复调用返回 FTL_ERROR（release 也拒绝）。
 *
 * @param die 紧凑 die 号（< DIE_MAX）
 * @return FTL_OK；FTL_ERROR 重复初始化 / 本 target 无 L2P cache / 元数据候选池构建失败
 */
ftl_status_e ftl_instance_init(BYTE die)
{
    ftl_ctx_s *ftl = ftl_instance_get(die); /* 同时校验 die < DIE_MAX */

    ASSERT(!s_instance_initialized[die]);
    if (s_instance_initialized[die] != false)
    {
        LOGE("[FTL ] instance already initialized die=%hhu\r\n", die);
        return FTL_ERROR;
    }

    const ftl_topo_s topo = g_die_table[die];

    ftl_table_refs_s refs;
    alc_bind_workspace(die, &refs);

    l2p_cache_s *cache = l2p_cache_workspace_get(topo);

    if (cache == NULL)
    {
        LOGE("[FTL ] no L2P cache for ch=%hhu ce=%hhu (topology/board target max mismatch)\r\n", topo.ch, topo.ce);
        return FTL_ERROR;
    }

    ftl_storage_s storage;
    storage.table_refs      = &refs;
    storage.metadata_dir    = md_workspace_get(die);
    storage.journal         = jrn_workspace_get(die);
    storage.cp_handle       = cp_workspace_get(die);
    storage.migrate         = mig_workspace_get(die);
    storage.wbuf            = wb_workspace_get(die);
    storage.rr_page_scratch = rr_workspace_get(die);

    if (ftl_init(ftl, topo, cache, &storage) != FTL_OK)
    {
        return FTL_ERROR;
    }

    /* L2P owner 端口：必须在任何 cache 访问（含后续格式化/恢复）之前就绪。 */
    l2p_owner_s owner;
    owner.die          = die;
    owner.metadata_dir = ftl->metadata_dir;
    owner.alloc        = &ftl->alloc;
    owner.topo         = ftl->topo;
    owner.journal      = ftl->journal;
    owner.sched        = &ftl->sched;
    l2p_owner_register(die, &owner);

    ftl_contract_audit(ftl);

    s_instance_initialized[die] = true;
    return FTL_OK;
}

/**
 * @brief 校验实例路由与紧凑 die 表的往返一致性
 *
 * @return true = 一致；false = 路由/拓扑公式漂移
 */
BOOL ftl_route_verify(void)
{
    return ftl_op_route_verify();
}

/**
 * @brief 撤销实例路由（boot 失败回退 fc_service 时用）
 */
void ftl_route_clear(void)
{
    ftl_op_route_set(NULL);
}

/**
 * @brief 中止属于同一 host 请求的所有 FTL 组
 *
 * @param ctx HIL 请求指针（非空）
 */
void ftl_abort_request(void *ctx)
{
    ASSERT(ctx != NULL);

    for (BYTE die = 0U; die < DIE_MAX; die++)
    {
        (void)pend_group_abort_ctx(&g_ftl[die].pending, ctx);
    }
}

/**
 * @brief 中止本实例的 TRIM job
 *
 * @param ftl 实例
 */
void ftl_trim_abort(ftl_ctx_s *ftl)
{
    ASSERT(ftl != NULL);

    ftl->trim.active    = false;
    ftl->trim.start_lba = 0U;
    ftl->trim.start_ms  = 0U;
    timer_deadline_clear(&ftl->trim.deadline);
}

/**
 * @brief 打印本实例的池占用（boot 基线）
 *
 * @param ftl 已初始化的实例（只读）
 */
void ftl_boot_log_usage(const ftl_ctx_s *ftl)
{
    ASSERT(ftl != NULL);

    const alc_s *alloc = &ftl->alloc;

    LOGI("[FTL ] instance ch=%hhu ce=%hhu lun=%hhu free=%hu op=%hu spare=%hu\r\n", ftl->topo.ch, ftl->topo.ce, ftl->topo.lun, alloc->free_count, alc_pool_sum_counts(alloc->op_count, PLANE_NUM), alc_pool_sum_counts(alloc->spare_count, PLANE_NUM));
}

/**
 * @brief 计算主机可见的 LBA 上限（开机一次）
 *
 * 按实际坏块集，取各实例/plane 的最小可用块数；runtime 新增坏块不缩容。
 *
 * @return 主机 LBA 上限（>= 1）
 */
DWORD ftl_host_lba_max(void)
{
    DWORD cap_blocks = UINT32_MAX;

    for (BYTE die = 0U; die < DIE_MAX; die++)
    {
        const ftl_ctx_s *ftl              = &g_ftl[die];
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

/**
 * @brief 注册 FTL 的周期任务（后台节拍 / GC-WL 求值）
 *
 * 必须在全部实例 boot 成功之后调用一次；失败回退 fc_service 时不调用。
 * 周期与内部策略由 FTL 决定，调用者不需要知道。
 * 重复调用会被拒绝（debug ASSERT；release 记日志后返回，不重复注册）。
 */
void ftl_timer_tasks_start(void)
{
    ASSERT(!s_timers_started);
    if (s_timers_started != false)
    {
        LOGE("[FTL ] timer tasks already started; ignored\r\n");
        return;
    }
    s_timers_started = true;

    DWORD task_id;

    if (!timer_scheduler_add(ftl_bg_tick_task, NULL, 0U, L2P_FLUSH_INTERVAL_MS, &task_id))
    {
        LOGE("[FTL ] bg tick task add failed\r\n");
    }

    for (BYTE die = 0U; die < DIE_MAX; die++)
    {
        if (!ftl_mig_timer_register(&g_ftl[die]))
        {
            LOGE("[FTL ] migrate timer register failed die=%hhu\r\n", die);
        }
    }
}

/**
 * @brief 取实例拓扑（只读）。
 * @param ftl FTL 上下文
 * @return 拓扑指针（非 NULL）
 */
const ftl_topo_s *ftl_get_topo(const ftl_ctx_s *ftl)
{
    return &ftl->topo;
}

/**
 * @brief 判定介质是否需要格式化。
 * @param ftl FTL 上下文
 * @param need_format 输出：true = 需要格式化
 * @return FTL_OK 判定完成；FTL_ERROR CP 介质读不通（调用者不得格式化）
 */
ftl_status_e ftl_boot_check_format(ftl_ctx_s *ftl, BOOL *need_format)
{
    return pfr_media_check_format(ftl, need_format);
}

/**
 * @brief 全量格式化当前实例的介质。
 * @param ftl FTL 上下文
 * @param verify true = 逐块擦除体检
 * @return FTL_OK 成功；FTL_ERROR 失败
 */
ftl_status_e ftl_boot_format(ftl_ctx_s *ftl, BOOL verify)
{
    return pfr_media_format(ftl, verify);
}

/**
 * @brief 执行启动恢复并输出报告。
 * @param ftl FTL 上下文
 * @param report 启动类型/Checkpoint/Journal 统计；可为 NULL
 * @return FTL_OK 成功；FTL_ERROR 恢复失败
 */
ftl_status_e ftl_boot_recover(ftl_ctx_s *ftl, ftl_boot_report_s *report)
{
    pfr_boot_result_s result;
    ftl_status_e      status = pfr_boot_recovery(ftl, &result);

    if ((status == FTL_OK) && (report != NULL))
    {
        report->boot_type       = (ftl_boot_type_e)result.boot_type;
        report->cp_sequence     = result.cp_sequence;
        report->journal_entries = result.journal_entries;
        report->blocks_restored = result.blocks_restored;
        report->scoped          = result.scoped;
    }
    return status;
}

/**
 * @brief 打印固定分区与数据区的坏块分布（开机基线）。
 * @param ftl FTL 上下文（只读）
 */
void ftl_boot_log_bad_areas(const ftl_ctx_s *ftl)
{
    pfr_boot_log_bad_areas(ftl);
}

/**
 * @brief 实例无关的每轮泵：推进 NAND + 完成派发
 *
 * 每轮只调一次；NAND 引擎与完成表是全局单份。
 */
void ftl_service_pump(void)
{
    // [pump] 推进 NAND：fc_service。
    // 第 1 次调用即把本轮所有完成抽干（fc_nand_dispatch_poll 全遍历 + sink 逐条消费），
    // 之后几次走填充轮次；预算因此封顶的是"本轮跑几次 fc_service"，不是完成条数。
    for (BYTE i = 0U; i < NAND_SUBMIT_BUDGET; i++)
    {
        if (fc_service() != FC_STATUS_OK)
        {
            break;
        }
    }

    // [pump] 完成泵：按 request.op 派发到服务函数数组（见 ftl_op.c）
    ftl_op_drain();
}

/**
 * @brief 单个 FTL 实例的服务（每轮每实例一次）
 *
 * 调用方须先 ftl_service_pump()；漏调实例其后台永不推进。
 *
 * @param ftl 实例
 */
void ftl_service(ftl_ctx_s *ftl)
{
    // 唯一调用点 main.c 传 ftl_instance_get(die)（结构性恒非空）；
    // NULL 属「不该发生」⇒ ASSERT（release 裁空后 NULL 解引用=HardFault 留现场，
    // 优于静默返回⇒该实例 journal/GC 永不推进的无声损坏）。
    ASSERT(ftl != NULL);

    // [svc] 元数据候选池重算：运行期元数据 BAD 后重算候选池（纯 RAM；等元数据 job 空闲才做）。
    // 必须早于本轮新 I/O：游标/候选表先一致，再允许分配与提交。
    md_pool_service(ftl);

    // [svc] 写确认结算：推进**待入账**的写确认。必须早于 host 补发——待入账意味着那个 LBA
    // 的 L2P 还是旧映射，先补发一条读同一 LBA 的子操作就会读到旧物理页（陈旧数据）。
    // 顺序约定：先处理**已完成**的，再下发新的。
    ftl_confirm_service(ftl);

    // [svc] cache 序列确认：推进 cache 写序列的「已编程页表」——按序确认（Journal→L2P→BST）、
    // 撤销失败页、弹出已证明页。组早已完成，这里只做元数据记账。
    ftl_cache_wr_confirm_step(ftl);

    // [svc] host 补发：request scheduler 按 HOST_READ 先于 HOST_WRITE 派发；
    // 组状态仍由 pending 持有，scheduler item 只承载调度坐标。
    ftl_sched_run_host(ftl);

    // 安全网：序列跨 host 段/命令保持打开等同一 LBA 流的下一段；孤儿序列超时关闭。
    ftl_cache_seq_idle_guard(ftl);

    // 有序收尾：写序列用 padding END（给提前回执的最后一页补 FAIL_CACHE 结算），
    // 读序列直接放弃。放在补发之后：本轮的窗口先发出去，再决定是否收尾。
    ftl_cache_seq_close_step(ftl);

    // [svc] 双副本写回：重试被序列门控拒绝的备副本提交。主副本已编程，
    // 不能因为第二次提交被门控就作废整对（那会白写一个页）。
    l2p_flush_resume(ftl->cache, topo_2_die(ftl->topo));

    // [svc] wbuf 空闲 flush：页打包写缓冲的**确定性空闲 flush**——补发循环跑完后，若再无写组会
    // 向当前窗口 append，就把缓冲编程掉（否则这些 host 写永不完成）。放在 bg_suspend
    // 判定之前：它是 host IO 的收尾，不是后台任务。
    {
        wb_trigger_s trigger;
        memset(&trigger, 0, sizeof(trigger));
        trigger.kind = WBUF_TRIGGER_MAYBE_FLUSH_IDLE;
        (void)wb_handle(ftl->wbuf, &trigger, NULL);
    }

    // [svc] 后台调度：工作量配额后台调度 —— 档位选择 / 10 项任务表 / 配额规模 / bg_suspend
    // 判定全在 ftl/bg_policy.c（本文件不持有后台策略）。
    bg_policy_service(ftl);

    // [svc] 唯一仲裁入口：host（本轮通常已由上面的快路径派发）→ 后台 job → L2P 写回，
    // 均按 FTL_SCHED_PRIO_E 顺序；后台 job 只执行 bg_policy 本轮已提交的 READY 项。
    ftl_sched_dispatch(ftl);
}
