/*
 * ftl.c — FTL 顶层（初始化 / 后台调度服务主循环 / 子操作下发 / 对外读写入口）
 *
 * 职责：
 *   FTL 的对外门面与主循环——ftl_init 装配各子模块；ftl_service 是**服务主循环**
 *   （NAND 泵 → 完成泵 → 补发 host 子操作 → 工作量配额后台调度）；ftl_submit_read/write
 *   把一条 host 请求拆成多个 FC 子操作下发；ftl_flush_begin/step / ftl_trim_begin/step
 *   供 HIL 调用（均为异步推进）。
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
 *          序列收尾 / wbuf 空闲 flush / 后台调度（bg_scheduler_service）
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
 *   9. 子操作下发      ftl_issue_subop / ftl_drain_group（static）+ ftl_submit_read / write
 *  10. 元数据落盘/TRIM ftl_flush_begin/step / ftl_trim_begin/step
 *
 * ⚠ 后台调度在 `ftl/bg_scheduler.c`：10 项任务表 / 档位 IDLE·NORMAL·BUSY → 预算规模 /
 *   mig_gc_evaluate（GC 统一触发求值）/ RR 巡检 / 后台调度入口 bg_scheduler_service；
 *   通用引擎也在同一文件。本文件不持有任何后台策略。
 *
 * ⚠ 页打包写缓冲在 `ftl/wbuf.c`（wbuf.h 定义 wb_handle_s），本文件只调用其接口。
 *
 * ⚠ 回收动作在 `ftl/migrate.c`：GC / WL / 预测性退役共用一台异步搬移 job
 *   （mig_gc_begin / mig_wl_begin / mig_predict_begin / mig_handle）。触发求值在
 *   `ftl/bg_scheduler.c`，写路径的按需 GC 在
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
#include "ftl_op.h" /* ftl_op_submit / FTL_OP_* */
#include "log.h"
#include "metadata_job.h"
#include "pending.h"
#include "ssd_compiler.h"
#include "timer.h"
#include "wbuf.h"

#include <string.h>

// FTL 每 I/O 热数据。链接脚本按整个 ftl.o 把 .bss 放进 DTCM。
// 这些对象都是 CPU-only：NAND/FC 的 DMA buffer 不在这里，DMA1/2 也访问不到 DTCM。
// `.dtcm_data` 是 NOLOAD ⇒ ftl_init 对每个实例显式 memset。
ftl_ctx_s   g_ftl[FTL_DIE_MAX];
wb_handle_s g_ftl_wbuf[FTL_DIE_MAX];
l2p_jrn_s   g_ftl_journal[FTL_DIE_MAX];

// dangling 守卫日志：
// · `st=BAD` 是**预期降级**（旧块已退役，数据本不可读）⇒ 每个旧 gblock 只打一行，
// 其余 LBA 只累计到该槽位的 lbas（一个块 256 个 LBA，逐条打必刷屏）。
// · `st=FREE/LOG/NONE` 是**非预期缺陷** ⇒ 仍用限流 ERROR，保留完整现场。
#define FTL_DANGLING_SLOTS (8U)

typedef struct FTL_DANGLING_ENTRY_S
{
    WORD  g;         /* 旧 gblock；lbas==0 表示空槽 */
    DWORD first_lba; /* 该块第一个命中的 LBA */
    DWORD lbas;      /* 该块累计命中的 LBA 数 */
} ftl_dangling_entry_s;

static ftl_dangling_entry_s s_dangling[FTL_DANGLING_SLOTS];

/* 定义在下方「确认路径」之后；三个减法入口（覆盖写 / 填洞未映射 / TRIM）共用。 */
static void ftl_dangling_check(ftl_ctx_s *ftl, DWORD l, DWORD old_pba, WORD new_g);
static BYTE s_dangling_next;

// 日志限流：同一类异常在短时间内重复出现时按间隔打一行；
// 时钟缺失时由 log_rate_due 按条数兜底。统一使用 log.h / timer.h 的
// log_ratelimit_s，本文件只保留一套限流类型（C9）。
static log_ratelimit_s s_dangling_log;        /* BAD 退役块的 dangling 聚合（1 s） */
static log_ratelimit_s s_cache_rd_err_log;    /* cache 读命令失败（1 s） */
static log_ratelimit_s s_confirm_log;         /* 待入账超时（5 s） */
static log_ratelimit_s s_flush_log;           /* flush 正常耗时（5 s） */
static log_ratelimit_s s_flush_err_log;       /* flush 失败 / 超时（5 s） */
static log_ratelimit_s s_lost_log;            /* 降级 region 读错误（5 s） */
static log_ratelimit_s s_nospace_log;         /* 无空间归因（5 s） */
static log_ratelimit_s s_subop_fail_log;      /* subop 永久失败（5 s） */
static log_ratelimit_s s_cache_wb_fail_log;   /* cache program 批次失败（5 s） */
static log_ratelimit_s s_seq_drain_stall_log; /* 序列 draining 兜底放锁（5 s） */

/* ── 内部辅助 ── */

static BOOL ftl_cache_wr_has_unproven(const ftl_ctx_s *ftl);
static void ftl_cache_wr_abort(ftl_ctx_s *ftl);
static void ftl_cache_wr_mark_failed(ftl_ctx_s *ftl, ftl_cache_page_s *pg, const char *why);
static BOOL ftl_cache_wr_verdict_take(ftl_ctx_s *ftl, DWORD lba, BOOL *out_ok);
static void ftl_cache_wr_verdict_clear(ftl_ctx_s *ftl);
static void ftl_cache_wr_verdict_store(ftl_ctx_s *ftl, DWORD lba, BOOL ok);

// 组下发机制（定义在文件后部；ftl_service 的 host 补发步先用到）。
/**
 * @brief 只清 RAM：关闭 cache 序列状态（不发任何设备命令）。
 *
 * @param ftl 实例。
 *
 * @note 设备侧未收 LAST 的缓存序列由随后的新命令终止（待上板确认）。
 *       本函数与 idle 守卫不在每 I/O 热路径上，放内部 Flash，不占 ITCM。
 */
void SSD_CODE_FAST ftl_cache_seq_release(ftl_ctx_s *ftl)
{
    // 判据必须含 `wr_inflight`：页在完成时才入表（早回执），组级失败可能正好落在窗口
    // 命令全在飞、页表为空时。此时干净清除（不升 gen）会让迟到完成把未证明页推进死
    // 序列；abort 升 gen 后迟到完成被丢弃、并按 IO_ERROR 结算组。
    if ((ftl->cache_stream.seq.kind == FTL_CACHE_SEQ_WRITE) &&
        ((ftl->cache_stream.seq.wr_inflight != 0U) || (ftl_cache_wr_has_unproven(ftl) == true)))
    {
        ftl_cache_wr_abort(ftl); /* 未证明/在飞页：按失败结算 + 升 gen + 关 SB + 清状态 */
        return;
    }

    // 序列被放弃时要分清「命令还在飞」与「已经排空」：前者必须继续关着门控，
    // 否则别的组会在 31h/3Fh 管线半途插命令；后者才真正放开。
    // 判据只数**序列自己拥有**的读子操作：读失败托管给 RR 的那一项已脱钩
    // （PENDING_OP_READ_RECOVERY），不占设备读管线。把它算成在飞会让 draining
    // 永远关着门，而 RR 的恢复读又被同一道门挡住 ⇒ 组永不完成，形成死锁。
    const WORD gi        = ftl->cache_stream.seq.owner_gi;
    const BOOL in_flight = (ftl->cache_stream.seq.kind == FTL_CACHE_SEQ_READ) && (gi < FTL_GROUP_MAX) && (ftl->pending.groups[gi].used == true) &&
                           (pend_group_has_op(&ftl->pending, gi, PENDING_OP_READ) == true);

    ftl->cache_stream.seq.kind     = FTL_CACHE_SEQ_NONE;
    ftl->cache_stream.seq.active   = false;
    ftl->cache_stream.seq.draining = in_flight;
    ftl_cache_wr_verdict_clear(ftl);
    ftl->cache_stream.seq.break_requested = false;
    ftl->cache_stream.seq.idle_ms         = 0U;
    ftl->cache_stream.seq.run_pairs       = 0U;
    ftl->cache_stream.seq.closing         = false;
    ftl->cache_stream.seq.close_requested = false;
    ftl->cache_stream.seq.own_page0       = false;
}

void SSD_CODE_FAST ftl_cache_seq_discard(ftl_ctx_s *ftl)
{
    if ((ftl->cache_stream.seq.kind == FTL_CACHE_SEQ_WRITE) &&
        ((ftl->cache_stream.seq.wr_inflight != 0U) || (ftl_cache_wr_has_unproven(ftl) == true)))
    {
        ftl_cache_wr_abort(ftl); /* 未证明/在飞页按失败结算（升 gen 丢弃迟到完成），下面再清门控 */
    }

    ftl->cache_stream.seq.kind            = FTL_CACHE_SEQ_NONE;
    ftl->cache_stream.seq.active          = false;
    ftl->cache_stream.seq.draining        = false;
    ftl->cache_stream.seq.owner_submit    = false;
    ftl->cache_stream.seq.break_requested = false;
    ftl->cache_stream.seq.close_requested = false;
    ftl->cache_stream.seq.closing         = false;
    ftl->cache_stream.seq.own_page0       = false;
    ftl->cache_stream.seq.idle_ms         = 0U;
    ftl->cache_stream.seq.yield_pending   = false;
    ftl->cache_stream.seq.yield_ms        = 0U;
    ftl_cache_wr_verdict_clear(ftl);
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
static void SSD_CODE_FAST ftl_cache_seq_yield(ftl_ctx_s *ftl)
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
static void SSD_CODE_FAST ftl_cache_seq_close(ftl_ctx_s *ftl, WORD gi)
{
    if ((ftl->cache_stream.seq.active == true) && (ftl->cache_stream.seq.owner_gi == gi))
    {
        ftl_cache_seq_release(ftl);
    }
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
            LOGW_IF(log_ratelimit_due(&s_seq_drain_stall_log), LOG_TAG_FTL "seq drain force-clear gi=%hu outstanding=%hu age=%lums (LUN gate reopened)\r\n", gi, ftl->pending.groups[gi].outstanding, now_ms - ftl->cache_stream.seq.idle_ms);

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

static void ftl_drain_group(ftl_ctx_s *ftl, WORD gi);

/**
 * @brief 待入账的依赖是否**确实有待推进的工作**。
 *
 * @param ftl 实例。
 * @return true = 至少一项有确定完成时刻的工作在推进：L2P region 加载 / region 写回 /
 *                Journal 页写 / Journal 换块擦除（`need_erase`）/ Journal 待刷条目 /
 *                **任何一个非待入账的 pending 项**（活的 FC 命令或 RR job）。
 *
 * @note 有 ⇒ 这次等待是**有限**的（各自都有完成点），应延长待入账超时预算而不是把后台拥塞
 *       结算成 host 写失败；全都没有 ⇒ 结构性卡死，仍让 `FTL_GROUP_DRAIN_TIMEOUT_MS`
 *       到期暴露。判据必须是**完成源**，不能按「组是否 used」计数：多个待入账项各占一个组、
 *       却没有任何命令在飞 ⇒ 按组计数会互相延长，超时永不触发。
 *       陷阱：Journal 换块擦除提交失败期间 `phase` 仍是 IDLE，只看 `jrn_is_idle()` 会漏掉
 *       这一类（表现为 `confirm park timeout`）。
 */
static BOOL ftl_confirm_progress_pending(const ftl_ctx_s *ftl)
{
    // 非待入账的 pending 项 = 活命令 / RR job / cache 成员：都有 NAND 完成点或超时。
    // 待入账项自己不算（否则自己等自己）。
    BOOL live_op = false;

    for (BYTE i = 0U; i < FTL_PENDING_MAX; i++)
    {
        if ((ftl->pending.slots[i].used == true) && (pend_confirm_is_parked(&ftl->pending, i) == false))
        {
            live_op = true;
            break;
        }
    }

    return (ftl->cache->inflight.load > 0U) || (ftl->cache->inflight.flush > 0U) || (jrn_is_idle(ftl->journal) == false) || (jrn_has_erase_pending(ftl->journal) == true) || (jrn_has_pending_entries(ftl->journal) == true) || (live_op == true);
}

/* 一页的「确认描述」：wbuf 路径从 pend_req_s 填、cache 路径从已编程页表填——
 * 单一定义，避免两套口径（C9）。 */
typedef struct FTL_PAGE_DESCRIPTOR_S
{
    DWORD lba;      /* 页基址（pair 对齐） */
    DWORD new_pba;  /* plane0 页 PBA */
    DWORD new_pba1; /* plane1 页 PBA */
    BYTE  cw_bitmap;
    BOOL  seq_page; /* 顺序页（cache program）⇒ Journal 走 jrn_append_seq */
} ftl_page_desc_s;

/**
 * @brief 写确认事务：region 未就绪即返回 FTL_AGAIN 继续待入账；就绪后按页型
 *        （顺序映射页 / 通用页）写 Journal，再更新 L2P。
 *
 * @param ftl 实例
 * @param d   页确认描述（页基址 / 两 plane 新 PBA / flat 位图 / 页型）
 *
 * @return FTL_OK    已全部应用
 *         FTL_AGAIN 有 region 未就绪或 Journal 暂不能接收（**一条都没动账**，调用者下轮重来）；
 *                   只有这两态——就绪前置挡住了唯一的失败源，第二遍不可能失败。
 *
 * @note · CW 级映射下 1 LBA = 1 CW：一个子操作覆盖 d->cw_bitmap 置位的连续**全局** LBA，
 * 每个 LBA 各自一条 Journal + 一条 L2P 更新（Journal 流量 ×4，已登记）。
 * · 目标页按 **LBA 的 plane 位**选：plane0 的 LBA 落 d->new_pba、plane1 的落
 * d->new_pba1。Policy B 下两 plane 独立选块，且「物理 plane 必须等于 LBA 的
 * plane 位」是页内 META 能还原出正确 LBA 的前提（ftl_config.h §三）。
 * · **不存 old_pba**：确认时按 LBA 重查 L2P 得"此刻被替换的映射"。同一 LBA 的两次
 * 写在飞时，先完成的先改 L2P，后完成的重查即得前者；提交时快照会让两条 Journal
 * 都指向最初映射，链断。⚠ **待入账没有削弱这条**：本函数在**确认时刻**重查
 * （待入账只是延后几轮，顺序仍按完成序），语义不变。
 * · valid_count 单位是 **CW**：新 CW +1、被替换的旧 CW -1。真有效性一律逐 CW
 * 由页内 META + L2P 反查现判（DRAM-less approach ③，无位图）——见 bst.h 文件头。
 * · ⚠ **两遍式**：第一遍只做**就绪前置**（region 未驻留则起异步加载并返
 * FTL_AGAIN）与 Journal 容量预留，第二遍才动账。绝不能边查边改——l2p_write 对
 * 正在加载/写回的行返 FTL_AGAIN，中途放弃会让同一页里部分 CW 已记账、部分没记，
 * 重试时重复记账（valid_count 虚高、journal 重复条目、total_writes 多计）。
 * **两遍之间无泵**：本函数不同步刷 Journal、不调用 ftl_sync_submit_wait；
 * Journal 容量不足时 jrn_reserve 只起异步刷页并返 FTL_AGAIN，确认阶段不碰
 * NAND、不重入完成派发 ⇒ 就绪判定与第二遍之间状态不可能变。
 */
static ftl_status_e ftl_confirm_write(ftl_ctx_s *ftl, const ftl_page_desc_s *d)
{
    // 三个调用点（host 完成路径 / cache 写确认步 / 写确认服务）都传非空 ftl 与描述符
    // ⇒ 不在本函数复检（§十附 / §二十六）。
    // 同理 d->lba 恒为**窗口基址**（唯一写点是 wb_handle(FLUSH) 的 wbuf.base）——
    // 下面用 ftl_lba_plane/cw(l) 取代"从位下标反解坐标"就依赖这条。

    // 页对基址：8 个 LBA 属于同一 region ⇒ 就绪只需探一次（原来逐 CW 8 次）。
    ASSERT(ftl_lba_is_pair_aligned(d->lba));

    const DWORD mask = d->cw_bitmap & ((1U << FTL_LBA_PER_PAIR) - 1U);

    if ((mask != 0U) && (l2p_entries_ready(ftl->cache, d->lba, mask) == false))
    {
        // 返回 AGAIN 交由待入账重试：预算是否延长由 `ftl_confirm_service` 按
        // 「确有可推进的工作」判定。加载**起不动**
        // （`load_inflight==0`）时该判据不成立 ⇒ 仍保留 100 ms 的有界失败。
        return FTL_AGAIN; /* 已起加载：本轮**一条都不动** */
    }

    // Journal 容量/坐标在动账前判定：不足或页型不符时只起异步刷页并返回 false，
    // 本函数直接待入账重试，绝不在这里同步刷 Journal（那会重入完成派发）。
    // 按最坏情况（mask 置位数）预留并留 1 个空槽 ⇒ 随后的 append 不会触发页满分支。
    // 两种页型走各自的入口：顺序写确认 = jrn_seq_ready + jrn_append_seq（4B 隐式
    // LBA），其余（wbuf / GC / page0）= jrn_reserve + jrn_append（16B 显式 LBA）。
    const BYTE n_entries = ftl_cw_bitmap_popcount(mask);
    // cache program 页（cache_role != NONE）属于顺序写流 ⇒ Journal 走顺序映射页
    // （4B/条、LBA 由页头基准 + 下标隐式给出）；其余（wbuf / GC / page0）走通用页。
    const BOOL seq_page  = d->seq_page;

    if (n_entries != 0U)
    {
        const BOOL reserved = (seq_page == true) ? jrn_seq_ready(ftl->journal, d->lba, n_entries)
                                                 : jrn_reserve(ftl->journal, n_entries);

        if (reserved == false)
        {
            // 同上：是否延长预算由 `ftl_confirm_service` 的进展判据决定
            // （Journal 页写 / 换块擦在飞 ⇒ 延长；完全空闲却取不到容量 ⇒ 到期暴露）。
            return FTL_AGAIN;
        }
    }

    DWORD oldp[FTL_LBA_PER_PAIR];
    DWORD newp[FTL_LBA_PER_PAIR];

    // 页内批量读：l2p_line_find 一次；只读取 mask 置位项（逐项记 hit，口径不变）。
    // 就绪前置已保证命中；非 OK 仅越界（不可能）⇒ 保守重试，尚未动账。
    if (l2p_entries_read(ftl->cache, d->lba, mask, oldp) != FTL_OK)
    {
        // 第三处 AGAIN：行在就绪检查之后变成 FLUSHING（写回在飞）⇒ 批量读被拒。
        // 预算延长同样由 `ftl_confirm_service` 判定（`flush_inflight > 0` 会命中），
        // 否则 100 ms 到期把一次**有限**的写回等待结算成 host 写失败。
        return FTL_AGAIN;
    }

    /* ── 先 Journal（掉电回放顺序），再一次性更新 L2P 行 ── */
    for (BYTE bit = 0U; bit < FTL_LBA_PER_PAIR; bit++)
    {
        if ((mask & (1U << bit)) == 0U)
        {
            continue;
        }

        const DWORD l    = (d->lba + bit);
        const BYTE  pl   = ftl_lba_plane(l);
        const BYTE  cw   = ftl_lba_cw(l);
        const DWORD page = (pl == 0U) ? d->new_pba : d->new_pba1;

        newp[bit] = pba_with_cw(page, cw);

        if (seq_page == true)
        {
            // 顺序页只记 new_pba；坐标/容量已由 jrn_seq_ready 全批判过 ⇒ 这里的
            // 失败是结构性簿记错误。丢弃条目会让已确认的写在掉电后丢映射，
            // 故 debug 留现场（C7：传错 lba 即可构造失败），release 也必须留下日志
            // （H14：ASSERT 与运行期留痕不是二选一）。
            const BOOL appended = jrn_append_seq(ftl->journal, l, newp[bit]);

            if (appended == false)
            {
                LOGE(LOG_TAG_FTL "seq journal append failed lba=%lu — mapping not persisted\r\n", l);
            }
            ASSERT(appended);
        }
        else
        {
            (void)jrn_append(ftl->journal, l, oldp[bit], newp[bit]);
        }
    }

    // 整页一次写 L2P（state=DIRTY + region 脏位只置一次）。就绪前置 + 两遍之间
    // 无泵 ⇒ 行始终保持 CLEAN/DIRTY，写不可能返 FTL_AGAIN；断言仅作结构破坏的
    // debug 探针（release 由前述不变量保证，不靠断言兜底）。
    const ftl_status_e wst = l2p_entries_write(ftl->cache, d->lba, mask, newp);

    ASSERT(wst == FTL_OK);
    (void)wst;

    for (BYTE bit = 0U; bit < FTL_LBA_PER_PAIR; bit++)
    {
        if ((mask & (1U << bit)) == 0U)
        {
            continue;
        }

        const DWORD l     = (d->lba + bit);
        const DWORD new_p = newp[bit];
        const WORD  newg  = ftl_pba_gblock(new_p);

        // 飞行记录器：记录成功确认的 host 写（kind=0）。
        alc_trace_record(&ftl->alloc, l, new_p, ALC_TRACE_WRITE_OK);

        ftl_block_add_valid(&ftl->alloc.blocks[newg]);

        // 覆盖写：旧 CW 失效。旧页若正是本页（同一 CW 重写）则不动计数。
        if ((oldp[bit] != NAND_PBA_INVALID) && (oldp[bit] != new_p))
        {
            const WORD gob = ftl_pba_gblock(oldp[bit]);

            ftl_dangling_check(ftl, l, oldp[bit], newg);

            // 坏块 / count 已为 0 时跳过：bbt_mark_bad 清过账，再减会断言/回绕。
            ftl_block_sub_valid_if_live(&ftl->alloc.blocks[gob]);
        }
    }

    jrn_watermark_update(ftl->journal);
    ftl->stats.total_writes++; /* 计「编程次数」（页打包后一页 = 一次编程），不是 CW 数 */
    return FTL_OK;
}

/**
 * @brief 覆盖/解除映射前的**常驻守卫**：L2P 指向的旧块在 BST 里 0 有效时留现场。
 *
 * 触发条件：`valid_count == 0` 却仍被 L2P 引用。此后每次覆盖写该 LBA 都会撞
 * `bst.h` 的 `valid_count > 0U` 断言；运行期被 `sub_valid_if_live` 静默夹住，
 * 但 journal 条目照样落盘 ⇒ 下次开机回放 `pfr_valid_sub` 判**下溢** ⇒ `ctx_ok=0`
 * ⇒ 整份块元数据快照作废（full metadata rebuild）。这是「重建漏认 / 分配器复用
 * 活块」类缺陷的**唯一故障点**，三个减法入口（覆盖写 / 填洞未映射 / TRIM）都必须过它。
 *
 * @param ftl     FTL 实例
 * @param l       LBA（日志定位）
 * @param old_pba L2P 里记录的旧物理地址
 * @param new_g   本次写入的新 gblock（仅用于日志对照）
 *
 * @note 状态一眼可判：BAD=坏块（预期降级）；FREE=已归还空闲池（会被复用，**丢数据**）；
 *       LOG=元数据日志块（分类判据过宽）；NONE=重建从未认领（页 0 判空/未扫描到）。
 */
static void ftl_dangling_check(ftl_ctx_s *ftl, DWORD l, DWORD old_pba, WORD new_g)
{
    const WORD gob = ftl_pba_gblock(old_pba);

    if (ftl->alloc.blocks[gob].valid_count != 0U)
    {
        return;
    }

    const bst_state_e old_st = ftl_block_state(&ftl->alloc.blocks[gob]);

    ftl->stats.dangling_total++;

    if (old_st == FTL_BLOCK_STATE_BAD)
    {
        // 预期降级：旧块已退役。每个旧 gblock 只打一行，其余 LBA 只累计。
        ftl_dangling_entry_s *slot = NULL;

        for (BYTE i = 0U; i < FTL_DANGLING_SLOTS; i++)
        {
            if ((s_dangling[i].lbas != 0U) && (s_dangling[i].g == gob))
            {
                slot = &s_dangling[i];
                break;
            }
        }

        if (slot == NULL)
        {
            slot = &s_dangling[s_dangling_next % FTL_DANGLING_SLOTS];
            s_dangling_next++;
            slot->g         = gob;
            slot->first_lba = l;
            slot->lbas      = 0U;

            LOGW(LOG_TAG_FTL "dangling g=%hu first_lba=%lu st=BAD newg=%hu (retired; later LBAs aggregated)\r\n", gob, l, new_g);
        }

        slot->lbas++;
        return;
    }

    // 非预期：FREE/LOG/NONE 都可能意味着重建/分配器/分类器缺陷。
    if (log_ratelimit_due(&s_dangling_log))
    {
        const char *old_state = ftl_block_state_name(old_st);
        const char *new_state = ftl_block_state_name(ftl_block_state(&ftl->alloc.blocks[new_g]));

        (void)LOGE(LOG_TAG_FTL "dangling lba=%lu old=%08lx g=%hu st=%s sb=%hu newg=%hu nst=%s\r\n", l, old_pba, gob, old_state, ftl->alloc.blocks[gob].sb_id, new_g, new_state);
    }
}

/* ── host I/O 完成的四个分支（ftl_host_io_callback 按 pending->op 与成败分派）── */

/**
 * @brief 读成功：按链序把数据拷给 caller。
 * desc 链序 == LBA 升序（plane-major + 段内 CW 升序，与 LBA 的 [plane][cw] 布局同序）
 * ⇒ 按链序连续拷出即 LBA 序。
 * ⚠ 返回值必须查：desc 链比请求短时尾部不写（ftl_cmd_read_data 只在这一种情况报错），
 * 不查就等于把未初始化缓冲当数据交给 host。失败 ⇒ 本次读的结果不可信，跳过随后的
 * ECC / read-disturb 记账（记账无意义）。
 *
 * @param ftl 实例（读 disturb / RR 趋势记在本实例的 `alloc.blocks[]`）
 * @param cmd 已完成的读命令（desc 链在此处仍有效，调用者随后才 release）
 * @param pending 对应的子操作记录（提供 buffer / n_cw / lba）
 * @param cmd_id 命令 id（仅用于错误日志定位）
 * @return SSD_STATUS_OK       数据已完整拷出
 *         SSD_STATUS_IO_ERROR 数据不足，结果不可信
 */
static ssd_status_e ftl_host_io_read_ok(ftl_ctx_s *ftl, fc_cmd_s *cmd, pend_req_s *pending, WORD cmd_id)
{
    DWORD data_size = FTL_IU_BYTES * pending->n_cw; /* 子操作字节数 */

    if (ftl_cmd_read_data(cmd, pending->buffer, data_size) != FTL_OK)
    {
        LOGE(LOG_TAG_FTL "read data short lba=%lu cmd=%hu\r\n", pending->lba, cmd_id);
        return SSD_STATUS_IO_ERROR;
    }

    // Read Recovery：逐 plane 记录 ECC 与 read disturb，用于预测性退役。
    // ⚠ 不能只记 `ftl_cmd_pba()` 的首个 desc：一个 multi-plane 子操作在 Policy B 下
    // 覆盖**两个独立块**，第二块的趋势会漏记。
    // ⚠ 须在 fc_command_release(cmd->cmd_id) 之前调用——desc 释放后失效。
    ftl_cmd_block_ecc_s blocks[PLANE_NUM];
    const BYTE          nblocks = ftl_cmd_block_ecc_list(cmd, blocks, PLANE_NUM);

    for (BYTE i = 0U; i < nblocks; i++)
    {
        rr_record_ecc(&ftl->alloc.blocks[blocks[i].gblock].rr, blocks[i].ecc_max);

        // read disturb 计数：读成功累加（周期归零由上层维护）；
        // 随下一次 Checkpoint 的磨损快照持久化。
        ftl->alloc.blocks[blocks[i].gblock].read_count++;
    }

    return SSD_STATUS_OK;
}

/**
 * @brief 写成功（非 cache 中间页）：逐 CW 确认 Journal + L2P + 有效计数。
 * ⚠ region 未就绪（正在异步加载）⇒ 把本项置为**待入账**：命令脱钩（cmd_id 置哨兵，本命令
 * 随即 release，id 会被复用）+ 组在飞数**不**递减（host 写组继续 pending），由
 * ftl_confirm_service 每轮推进。与读失败的托管同构（命令脱钩、组不结算），区别是推进者
 * 在本文件而非 RR 引擎。
 * 返回 true = 已待入账 ⇒ 调用者**直接 release 并 return**（勿再动组簿记）。
 *
 * @param ftl 实例
 * @param pending 已完成的写子操作记录（其命令由调用点随后 release）
 * @return true  因 region 未就绪已待入账
 *         false 已确认成功，按正常子操作完成继续
 */
static BOOL ftl_host_io_write_ok(ftl_ctx_s *ftl, pend_req_s *pending)
{
    ftl_page_desc_s d;

    d.lba       = pending->lba;
    d.new_pba   = pending->new_pba;
    d.new_pba1  = pending->new_pba1;
    d.cw_bitmap = pending->cw_bitmap;
    d.seq_page  = (pending->cache_role != PEND_CACHE_ROLE_NONE);

    if (ftl_confirm_write(ftl, &d) != FTL_AGAIN)
    {
        return false;
    }

    // 待入账：命令脱钩、组在飞数不递减；待入账项存 pending 表，推进由本文件的
    // 写确认服务负责（docs §5「Confirm → pending 服务」）。
    const WORD pending_idx = (pending - ftl->pending.slots);

    pend_confirm_park(&ftl->pending, pending_idx, pending->owners);
    return true;
}

/**
 * @brief 写确认服务：推进**待入账**的写确认（ftl_service 的 [svc] 步）。
 *
 * @param ftl 实例。
 *
 * @note 循环在 FTL（控制器写管线的形态：映射更新由 FTL 执行），pending 只提供
 *       表原语；结算顺序不变量由 pend_confirm_settle 维护。
 */
static void ftl_confirm_service(ftl_ctx_s *ftl)
{
    const DWORD now_ms = platform_now_ms();

    for (BYTE i = 0U; i < FTL_PENDING_MAX; i++)
    {
        if (pend_confirm_is_parked(&ftl->pending, i) == false)
        {
            continue;
        }

        pend_req_s *pending = &ftl->pending.slots[i];

        if (pending->park_ms == 0U)
        {
            // 懒初始化待入账时刻。⚠ 平台时钟缺失时 now_ms 恒 0 ⇒ park_ms 保持 0、
            // 超时永不触发（有界失败依赖平台时钟已在 main 的装配阶段注册）。
            pending->park_ms = now_ms;
        }

        ftl_page_desc_s d;

        d.lba       = pending->lba;
        d.new_pba   = pending->new_pba;
        d.new_pba1  = pending->new_pba1;
        d.cw_bitmap = pending->cw_bitmap;
        d.seq_page  = (pending->cache_role != PEND_CACHE_ROLE_NONE);

        const ftl_status_e st = ftl_confirm_write(ftl, &d);

        if (st == FTL_AGAIN)
        {
            // 预算按「无进展时长」计：确有完成源在推进
            // （region 加载或写回在飞、Journal 页写或换块擦、任一非待入账 pending 项）⇒
            // 刷新起点；全都没有才是结构性卡死，让 100 ms 到期暴露成一次写失败。
            if (ftl_confirm_progress_pending(ftl) == true)
            {
                pending->park_ms = now_ms;
                continue;
            }

            if (pend_confirm_is_expired(pending, now_ms) == false)
            {
                continue; /* region 仍在加载：保留待入账，下轮再来 */
            }

            // 限流：元数据日志区卡死时，每个 host 写都会超时（100ms 一个），
            // 逐条 ERR 会刷屏；5s 一行仍能看见问题。
            if (log_ratelimit_due(&s_confirm_log))
            {
                // 只在真正要打日志时统计。日志必须报「同时有多少条在等」——只打
                // 扫描到的第一条（常是 idx=0）会被误读成「只有一个 region 卡住」
                // 。计数含本轮尚未结算的项。
                const WORD parked = pend_confirm_get_count(&ftl->pending);

                LOGE(LOG_TAG_FTL "confirm park timeout lba=%lu idx=%hhu parked=%hu\r\n",
                     pending->lba,
                     i,
                     parked);
            }
        }
        else if (st != FTL_OK)
        {
            LOGE(LOG_TAG_FTL "confirm failed lba=%lu idx=%hhu\r\n", pending->lba, i);
        }

        const ssd_status_e status = (st == FTL_OK) ? SSD_STATUS_OK : SSD_STATUS_IO_ERROR;

        pend_confirm_settle(&ftl->pending, i, status);
    }
}

/**
 * @brief 读失败 ⇒ **托管**给 Read Recovery job（见 read_recovery.c）：
 * · pending 项**保留**（不 pend_remove）、组在飞数**不**递减 ⇒ host 读组继续
 * pending，等 RR 收尾时统一结算；
 * · cmd_id 置哨兵：本命令随即 release，其 id 会被后续 fc_command_alloc 复用，
 * 留旧值会让新命令的完成误撞上本项；
 * · 收尾（含一律报 IO_ERROR 的语义）在 rr_job_finish 里，见该处注释。
 * ⚠ 恢复**必须**异步：本函数跑在完成泵里，当场同步跑三级恢复就是泵里再开泵（嵌套泵），
 * 且整条 host 读流水线被一个坏页钉住。
 *
 * @param ftl 实例（RR 触发入口在本实例的 `read_recovery_handle`）
 * @param pending 失败的读子操作记录（原地改成 READ_RECOVERY 托管态）
 */
static void ftl_host_io_read_fail(ftl_ctx_s *ftl, pend_req_s *pending)
{
    pending->op = PENDING_OP_READ_RECOVERY;
    pend_detach_command(&ftl->pending, pending);

    // typed 触发：把 owner 交给 RR handle；RR busy 时该项仍留在 pending 表，
    // 当前 job 收尾后由 pending 端口的 take 领走（行为与旧扫描一致）。
    const WORD pending_idx = (pending - ftl->pending.slots);
    rr_owner_s owner;

    owner.owners      = pending->owners;
    owner.pending_idx = pending_idx;
    owner.upper_buf   = (BYTE *)pending->buffer;
    owner.lba         = pending->lba;
    owner.n_cw        = pending->n_cw;

    rr_trigger(&ftl->read_recovery_handle, &owner);
}

/**
 * @brief 写失败：行业坏块替换流程
 *
 * ① bbt_mark_bad（RAM only，后台 bbt_handle 持久化）——先标坏，让 ② 关闭 SB 时把该
 * plane 从 Closed 条目剔除。
 * ② 关闭当前 Open Super Block → 进 closed_sb_list；好 plane 随条目由 GC 回收。
 * Policy B 下两 plane 是独立块，坏 plane 不再被 GC 读/擦。
 * ③ L2P 不动 → 自动回滚到旧映射，旧数据完好。
 *
 * 失败 plane 定位：遍历该命令的 desc 链，逐 CW 取 status；失败 CW 的 desc->paa
 * 给出 plane ⇒ 两个 plane 各自的块都能当场标坏（不再依赖 GC 擦除时才发现 plane1）。
 *
 * @param ftl 实例
 * @param pending 失败的写子操作记录（提供本次分配的 new_pba / new_pba1）
 * @param completion 命令完成状态（提供 cmd_id，用于遍历 desc 链取逐 CW 状态）
 */
static void ftl_host_io_write_fail(ftl_ctx_s *ftl, pend_req_s *pending, const fc_cmd_status_s *completion)
{
    // 失败 plane 掩码：逐 desc 取 CW 状态 + desc->paa 的 plane。
    // FC 把 NAND per-CW FAIL 并进 desc（program 与 read 都并），plane1 的失败不再
    // 等到 GC 擦除才暴露。
    DWORD bad_plane_mask = 0U;

    fc_cmd_s *cmd = fc_command_id_to_command(completion->cmd_id);
    WORD      did = cmd->desc_id;

    while (did != FC_DESC_ID_INVALID)
    {
        const fc_desc_s *d = fc_descriptor_id_to_descriptor(did);

        if (d == NULL)
        {
            break;
        }
        if (fc_cw_status_has_error(d->status))
        {
            bad_plane_mask |= (1U << paa_to_plane(d->paa));
        }
        did = d->next_id;
    }

    if (bad_plane_mask == 0U)
    {
        // 没有逐 CW 失败位（如无 desc 的命令）：按 plane0 表项标坏。
        // 带 desc 的 program 现在由 FC 把 NAND per-CW FAIL 并进 desc ⇒ 正常走上面的掩码。
        bad_plane_mask = (1U << pba_to_plane(pending->new_pba));
    }

    char stbuf[48];

    (void)fc_cmd_status_str(completion->status, stbuf, sizeof(stbuf));
    LOGW(LOG_TAG_FTL "program-fail lba=%lu pl=0x%lx st=%s\r\n", pending->lba, bad_plane_mask, stbuf);

    if (((bad_plane_mask & 0x1U) != 0U) && (pending->new_pba != NAND_PBA_INVALID))
    {
        alc_trace_record(&ftl->alloc, pending->lba, pending->new_pba, ALC_TRACE_PROGRAM_FAIL);
        alc_trace_dump(&ftl->alloc, pending->lba, pending->new_pba);
        (void)bbt_mark_bad(&ftl->alloc, ftl_pba_gblock(pending->new_pba));
    }
    if (((bad_plane_mask & 0x2U) != 0U) && (pending->new_pba1 != NAND_PBA_INVALID))
    {
        alc_trace_record(&ftl->alloc, pending->lba, pending->new_pba1, ALC_TRACE_PROGRAM_FAIL);
        alc_trace_dump(&ftl->alloc, pending->lba, pending->new_pba1);
        (void)bbt_mark_bad(&ftl->alloc, ftl_pba_gblock(pending->new_pba1));
    }

    (void)alc_super_close(&ftl->alloc, FTL_TEMP_HOT);
}

// 透明重试日志节流：第 1 次立即打印，此后每 N 次一条（program-fail 风暴不刷屏）。
#define FTL_WRITE_RETRY_LOG_EVERY (64U)
static DWORD s_write_retry_logged;

/**
 * @brief 写失败后把同一页重发到新块（透明重试，主流 program-fail 恢复）
 *
 * 数据来源：旧命令的 desc 链在 `fc_command_release` 之前仍持有本页数据
 * （`ftl_cmd_read_data` 的既有契约，见 ftl_fc_interface.h）。因此不需要常驻
 * 页缓冲；只在失败时拷一次。
 *
 * 顺序：读回数据 → 分配新页对 → 建新命令（**链序**填数）→ 提交 →
 * 成功后才更新 pending 的 cmd_id/new_pba（提交失败时保持旧 id，让调用者
 * 仍能按本次 completion 的 cmd_id 收尾）。
 *
 * 其他 = 无法重试，调用者按原 IO_ERROR 收尾。
 *
 * @param ftl 实例（数据拷出到 `write_retry_buf`、新页对从本实例 allocator 分配）
 * @param pending 当前写子操作记录（提交成功后原地更新 cmd_id / new_pba / retry_count）
 * @param old_cmd 失败的旧命令（desc 链仍持有本页数据，调用者随后才 release）
 * @return FTL_OK    已提交（同一 pending 继续在飞，组在飞数**不**递减）
 *         FTL_AGAIN 瞬态资源不足（Free/OP 耗尽、命令槽满、提交背压），本次未提交
 *         FTL_ERROR 读回源数据失败或建链失败，无法重试
 */
static ftl_status_e ftl_write_retry(ftl_ctx_s *ftl, pend_req_s *pending, fc_cmd_s *old_cmd)
{
    const DWORD len = (DWORD)pending->n_cw * FTL_IU_BYTES;

    if (ftl_cmd_read_data(old_cmd, ftl->write_retry_buf, len) != FTL_OK)
    {
        if ((s_write_retry_logged++ % FTL_WRITE_RETRY_LOG_EVERY) == 0U)
        {
            LOGE(LOG_TAG_FTL "retry: page readback failed lba=%lu count=%lu\r\n", pending->lba, s_write_retry_logged);
        }
        return FTL_ERROR;
    }

    DWORD pba[PLANE_NUM];

    if (alc_super_open_alloc_pair(&ftl->alloc, ALC_SUPER_ALLOC_HOT, pba) != FTL_OK)
    {
        return FTL_AGAIN; /* Free/OP 耗尽：无法替换 */
    }

    for (BYTE p = 0U; p < PLANE_NUM; p++)
    {
        if (bit_mask_any(pending->cw_bitmap, ftl_cw_plane_mask(p)) && (pba_to_page(pba[p]) > 0U) && (alc_super_open_get_pba_state(&ftl->alloc, FTL_TEMP_HOT, pba[p]) != ALC_SUPER_PBA_OPEN_PAGE0_DONE))
        {
            alc_super_open_rewind_pair(&ftl->alloc, FTL_TEMP_HOT);
            return FTL_AGAIN;
        }
    }

    fc_cmd_s *cmd = fc_command_alloc();

    if (cmd == NULL)
    {
        alc_super_open_rewind_pair(&ftl->alloc, FTL_TEMP_HOT);
        return FTL_AGAIN;
    }
    const fc_cmd_id_t cmd_id = cmd->cmd_id;

    cmd->config = old_cmd->config;
    cmd->meta   = pending->lba;

    ftl_status_e st = ftl_cmd_setup_ppa_pair_cw(cmd, pba, pending->cw_bitmap);

    if (st == FTL_OK)
    {
        st = ftl_cmd_write_data(cmd, ftl->write_retry_buf, len);
    }
    if (st != FTL_OK)
    {
        (void)fc_command_release(cmd_id);
        alc_super_open_rewind_pair(&ftl->alloc, FTL_TEMP_HOT);
        return st;
    }

    if (ftl_op_submit(cmd, FTL_OP_HOST_IO, NULL) != FTL_OK)
    {
        (void)fc_command_release(cmd_id);
        alc_super_open_rewind_pair(&ftl->alloc, FTL_TEMP_HOT);
        return FTL_AGAIN;
    }

    // 提交成功后才改 pending：失败路径仍按旧 cmd_id 收尾（见函数头）。
    pend_bind(&ftl->pending, pending, cmd_id);
    pending->new_pba  = pba[0];
    pending->new_pba1 = pba[1];
    pending->retry_count++;

    if ((s_write_retry_logged++ % FTL_WRITE_RETRY_LOG_EVERY) == 0U)
    {
        LOGW(LOG_TAG_FTL "write retry lba=%lu cw=0x%02hhx retry=%hhu pba=0x%08lx/0x%08lx count=%lu\r\n", pending->lba, pending->cw_bitmap, pending->retry_count, pba[0], pba[1], s_write_retry_logged);
    }

    return FTL_OK;
}

/* ── host cache program 序列（PENDING_OP_WRITE_CACHE）──
 *
 * 早回执：页在**自身命令完成**时就算「已编程」——
 *   ① 立刻结算它的子操作（组完成 ⇒ host 看到完成），不再等下一命令的 FAILC；
 *   ② 页面进入「已编程页表」（`cache_stream.seq.wr`），等后继 FAILC 证明；
 *   ③ 证明之前读该 LBA 走页表覆盖（指到刚编程的 PBA，见 ftl_cache_wr_lookup）；
 *   ④ 证明失败 ⇒ 撤销映射 + 标记 region degraded + 粘性错误。
 *
 * 确认（Journal→L2P→BST）与组完成解耦：由 ftl_cache_wr_confirm_step() 每轮按序推进。
 */

/**
 * @brief 把一页登记进「已编程页表」，并把结论游标指向它。
 *
 * @param ftl 实例。
 * @param pg  页描述（pba / bitmap / 是否已由自身 STATUS 证明）。
 *
 * @return true = 入表；false = 环满（结构性错误，调用者断言）。
 */
static BOOL ftl_cache_wr_ring_push(ftl_ctx_s *ftl, const ftl_cache_page_s *pg)
{
    ftl_cache_wr_s *wr = &ftl->cache_stream.seq.wr;

    if (wr->count >= FTL_CACHE_SEQ_RING)
    {
        return false;
    }

    const BYTE idx = (BYTE)((wr->head + wr->count) % FTL_CACHE_SEQ_RING);

    wr->page[idx] = *pg;
    wr->count++;

    // 结论按 LBA 绑定（见 ftl.h）：本页若已有暂存结论，立即消费。
    BOOL verdict_ok = false;

    if (ftl_cache_wr_verdict_take(ftl, pg->lba, &verdict_ok) == true)
    {
        if (verdict_ok == true)
        {
            wr->page[idx].proven = true;
        }
        else
        {
            ftl_cache_wr_mark_failed(ftl, &wr->page[idx], "FAIL_CACHE (deferred)");
        }
    }
    return true;
}

/**
 * @brief 取走「下一条命令结论的落点」页，并清游标。
 *
 * @param ftl 实例。
 *
 * @return 待证页；NULL = 本命令没有上一页（新序列首条 / 上一页是 END 页）。
 */
static void ftl_cache_wr_verdict_clear(ftl_ctx_s *ftl)
{
    ftl_cache_wr_s *wr = &ftl->cache_stream.seq.wr;

    for (BYTE i = 0U; i < FTL_CACHE_SEQ_RING; i++)
    {
        wr->verdict[i].valid = false;
    }
    wr->verdict_next = 0U;
}

static void ftl_cache_wr_verdict_store(ftl_ctx_s *ftl, DWORD lba, BOOL ok)
{
    ftl_cache_wr_s      *wr   = &ftl->cache_stream.seq.wr;
    ftl_cache_verdict_s *slot = NULL;

    for (BYTE i = 0U; i < FTL_CACHE_SEQ_RING; i++)
    {
        if ((wr->verdict[i].valid == true) && (wr->verdict[i].lba == lba))
        {
            slot = &wr->verdict[i];
            break;
        }
    }

    if (slot == NULL)
    {
        for (BYTE i = 0U; i < FTL_CACHE_SEQ_RING; i++)
        {
            const BYTE idx = (BYTE)((wr->verdict_next + i) % FTL_CACHE_SEQ_RING);

            if (wr->verdict[idx].valid == false)
            {
                slot             = &wr->verdict[idx];
                wr->verdict_next = (BYTE)((idx + 1U) % FTL_CACHE_SEQ_RING);
                break;
            }
        }
    }

    if (slot == NULL)
    {
        /* 表满不应发生（在飞页 ≤ 环深）：覆盖轮转位。 */
        slot             = &wr->verdict[wr->verdict_next];
        wr->verdict_next = (BYTE)((wr->verdict_next + 1U) % FTL_CACHE_SEQ_RING);
    }

    slot->lba   = lba;
    slot->ok    = ok;
    slot->valid = true;
}

static BOOL ftl_cache_wr_verdict_take(ftl_ctx_s *ftl, DWORD lba, BOOL *out_ok)
{
    ftl_cache_wr_s *wr = &ftl->cache_stream.seq.wr;

    for (BYTE i = 0U; i < FTL_CACHE_SEQ_RING; i++)
    {
        if ((wr->verdict[i].valid == true) && (wr->verdict[i].lba == lba))
        {
            wr->verdict[i].valid = false;
            *out_ok              = wr->verdict[i].ok;
            return true;
        }
    }

    return false;
}

/**
 * @brief 弹出队头（已证明且已确认 / 已撤销）。
 *
 * @param ftl 实例。
 *
 * @note 只动 head/count，不搬元素——页表里的绝对下标（含结论游标）因此不会因
 *       弹出而漂移。
 */
static void ftl_cache_wr_ring_pop(ftl_ctx_s *ftl)
{
    ftl_cache_wr_s *wr = &ftl->cache_stream.seq.wr;

    if (wr->count == 0U)
    {
        return;
    }

    wr->head = (BYTE)((wr->head + 1U) % FTL_CACHE_SEQ_RING);
    wr->count--;
}

/**
 * @brief 页表里是否还有「等不到结论」的页（未证明且未判失败）。
 *
 * @param ftl 实例。
 *
 * @return true = 存在未证明页（序列释放时必须 abort）。
 */
static BOOL ftl_cache_wr_has_unproven(const ftl_ctx_s *ftl)
{
    const ftl_cache_wr_s *wr = &ftl->cache_stream.seq.wr;

    for (BYTE k = 0U; k < wr->count; k++)
    {
        const ftl_cache_page_s *pg = &wr->page[(BYTE)((wr->head + k) % FTL_CACHE_SEQ_RING)];

        if ((pg->proven == false) && (pg->state != FTL_CACHE_PAGE_FAILED))
        {
            return true;
        }
    }

    return false;
}

/**
 * @brief 在已编程页表里查**覆盖该 LBA 的最新**一页。
 *
 * @param ftl 实例。
 * @param lba 全局 host LBA。
 *
 * @return 命中的页；NULL = 页表里没有覆盖该 LBA 的页。
 *
 * @note 读覆盖层与失败拦截共用这一条扫描规则（C2）：从**最新**往回查，第一个覆盖
 *       该 LBA 的环项就定论 —— 后写必须盖住先写，绝不回退到更早的项（否则会把
 *       旧数据复活）。
 */
static const ftl_cache_page_s *ftl_cache_wr_find(const ftl_ctx_s *ftl, DWORD lba)
{
    const ftl_cache_wr_s *wr   = &ftl->cache_stream.seq.wr;
    const DWORD           flat = ftl_cw_bit(ftl_lba_plane(lba), ftl_lba_cw(lba));

    // 同一条扫描规则（读到它的两处：读覆盖层 / 失败拦截）：从**最新**往回查，
    // 第一个覆盖该 LBA 的环项就定论（后写必须盖住先写，绝不回退到更早的项）。
    for (BYTE k = wr->count; k > 0U; k--)
    {
        const ftl_cache_page_s *pg = &wr->page[(BYTE)((wr->head + k - 1U) % FTL_CACHE_SEQ_RING)];

        if ((lba < pg->lba) || (lba >= (pg->lba + FTL_LBA_PER_PAIR)))
        {
            continue;
        }
        if (bit_mask_any(pg->cw_bitmap, flat) == false)
        {
            continue;
        }

        return pg;
    }

    return NULL;
}

/**
 * @brief 读覆盖层：该 LBA 是否有「已编程、尚未进 L2P」的页。
 *
 * @param ftl     实例。
 * @param lba     全局 host LBA。
 * @param out_pba 命中时写入该 LBA 所在 plane 的**页基址 PBA**（调用者再按 cw 取）。
 *
 * @return true = 命中（读必须走 out_pba，不能读 L2P）。
 *
 * @note PROGRAMMED 用它；CONFIRMED（L2P 才是真值）或 FAILED（映射已作废）都返回
 *       false —— 都不回退到更早的环项。
 */
BOOL ftl_cache_wr_lookup(const ftl_ctx_s *ftl, DWORD lba, DWORD *out_pba)
{
    const ftl_cache_page_s *pg = ftl_cache_wr_find(ftl, lba);

    if ((pg == NULL) || (pg->state != FTL_CACHE_PAGE_PROGRAMMED))
    {
        return false; /* CONFIRMED：L2P 真值；FAILED：映射已作废 —— 都不回退 */
    }

    const BYTE  plane    = ftl_lba_plane(lba);
    const DWORD page_pba = (plane == 0U) ? pg->pba0 : pg->pba1;

    *out_pba = pba_with_cw(page_pba, ftl_lba_cw(lba));
    return true;
}

/**
 * @brief 该 LBA 是否落在**已判失败、尚未撤销**的 cache 页上。
 *
 * @param ftl 实例。
 * @param lba 全局 host LBA。
 *
 * @return true = 读该 LBA 必须报 IO_ERROR（不能读 L2P 里的可疑 PBA）。
 *
 * @note 用 ftl_cache_wr_find 的同一条扫描规则（从新到旧、第一个覆盖项定论）。
 *       失败页在撤销完成前仍留在环里，而它的 L2P 映射可能还指向编程失败的页
 *       ⇒ 这一层必须在 l2p_read 之前拦下。
 */
BOOL ftl_cache_wr_failed_covers(const ftl_ctx_s *ftl, DWORD lba)
{
    const ftl_cache_page_s *pg = ftl_cache_wr_find(ftl, lba);

    return (pg != NULL) && (pg->state == FTL_CACHE_PAGE_FAILED);
}

/**
 * @brief 该页所属的 hot SB 若仍打开则关闭它。
 *
 * @param ftl  实例。
 * @param pba0 plane0 页 PBA。
 * @param pba1 plane1 页 PBA。
 *
 * @note 先查再关：失败页可能属于一个早已关闭/换掉的块，无条件 close 会误关无关块。
 */
static void ftl_cache_wr_close_sb_if_open(ftl_ctx_s *ftl, DWORD pba0, DWORD pba1)
{
    const BOOL in_sb = ((alc_super_open_get_pba_state(&ftl->alloc, FTL_TEMP_HOT, pba0) != ALC_SUPER_PBA_NOT_OPEN) ||
                        (alc_super_open_get_pba_state(&ftl->alloc, FTL_TEMP_HOT, pba1) != ALC_SUPER_PBA_NOT_OPEN));

    if (in_sb == true)
    {
        (void)alc_super_close(&ftl->alloc, FTL_TEMP_HOT);
    }
}

/**
 * @brief 把一页标记为失败：关 hot SB、region 立即降级、置粘性 deferred error。
 *
 * @param ftl 实例。
 * @param pg  失败页。
 * @param why 失败来源（日志用：FAIL_CACHE / abort / pad …）。
 *
 * @note 撤销（L2P 作废）交给 confirm_step：它可能要等 Journal 容量，不能在这里同步做。
 *       region 降级**立即**置位，让撤销完成前的读也报 IO_ERROR。
 *       `cache_wb_aborts` 按**命令级中止**计一次（member_done 的错误分支），这里不计，
 *       否则一条 FAILC 会同时经「结论落点失败」与「本命令中止」记两笔。
 */
static void ftl_cache_wr_mark_failed(ftl_ctx_s *ftl, ftl_cache_page_s *pg, const char *why)
{
    pg->state  = FTL_CACHE_PAGE_FAILED;
    pg->proven = false;

    ftl_cache_wr_close_sb_if_open(ftl, pg->pba0, pg->pba1);
    ftl->flags.deferred_error = true;

    // region 降级**立即**置位（不等撤销）：FAILC 之后该页的 L2P 映射还在，读会指到
    // 编程失败的页；降级位让「撤销完成前」的读也报 IO_ERROR，而不是返回可疑页内容。
    l2p_note_region_lost(ftl->cache, lba_to_global_region(pg->lba), pg->lba, pg->cw_bitmap);

    LOGE_IF(log_ratelimit_due(&s_cache_wb_fail_log), LOG_TAG_FTL "cache-wr failed lba=%lu (%s) — close hot SB\r\n", pg->lba, why);
}

/* 按 LBA 落 FAIL_CACHE 结论；页未入表则暂存。返回 true = 失败结论。 */
static BOOL ftl_cache_wr_verdict_deliver(ftl_ctx_s *ftl, DWORD lba, BOOL ok)
{
    ftl_cache_wr_s *wr = &ftl->cache_stream.seq.wr;

    for (BYTE k = wr->count; k > 0U; k--)
    {
        ftl_cache_page_s *pg = &wr->page[(BYTE)((wr->head + k - 1U) % FTL_CACHE_SEQ_RING)];

        if ((lba < pg->lba) || (lba >= (pg->lba + FTL_LBA_PER_PAIR)))
        {
            continue;
        }
        if (bit_mask_any(pg->cw_bitmap, ftl_cw_bit(ftl_lba_plane(lba), ftl_lba_cw(lba))) == false)
        {
            continue;
        }

        if ((pg->proven == false) && (pg->state != FTL_CACHE_PAGE_FAILED))
        {
            if (ok == true)
            {
                pg->proven = true;
            }
            else
            {
                ftl_cache_wr_mark_failed(ftl, pg, "FAIL_CACHE");
            }
        }
        return (ok == false);
    }

    ftl_cache_wr_verdict_store(ftl, lba, ok);
    return (ok == false);
}

/**
 * @brief 撤销一页失败写：把该页覆盖的 LBA 从 L2P 里作废（Journal 先行，与 TRIM 同源）。
 *
 * @param ftl 实例。
 * @param pg  已判失败的页。
 *
 * @return FTL_OK    已作废
 *         FTL_AGAIN region 加载 / Journal 容量未就绪（下轮重试）
 *         FTL_ERROR L2P 读失败（LBA 越界 ⇒ 编程错误，按丢弃处理）
 *
 * @note region 的 degraded 标记在判失败时就置好了（mark_failed），这里只作废映射。
 */
static ftl_status_e ftl_cache_wr_undo_page(ftl_ctx_s *ftl, const ftl_cache_page_s *pg)
{
    for (BYTE bit = 0U; bit < FTL_LBA_PER_PAIR; bit++)
    {
        const DWORD l = (pg->lba + bit);

        // flat 位图：bit = plane*CW_NUM + cw（页对覆盖 2 plane × 4 CW）。走 helper 而不是
        // 手写 `1U << bit`——同一个坐标在页表里只有这一套（C9/H15）。
        if (bit_mask_any(pg->cw_bitmap, ftl_cw_bit(ftl_lba_plane(l), ftl_lba_cw(l))) == false)
        {
            continue;
        }

        if (l2p_is_ready(ftl->cache, l) == false)
        {
            return FTL_AGAIN; /* 已起异步加载：下轮再来 */
        }
        if (jrn_reserve(ftl->journal, 1U) == false)
        {
            return FTL_AGAIN; /* 已起异步刷页 */
        }

        DWORD old = NAND_PBA_INVALID;

        const ftl_status_e rst = l2p_read(ftl->cache, l, &old);

        if (rst == FTL_AGAIN)
        {
            return FTL_AGAIN;
        }
        if (rst != FTL_OK)
        {
            return FTL_ERROR;
        }
        if (old == NAND_PBA_INVALID)
        {
            continue; /* 本来就未映射 */
        }

        (void)jrn_append(ftl->journal, l, old, NAND_PBA_INVALID);

        const ftl_status_e wst = l2p_write(ftl->cache, l, NAND_PBA_INVALID);

        ASSERT(wst == FTL_OK);
        (void)wst;

        // 解除映射也要过守卫：count 已为 0 时运行期静默夹住，但 journal 已记账
        // ⇒ 下次开机回放 `pfr_valid_sub` 判下溢、整份块元数据快照作废。
        ftl_dangling_check(ftl, l, old, FTL_GBLOCK_INVALID);
        ftl_block_sub_valid_if_live(&ftl->alloc.blocks[ftl_pba_gblock(old)]);
    }

    return FTL_OK;
}

/**
 * @brief 推进「已编程页表」：按序确认 / 撤销 / 弹出已证明的页。
 *
 * @param ftl 实例。
 *
 * @note 每轮 ftl_service 调用一次。顺序 = 提交顺序（Journal 章节也因此有序）。
 *       遇到确认/撤销的瞬态背压（region 加载、Journal 容量）就地停下，下轮继续。
 */
static void ftl_cache_wr_confirm_step(ftl_ctx_s *ftl)
{
    ftl_cache_wr_s *wr = &ftl->cache_stream.seq.wr;

    while (wr->count > 0U)
    {
        ftl_cache_page_s *pg = &wr->page[wr->head];

        if (pg->state == FTL_CACHE_PAGE_FAILED)
        {
            const ftl_status_e st = ftl_cache_wr_undo_page(ftl, pg);

            if (st == FTL_AGAIN)
            {
                return;
            }
            if (st != FTL_OK)
            {
                // 硬错误只可能是 L2P 读的 LBA 越界（编程错误）：本页已判失败、region 已
                // 降级，丢弃该项避免永久卡住队头（flush 屏障会一直等它）。
                LOGE(LOG_TAG_FTL "cache-wr undo failed lba=%lu — drop entry\r\n", pg->lba);
            }
            ftl_cache_wr_ring_pop(ftl);
            continue;
        }

        if (pg->state == FTL_CACHE_PAGE_PROGRAMMED)
        {
            ftl_page_desc_s d;

            d.lba       = pg->lba;
            d.new_pba   = pg->pba0;
            d.new_pba1  = pg->pba1;
            d.cw_bitmap = pg->cw_bitmap;
            d.seq_page  = true;

            if (ftl_confirm_write(ftl, &d) != FTL_OK)
            {
                return; /* FTL_AGAIN：背压，下轮再来 */
            }
            pg->state = FTL_CACHE_PAGE_CONFIRMED;
        }

        if (pg->proven == false)
        {
            // 未来结论只可能来自后继命令的 FAILC 或 padding END；都没有时判失败并撤销，
            // 否则页表排不空（flush 只能无进展超时）。`wr_inflight` 独立于 kind：序列已被
            // 释放（kind 清空）时，在飞成员的 FAILC 仍可能证明本页。
            const ftl_cache_seq_s *seq             = &ftl->cache_stream.seq;
            const BOOL             may_get_verdict = (seq->wr_inflight != 0U) ||
                                         ((seq->kind == FTL_CACHE_SEQ_WRITE) &&
                                          ((seq->active == true) || (seq->closing == true) || (seq->close_requested == true)));

            if (may_get_verdict == true)
            {
                return; /* 等后继命令的 FAIL 结论；确认已完成，读走 L2P */
            }

            ftl_cache_wr_mark_failed(ftl, pg, "no verdict");
            continue; /* 下一轮按 FAILED 撤销并弹出 */
        }

        ftl_cache_wr_ring_pop(ftl);
    }
}

/**
 * @brief 中止写序列：未证明页一律按 IO_ERROR 结算、关闭 hot SB、清序列状态。
 *
 * @param ftl 实例。
 *
 * @note 只处理仍在本序列页表里的页：**器件已证明**的页保留（数据有效，交给确认路径），
 *       已判失败的页不重复记账。在飞成员（尚未进页表的命令）由完成入口的序列代号
 *       守卫丢弃（失败序列其后页一律不确认）。代号在这里递增，
 *       是本函数与迟到完成的唯一分界。
 */
static void ftl_cache_wr_abort(ftl_ctx_s *ftl)
{
    ftl_cache_wr_s *wr = &ftl->cache_stream.seq.wr;

    for (BYTE k = 0U; k < wr->count; k++)
    {
        ftl_cache_page_s *pg = &wr->page[(BYTE)((wr->head + k) % FTL_CACHE_SEQ_RING)];

        if (pg->proven == true)
        {
            continue; /* 器件已证明该页编程成功：数据有效，交给 confirm_step 正常确认并弹出
                       * （早回执下它可能还没进 L2P，但不是「失败」） */
        }
        if (pg->state == FTL_CACHE_PAGE_FAILED)
        {
            continue; /* 已判失败（如 FAILC 落点）：等 confirm_step 撤销，不重复记账 */
        }

        ftl_cache_wr_mark_failed(ftl, pg, "abort");
    }

    ftl_cache_wr_verdict_clear(ftl);
    ftl->cache_stream.seq.kind            = FTL_CACHE_SEQ_NONE;
    ftl->cache_stream.seq.gen             = (BYTE)(ftl->cache_stream.seq.gen + 1U); /* 迟到完成 ⇒ 代号不符 */
    ftl->cache_stream.seq.active          = false;
    ftl->cache_stream.seq.break_requested = false;
    ftl->cache_stream.seq.idle_ms         = 0U;
    ftl->cache_stream.seq.run_pairs       = 0U;
    ftl->cache_stream.seq.closing         = false;
    ftl->cache_stream.seq.close_requested = false;
    ftl->cache_stream.seq.own_page0       = false;
}

/**
 * @brief cache program 成员的完成入口（早回执）。
 *
 * 命令 i 完成时：
 *  1. 代号不符（序列已中止 / 已被新序列取代）⇒ 迟到完成：只按 IO_ERROR 结算组，不碰页表；
 *  2. 状态带 FAIL_CACHE ⇒ **结论游标页**（= page i-1）判失败；否则该页被证明成功；
 *  3. 状态带 NAND 级错误 **或** FAIL_CACHE ⇒ 序列中止：本页不入表、按 IO_ERROR 结算，
 *     未证明页一律不确认，器件已证明的页保留；
 *  4. 否则本页已编程 ⇒ 入表 + **立即结算子操作**（组完成 ⇒ host 早回执）。
 *
 * @note 找不到 pending 的迟到完成由调用者兜底（本函数只处理仍登记在序列里的页）。
 */
static void ftl_cache_wr_member_done(ftl_ctx_s *ftl, pend_req_s *pending, const fc_cmd_status_s *completion)
{
    const BYTE  idx      = (BYTE)(pending - ftl->pending.slots);
    const DWORD page_lba = pending->lba;
    const DWORD owners   = pending->owners;

    // 在飞成员数：flush 屏障与「开新序列」都等它归零（页在完成时才进页表，
    // 只看 wr.count 会漏掉仍在编程的页）。所有出口都必须走到这里一次。
    ASSERT(ftl->cache_stream.seq.wr_inflight > 0U);
    ftl->cache_stream.seq.wr_inflight--;

#if defined(FC_NAND_TEST_ENABLE)
    // 测试镜像专属注入：在本条 cache program 命令的完成上伪造 FAIL_CACHE。
    // 器件语义 = 第 N 条命令的 70h 状态报**上一页**的结论（op44/op48 映射，op49 首页
    // 不映射）⇒ 注入到某页的完成 = 判它的**前一页**失败。
    // 生产构建整段裁掉：连 completion 拷贝都不存在（宏隔离，H14）。
    fc_cmd_status_s completion_used = *completion;

    if (fi_cache_fail_take(page_lba))
    {
        completion_used.status |= FC_CMD_STATUS_FAIL_CACHE; /* 常量落在 8 位字段内（H48 ②） */
    }
    completion = &completion_used;
#endif

    // 迟到完成：所属写序列已中止 / 已被新序列取代（代号不符）⇒ 本页不确认映射
    // （失败序列其后页一律不确认，否则 rebuild 的 page0 捷径会被击穿）。
    if (pending->cache_gen != ftl->cache_stream.seq.gen)
    {
        LOGW_IF(log_ratelimit_due(&s_cache_wb_fail_log), LOG_TAG_FTL "cache-wr late completion lba=%lu gen=%hhu stale — drop page\r\n", page_lba, pending->cache_gen);

        pend_release_index(&ftl->pending, idx);
        pend_group_complete_subop_multi(&ftl->pending, owners, SSD_STATUS_IO_ERROR);
        return;
    }

    const DWORD status     = completion->status;
    const BOOL  is_end     = (pending->cache_role == PEND_CACHE_ROLE_END);
    const BOOL  fail_cache = ((status & FC_CMD_STATUS_FAIL_CACHE) != 0U);
    const BOOL  own_fail   = ((status & (FC_CMD_STATUS_NAND_FAIL | FC_CMD_STATUS_TIMEOUT | FC_CMD_STATUS_DATA_ERROR)) != 0U);

    // 1) 本命令的 FAILC 按**提交序前一页**的 LBA 落结论（建链时记在 pending 槽里；
    //    页未入表则暂存，入表时消费 ⇒ 完成顺序反转不影响正确性）。
    BOOL verdict_failed = false;

    if (pending->verdict_valid == true)
    {
        verdict_failed = ftl_cache_wr_verdict_deliver(ftl, pending->verdict_lba, (fail_cache == false));
    }

    // 2) 序列中止：① 本命令自身编程失败；② 本命令的 FAILC 判了前一页失败。
    //    为什么 FAILC 也要中止：器件级 FAIL_CACHE 表示**缓存程序管线**里上一页没编程
    //    成功，后续页还在同一条可疑管线上；依赖「任一错误 ⇒ 其后页一律
    //    不确认」（rebuild 的「page0 空白 ⇒ 整块空白」捷径），继续写下去会击穿它。
    //    本页不入表、按 IO_ERROR 结算组；控制器已提交即回执，失败由 deferred error
    //    在下一条命令上报。
    //    `verdict_valid == false` 说明本命令是第一页（first，前无本序列的页）：器件对
    //    op49 不映射 FAILC，此时该位只能是上一序列的残留 ⇒ 忽略。
    if ((own_fail == true) || (verdict_failed == true))
    {
        char stbuf[48];
        (void)fc_cmd_status_str(status, stbuf, sizeof(stbuf));
        ftl->cache_stream.wb_aborts++;

        LOGE_IF(log_ratelimit_due(&s_cache_wb_fail_log), LOG_TAG_FTL "cache-wr cmd failed lba=%lu st=%s\r\n", page_lba, stbuf);

        ftl_cache_wr_close_sb_if_open(ftl, pending->new_pba, pending->new_pba1);
        ftl->flags.deferred_error = true;

        // 顺序：先中止序列，再结算组。组完成会回调 `on_complete`，回调可以重入
        // `ftl_submit_*`（`pend_group_maybe_complete` 的既有契约）；若结算在前，
        // 回调可能已经开出一条新序列，随后的 abort 会把**新序列**一起中止。
        ftl_cache_wr_abort(ftl); /* 其余页：已回执的撤销、未证明的丢弃 */

        pend_release_index(&ftl->pending, idx);
        pend_group_complete_subop_multi(&ftl->pending, owners, SSD_STATUS_IO_ERROR);
        return;
    }

    // 3) 本页已编程 ⇒ 入表 + **早回执**。
    //    page0 屏障记账（cache 路径自己打开的块）：成功编程即记，与确认解耦。
    if (pba_to_page(pending->new_pba) == 0U)
    {
        alc_super_open_note_page_programmed(&ftl->alloc, FTL_TEMP_HOT, pending->new_pba);
        alc_super_open_note_page_programmed(&ftl->alloc, FTL_TEMP_HOT, pending->new_pba1);
    }

    {
        ftl_cache_page_s pg;

        pg.lba       = page_lba;
        pg.pba0      = pending->new_pba;
        pg.pba1      = pending->new_pba1;
        pg.cw_bitmap = pending->cw_bitmap;
        pg.state     = FTL_CACHE_PAGE_PROGRAMMED;
        pg.proven    = is_end; /* END 的 FAIL 位就是它自己的结论，不会有后继命令 */

        const BOOL wr_ring_pushed = ftl_cache_wr_ring_push(ftl, &pg);

        if (wr_ring_pushed == false)
        {
            // 环满属结构性错误：环深 16 ≫ 在飞窗口（≤4 页/窗口 + 前窗尾页）。
            // release 下不能只靠 ASSERT：按 OK 结算会让这页永不确认，
            // host 已看到成功的写静默丢失。按失败中止序列 + 本组报 IO_ERROR（host 重试）。
            LOGE_IF(log_ratelimit_due(&s_cache_wb_fail_log), LOG_TAG_FTL "cache-wr page table full lba=%lu — abort sequence\r\n", page_lba);
            ASSERT_MSG(wr_ring_pushed, "cache-wr page table full lba=%lu", page_lba);
            ftl_cache_wr_abort(ftl);
            pend_release_index(&ftl->pending, idx);
            pend_group_complete_subop_multi(&ftl->pending, owners, SSD_STATUS_IO_ERROR);
            return;
        }
    }

    pend_release_index(&ftl->pending, idx);
    pend_group_complete_subop_multi(&ftl->pending, owners, SSD_STATUS_OK);
}

/* ── 写序列的有序收尾（padding END）── */

// padding 页镜像：整页零。页不被任何 L2P 引用（META = UNKNOWN），只用于给
// 已经提前回执的最后一页提供 FAIL_CACHE 结算点。ftl.o 的 .bss 随整文件规则落
// DTCM（NOLOAD）⇒ 启动不清零，由 ftl_global_init 显式 memset；只读使用。
static BYTE s_cache_pad_page[FTL_LBA_PER_PAIR * FTL_IU_BYTES];

/**
 * @brief 为打开中的写序列提交一条 padding END（48h）。
 *
 * @param ftl 实例
 * @return FTL_OK    已提交（seq->closing = true，门控继续关到完成）
 *         FTL_AGAIN 页对 / 命令资源不足（已登记紧急 GC，下轮重试）
 *         FTL_ERROR 建链失败
 *
 * @note 为什么需要它：设备序列没有"无数据 END"。写序列在 host 命令边界保持打开
 *       （提前回执模型），当下一段 LBA 不连续 / host 空闲 / flush 到来时，必须
 *       用一个**数据页**把最后已提交页的 FAIL_CACHE 结算出来。本仓用一个不映射的
 *       零页承担这个角色（每段最多一次，≈0.4% 额外页写）。
 */
static SSD_CODE_FAST ftl_status_e ftl_cache_pad_submit(ftl_ctx_s *ftl)
{
    ftl_cache_seq_s *seq = &ftl->cache_stream.seq;

    if ((seq->active != true) || (seq->kind != FTL_CACHE_SEQ_WRITE) || (seq->closing == true))
    {
        return FTL_OK;
    }

    DWORD pba[PLANE_NUM];

    if (alc_super_open_alloc_pair(&ftl->alloc, ALC_SUPER_ALLOC_HOT, pba) != FTL_OK)
    {
        mig_gc_request_urgent(ftl->mig_handle);

        // ⚠ 无空闲页 ⇒ 收尾页也提交不了。若继续等（序列门控 + GC 需要 Journal 容量），
        // 会与 GC 互相等待（livelock）：GC 的 jrn_reserve 被序列门控拒绝，而序列又
        // 需要 GC 腾出的页才能收尾。**放弃本序列**换活性：未确认页按 IO_ERROR 结算
        // （host 已提前回执 ⇒ deferred error），门控放开后 GC 可以刷 Journal 页并回收。
        ftl_cache_wr_abort(ftl);
        return FTL_ERROR;
    }

    fc_cmd_s          *cmd = NULL;
    const ftl_status_e st  = ftl_async_write_pair_cmd(&cmd, pba, s_cache_pad_page, FTL_CACHE_FULL_CW, OP_TWO_PLANE_PROGRAM_CACHE_END, FTL_PAGE_CFG_DATA_ANY());

    if (st != FTL_OK)
    {
        alc_super_open_rewind_pair(&ftl->alloc, FTL_TEMP_HOT);
        return st;
    }

    cmd->config.chain = FC_CHAIN_END;

    // 序列所有者通道：本命令就是该 LUN 序列的收尾，门控必须旁路。
    const BOOL owner_prev     = seq->owner_submit;
    seq->owner_submit         = true;
    const ftl_status_e sub_st = ftl_op_submit(cmd, FTL_OP_CACHE_PAD, NULL);
    seq->owner_submit         = owner_prev;

    if (sub_st != FTL_OK)
    {
        (void)fc_command_release(cmd->cmd_id);
        alc_super_open_rewind_pair(&ftl->alloc, FTL_TEMP_HOT);
        return sub_st;
    }

    // 记账用**真实**两 plane 地址：Policy B 下它们可能是不同的物理块，不能用
    // pba_with_plane() 从 plane0 推导（否则 plane1 的 page0 屏障会漏记，见回调）。
    seq->pad_pba[0] = pba[0];
    seq->pad_pba[1] = pba[1];

    // pad 的 FAIL_CACHE 指向序列最后一页（提交时确定，按 LBA 绑定）。
    seq->pad_verdict_valid = (seq->wr.count != 0U);
    seq->pad_verdict_lba   = (seq->next_lba >= FTL_LBA_PER_PAIR) ? (seq->next_lba - FTL_LBA_PER_PAIR) : 0U;

    seq->closing         = true;
    seq->close_requested = false;
    seq->break_requested = false;
    return FTL_OK;
}

#if defined(FC_NAND_TEST_ENABLE)
ftl_status_e ftl_cache_test_pad_submit(ftl_ctx_s *ftl)
{
    return ftl_cache_pad_submit(ftl);
}
#endif

/**
 * @brief padding 收尾页完成服务
 *
 * 按 FAILC 判定上一页成败（收尾页自身不映射数据），并补 page0 屏障记账。
 *
 * @param ftl        实例
 * @param completion FC 完成状态
 * @param ctx        恒 NULL（序列状态在 ftl->cache_stream.seq）
 */
void SSD_CODE_FAST ftl_cache_pad_callback(ftl_ctx_s *ftl, const fc_cmd_status_s *completion, void *ctx)
{
    // ctx 恒 NULL（见 ftl_op.h 契约）；序列状态在 ftl->cache_stream.seq。
    (void)ctx;

    ftl_cache_seq_s *seq = &ftl->cache_stream.seq;

    // 本服务函数拥有命令，下面**任何**退出路径都必须释放（H1）。必须先取再判：
    // 序列已死时的早退若直接 return，会把 command / desc 链 / buffer 一起漏掉
    // （⇒ 反复触发耗尽 FC 池）。
    fc_cmd_s *cmd = fc_command_id_to_command(completion->cmd_id);

    // 收尾页在飞期间序列被中止（盘满 abort / 失败 abort）⇒ 这条回调属于已死的序列：
    // 不能再动 allocator 的 page0/SB 记账（那会误关新块的 SB）。
    if ((seq->kind != FTL_CACHE_SEQ_WRITE) || (seq->closing == false))
    {
        if (cmd != NULL)
        {
            (void)fc_command_release(cmd->cmd_id);
        }
        return;
    }

    // 收尾页自身的结果（NAND_FAIL）不映射任何数据 ⇒ 只取 FAIL_CACHE 判定**上一页**；
    // 超时意味着上一页结论未知，按失败处理（保守）。
    const BOOL prev_ok = ((completion->status & (FC_CMD_STATUS_FAIL_CACHE | FC_CMD_STATUS_TIMEOUT)) == 0U);

    // padding 页落在新块 page0 时承担 page0 异步写屏障：成功必须记账，否则
    // allocator 的 `alc_super_open_get_room` 永远返回 0（后续窗口全被拒）。
    // 失败（含上一页 FAILC）⇒ 关 hot SB：与 cache 成员失败同口径，绝不继续写可疑块。
    if ((cmd != NULL) && (fc_cmd_status_is_success(completion->status) == true))
    {
        if (pba_to_page(seq->pad_pba[0]) == 0U)
        {
            // ⚠ 逐 plane 用 submit 时记下的**真实**地址：Policy B 下两 plane 的块可能
            // 不同，用 pba_with_plane(pad_pba[0], 1) 推导会把 plane1 的 page0 屏障
            // 漏记（实测：`p0=01` ⇒ wbuf 永久 AGAIN）。
            for (BYTE p = 0U; p < PLANE_NUM; p++)
            {
                alc_super_open_note_page_programmed(&ftl->alloc, FTL_TEMP_HOT, seq->pad_pba[p]);
            }
        }
    }
    else
    {
        (void)alc_super_close(&ftl->alloc, FTL_TEMP_HOT);
    }

    // pad 的 FAIL_CACHE 落给序列最后一页（提交时已绑定 LBA）；超时按失败处理。
    if (seq->pad_verdict_valid == true)
    {
        (void)ftl_cache_wr_verdict_deliver(ftl, seq->pad_verdict_lba, prev_ok);
        seq->pad_verdict_valid = false;
    }

    if (seq->closing == true)
    {
        // 收尾后不得留下无结论页：pad 只覆盖一页，其余未证明页（含 head）一律
        // 判失败，否则页表无法排空、flush 只能以无进展超时结束。
        // ⚠ **只判未证明页**（与 ftl_cache_wr_abort 同口径）：已编程成功、只是在等
        // 确认的页要留给 confirm_step，判失败会撤销有效映射。
        for (BYTE k = 0U; k < seq->wr.count; k++)
        {
            ftl_cache_page_s *pg = &seq->wr.page[(BYTE)((seq->wr.head + k) % FTL_CACHE_SEQ_RING)];

            if ((pg->proven == false) && (pg->state != FTL_CACHE_PAGE_FAILED))
            {
                ftl_cache_wr_mark_failed(ftl, pg, "pad: stray page");
            }
        }

        ftl_cache_wr_verdict_clear(ftl);

        seq->kind            = FTL_CACHE_SEQ_NONE;
        seq->active          = false;
        seq->closing         = false;
        seq->close_requested = false;
        seq->break_requested = false;
        seq->own_page0       = false;
        seq->draining        = false;
        seq->idle_ms         = 0U;

        ftl->cache_stream.wb_pad_closes++;
    }

    // 本服务函数拥有命令。
    if (cmd != NULL)
    {
        (void)fc_command_release(cmd->cmd_id);
    }
}

/**
 * @brief 序列有序收尾入口（host 补发之后每轮调用）。
 *
 * 读序列：直接放弃（不发 LAST）；写序列：提交 padding END（不能 abort——未确认页
 * 已经对 host 提前回执）。资源不足时保持关闭请求，下轮重试。
 *
 * @param ftl 实例
 */
static SSD_CODE_FAST void ftl_cache_seq_close_step(ftl_ctx_s *ftl)
{
    ftl_cache_seq_s *seq = &ftl->cache_stream.seq;

    if ((seq->active != true) || (seq->owner_submit == true) || (seq->closing == true))
    {
        return;
    }
    if ((seq->close_requested == false) && (seq->break_requested == false))
    {
        return;
    }

    if (seq->kind == FTL_CACHE_SEQ_READ)
    {
        ftl_cache_seq_release(ftl);
        return;
    }

    // ⚠ 必须等所有数据成员完成并进入页表（`wr_inflight == 0`）再提交 padding END：
    //   · pad 的 FAIL_CACHE 是对**器件管线里上一条命令**的结论，必须对准真正的最后一页；
    //   · 若此时仍有数据命令在飞，pad 会先收尾（active=false），迟到的成员完成再把页
    //     推进死序列——该页没有后继 FAIL_CACHE 可证明，永久卡住队头 ⇒ flush 超时。
    if (seq->wr_inflight != 0U)
    {
        return;
    }

    (void)ftl_cache_pad_submit(ftl);
}

/**
 * @brief host IO 命令完成入口
 *
 * 按 {读,写}×{成功,失败} 分派到四个分支；组结算见各分支。
 *
 * @param ftl        实例
 * @param completion FC 完成状态
 * @param ctx        pending 槽
 */
void ftl_host_io_callback(ftl_ctx_s *ftl, const fc_cmd_status_s *completion, void *ctx)
{
    (void)ctx; /* host I/O 不登记request 表 */

    // cmd_id 由 NAND 从本命令回填；公共壳 fc_command_id_to_command 已断言
    // ID 在池内，恒返回非空指针。
    WORD      cmd_id = completion->cmd_id;
    fc_cmd_s *cmd    = fc_command_id_to_command(cmd_id);

    // ftl 恒非空（路由表早于逐实例 boot 设好，见 ftl_op.h）；NULL = 编程错误 ⇒
    // ASSERT 而非 if-guard：release 下解引用留现场，比静默吞完成可定位。
    ASSERT(ftl != NULL);

    pend_req_s *pending = pend_find(&ftl->pending, cmd_id);

    if (pending != NULL)
    {
        ssd_status_e sub_status = SSD_STATUS_OK;

        // 成败判据 = **全部**错误位（UNC / CRC / META_ERROR / NAND_FAIL / TIMEOUT / RAID），
        // 不是单看 NAND_FAIL：ECC 不可纠只置 UNC（fc_data_path.c 的 ECC 分支），
        // 页内 META 不符只置 META_ERROR——两者的 NAND_FAIL 都恒清，单看它会把
        // 「内容不可信」当成功。且 UNC 时 FC 跳过后面的解扰/修正步骤，缓冲里
        // 是**加扰垃圾**，搬给 host 就是静默数据损坏。判据见 fc_cmd_status_has_error()。
        //
        // 分流依据是 pending->op（显式操作类型），不是 buffer——写请求的 buffer
        // 是数据源、恒非空，用 buffer 判别会把写全判成读。
        const BOOL is_read = pend_op_is_read(pending->op);

        if (pend_op_is_write_cache(pending->op))
        {
            // cache program 成员：中间页/末页/失败均由 batch 上下文结算；
            // 不走透明重试（op44 不能回读重发），也不走普通写失败分支（不按 plane 标坏）。
            ftl_cache_wr_member_done(ftl, pending, completion);
            (void)fc_command_release(cmd->cmd_id);
            return;
        }

        if (fc_cmd_status_is_success(completion->status))
        {
            if (is_read == false)
            {
                // page0 异步写屏障：只记**本命令实际覆盖的 plane**。PAGE0 补全写
                // 覆盖两 plane（缺失 plane 写零），两个 page0 都已编程。
                if (pending->op == PENDING_OP_WRITE_PAGE0)
                {
                    alc_super_open_note_page_programmed(&ftl->alloc, FTL_TEMP_HOT, pending->new_pba);
                    alc_super_open_note_page_programmed(&ftl->alloc, FTL_TEMP_HOT, pending->new_pba1);
                }
                else
                {
                    for (BYTE p = 0U; p < PLANE_NUM; p++)
                    {
                        if (bit_mask_any(pending->cw_bitmap, ftl_cw_plane_mask(p)))
                        {
                            const DWORD pba = (p == 0U) ? pending->new_pba : pending->new_pba1;

                            alc_super_open_note_page_programmed(&ftl->alloc, FTL_TEMP_HOT, pba);
                        }
                    }
                }
            }

            if (is_read)
            {
                sub_status = ftl_host_io_read_ok(ftl, cmd, pending, cmd_id);
            }
            else
            {
                // 普通写 / page0 写：完成后确认映射（cache program 成员已在上面分流）。
                if (ftl_host_io_write_ok(ftl, pending))
                {
                    (void)fc_command_release(cmd->cmd_id);
                    return;
                }
            }
        }
        else if (is_read)
        {
            if (pending->op == PENDING_OP_READ_CACHE)
            {
                // cache sequence 仍在同 LUN 队列里：这里启动 RR 会向同 LUN 插命令，
                // 破坏 31h/3Fh 管线。直接报组错误；后续成员照常完成。
                // 失败原因（NAND_FAIL/UNC/CRC/META/EMPTY…）只在这条 completion 上有，
                // 限流打出来：定位「缓存读指到了什么页」不靠猜。
                LOGE_IF(log_ratelimit_due(&s_cache_rd_err_log), LOG_TAG_FTL "cache read cmd failed lba=%lu cmd=%hu st=0x%02lX pba=%08lX\r\n", pending->lba, cmd_id, (DWORD)completion->status, ftl_cmd_pba(cmd));
                sub_status = SSD_STATUS_IO_ERROR;
            }
            else
            {
                ftl_host_io_read_fail(ftl, pending);
                (void)fc_command_release(cmd->cmd_id);
                return;
            }
        }
        else
        {
            ftl_host_io_write_fail(ftl, pending, completion);

            // 透明重试：同一页在新块上重发一次。成功 ⇒ 组保持 pending（**不**递减
            // 在飞数），由新命令的完成继续推进；失败 ⇒ 走原 IO_ERROR 收尾。
            if ((pending->op != PENDING_OP_WRITE_PAGE0) && (pending->retry_count < FTL_WRITE_RETRY_MAX) && (ftl_write_retry(ftl, pending, cmd) == FTL_OK))
            {
                (void)fc_command_release(cmd->cmd_id);
                return;
            }

            sub_status = SSD_STATUS_IO_ERROR;
        }

        // 子操作完成 → 组聚合：通知字段（complete_flag/status_out/
        // on_complete）只在组全部子操作完成后触发一次。
        // ⚠ 页打包下一页可能由**多个组**拼成 ⇒ 按 owners 位掩码逐组递减。
        // ⚠ owners 必须在 pend_remove **之前**取走：组完成通知会调 on_complete，
        // 回调可能重入 ftl_submit_* 并复用本槽 ⇒ 释放后再读就是读别人的字段。
        // （另外两处结算点 read_recovery.c / pend_confirm_settle 同此顺序。）
        DWORD owners = pending->owners;

        pend_remove(&ftl->pending, cmd_id);
        pend_group_complete_subop_multi(&ftl->pending, owners, sub_status);
    }

    (void)fc_command_release(cmd->cmd_id);
}

/* ── 对外接口 ── */

/**
 * @brief 复位 ftl.o 的文件级静态（NOLOAD 段不清零）
 *
 * 只调用一次，且必须在所有实例 ftl_init 之前。
 */
void ftl_global_init(void)
{
    // ftl.o 的 .bss 随整文件规则落 DTCM（NOLOAD），文件级日志/守卫静态不在
    // `ftl` 结构体内，必须显式复位。`s_dangling_next` 虽然取模后不会越界，
    // 但 `s_dangling[].lbas` 的随机残留会污染 dangling 日志聚合。
    memset(s_dangling, 0, sizeof(s_dangling));
    s_dangling_next      = 0U;
    s_write_retry_logged = 0U;

    // padding 页内容不映射任何 LBA，但必须是确定的零页；NOLOAD 段的随机残留
    // （可能是上一次运行的主机数据）不应被写进介质。
    memset(s_cache_pad_page, 0, sizeof(s_cache_pad_page));

    // NOLOAD 段不清零：限流对象的 interval 必须在这里装好（last/suppressed 归零）。
    log_ratelimit_init(&s_dangling_log, LOG_RATE_INTERVAL_MS);
    log_ratelimit_init(&s_cache_rd_err_log, LOG_RATE_INTERVAL_MS);
    log_ratelimit_init(&s_confirm_log, LOG_RATE_SLOW_INTERVAL_MS);
    log_ratelimit_init(&s_flush_log, LOG_RATE_SLOW_INTERVAL_MS);
    log_ratelimit_init(&s_flush_err_log, LOG_RATE_SLOW_INTERVAL_MS);
    log_ratelimit_init(&s_lost_log, LOG_RATE_SLOW_INTERVAL_MS);
    log_ratelimit_init(&s_nospace_log, LOG_RATE_SLOW_INTERVAL_MS);
    log_ratelimit_init(&s_subop_fail_log, LOG_RATE_SLOW_INTERVAL_MS);
    log_ratelimit_init(&s_cache_wb_fail_log, LOG_RATE_SLOW_INTERVAL_MS);
    log_ratelimit_init(&s_seq_drain_stall_log, LOG_RATE_SLOW_INTERVAL_MS);
}

/* ══════════════════════════════════════════════════════════════
 * 紧凑 die 表（扫描结果的 FTL 调度坐标）
 *
 * 板级 target 扫描（armed 之前）只给出哪些 (ch,ce) target 存在、
 * logical_units 多少；这里给实际存在的 LUN 分配紧凑 die 号，并填两张逆映射表。
 * ══════════════════════════════════════════════════════════════ */
BYTE       g_die_count;
BYTE       g_chcelun_table[CH_NUM][CE_NUM][LUN_NUM];
ftl_topo_s g_die_table[FTL_DIE_MAX];

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

    LOGI(LOG_TAG_FTL "topology: dies=%hhu (max=%u)\r\n", g_die_count, (unsigned int)FTL_DIE_MAX);
    return true;
}

/**
 * @brief 初始化 FTL 实例
 *
 * 只绑指针；storage 数组/挂载块由 HIL 提供，本函数显式 memset 表。
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

    // ├─ 绑定外部存储（storage 数组/挂载块由 HIL 按域提供）──
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
    // 在此清表会抹掉其它实例在飞的 ctx 登记，也会抹掉 main.c 已设好的路由表。
    // 该函数由 main.c 的 ssd_ftl_boot_all 在**所有实例 boot 之前**调用一次。
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

    // ├─ 统计 ──
    // ├─ 后台调度器任务表（10 项；must-run + interval/budget）──
    // 必须在所有在飞的字段就绪后装载（适配器只依赖实例字段与 kick，无额外依赖）。
    bg_scheduler_init(&ftl->bg);

    return FTL_OK;
}

/* ── 初始化契约自检（debug-only）──
 *
 * 只在 ftl_init + 设备级 owner 登记完成后跑一次，证明“初始化后不可变”的绑定。
 * 覆盖：句柄、storage 数组、模块窄上下文、topology 副本、PBA 布局、bg 任务表、
 *       L2P owner / completion route 身份、各 job 的初始 idle 状态。
 *
 * ⚠ 边界（不要据此删运行期断言）：
 *   · 不覆盖运行期可变状态（phase 重启、计数、池成员、valid_count）；
 *   · 不覆盖每次调用的入参前置条件（out 指针、enum/index、cmd/desc）；
 *   · release（NDEBUG）下本组函数整体不存在，不能替代 release 拒绝（判据 9）。
 *   · 下方 helper 全部包在 #if !defined(NDEBUG) 内，避免 release 的 unused 告警。
 *   · 审计通过后，下游对同一绑定**不逐次复检**——这些绑定在生命周期内不可变。
 */
#if !defined(NDEBUG)

/**
 * @brief 每个 ASSERT 按模块聚合：一条断言覆盖同一生命周期的一组字段。
 * 失败时 file:line 定位到模块组；需要逐字段定位时临时拆分。
 *
 * @param ftl 已初始化的实例（只读）
 */
static SSD_CODE_FAST void contract_check_handles(const ftl_ctx_s *ftl)
{
    ASSERT((ftl->cache != NULL) && (ftl->metadata_dir != NULL) && (ftl->journal != NULL) && (ftl->cp_handle != NULL) && (ftl->mig_handle != NULL) && (ftl->wbuf != NULL) && (ftl->rr_page_scratch != NULL));

    ASSERT((ftl->table_refs.blocks != NULL) && (ftl->table_refs.free_blocks != NULL) && (ftl->table_refs.op_blocks != NULL) && (ftl->table_refs.spare_blocks != NULL) && (ftl->table_refs.closed_sb_list != NULL));

    ASSERT((ftl->alloc.blocks == ftl->table_refs.blocks) && (ftl->alloc.free_blocks == ftl->table_refs.free_blocks) && (ftl->alloc.op_blocks == ftl->table_refs.op_blocks) && (ftl->alloc.spare_blocks == ftl->table_refs.spare_blocks) && (ftl->alloc.closed_sb_list == ftl->table_refs.closed_sb_list));
}

/**
 * @brief 校验 topology 副本与实例路由一致（各模块 init 时都拷了一份）。
 *
 * @param ftl 已初始化的实例（只读）
 */
static SSD_CODE_FAST void contract_check_topology(const ftl_ctx_s *ftl)
{
    const ftl_topo_s *topo = &ftl->topo;
    const BYTE        die  = topo_2_die(*topo);

    ASSERT((topo->ch < CH_NUM) && (topo->ce < CE_NUM) && (topo->lun < LUN_NUM) && (die < DIE_MAX) && (die_2_ch(die) == topo->ch) && (die_2_ce(die) == topo->ce) && (die_2_lun(die) == topo->lun));

    ASSERT((ftl->alloc.ch == topo->ch) && (ftl->alloc.ce == topo->ce) && (ftl->alloc.lun == topo->lun) && (ftl->cache->ch == topo->ch) && (ftl->cache->ce == topo->ce));

    // 各模块在 init 时拷贝 topology；副本必须与实例路由一致。
    ASSERT((ftl->metadata_dir->topo.ch == topo->ch) && (ftl->metadata_dir->topo.ce == topo->ce) && (ftl->metadata_dir->topo.lun == topo->lun) && (ftl->journal->topo.ch == topo->ch) && (ftl->journal->topo.ce == topo->ce) && (ftl->journal->topo.lun == topo->lun));

    ASSERT((ftl->cp_handle->topo.ch == topo->ch) && (ftl->cp_handle->topo.ce == topo->ce) && (ftl->cp_handle->topo.lun == topo->lun) && (ftl->mig_handle->topo.ch == topo->ch) && (ftl->mig_handle->topo.ce == topo->ce) && (ftl->mig_handle->topo.lun == topo->lun) && (ftl->read_recovery_handle.topo.ch == topo->ch) && (ftl->read_recovery_handle.topo.ce == topo->ce) && (ftl->read_recovery_handle.topo.lun == topo->lun));
}

/**
 * @brief 校验各模块窄上下文绑定的指针与本实例账本一致。
 *
 * @param ftl 已初始化的实例（只读）
 */
static SSD_CODE_FAST void contract_check_module_contexts(const ftl_ctx_s *ftl)
{
    const md_dir_cache_s *md = ftl->metadata_dir;

    ASSERT((md->alloc == &ftl->alloc) && (md->total_writes == &ftl->stats.total_writes));

    ASSERT(ftl->journal->alloc == &ftl->alloc);

    const cp_handle_s *cp = ftl->cp_handle;

    ASSERT((cp->cache == ftl->cache) && (cp->journal == ftl->journal) && (cp->metadata_dir == ftl->metadata_dir) && (cp->alloc == &ftl->alloc));

    const mig_handle_s *mg = ftl->mig_handle;

    ASSERT((mg->alloc == &ftl->alloc) && (mg->journal == ftl->journal) && (mg->cache == ftl->cache) && (mg->total_writes == &ftl->stats.total_writes));

    const rr_handle_s *rr = &ftl->read_recovery_handle;

    ASSERT((rr->cache == ftl->cache) && (rr->alloc == &ftl->alloc) && (rr->page_scratch == ftl->rr_page_scratch) && (rr->pending == &ftl->pending));

    const wb_handle_s *wb = ftl->wbuf;

    ASSERT((wb->alloc == &ftl->alloc) && (wb->pending == &ftl->pending) && (wb->migrate == ftl->mig_handle));
}

/**
 * @brief 校验本实例的 owner / route 身份（只查本实例拥有的 region）。
 *
 * @param ftl 已初始化的实例（只读）
 */
static SSD_CODE_FAST void contract_check_identity(const ftl_ctx_s *ftl)
{
    const ftl_topo_s topo = ftl->topo;
    const BYTE       die  = topo_2_die(topo);

    ASSERT(ftl->cache->owner_of_region != NULL);
    ASSERT(ftl_op_route_get(topo.ch, topo.ce, topo.lun) == ftl);

    // 只查本实例拥有的 region：其他 LUN 的 owner 可能尚未 boot。
    for (DWORD local = 0U; local < L2P_REGIONS_PER_INSTANCE; local++)
    {
        const DWORD        global = (local * DIE_MAX) + die;
        const l2p_owner_s *owner  = ftl->cache->owner_of_region(ftl->cache->owner_ctx, global);
        const QWORD        lba    = (QWORD)global * L2P_ENTRIES_PER_REGION;

        ASSERT((owner != NULL) && (owner->die == die) && (owner->metadata_dir == ftl->metadata_dir) && (owner->alloc == &ftl->alloc) && (owner->topo.ch == ftl->cache->ch) && (owner->topo.ce == ftl->cache->ce) && (owner->topo.lun == ftl->topo.lun) && (topo_2_die(owner->topo) == die));

        ASSERT((ftl_op_route_get_region(global) == ftl) && (lba_2_ch(lba) == ftl->cache->ch) && (lba_2_ce(lba) == ftl->cache->ce));
    }
}

/**
 * @brief 校验后台调度器任务表已装载且处于初始 IDLE 档。
 *
 * @param ftl 已初始化的实例（只读）
 */
static SSD_CODE_FAST void contract_check_bg(const ftl_ctx_s *ftl)
{
    ASSERT((ftl->bg.tasks != NULL) && (ftl->bg.task_count > 0U) && (ftl->bg.task_count <= BG_TASK_COUNT) && (ftl->bg.cfg == NULL) && (ftl->bg.level == BG_LEVEL_IDLE));

    for (BYTE i = 0U; i < ftl->bg.task_count; i++)
    {
        ASSERT(ftl->bg.tasks[i].func != NULL);
    }
}

/**
 * @brief 校验各异步 job（journal / checkpoint / metadata / migrate / RR / wbuf）初始 idle。
 *
 * @param ftl 已初始化的实例（只读）
 */
static SSD_CODE_FAST void contract_check_jobs_idle(const ftl_ctx_s *ftl)
{
    const jrn_handle_s *j = &ftl->journal->handle;

    ASSERT((j->job.phase == JRN_IDLE) && (j->state.current_block < JOURNAL_BLOCK_COUNT) && (j->state.current_page < PAGES_PER_BLOCK) && (j->state.current_entry == 0U) && (j->state.sequence_counter == 1U) && (j->state.merge_floor == 0U) && (j->job.need_erase == false) && (j->state.checkpoint_needed == false) && (j->state.emergency == false) && (j->state.write_fail_count == 0U) && (j->state.write_fail_pending == false) && (j->state.pre_erased_block == JRN_BLOCK_NONE) && (j->state.bad_mask == 0U) && (j->state.erase_fail_count == 0U) && (j->state.ring_full == false));

    const cp_handle_s *cp = ftl->cp_handle;

    ASSERT((cp->active == false) && (cp->waiting == false) && (cp->failed == false) && (cp->phase == CP_IDLE) && (cp->cp_state.advance_next == false) && (cp->cp_state.cursor_valid == false));

    ASSERT((md_is_busy(ftl->metadata_dir) == false) && (ftl->metadata_dir->count <= METADATA_DIR_ENTRIES) && md_cursor_bounds_ok(ftl->metadata_dir));

    ASSERT((ftl->mig_handle->job.active == false) && (ftl->mig_handle->job.waiting == false) && (ftl->mig_handle->job.phase == MIG_IDLE) && mig_fail_state_is_clean(ftl->mig_handle));

    ASSERT((ftl->read_recovery_handle.stage == RR_STAGE_IDLE) && (ftl->read_recovery_handle.waiting == false) && (ftl->read_recovery_handle.has_owner == false));

    ASSERT((ftl->wbuf->open == false) && (ftl->wbuf->occupied == 0U) && (ftl->wbuf->error == FTL_OK));
}

/**
 * @brief 校验开机零态：pending 空、计数为 0、无在飞 loading / flush。
 *
 * @param ftl 已初始化的实例（只读）
 */
static SSD_CODE_FAST void contract_check_boot_zero(const ftl_ctx_s *ftl)
{
    ASSERT((pend_slot_get_count(&ftl->pending) == 0U) && (pend_group_get_count(&ftl->pending) == 0U) && (ftl->stats.total_writes == 0U) && (ftl->cache->inflight.load == 0U) && (ftl->cache->inflight.flush == 0U) && (ftl->flags.bg_suspend == false) && (ftl->flags.l2p_defer == false));
}

#endif /* !NDEBUG */

/**
 * @brief debug-only 初始化契约自检（release 空实现）
 *
 * 只证明初始化后不可变的绑定；不检查运行期状态。
 *
 * @param ftl 实例
 */
SSD_CODE_FAST void ftl_contract_audit(const ftl_ctx_s *ftl)
{
#if defined(NDEBUG)
    (void)ftl; /* release：审计不存在，行为与今天一致 */
#else
    ASSERT(ftl != NULL);
    ASSERT(pba_fields_layout_matches());

    contract_check_handles(ftl);
    contract_check_topology(ftl);
    contract_check_module_contexts(ftl);
    contract_check_identity(ftl);
    contract_check_bg(ftl);
    contract_check_jobs_idle(ftl);
    contract_check_boot_zero(ftl);

    LOGI("[FTL ] contract ok ch=%hhu ce=%hhu lun=%hhu regions=%u tasks=%hhu\r\n", ftl->topo.ch, ftl->topo.ce, ftl->topo.lun, L2P_REGIONS_PER_INSTANCE, ftl->bg.task_count);
#endif
}

/* ── 服务主循环（实例无关泵 + 每实例推进）── */

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
    // 唯一调用点 main.c 传 &g_ftl[die]（静态数组元素，结构性恒非空）；
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

    // [svc] host 补发：下发各组的待补发子操作。完成先腾出 FC 资源（命令/desc/buffer 池、SQ 空位），
    // 再把各组的待下发子操作补上；顺序保证 host IO 优先于后台调度。
    // 只遍历**本实例**的组（每实例一份 groups 表）。
    for (BYTE i = 0U; i < FTL_GROUP_MAX; i++)
    {
        if (ftl->pending.groups[i].used == true)
        {
            ftl_drain_group(ftl, i);
        }
    }

    // 安全网：序列跨 host 段/命令保持打开等同一 LBA 流的下一段；孤儿序列超时关闭。
    ftl_cache_seq_idle_guard(ftl);

    // 有序收尾：写序列用 padding END（给提前回执的最后一页补 FAIL_CACHE 结算），
    // 读序列直接放弃。放在补发之后：本轮的窗口先发出去，再决定是否收尾。
    ftl_cache_seq_close_step(ftl);

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
    // 判定全在 ftl/bg_scheduler.c（本文件不持有后台策略）。
    bg_scheduler_service(ftl);
}

/* ── 子操作下发（一条 host 请求 → 多个 FC 子操作）──
 *
 * ftl_issue_subop 下发**一个**子操作 = 一个 **pair 窗口内**的连续 LBA 段
 * （≤ FTL_LBA_PER_PAIR = 8 个 CW = 2 个 plane 页），三态返回：
 *   FTL_OK    已下发（或如未映射读般**同步完成**，此时不进 outstanding）
 *   FTL_AGAIN 瞬态背压（命令/desc/buffer 池满、SQ 满、pending 表满）——排队重试
 *   FTL_ERROR 永久失败（无可用块 / 配置错误 / 映射表损坏）
 *
 * ⚠ 三态分类是全局枢纽：现状把这三类全当致命，正是"大请求在第 N 个子操作崩"的根因。
 *
 * ⚠ n_cw 是**入参兼出参**：调用者给出本窗口内可提供的最大 LBA 数，本函数按
 *   「L2P 可分组性」（读）或「窗口边界」（写）收缩后回填**实际消费的 LBA 数**，
 *   调用者（ftl_drain_group）据此推进进度。
 *
 * ⚠ **一个子操作最多覆盖每个 plane 一个物理页**——这是 FC 建链的硬约束：
 *   fc_build_request_data_planes 断言同 plane 的各 desc 共享 block/page/lun，且
 *   request->plane_paa[plane] 取自该 plane **第一条** desc（release 下只是断言 ⇒
 *   违反会静默用错行地址）。子操作被 pair 窗口（FTL_LBA_PER_PAIR 对齐）夹住 ⇒
 *   连续 LBA 段最多跨两个 plane 段，每段只落在一个页内，约束自动满足。
 */

/**
 * @brief 下发**一个**子操作（pair 窗口内的连续 LBA 段）——查/分配 PBA、建 FC 命令、注册 pending、submit。
 *
 * @param ftl 实例
 * @param gi 组下标
 * @param internal_lba **全局** host LBA（扇区号，数据通路全程用全局 LBA，不预转 local）
 * @param buffer 数据缓冲
 * @param n_cw 入参兼出参（最大可提供 / 实际消费的 LBA 数）。
 *
 * @return FTL_OK    已下发（或如未映射读般**同步完成**，不进 outstanding）
 *         FTL_AGAIN 瞬态背压（命令/desc/buffer 池满、SQ 满、pending 表满）→ 排队重试
 *         FTL_ERROR 永久失败（无可用块 / 映射表损坏）
 *
 * @note ⚠ 三态分类是全局枢纽（见上方分区头）。
 * · 读：逐 LBA 查 L2P，按 (物理 plane, 页) 分组——未映射、或同 plane 换了页
 * 即收缩；首个 LBA 就未映射则同步零填 1 个 LBA 并消费它。
 * · 写：按窗口内 LBA 段算 flat 位图（每个 LBA 落自己 plane 位的页 + 自己的
 * CW），一次 alc_super_open_alloc_pair 拿两个 plane 的页。
 */
static ftl_status_e ftl_issue_subop(ftl_ctx_s *ftl, WORD gi, DWORD internal_lba, void *buffer, BYTE *n_cw)
{
    pend_group_s *g = &ftl->pending.groups[gi];

    // 夹到 pair 窗口：一个子操作不跨窗口（窗口内最多两个 plane 段、每段一个页）。
    // ⚠ L2P region 512 对齐且 pair 窗口只有 8 个 LBA ⇒ 窗口绝不跨 region。
    BYTE n;
    {
        const DWORD window_end = ftl_lba_pair_base(internal_lba) + FTL_LBA_PER_PAIR;
        const BYTE  avail      = (window_end - internal_lba);

        n = (*n_cw < avail) ? *n_cw : avail;
    }

    // 写：攒进页打包缓冲（窗口满 / 切换窗口时内部触发编程），本函数对写**到此为止**。
    // 数据在 wb_handle(FLUSH) 里一次编程——分配/建链/提交/确认全在那里。
    if (pend_op_is_read(g->op) == false)
    {
        // 回填窗口夹住后的实际消费数——调用者（ftl_drain_group）据此推进 g->next。
        // ⚠ 不可漏：内部 n 可能被窗口边界夹小（internal_lba 未 8 对齐时 avail<8），
        // 漏回填会让 g->next 越过窗口、跳过未下发的 LBA。
        *n_cw = n;

        wb_trigger_s trigger;
        memset(&trigger, 0, sizeof(trigger));
        trigger.kind = WBUF_TRIGGER_APPEND;
        trigger.lba  = internal_lba;
        trigger.data = buffer;
        trigger.n    = n;
        trigger.gi   = gi;

        return wb_handle(ftl->wbuf, &trigger, NULL);
    }

    /* ── 以下为读路径 ── */

    // 命中写缓冲：该 LBA 尚未编程，从缓冲取（否则会读到陈旧 L2P 映射）。同步完成、
    // 消费 1 个 LBA（不进 outstanding）。
    if (wb_read(ftl->wbuf, internal_lba, buffer))
    {
        *n_cw = 1U;
        return FTL_OK;
    }

    DWORD pba[PLANE_NUM];
    for (BYTE i = 0U; i < PLANE_NUM; i++)
    {
        pba[i] = NAND_PBA_INVALID; /* 未覆盖的 plane 保持 INVALID（见 pend_req_s.new_pba 约定） */
    }

    DWORD cw_bitmap = 0U; /* flat：bit = plane*CW_NUM + cw */
    {
        // 逐 LBA 查 L2P 并按 (物理 plane, 页) 分组。收缩条件：
        // · 该 LBA 仍在写缓冲里（未编程）—— 本 LBA 不进本子操作（下一轮从缓冲取）
        // · 未映射（INVALID）—— 本 LBA 不进本子操作（下一轮单独处理）
        // · region 尚未就绪（异步加载在飞）—— 本子操作到此为止，下轮再来
        // · 同 plane 但换了页 —— 一条命令每 plane 只能一个页，装不下
        // · 非法 PBA / 物理 plane ≠ LBA 的 plane 位 —— 映射表损坏 → FTL_ERROR
        BYTE k;
        BOOL l2p_again = false; /* 本轮是否因 L2P 值不可用而中断（≠ 未映射） */

        for (k = 0U; k < n; k++)
        {
            const DWORD l = (internal_lba + k);
            DWORD       p = NAND_PBA_INVALID; /* 越界（FTL_ERROR）保持此值 = 未映射 */

            if (wb_has(ftl->wbuf, l))
            {
                break; /* 还在写缓冲（未编程）：收缩到此为止，下一轮同步从缓冲取 */
            }

            // cache 写序列里**已编程、尚未确认**的页：L2P 仍是旧映射，必须读刚编程的
            // 页。命中即用它的 PBA，跳过 L2P。
            if (ftl_cache_wr_lookup(ftl, l, &p) == false)
            {
                // 已判失败、撤销还没走完的页：L2P 可能仍指着编程失败的 PBA
                // ⇒ 必须报错，不能把它的内容当数据返回。
                if (ftl_cache_wr_failed_covers(ftl, l) == true)
                {
                    LOGE_IF(log_ratelimit_due(&s_lost_log), LOG_TAG_FTL "rd lba=%lu: cache page failed (FAIL_CACHE) — IO_ERROR\r\n", l);
                    return FTL_ERROR;
                }

                // ⚠ 三态：FTL_AGAIN = region 未驻留（已起异步加载）。**必须**与
                // "未映射"（FTL_OK + INVALID）分清——把前者当后者会让 host 读到假数据。
                // FTL_ERROR 仅越界，此时 p 保持 INVALID。
                if (l2p_read(ftl->cache, l, &p) == FTL_AGAIN)
                {
                    // ⚠ 「进展」只认**在飞命令**：加载可能**启动失败**（无空闲行 / 提交被拒），
                    // 此时若宣称"在飞"，drain 的 stall 检测会永久失效，形成环形等待
                    // 。主流判据 = 队列/完成模型：没有在飞命令就没有确定完成时刻。
                    ftl->flags.l2p_defer = l2p_cache_any_inflight(ftl->cache);
                    l2p_again            = true; /* 值不可用：不得按「未映射」处理 */
                    break;                       /* 本子操作到此为止；已凑到的 k 个 LBA 照常下发 */
                }
            }

            if (p == NAND_PBA_INVALID)
            {
                break; /* 未映射：收缩到此为止 */
            }

            // 边界不变量（fc.c 的 fc_poll_fill_queue 靠它）：**进 FC 的 PBA 必须合法**。
            // L2P 条目既不是 NAND_PBA_INVALID 又不是合法 PBA ⇒ 映射表已损坏（介质 bit rot、
            // 半编程页被 ECC「纠」过、跨介质格式复用旧页）。**必须在此拦下**：
            // pba_to_ch/ce/lun 会把越界位当路由字段，放行即让 FC 按它直接索引
            // suspended[][][]（栈越界写）；且坏 PBA 还会经 pba_to_paa 掩位后建出一条
            // 读**别的物理页**的 desc 链——静默读到错误数据。
            // 这是**数据**损坏而非编程错误 ⇒ 报错返回（不 ASSERT），由 drain 把本组标
            // IO_ERROR 交给 host，不 brick 设备。
            if (pba_is_valid(p) == false)
            {
                LOGE(LOG_TAG_FTL "rd lba=%lu: L2P entry 0x%08lx is not a valid PBA\r\n", l, p);
                return FTL_ERROR;
            }

            BYTE  pl = pba_to_plane(p);
            DWORD pg = pba_page(p);

            // CW 级映射的结构不变量：**物理 plane == LBA 的 plane 位**。
            // 违反 ⇒ FC 侧 desc->meta（由**物理** plane/cw 铸出）还原出的扇区 LBA 不是
            // 这一条，读校验必挂 META_ERROR；且位图段（按 plane 位算）与 paa 的 plane 位
            // 会分叉 ⇒ 空链。写路径由构造保证（新页从 pair 取），读路径在此拦下。
            // 属映射表损坏（旧介质格式 / bit rot），不是编程错误。
            if (pl != ftl_lba_plane(l))
            {
                LOGE(LOG_TAG_FTL "rd lba=%lu: plane mismatch pba=0x%08lx\r\n", l, p);
                return FTL_ERROR;
            }

            if ((pba[pl] != NAND_PBA_INVALID) && (pba_same_page(pba[pl], p) == false))
            {
                break; /* 同 plane 跨页：本子操作到此为止，余下交给下一轮 */
            }

            const BYTE cw = ftl_lba_cw(l);

            pba[pl] = pg;
            cw_bitmap |= ftl_cw_bit(pl, cw);
        }

        if (k == 0U)
        {
            // 首 LBA 的值不可用（region 加载在飞，**或加载启动失败**）⇒ 本轮无进展，返背压。
            // ⚠ 必须**先**判这一条：把"还不知道"当成"未映射"会给 host 填一页零。
            // 启动失败时没有在飞命令（l2p_defer=false），但它同样不是未映射；持续无进展
            // 由 drain 的 FTL_DRAIN_STALL_MAX 结算成 IO_ERROR，而不是零填。
            if (l2p_again == true)
            {
                return FTL_AGAIN;
            }

            // 降级 L2P region 的未映射不是「合法未映射」：该范围的旧映射已经丢失，
            // 必须报 IO_ERROR，不能零填（静默丢数据）。
            // 回放已恢复的 LBA 有有效 PBA，不会走到这里。
            if (l2p_is_region_degraded(ftl->cache, lba_to_global_region(internal_lba)))
            {
                LOGE_IF(log_ratelimit_due(&s_lost_log), LOG_TAG_FTL "rd lba=%lu: mapping lost (degraded L2P region) — IO_ERROR\r\n", internal_lba);
                return FTL_ERROR;
            }

            // 首 LBA 未映射：同步返回全零，消费 1 个 LBA（不进 outstanding）。
            // ⚠ 只覆盖**自己**这一个 LBA：相邻 LBA 的映射与它无关，由 drain 下一轮处理。
            memset(buffer, 0, FTL_IU_BYTES);
            *n_cw = 1U;
            return FTL_OK;
        }
        n = k;
    }

    // 本子操作覆盖的 plane 数（决定 operation 码与 plane 段）。
    // cw_bitmap 非空由 ftl_issue_subop 的拆分子操作保证（至少 1 个 CW）。
    BYTE nplanes = ftl_cw_bitmap_plane_count(cw_bitmap);

    // 资源次序：**先查 pending 表余量，再配 desc 链**。末尾要用一个 pending 槽
    // 登记本子操作；表满却先建好链，只能整条回滚——白配一场。
    // ⚠ 必须在 wbuf 命中分支**之后**：那条路径同步完成、不占 pending 槽，不该被表满挡住。
    if (pend_slot_get_count(&ftl->pending) >= FTL_PENDING_MAX)
    {
        return FTL_AGAIN;
    }

    // host 读也必须走预算：否则读突发会把给后台留的命令/buffer 吃光，
    // CP/收尾命令申请不到槽 ⇒ confirm park timeout。
    if (ftl_fc_host_budget_ok() == false)
    {
        return FTL_AGAIN;
    }

    // 分配 FC 命令槽（池空 = 瞬态）
    fc_cmd_s *cmd = fc_command_alloc();
    if (cmd == NULL)
    {
        return FTL_AGAIN;
    }
    const fc_cmd_id_t cmd_id = cmd->cmd_id;

    // 配置命令头（multi_plane 决定 plane 段，必须早于 ftl_cmd_setup_ppa_pair_cw——
    // 后者按段校验位图）
    cmd->config.operation   = ftl_plane_read_op(nplanes);
    cmd->config.multi_plane = ftl_plane_multi_field(nplanes);
    cmd->config.cw_count    = CW_NUM - 1U;
    cmd->config.crc         = FC_CRC_ENABLE;
    cmd->config.random      = FC_RANDOM_ENABLE;
    cmd->config.ldpc        = FC_ECC_MODE_NORMAL;
    cmd->config.chain       = FC_CHAIN_END;
    cmd->config.page_type   = FC_PAGE_TYPE_LP;
    // 本命令的**窗口基址**（扇区 LBA，低 3 位清零）：FC 侧 desc->meta =
    // (meta & ~PLANE_CW_MASK) | (plane<<2) | cw ⇒ 逐 CW 铸出该 CW 的扇区 LBA。
    // 传窗口基址（而非 internal_lba）语义更直白，两者在 FC 掩掉低 3 位后等价。
    cmd->meta               = ftl_lba_pair_base(internal_lba);

    // 建 desc 链（逐 plane 各自的 PBA，位图 = 本子操作实际覆盖的 CW）；
    // desc/buffer 池空 → 瞬态背压
    ftl_status_e ret = ftl_cmd_setup_ppa_pair_cw(cmd, pba, cw_bitmap);
    if (ret != FTL_OK)
    {
        (void)fc_command_release(cmd_id);
        return ret; /* FTL_AGAIN（池空，瞬态）/ FTL_ERROR 原样上抛 */
    }

    // 注册 pending 子操作（必须早于 submit，避免竞态）。读：owners = 单组位、写专用字段传
    // INVALID、cw_bitmap 传 0；lba 传**全局** LBA（L2P/Journal/META 均按全局索引）。
    pend_req_s *pending = pend_alloc(&ftl->pending, cmd_id);

    if (pending == NULL)
    {
        (void)fc_command_release(cmd_id);
        return FTL_AGAIN; /* pending 表满：瞬态 */
    }

    pending->owners     = ftl_group_bit(gi);
    pending->op         = PENDING_OP_READ;
    pending->n_cw       = n;
    pending->cw_bitmap  = 0U;
    pending->lba        = internal_lba;
    pending->buffer     = buffer;
    pending->new_pba    = NAND_PBA_INVALID;
    pending->new_pba1   = NAND_PBA_INVALID;
    pending->cache_role = PEND_CACHE_ROLE_NONE;
    pend_validate(pending);

    *n_cw = n; /* 回填实际消费的 LBA 数（调用者据此推进） */

    // 记账（**必须早于 submit**——submit 之后 completion 可能立即被 drain 处理）
    pend_group_add_subop(&ftl->pending, gi);

    // 提交（tag = HOST_IO；ctx 传 NULL —— 私有状态在 pending 表）
    if (ftl_op_submit(cmd, FTL_OP_HOST_IO, NULL) != FTL_OK)
    {
        pend_remove(&ftl->pending, cmd_id);
        (void)fc_command_release(cmd_id);
        pend_group_drop_subop(&ftl->pending, gi);
        return FTL_AGAIN; /* SQ 满：瞬态 */
    }

    return FTL_OK;
}

/**
 * @brief drain 判卡死时的现场转储（调用点已限流）。
 *
 * why：「no progress」只是症状，真正没回来的资源可能是序列状态 / Journal 页写或擦 /
 * wbuf 忙 / pending 池 / 分配器 Open SB / FC 资源池中的任意一个。常规 `info` 只有
 * 计数，看不到哪一项卡着；把这几项一起打出来，下一次复现就能直接归因。
 *
 * @param ftl 实例
 * @param gi  被判定卡死的组
 *
 * @note 只读状态，不改任何记录。拆三行保证单行不超日志缓冲。
 */
static void ftl_drain_stall_dump(const ftl_ctx_s *ftl, WORD gi)
{
    const ftl_cache_seq_s *seq = &ftl->cache_stream.seq;
    const jrn_handle_s    *jrn = &ftl->journal->handle;

    (void)LOGE(LOG_TAG_FTL "stall: seq kind=%d act=%d drain=%d close=%d brk=%d wr=%hu next=%lu\r\n", (int)seq->kind, (int)seq->active, (int)seq->draining, (int)seq->close_requested, (int)seq->break_requested, seq->wr.count, seq->next_lba);

    (void)LOGE(LOG_TAG_FTL "stall: jrn phase=%d erase=%d full=%d kind=%d ent=%hu seq=%lu jf=%lu\r\n", (int)jrn->job.phase, (int)jrn->job.need_erase, (int)jrn->state.ring_full, (int)jrn->state.page_kind, jrn->state.current_entry, jrn->state.sequence_counter, jrn_page_writes(ftl->journal));

    {
        fc_resource_stats_s rs;
        WORD                fpmin = 0U;

        memset(&rs, 0, sizeof(rs));
        (void)fc_get_resource_stats(&rs);

        // per-plane 最小空闲块数（成对分配看的是最饿的那个 plane）。
        for (BYTE p = 0U; p < PLANE_NUM; p++)
        {
            const WORD c = alc_free_get_count(&ftl->alloc, p);

            if ((p == 0U) || (c < fpmin))
            {
                fpmin = c;
            }
        }

        (void)LOGE(LOG_TAG_FTL "stall: gi=%hu next=%lu/%lu wb_busy=%d pend=%hu free=%hu fpmin=%hu sb_open=%d np=%hhu p0=%02hhx fc cmd=%hu/%hu desc=%hu/%hu buf=%hu/%hu sq=%hu/%hu cq=%hu/%hu\r\n", gi, ftl->pending.groups[gi].next, ftl->pending.groups[gi].total, (int)(wb_is_idle(ftl->wbuf) == false), pend_slot_get_count(&ftl->pending), ftl->alloc.free_count, fpmin, (int)ftl->alloc.hot_sb.is_open, ftl->alloc.hot_sb.next_page, ftl->alloc.hot_sb.page0_programmed, rs.command_used, (WORD)FC_COMMAND_POOL_DEPTH, rs.descriptor_used, (WORD)FC_DESCRIPTOR_POOL_DEPTH, rs.buffer_used, (WORD)FC_BUFFER_POOL_DEPTH, rs.sq_used, (WORD)FC_SQ_DEPTH, rs.cq_used, (WORD)FC_CQ_DEPTH);
    }
}

/**
 * @brief 连续下发一个组的待下发子操作，直到全部下发完 / 遇瞬态背压（FTL_AGAIN）/遇永久失败（FTL_ERROR）。
 *
 * @param ftl 实例
 * @param gi 组下标。
 *
 * @note **进度单位是 LBA 偏移**（g->next / g->total 都以 LBA 计），不是子操作序号：
 * 一个子操作消费 1..FTL_LBA_PER_PAIR（8）个 LBA，取决于 FTL 现场判断
 * （读路径按 L2P 分组收缩、写路径按 pair 窗口夹住，见 ftl_issue_subop）——
 * "怎么拆"完全由 FTL 决定，调用者不必预先算出子操作数。每次迭代**重取** g
 * （下发可能改变组状态甚至释放本组）；全部子操作都在本函数外异步完成，
 * 故循环末尾统一试触发组完成通知——覆盖"整段未映射读"这种从不进 outstanding、
 * 无人触发完成的组。
 */
static void ftl_drain_group(ftl_ctx_s *ftl, WORD gi)
{
    pend_group_s *g = &ftl->pending.groups[gi];

    while ((g->used == true) && (g->next < g->total))
    {
        const DWORD off    = g->next; /* 已下发到的 LBA 偏移（g->next 以 LBA 计，非子操作序号） */
        const DWORD remain = g->total - off;
        DWORD       glba   = (g->lba + off);
        void       *buffer = (BYTE *)g->completion.buffer + (off * FTL_IU_BYTES);
        // 本窗口内可提供的最大 LBA 数（≤ 一个 pair 窗口）。实际消费几个由
        // ftl_issue_subop 回填——读路径按 L2P 分组收缩，写路径按窗口边界收缩。
        BYTE n_cw          = (remain >= FTL_LBA_PER_PAIR) ? FTL_LBA_PER_PAIR : remain;

        ftl->flags.l2p_defer = false; /* 每次下发前清零：本轮的背压原因由本次下发重新判定 */

        ftl_status_e st         = FTL_OK;
        BOOL         use_normal = true;

        // 连续 pair 的 two-plane cache read：是否进入序列由 FTL 顺序流检测器判
        // （cache_stream.rd），序列所有者必须每轮尝试续窗口（窗口消费量不一定落在固定
        // 节拍上）。随机读只在检测器判为顺序流后才尝试，避免每次先付一轮 L2P 查找。
        const BOOL seq_owner = (ftl->cache_stream.seq.active == true) && (ftl->cache_stream.seq.owner_gi == gi);

        if (pend_op_is_read(g->op) && (seq_owner || (ftl->cache_stream.rd.active == true)))
        {
            DWORD                   used = 0U;
            ftl_cache_read_result_e cr   = ftl_cache_read_try(ftl, gi, glba, buffer, remain, &used);

            switch (cr)
            {
                case FTL_CACHE_READ_OK:
                    g->stalls = 0U;
                    g->next += used;
                    continue;

                case FTL_CACHE_READ_DECLINED:
                    break; /* 条件不满足：显式回退普通读 */

                case FTL_CACHE_READ_AGAIN:
                    st         = FTL_AGAIN;
                    use_normal = false;
                    break;

                case FTL_CACHE_READ_ERROR:
                    st         = FTL_ERROR;
                    use_normal = false;
                    break;

                default:
                    ASSERT_DEFAULT(ftl_cache_read_result_e);
                    break;
            }
        }

        // host 16 KiB cache program：最多 4 个 pair 页；条件不满足回退 wbuf。
        if (use_normal && (g->op == PENDING_OP_WRITE))
        {
            DWORD                          used = 0U;
            const ftl_cache_write_result_e cw   = ftl_cache_write_try(ftl, gi, glba, buffer, remain, &used);

            switch (cw)
            {
                case FTL_CACHE_WRITE_OK:
                    g->stalls = 0U;
                    g->next += used;
                    continue;

                case FTL_CACHE_WRITE_DECLINED:
                    break; /* 条件不满足：显式回退 wbuf 普通写 */

                case FTL_CACHE_WRITE_AGAIN:
                    st         = FTL_AGAIN;
                    use_normal = false;
                    break;

                case FTL_CACHE_WRITE_ERROR:
                    st         = FTL_ERROR;
                    use_normal = false;
                    break;

                default:
                    ASSERT_DEFAULT(ftl_cache_write_result_e);
                    break;
            }
        }

        if (use_normal)
        {
            st = ftl_issue_subop(ftl, gi, glba, buffer, &n_cw);
        }

        g = &ftl->pending.groups[gi]; /* 重取：下发可能改变组状态 */

        if (st == FTL_OK)
        {
            g->stalls = 0U;
            g->next += n_cw;
            continue;
        }

        if (st == FTL_AGAIN)
        {
            // 瞬态背压：本轮到此为止，下轮 ftl_service 的 host 补发步再补。
            // 连续无进展超预算 → 判真卡死，弃未下发者并报错（已下发的照常完成）。
            // ⚠ **有确定完成时刻的后台进展不算无进展**：
            // · L2P region 加载在飞（l2p_defer）：NAND 层 wait/jump 超时保证命令必有完成；
            // · 搬移 job 在飞（GC / WL / slow-death）：它自己带 ms deadline，且 wbuf 在
            // Free Pool 空时正是靠它补池。一次回收要数百 ms，8 轮预算远不够，
            // 计进来会把正常等待判成 IO_ERROR。
            // 真卡死（命令丢失 / 搬移超时不退）由各自 deadline 与 NAND 超时兜底。
            // · 别的组持有 cache 设备序列：本组被序列门控挡回是**确定性等待**
            //   （所有者每轮都在推进），不能累计成「卡死」。
            const BOOL other_seq       = (ftl->cache_stream.seq.active == true) && (ftl->cache_stream.seq.owner_gi != gi);
            // 写序列所有者被确认 FIFO / 资源暂时挡回时，队列里的页命令一定有完成
            // （NAND 每命令有超时），属**有确定完成时刻的进展**，不计 stall。
            // 注意与 cache_stream.seq.wr_inflight（在飞成员计数）区分：本项只看「本组的页还在页表里」。
            const BOOL wr_ring_pending = (ftl->cache_stream.seq.kind == FTL_CACHE_SEQ_WRITE) && (ftl->cache_stream.seq.owner_gi == gi) &&
                                         ((ftl->cache_stream.seq.wr.count > 0U) || (ftl->cache_stream.seq.wr_inflight > 0U));
            // 读序列所有者的在飞子操作同理：NAND 每命令有超时 ⇒ 必有完成。
            // 没有它，读窗口一次占满 FC 池时后续窗口的预算等待会被误判成卡死。
            const BOOL rd_inflight    = (ftl->cache_stream.seq.kind == FTL_CACHE_SEQ_READ) && (ftl->cache_stream.seq.owner_gi == gi) && (g->outstanding > 0U);
            // 序列正在有序收尾（padding END 已提交或在途）：这是确定性进展，等它完成。
            const BOOL seq_closing    = (ftl->cache_stream.seq.closing == true) || (ftl->cache_stream.seq.close_requested == true) || (ftl->cache_stream.seq.break_requested == true) || (ftl->cache_stream.seq.draining == true);
            // host 正在给后台让行：后台提交成功或 20ms 超时后必然恢复，属确定性等待。
            const BOOL seq_yield      = (ftl->cache_stream.seq.yield_pending == true);
            // Journal 页写 / 换块擦在飞：NAND 命令有超时 ⇒ 必有完成，属确定性等待。
            // 少这一条，序列边界触发的页刷若跨过 8 轮预算，正常的写会被误判成卡死。
            const BOOL jrn_inflight   = (jrn_is_idle(ftl->journal) == false) || (ftl->journal->handle.job.need_erase == true);
            // 待入账的写确认：要么 region 就绪后结算，要么 100 ms 后超时释放（pending.c）
            // ⇒ 确定性等待。待入账本身不占 drain 的进展，不能算卡死。
            const BOOL confirm_parked = pend_find_op(&ftl->pending, PENDING_OP_WRITE_CONFIRM, NULL);
            // 搬移引擎：job 在飞，或 job 已 abort 但命令仍在 FC 侧（新 job 被 inflight 门控
            // 拒绝）——后者同样是确定性等待（NAND 保证每条命令有完成），不能计成卡死。
            const BOOL mig_inflight   = (ftl->mig_handle->job.active == true) || (ftl->mig_handle->state.inflight != 0U);
            /* wbuf 有打开窗口/页在飞也是**确定性进展**：它必然由 NAND 编程超时或
             * 后续 flush 收尾。speed 的 drain 循环远快于 600 µs tPROG，漏掉这一项
             * 会在 8 轮内把「wbuf 正在编程」误判成 IO_ERROR（板端 stall 现场
             * `wb_busy=1 cmd=2 desc=8 buf=8`）。 */
            const BOOL wb_busy        = (wb_is_idle(ftl->wbuf) == false);
            const BOOL bg_progress    = (ftl->flags.l2p_defer == true) || mig_inflight || other_seq || wr_ring_pending || rd_inflight || seq_closing || seq_yield || jrn_inflight || confirm_parked || wb_busy;

            if (bg_progress == false)
            {
                if (++g->stalls > FTL_DRAIN_STALL_MAX)
                {
                    // 归因：GC 已判定「无收益 victim」时，这不是普通超时，是真正的无空间
                    // （free 见底且搬不动）。打印区分行，避免与瞬态背压混淆。
                    // 无空间是终止态、host 会持续重试 ⇒ 用限流器，避免逐请求刷屏。
                    if (log_ratelimit_due(&s_nospace_log))
                    {
                        if (ftl->mig_handle->state.benefit.nogain)
                        {
                            LOGE(LOG_TAG_FTL "no space: GC no-gain (free=%hu) — IO_ERROR\r\n", ftl->alloc.free_count);
                        }

                        LOGE(LOG_TAG_FTL "drain stalled gi=%hhu next=%lu/%lu\r\n", gi, g->next, g->total);
                        ftl_drain_stall_dump(ftl, gi);
                    }
                    g->agg_status = SSD_STATUS_IO_ERROR;
                    g->next       = g->total;
                    ftl_cache_seq_close(ftl, gi);
                }
            }
            break;
        }

        // FTL_ERROR：永久失败 → 弃未下发者，组最终报错（已下发的照常完成）。
        // host 会重试 ⇒ 限流打印（保留首个失败位置与抑制计数）。
        LOGE_IF(log_ratelimit_due(&s_subop_fail_log), LOG_TAG_FTL "subop failed gi=%hhu next=%lu/%lu\r\n", gi, g->next, g->total);
        g->agg_status = SSD_STATUS_IO_ERROR;
        g->next       = g->total;
        ftl_cache_seq_close(ftl, gi);
        break;
    }

    pend_group_maybe_complete(&ftl->pending, gi);
}

/**
 * @brief 复位顺序读检测状态，并使已算出的预取计划失效。
 *
 * @param ftl 实例（cache_stream 的预取字段由本函数更新）
 *
 * @note 写 / flush / trim 入口显式调用：它们会打断顺序读流。
 */
static void ftl_seq_reset(ftl_ctx_s *ftl)
{
    ftl->cache_stream.prefetch_last_end = FTL_LBA_INVALID; /* 与 stream 检测器同一空哨兵 */
    ftl->cache_stream.prefetch_run      = 0U;
    ftl->cache_stream.prefetch_valid    = false;
    ftl->cache_stream.prefetch_target   = 0U;
    ftl->cache_stream.prefetch_generation++;
}

/**
 * @brief 复位一个方向的顺序流检测器。
 *
 * @param det 检测器（cache_stream.rd / cache_stream.wr）
 */
static void ftl_stream_reset(ftl_stream_det_s *det)
{
    det->last_end = FTL_LBA_INVALID;
    det->run      = 0U;
    det->active   = false;
}

/**
 * @brief 按本请求更新顺序流检测器：只有与上一请求**首尾相接**才累计。
 *
 * @param det   检测器
 * @param lba   本请求起始 LBA
 * @param count 本请求 LBA 数
 *
 * @note run 在达到阈值后不再累加（只判 ≥ 阈值，避免长会话回绕）。
 */
static void ftl_stream_note(ftl_stream_det_s *det, DWORD lba, DWORD count)
{
    if (det->last_end == lba)
    {
        if (det->run < FTL_STREAM_MIN_LBAS)
        {
            det->run += count;
        }
    }
    else
    {
        det->run = count;
    }

    det->last_end = lba + count;
    det->active   = (det->run >= FTL_STREAM_MIN_LBAS);
}

/**
 * @brief 只更新检测状态并算出预取目标，不在这里起加载（host 读应先拿资源）。
 *
 * @param ftl 实例（检测字段与 seq_prefetch_target 由本函数更新）
 * @param req 刚受理的读请求（只读 lba / count）
 */
static void ftl_seq_note_read(ftl_ctx_s *ftl, const ftl_io_req_s *req)
{
    ftl->cache_stream.prefetch_generation++;
    ftl->cache_stream.prefetch_valid = false;

    if (ftl->flush.active || ftl->trim.active || ftl->flags.bg_suspend)
    {
        ftl_seq_reset(ftl);
        return;
    }

    const DWORD lba   = (DWORD)req->lba;
    const DWORD count = req->count;

    if ((ftl->cache_stream.prefetch_run != 0U) && (lba == ftl->cache_stream.prefetch_last_end))
    {
        ftl->cache_stream.prefetch_run++;
    }
    else
    {
        ftl->cache_stream.prefetch_run = 1U;
    }

    ftl->cache_stream.prefetch_last_end = lba + count;

    if (ftl->cache_stream.prefetch_run > ftl->cache_stream.prefetch_run_max)
    {
        ftl->cache_stream.prefetch_run_max = ftl->cache_stream.prefetch_run;
    }

    if (ftl->cache_stream.prefetch_run < FTL_SEQ_PREFETCH_MIN_RUN)
    {
        return;
    }

    const DWORD last         = lba + count - 1U;
    const DWORD last_region  = lba_to_global_region(last);
    const DWORD first_region = lba_to_global_region(lba);
    const DWORD used         = (DWORD)lba_to_entry(last) + 1U;
    const BOOL  cross_region = (last_region > first_region);
    const BOOL  near_end     = (used + FTL_SEQ_PREFETCH_AHEAD_LBAS) >= L2P_ENTRIES_PER_REGION;

    if ((cross_region == false) && (near_end == false))
    {
        return;
    }

    // 本实例的下一个 region（与 cache 序列跨 region 预加载共用同一坐标推导）。
    const DWORD target_lba = lba_next_region_base(last);

    if ((target_lba == FTL_LBA_INVALID) || ((QWORD)target_lba >= FTL_LBA_MAX) || (lba_routes_to(target_lba, ftl->topo) == false))
    {
        return; /* 只预取本实例会访问、且主机可见的 region */
    }

    ftl->cache_stream.prefetch_target = lba_to_global_region(target_lba);
    ftl->cache_stream.prefetch_valid  = true;
}

/**
 * @brief drain 之后执行预取；若 on_complete 重入更新过检测器，generation 变化即放弃。
 *
 * @param ftl 实例
 * @param generation 调用者记录的检测代次（与 ftl->cache_stream.prefetch_generation 不等即放弃）
 */
static void ftl_seq_prefetch_pending(ftl_ctx_s *ftl, DWORD generation)
{
    if ((generation != ftl->cache_stream.prefetch_generation) || (ftl->cache_stream.prefetch_valid == false))
    {
        return;
    }

    const DWORD target               = ftl->cache_stream.prefetch_target;
    ftl->cache_stream.prefetch_valid = false;

    if (ftl->flush.active || ftl->trim.active || ftl->flags.bg_suspend)
    {
        return;
    }

    (void)l2p_region_prefetch(ftl->cache, target);
}

/**
 * @brief 读/写共用的提交骨架——入参校验 + 建组 + 快路径下发。
 *
 * @param ftl 实例
 * @param req 请求（count = 连续 LBA 数，buffer 非空）
 * @param op PENDING_OP_READ / PENDING_OP_WRITE（决定组类型与日志标签）。
 *
 * @return FTL_OK    已建组（可能仍有余量待 host 补发步补发）
 *         FTL_ERROR 参数非法 / 越界 / 组槽满
 *
 * @note 两个公开入口的流程**逐字相同**（差异只有 op 与日志文案）⇒ 收敛到本函数
 * 一份，避免两处各自漂移（H6：注释与代码不一致即缺陷，重复实现更甚）。
 */
static ftl_status_e ftl_submit_common(ftl_ctx_s *ftl, const ftl_io_req_s *req, pend_op_e op)
{
    const char *label = (op == PENDING_OP_READ) ? "read" : "write";

    // host 入口：入参来自 HIL，属**不可信输入**（H29）⇒ 保留返回码，不转 ASSERT。
    if ((ftl == NULL) || (req == NULL) || (req->buffer == NULL))
    {
        return FTL_ERROR;
    }

    // volatile write cache 的**延后错误**：上一次已回执的写后来被判
    // 失败 ⇒ 用**下一条**命令承载这个结论（主机看到该命令失败一次），与控制器层
    // `usb_msc_take_deferred_error` 同口径。
    if (ftl->flags.deferred_error == true)
    {
        ftl->flags.deferred_error = false;
        LOGE(LOG_TAG_FTL "%s: deferred write error from an earlier acked segment — fail this command once\r\n", label);
        return FTL_ERROR;
    }

    // 长度 = 连续 LBA 数（上层 block_count）；FTL 自行决定单/双 plane 拆分
    DWORD count = req->count;
    if (count == 0U)
    {
        return FTL_ERROR;
    }

    // LBA 范围检查（含末 LBA）。req->lba 是 QWORD ⇒ 加法按 64 位进行，不回绕。
    if ((req->lba + count) > FTL_LBA_MAX)
    {
        LOGE(LOG_TAG_FTL "%s lba out of range lba=%llu count=%lu\r\n", label, req->lba, count);
        return FTL_ERROR;
    }

    // 写/flush/trim 会打断顺序读流；写在入口处重置检测器。
    if (op != PENDING_OP_READ)
    {
        ftl_seq_reset(ftl);
    }

    // 顺序流检测（FTL 自判，不是 host 开关）：读 / 写各自累计连续 LBA；换向即复位另一侧。
    // 它决定 cache 序列**能否开启**；序列打开后的延续由 cache_stream.seq.next_lba 判定
    // （host 传输段边界不构成断点，见 docs/design/ftl_design.md §3.16）。
    if (op == PENDING_OP_READ)
    {
        ftl_stream_note(&ftl->cache_stream.rd, (DWORD)req->lba, count);
        ftl_stream_reset(&ftl->cache_stream.wr);
    }
    else
    {
        ftl_stream_note(&ftl->cache_stream.wr, (DWORD)req->lba, count);
        ftl_stream_reset(&ftl->cache_stream.rd);
    }

    // 建完成组（一条 host 请求 = 一个组）。通知字段必须**早于任何下发**填入：
    // 下发可能当场完成整组，届时通知就要读它们。
    BYTE gi = 0U;

    if (pend_group_alloc(&ftl->pending, &gi) == false)
    {
        // 组表满是**瞬态资源**（H38）：上层据此排队重试，不能报永久失败。
        LOGE(LOG_TAG_FTL "%s: no free group slot\r\n", label);
        return FTL_AGAIN;
    }
    pend_group_s *g      = &ftl->pending.groups[gi];
    g->op                = op;
    g->total             = count; /* 进度以 **LBA** 计（子操作数由 drain 现场决定，可降级） */
    g->next              = 0U;
    g->outstanding       = 0U;
    g->stalls            = 0U;
    g->lba               = req->lba;
    g->completion.buffer = req->buffer;
    g->agg_status        = SSD_STATUS_OK;
    g->cmd_remaining     = req->cmd_remaining;

    // 序列延续只看 **LBA 连续性**（与 host 传输段边界 / 命令身份无关）：方向不同或
    // 接续点不一致 ⇒ 上一序列不可能再由本请求续。
    //   读序列：直接放弃（不发 LAST）；
    //   写序列：设备侧没有数据页可 END ⇒ 由 service 用 **padding END** 有序收尾
    //          （不能 abort：未确认页已经对 host 提前回执，abort 会丢已确认数据）。
    if (ftl->cache_stream.seq.active == true)
    {
        const ftl_cache_seq_kind_e want      = (op == PENDING_OP_READ) ? FTL_CACHE_SEQ_READ : FTL_CACHE_SEQ_WRITE;
        const BOOL                 continues = (ftl->cache_stream.seq.kind == want) && (ftl->cache_stream.seq.next_lba == (DWORD)req->lba);

        if (continues == false)
        {
            if (ftl->cache_stream.seq.kind == FTL_CACHE_SEQ_READ)
            {
                ftl_cache_seq_release(ftl);
            }
            else
            {
                ftl->cache_stream.seq.close_requested = true;
            }
        }
    }

    g->completion.complete_flag   = req->complete_flag;
    g->completion.status_out      = req->status_out;
    g->completion.on_complete     = req->on_complete;
    g->completion.on_complete_ctx = req->on_complete_ctx;

    // 先更新顺序检测（纯 RAM，无 I/O），drain 后才真正起预取，给 host 读让资源。
    // 记 generation：drain 内 on_complete 可能重入 submit 并再次更新检测器。
    DWORD seq_gen = 0U;

    if (op == PENDING_OP_READ)
    {
        ftl_seq_note_read(ftl, req);
        seq_gen = ftl->cache_stream.prefetch_generation;
    }

    // 快路径：小请求当场下发完；遇背压则余下留待 ftl_service 的 host 补发步补发。
    // 数据在 ftl_op_drain 中拷贝回 buffer（读），Journal + L2P 在完成
    // handler 中确认 NAND PROGRAM 成功后执行（写）。
    ftl_drain_group(ftl, gi);

    if (op == PENDING_OP_READ)
    {
        const pend_group_s *g_done = &ftl->pending.groups[gi];

        // 本组还有未下发的子操作（背压）⇒ 不给预取抢资源；等本组下发完再预取。
        if (g_done->next >= g_done->total)
        {
            ftl_seq_prefetch_pending(ftl, seq_gen);
        }
    }

    return FTL_OK;
}

/**
 * @brief 取走「延迟错误」标志（读-改-清）
 *
 * @param ftl 实例
 * @return true 有错误；false 无
 */
BOOL ftl_take_deferred_error(ftl_ctx_s *ftl)
{
    if (ftl == NULL)
    {
        return false;
    }

    const BOOL has = ftl->flags.deferred_error;

    ftl->flags.deferred_error = false;
    return has;
}

/**
 * @brief host 读提交
 *
 * 与写共用 ftl_submit_common，只差 pending 的 op 类型。
 *
 * @param ftl 实例
 * @param req 请求
 * @return FTL_OK；FTL_AGAIN 背压；FTL_ERROR 参数/状态错误
 */
ftl_status_e ftl_submit_read(ftl_ctx_s *ftl, const ftl_io_req_s *req)
{
    return ftl_submit_common(ftl, req, PENDING_OP_READ);
}

/**
 * @brief host 写提交
 *
 * 与读共用 ftl_submit_common，只差 pending 的 op 类型。
 *
 * @param ftl 实例
 * @param req 请求
 * @return FTL_OK；FTL_AGAIN 背压；FTL_ERROR 参数/状态错误
 */
ftl_status_e ftl_submit_write(ftl_ctx_s *ftl, const ftl_io_req_s *req)
{
    return ftl_submit_common(ftl, req, PENDING_OP_WRITE);
}

/* ── 元数据落盘 / TRIM（供 HIL 调用）── */

/**
 * @brief 汇总本轮 CP 触发信息
 *
 * @param ftl    实例
 * @param reason 触发原因
 * @param force  显式请求
 * @return 填好的 trigger
 */
cp_trigger_s ftl_cp_trigger_build(const ftl_ctx_s *ftl, cp_reason_e reason, BOOL force)
{
    cp_trigger_s trigger;

    trigger.now_ms            = platform_now_ms();
    trigger.total_writes      = ftl->stats.total_writes;
    trigger.journal_seq       = jrn_sequence_get(ftl->journal);
    trigger.journal_watermark = jrn_watermark_get(ftl->journal);
    trigger.emergency         = jrn_is_emergency(ftl->journal);
    trigger.force             = force;
    trigger.reason            = reason;
    return trigger;
}

/**
 * @brief host flush 也是"该落一次 checkpoint"的信号：没有 CP 时，掉电后只能拿初始 CP
 * （无块表）做全量重建。异步启动，限流 5s（半个 T3），避免 host 频繁 flush 写放大。
 *
 * @param ftl 实例（触发参数取自本实例的 CP / journal 状态）
 */
static void ftl_flush_kick_cp(ftl_ctx_s *ftl)
{
    const DWORD now_ms = platform_now_ms();

    if ((now_ms == 0U) || ((now_ms - ftl->cp_handle->cp_state.last_cp_ms) < (CP_TIME_INTERVAL_MS / 2U)))
    {
        return;
    }

    const cp_trigger_s trigger = ftl_cp_trigger_build(ftl, CP_REASON_EXPLICIT, true);

    cp_handle(ftl->cp_handle, &trigger);
}

/**
 * @brief 取消 flush：只清 job 标志
 *
 * 在飞的 wbuf / L2P / journal 操作各自完成，不破坏不变量。
 *
 * @param ftl 实例
 */
void ftl_flush_cancel(ftl_ctx_s *ftl)
{
    // 只摘 job 标志：wbuf / L2P / journal 的在飞操作有各自的服务路径，
    // 继续完成不会破坏不变量。下一次 begin 以新的起点重新开始。
    ftl->flush.active = false;
}

/**
 * @brief 启动异步 FLUSH job（提交即返回）
 *
 * 完成由 ftl_flush_step 推进，墙钟上限见 FTL_FLUSH_TIMEOUT_MS。
 *
 * @param ftl 实例
 */
void ftl_flush_begin(ftl_ctx_s *ftl)
{
    if (ftl->flush.active)
    {
        return; /* 幂等：已有在飞的 flush */
    }

    ftl->flush.active           = true;
    ftl->flush.start_ms         = platform_now_ms();
    ftl->flush.last_progress_ms = ftl->flush.start_ms;
    ftl->flush.progress         = 0U;
    ftl_seq_reset(ftl);

    // 序列必须先退出，flush 的 L2P/journal 写才不会被门控。
    ftl_cache_seq_yield(ftl);

    // 先把页打包缓冲里尚未编程的写编程掉：它们还没进 L2P/journal，"落盘"不成立。
    // 遇瞬态背压会保留缓冲，后续 step 的 MAYBE_FLUSH_IDLE 会重试。
    wb_trigger_s trigger;

    memset(&trigger, 0, sizeof(trigger));
    trigger.kind = WBUF_TRIGGER_FLUSH;
    (void)wb_handle(ftl->wbuf, &trigger, NULL);
}

/**
 * @brief 推进 FLUSH 一轮
 *
 * 推进 wbuf / L2P / journal；四类在飞数据全部落定（或失败/超时）才返回 true。
 *
 * @param ftl 实例
 * @return true 已完成；false 仍需下一轮
 */
BOOL ftl_flush_step(ftl_ctx_s *ftl)
{
    if (ftl->flush.active == false)
    {
        return true; /* 没有在飞的 flush：对调用者而言就是"已完成" */
    }

    const BYTE die = topo_2_die(ftl->topo);

    // ① wbuf：把打开的窗口推下去，并取当前状态。
    wb_trigger_s wb_trigger;

    memset(&wb_trigger, 0, sizeof(wb_trigger));
    wb_trigger.kind = WBUF_TRIGGER_MAYBE_FLUSH_IDLE;

    wb_status_s wb_status;
    (void)wb_handle(ftl->wbuf, &wb_trigger, &wb_status);

    // ② L2P：继续 kick 本 owner 的脏行（受共享 credit 池门控）。
    l2p_owner_flush_kick(ftl->cache, die, L2P_CACHE_SIZE, true);

    // ③ Journal：推进异步刷页 job。
    const jrn_trigger_e jrn_trigger = JRN_TRIGGER_FLUSH;
    jrn_handle(ftl->journal, &jrn_trigger);

    // ④ 完成判据：四类在飞数据全部落定。待入账中的写确认也必须先完成——它对应的
    // L2P 脏行还没出现，否则 flush 会「提前完成」。
    const BOOL confirm_pending  = pend_find_op(&ftl->pending, PENDING_OP_WRITE_CONFIRM, NULL);
    // 早回执后 cache 页的等待对象变了：组早已完成，flush 要等**已编程页表**排空
    // （每页都已被后继/收尾证明、且已确认进 L2P），否则 SYNCHRONIZE CACHE 会在
    // 「未证明」窗口里提前回执（屏障语义）。
    // 页在**完成时**才进页表 ⇒ 屏障必须同时看「在飞成员」：控制器可能已对写段
    // 提前回执（BOT 串行下 SYNCHRONIZE CACHE 紧接着到来），此时命令还在 NAND 里编程。
    const BOOL cache_wb_pending = (ftl->cache_stream.seq.wr.count != 0U) || (ftl->cache_stream.seq.wr_inflight != 0U);
    const BOOL cache_wr_active  = (ftl->cache_stream.seq.active == true) && (ftl->cache_stream.seq.kind == FTL_CACHE_SEQ_WRITE);
    const BOOL wb_done          = (wb_status.state == WBUF_STATE_IDLE) && (wb_status.occupied == 0U);
    const BOOL l2p_done         = l2p_flush_owner_idle(ftl->cache, die);
    const BOOL jrn_done         = jrn_is_idle(ftl->journal) && (jrn_has_pending_entries(ftl->journal) == false);
    const BOOL jrn_fail         = jrn_has_write_failure(ftl->journal);
    const BOOL fail             = jrn_fail;

    // 无进展上限：wbuf / L2P / journal / 写确认任一项在推进就续期；连续 1 s 没有任何
    // 推进才判超时（满盘 flush 的正常时长可以超过 1 s，见 FTL_FLUSH_TIMEOUT_MS）。
    const DWORD now_ms = platform_now_ms();
    DWORD       sig    = ((DWORD)ftl->cache_stream.seq.wr.count) |
                ((DWORD)ftl->cache_stream.seq.wr_inflight << 4U) |
                ((DWORD)wb_status.occupied << 8U) |
                ((DWORD)wb_status.state << 12U) |
                ((DWORD)(l2p_done ? 1U : 0U) << 16U) |
                ((DWORD)(jrn_done ? 1U : 0U) << 17U) |
                ((DWORD)(confirm_pending ? 1U : 0U) << 18U) |
                ((DWORD)(cache_wr_active ? 1U : 0U) << 19U);

    sig ^= (jrn_sequence_get(ftl->journal) * 2654435761U);
    sig ^= ((DWORD)jrn_watermark_get(ftl->journal) << 7U);

    if (sig != ftl->flush.progress)
    {
        ftl->flush.progress         = sig;
        ftl->flush.last_progress_ms = now_ms;
    }

    const DWORD flush_ms = now_ms - ftl->flush.start_ms;
    const DWORD idle_ms  = (ftl->flush.last_progress_ms != 0U) ? (now_ms - ftl->flush.last_progress_ms) : 0U;
    const BOOL  timeout  = (ftl->flush.last_progress_ms != 0U) && (idle_ms >= FTL_FLUSH_TIMEOUT_MS);

    if (((wb_done && l2p_done && jrn_done && (confirm_pending == false) && (cache_wb_pending == false) && (cache_wr_active == false)) || fail || timeout) == false)
    {
        return false;
    }

    ftl->flush.active = false;

    if (fail)
    {
        // best-effort：journal 页写/擦持续失败。CP 成功后
        // jrn_write_failure_clear 清 journal 标志。
        LOGW_IF(log_ratelimit_due(&s_flush_err_log), LOG_TAG_FTL "flush: journal write failure pending — complete best-effort\r\n");
    }
    else if (timeout)
    {
        const BOOL wr_pending = (ftl->cache_stream.seq.wr.count != 0U) || (ftl->cache_stream.seq.wr_inflight != 0U);

        // 已回执的写映射未确认时，不能让 host 把这次 FLUSH 当成成功（H14 /
        // ）：置 deferred_error，由下一条命令如实上报 IO_ERROR。
        if (wr_pending == true)
        {
            ftl->flags.deferred_error = true;
        }

        // dirty CMT / 未确认写页 / 其它：三选一打印，共用限流器，不刷屏。
        if (ftl->cache->cmt_dirty_owner[die] != 0U)
        {
            LOGE_IF(log_ratelimit_due(&s_flush_err_log), LOG_TAG_FTL "flush stalled %lums (no progress %lums) with %lu dirty CMT entr(ies) — mappings not persisted\r\n", flush_ms, idle_ms, ftl->cache->cmt_dirty_owner[die]);
        }
        else if (wr_pending == true)
        {
            LOGE_IF(log_ratelimit_due(&s_flush_err_log), LOG_TAG_FTL "flush stalled %lums (no progress %lums) with %hhu unconfirmed write page(s) — deferred IO_ERROR\r\n", flush_ms, idle_ms, ftl->cache_stream.seq.wr.count);
        }
        else
        {
            LOGW_IF(log_ratelimit_due(&s_flush_err_log), LOG_TAG_FTL "flush stalled %lums (no progress %lums) (wb=%d l2p=%d jrn=%d) — best-effort complete\r\n", flush_ms, idle_ms, wb_done, l2p_done, jrn_done);
        }
    }

    ftl_flush_kick_cp(ftl);

    // 耗时日志限流 5 s（单线程 superloop；多实例共享一个时间戳即可）。host 若
    // 频繁 fsync，逐次打印会刷屏。
    LOGW_IF((ftl->flush.start_ms != 0U) && (flush_ms >= FTL_FLUSH_SLOW_LOG_MS) && (timeout == false) && log_ratelimit_due(&s_flush_log), LOG_TAG_FTL "flush took %lums (async job)\r\n", flush_ms);

    return true;
}

#define FTL_TRIM_STEP_LBAS  (8U)
#define FTL_TRIM_TIMEOUT_MS (1000U) /* 无进展上限：Journal ring_full / L2P 加载卡死时让 TRIM 失败 */

/**
 * @brief 本轮没有进展时的等待记账：超过上限 ⇒ 中止 TRIM（避免 CLI/USB 永久 BUSY）。
 * 有进展（processed > 0）就清等待起点；平台时钟缺失时退化为无超时（同 FLUSH）。
 *
 * @param ftl 实例（trim_deadline 由本函数更新）
 * @param processed 本轮已处理的 LBA 数（0 = 无进展）
 * @return FTL_AGAIN 继续等待
 *         FTL_ERROR 等待超时，已中止 TRIM
 */
static ftl_status_e ftl_trim_defer(ftl_ctx_s *ftl, BYTE processed)
{
    if (processed != 0U)
    {
        timer_deadline_clear(&ftl->trim.deadline);
        jrn_watermark_update(ftl->journal);
        return FTL_AGAIN;
    }

    const DWORD now_ms = platform_now_ms();

    if (now_ms != 0U)
    {
        if (ftl->trim.deadline.armed == false)
        {
            timer_deadline_arm(&ftl->trim.deadline, now_ms, FTL_TRIM_TIMEOUT_MS);
        }
        else if (timer_deadline_expired(&ftl->trim.deadline, now_ms))
        {
            LOGE(LOG_TAG_FTL "trim: wait timeout lba=%lu\r\n", ftl->trim.lba);
            ftl->trim.active = false;
            timer_deadline_clear(&ftl->trim.deadline);
            return FTL_ERROR;
        }
    }

    return FTL_AGAIN;
}

/**
 * @brief 启动异步 TRIM job
 *
 * 每次 step 至多 FTL_TRIM_STEP_LBAS 条，避免长 TRIM 占满主循环。
 *
 * @param ftl   实例
 * @param lba   起始 LBA
 * @param count LBA 数
 * @return FTL_OK；FTL_AGAIN 已有 job；FTL_ERROR 参数非法
 */
ftl_status_e ftl_trim_begin(ftl_ctx_s *ftl, DWORD lba, DWORD count)
{
    ASSERT(ftl != NULL);

    if (ftl->trim.active)
    {
        return FTL_AGAIN; /* 单飞行：已有 TRIM job */
    }
    if (count == 0U)
    {
        return FTL_ERROR;
    }
    if (((QWORD)lba + count) > (QWORD)FTL_LBA_MAX)
    {
        return FTL_ERROR;
    }

    ftl->trim.lba       = lba;
    ftl->trim.end       = lba + count;
    ftl->trim.start_lba = lba;
    ftl->trim.start_ms  = platform_now_ms();
    ftl->trim.active    = true;
    timer_deadline_clear(&ftl->trim.deadline);
    ftl_seq_reset(ftl);

    // 与 flush 同口径：TRIM 要写 Journal/L2P，序列必须先退出。
    ftl_cache_seq_yield(ftl);

    LOGI(LOG_TAG_TRIM "start lba=%lu n=%lu\r\n", lba, count);
    return FTL_OK;
}

/**
 * @brief 推进 TRIM 一轮
 *
 * 最多处理 FTL_TRIM_STEP_LBAS 条；未完成返回 FTL_AGAIN，完成/映射错误返回终态。
 *
 * @param ftl 实例
 * @return FTL_OK 完成；FTL_AGAIN 继续；FTL_ERROR 失败
 */
ftl_status_e ftl_trim_step(ftl_ctx_s *ftl)
{
    ASSERT(ftl != NULL);

    if (ftl->trim.active == false)
    {
        return FTL_OK;
    }

    BYTE processed = 0U;

    while ((ftl->trim.lba < ftl->trim.end) && (processed < FTL_TRIM_STEP_LBAS))
    {
        const DWORD l = ftl->trim.lba;

        // 该 LBA 还在页打包写缓冲里（未编程）：只清缓冲槽，不碰 L2P/journal——它从未
        // 落盘，无映射变更需记。缓冲的内部不变量（清空 ⇒ 结算 owners）由 wb_handle(DROP)
        // 收口。
        // ⚠ 这里不清旧 L2P 映射：当前依赖 Controller 串行化（TRIM 时该 LBA 没有未完成
        // 写）。若将来并发 TRIM，drop 后旧映射仍可读 ⇒ 。
        if (wb_has(ftl->wbuf, l))
        {
            wb_trigger_s trigger;
            memset(&trigger, 0, sizeof(trigger));
            trigger.kind = WBUF_TRIGGER_DROP;
            trigger.lba  = l;
            (void)wb_handle(ftl->wbuf, &trigger, NULL);

            ftl->trim.lba++;
            processed++;
            continue;
        }

        if (l2p_is_ready(ftl->cache, l) == false)
        {
            return ftl_trim_defer(ftl, processed); /* 已起异步加载：游标不动 */
        }
        if (jrn_reserve(ftl->journal, 1U) == false)
        {
            return ftl_trim_defer(ftl, processed); /* 已起异步刷页：游标不动 */
        }

        DWORD old_pba;

        const ftl_status_e rst = l2p_read(ftl->cache, l, &old_pba);
        if (rst == FTL_AGAIN)
        {
            return ftl_trim_defer(ftl, processed);
        }
        if (rst != FTL_OK)
        {
            LOGE(LOG_TAG_FTL "trim: l2p read lba=%lu failed\r\n", l);
            ftl->trim.active = false;
            return FTL_ERROR;
        }

        // Journal 先于 L2P（同 host 写确认）；预留已保证 append 不触发页满分支。
        (void)jrn_append(ftl->journal, l, old_pba, NAND_PBA_INVALID);

        const ftl_status_e wst = l2p_write(ftl->cache, l, NAND_PBA_INVALID);

        ASSERT(wst == FTL_OK);
        (void)wst;

        // 有效计数在两步之后减：它只是 RAM 统计，不进 Journal。
        if (old_pba != NAND_PBA_INVALID)
        {
            const WORD gblock = ftl_pba_gblock(old_pba);

            // 同写确认路径：BAD / count 已清时跳过，避免断言或 `WORD` 回绕。
            ftl_dangling_check(ftl, l, old_pba, FTL_GBLOCK_INVALID);
            ftl_block_sub_valid_if_live(&ftl->alloc.blocks[gblock]);
        }

        ftl->trim.lba++;
        processed++;
    }

    if (processed != 0U)
    {
        timer_deadline_clear(&ftl->trim.deadline);
        jrn_watermark_update(ftl->journal);
    }

    if (ftl->trim.lba >= ftl->trim.end)
    {
        const DWORD n       = ftl->trim.end - ftl->trim.start_lba;
        const DWORD elapsed = (ftl->trim.start_ms != 0U) ? timer_age_ms(&ftl->trim.start_ms) : 0U;

        LOGI(LOG_TAG_TRIM "done n=%lu elapsed=%lums\r\n", n, elapsed);
        ftl->trim.active    = false;
        ftl->trim.start_lba = 0U;
        ftl->trim.start_ms  = 0U;
        timer_deadline_clear(&ftl->trim.deadline);
        return FTL_OK;
    }

    return FTL_AGAIN;
}

/* ── 诊断：FTL 各模块关键现场（CLI `ftl_ctx` / 断言终端）── */

/* 模块缩写过滤器：NULL / "all" = 全部；未知返回 false。 */
typedef struct FTL_DBG_FILTER_S
{
    BOOL all;
    BOOL alloc;
    BOOL pend;
    BOOL seq;
    BOOL cache;
    BOOL jrn;
    BOOL cp;
    BOOL mig;
    BOOL md;
    BOOL wb;
} ftl_dbg_filter_s;

static BOOL ftl_dbg_filter_init(const char *module, ftl_dbg_filter_s *flt)
{
    if ((module == NULL) || (strcmp(module, "all") == 0))
    {
        memset(flt, 0, sizeof(*flt));
        flt->all = true;
        return true;
    }

    memset(flt, 0, sizeof(*flt));
    if (strcmp(module, "alloc") == 0)
    {
        flt->alloc = true;
    }
    else if (strcmp(module, "pend") == 0)
    {
        flt->pend = true;
    }
    else if (strcmp(module, "seq") == 0)
    {
        flt->seq = true;
    }
    else if (strcmp(module, "cache") == 0)
    {
        flt->cache = true;
    }
    else if (strcmp(module, "jrn") == 0)
    {
        flt->jrn = true;
    }
    else if (strcmp(module, "cp") == 0)
    {
        flt->cp = true;
    }
    else if (strcmp(module, "mig") == 0)
    {
        flt->mig = true;
    }
    else if (strcmp(module, "md") == 0)
    {
        flt->md = true;
    }
    else if (strcmp(module, "wb") == 0)
    {
        flt->wb = true;
    }
    else
    {
        return false;
    }
    return true;
}

/**
 * @brief 打印 FTL 各模块的关键现场（可按模块缩写过滤）。
 *
 * @param module NULL / "all" = 全部；alloc / pend / seq / cache / jrn / cp / mig / md / wb。
 * @return true = 参数合法并已输出；false = 未知模块。
 *
 * @note 只读标量快照，不调用会断言的池扫描；CLI `ftl_ctx` 与断言终端共用。
 */
BOOL ftl_debug_dump(const char *module)
{
    ftl_dbg_filter_s flt;

    if (ftl_dbg_filter_init(module, &flt) == false)
    {
        return false;
    }

    if (flt.all)
    {
        LOGW(LOG_TAG_FTL "DBG req=%hu die_max=%hhu\r\n", ftl_op_req_count(), DIE_MAX);
    }

    for (BYTE die = 0U; die < DIE_MAX; die++)
    {
        const ftl_ctx_s *f = &g_ftl[die];

        if ((f->cache == NULL) || (f->journal == NULL) || (f->metadata_dir == NULL))
        {
            continue;
        }

        if (flt.all || flt.alloc)
        {
            LOGW(LOG_TAG_FTL "DBG d=%hhu alloc free=%hu bad=%hu closed=%hu op=%hu/%hu spare=%hu/%hu hot=%d/%hhu/%hhu cold=%d/%hhu\r\n",
                 die,
                 f->alloc.free_count,
                 f->alloc.bad_count,
                 f->alloc.closed_sb_count,
                 alc_op_get_count(&f->alloc, 0U),
                 alc_op_get_count(&f->alloc, 1U),
                 alc_spare_get_count(&f->alloc, 0U),
                 alc_spare_get_count(&f->alloc, 1U),
                 (int)f->alloc.hot_sb.is_open,
                 f->alloc.hot_sb.next_page,
                 f->alloc.hot_sb.page0_programmed,
                 (int)f->alloc.cold_sb.is_open,
                 f->alloc.cold_sb.next_page);
        }
        if (flt.all || flt.pend)
        {
            LOGW(LOG_TAG_FTL "DBG d=%hhu pend slot=%hu group=%hu req=%hu\r\n",
                 die,
                 pend_slot_get_count(&f->pending),
                 pend_group_get_count(&f->pending),
                 ftl_op_req_count());

            for (BYTE gi = 0U; gi < (BYTE)FTL_GROUP_MAX; gi++)
            {
                const pend_group_s *g = &f->pending.groups[gi];

                if (g->used == false)
                {
                    continue;
                }
                LOGW(LOG_TAG_FTL "DBG d=%hhu g=%hhu op=%d lba=%lu next=%lu/%lu out=%hu stalls=%hhu agg=%d\r\n",
                     die,
                     gi,
                     (int)g->op,
                     (DWORD)g->lba,
                     g->next,
                     g->total,
                     g->outstanding,
                     g->stalls,
                     (int)g->agg_status);
            }
        }
        if (flt.all || flt.seq)
        {
            LOGW(LOG_TAG_FTL "DBG d=%hhu seq kind=%d act=%d drain=%d own=%hu next=%lu wr=%hhu/%hhu head=%hhu close=%d/%d\r\n",
                 die,
                 (int)f->cache_stream.seq.kind,
                 (int)f->cache_stream.seq.active,
                 (int)f->cache_stream.seq.draining,
                 f->cache_stream.seq.owner_gi,
                 f->cache_stream.seq.next_lba,
                 f->cache_stream.seq.wr.count,
                 f->cache_stream.seq.wr_inflight,
                 f->cache_stream.seq.wr.head,
                 (int)f->cache_stream.seq.close_requested,
                 (int)f->cache_stream.seq.closing);
            if (f->cache_stream.seq.wr.count > 0U)
            {
                const ftl_cache_page_s *hp = &f->cache_stream.seq.wr.page[f->cache_stream.seq.wr.head];
                LOGW(LOG_TAG_FTL "DBG d=%hhu seq head lba=%lu st=%hhu proven=%d cw=0x%02hhx\r\n",
                     die,
                     hp->lba,
                     hp->state,
                     (int)hp->proven,
                     hp->cw_bitmap);
            }
        }
        if (flt.all || flt.cache)
        {
            LOGW(LOG_TAG_FTL "DBG d=%hhu cache load=%hhu flush=%hhu cmt=%lu flags def=%d bg=%d l2p=%d md=%d\r\n",
                 die,
                 f->cache->inflight.load,
                 f->cache->inflight.flush,
                 f->cache->stats.cmt_dirty,
                 (int)f->flags.deferred_error,
                 (int)f->flags.bg_suspend,
                 (int)f->flags.l2p_defer,
                 (int)f->flags.md_load_failed);
        }
        if (flt.all || flt.jrn)
        {
            LOGW(LOG_TAG_FTL "DBG d=%hhu jrn seq=%lu water=%hhu idle=%d pend=%d emerg=%d\r\n",
                 die,
                 jrn_sequence_get(f->journal),
                 jrn_watermark_get(f->journal),
                 (int)jrn_is_idle(f->journal),
                 (int)jrn_has_pending_entries(f->journal),
                 (int)jrn_is_emergency(f->journal));
        }
        if (flt.all || flt.cp)
        {
            LOGW(LOG_TAG_FTL "DBG d=%hhu cp act=%d/%d ph=%d\r\n",
                 die,
                 (int)f->cp_handle->active,
                 (int)f->cp_handle->waiting,
                 (int)f->cp_handle->phase);
        }
        if (flt.all || flt.mig)
        {
            LOGW(LOG_TAG_FTL "DBG d=%hhu mig act=%d/%d ph=%d\r\n",
                 die,
                 (int)f->mig_handle->job.active,
                 (int)f->mig_handle->job.waiting,
                 (int)f->mig_handle->job.phase);
        }
        if (flt.all || flt.md)
        {
            LOGW(LOG_TAG_FTL "DBG d=%hhu md idle=%d ph=%d\r\n",
                 die,
                 (int)md_handle_is_idle(f->metadata_dir),
                 (int)f->metadata_dir->handle.phase);
        }
        if (flt.all || flt.wb)
        {
            LOGW(LOG_TAG_FTL "DBG d=%hhu wb open=%d base=%lu occ=0x%02hhX own=0x%08lX err=%d\r\n",
                 die,
                 (int)f->wbuf->open,
                 f->wbuf->base,
                 f->wbuf->occupied,
                 f->wbuf->owners,
                 (int)f->wbuf->error);
        }
    }
    return true;
}

void ftl_assert_context(void)
{
    (void)ftl_debug_dump(NULL);
}
