/*
 * migrate.c — Super Block 搬移引擎（GC / 静态 WL / 慢死预测共用）
 *
 * 职责：
 *   把 victim SB 的有效 CW 逐页搬到 Cold 流新块，更新 L2P / Journal / 有效计数，
 *   最后擦除 victim 并归还（GC / 静态 WL）或退役（慢死预测）。三处搬移的差别只在**谁当 victim**
 *   （kind）与**擦完怎么处置**（post），故共用一台异步 job。
 *
 * 文件结构：
 *   1. 内部工具       mig_src_gblock / mig_victim_from_planes / mig_next_*（static）
 *   2. 提交原语       mig_submit / mig_submit_read / mig_submit_write / mig_submit_erase（static）
 *   3. 启动           mig_start（static）/ mig_begin_gc / mig_begin_wl / mig_begin_predict
 *   4. 收尾           mig_abort / mig_stall / mig_next_page / mig_finish（static）
 *   5. 推进           ／各 phase 处理函数（static）/ mig_step（static）/ mig_handle
 *   6. 完成服务       ftl_migrate_callback（g_op_callbacks[FTL_OP_GC] / [FTL_OP_WL]）
 *
 * 设计要点见 include/ftl/migrate.h（状态机、单飞行、F10 契约）。
 *
 * ⚠ 全部按**全局块号**索引 BST（跨 plane），退化单 plane 条目跳过哨兵 plane。
 *
 * ⚠ **本文件每条命令都走 ftl_async_* + ftl_op_submit（不同步等待）**——服务函数里再跑
 *   完成泵就是嵌套泵（见 ftl_op.h）。搬移主体（读/写/擦）全部异步。
 *
 * ⚠ **L2P 反查/改写在独立的纯计算阶段**（MIG_READ_SETTLE / MIG_WRITE_SETTLE）：
 *   读完成时逐 CW 反查有效性、写完成时改 L2P——二者的 LBA 取自 victim 块的**页内 META**，
 *   与刚访问过的 host LBA 无必然联系 ⇒ region miss 是**真实可达**的。放进服务函数里做
 *   就得同步等加载（嵌套泵）；放在纯计算阶段可以「先起异步加载、原地返回、下轮重扫」。
 *   ⚠ 这两个阶段**必须先就绪前置再动账**（否则半途而废会重复记账 / 位图一半有一半没有）。
 *   ⚠ MIG_WRITE_SETTLE 在动账前还要 `jrn_reserve` 预留 Journal 槽位；不足时只起
 *     异步刷页并挂起本 job，不同步刷 Journal。
 */

#include "migrate.h"

#include "assertion.h"
#include "bbt.h"              /* bbt_mark_bad */
#include "bg_scheduler.h"     /* BG_LEVEL_*（bg_info_t.load_level） */
#include "bst.h"              /* ftl_block_state / ftl_gblock */
#include "fc.h"               /* fc_command_release / fc_command_id_to_command */
#include "ftl.h"              /* ftl_ctx_t（仅完成服务的路由断言） */
#include "ftl_fc_interface.h" /* ftl_async_read_page / ftl_async_write_page_cw / ftl_async_erase_block */
#include "ftl_job.h"          /* 心跳（[JOB] 观测口）*/
#include "ftl_op.h"           /* ftl_op_submit / FTL_OP_GC / FTL_OP_WL */
#include "gc.h"               /* gc_select_victim / gc_decode_cw_lba */
#include "journal.h"          /* jrn_handle / jrn_append / JRN_IDLE */
#include "l2p_cache.h"        /* l2p_read / l2p_write */
#include "log.h"
#include "read_recovery.h" /* rr_record_ecc / rr_predictive_scan / rr_retire_block */
#include "wear_leveling.h" /* wl_should_trigger / wl_select_victim */

#include <string.h>

/* 前置声明：状态机主体定义在第 5 节（mig_start 要先见到它）。 */
static void mig_step(mig_handle_t *m);

/* ── 1. 内部工具 ── */

/* 用 plane_blocks[] + 写指针组装本引擎的 victim 视图。
 *
 * `next_page` **必须**一起带过来：它是该块已编程页数的上界，搬移只读 [0, next_page)。
 * 半写块（提前关闭 / GC 目的块）的空洞页物理上是擦除态，读回来 ECC 必失败；
 * 不按写指针截断就会把空洞当成「victim 读失败」并误标坏块（RISK-GC-11）。
 * temperature 不参与搬移决策，保持 memset 的默认值。 */
static void mig_victim_from_planes(super_block_t *sb, const WORD plane_blocks[PLANE_NUM], BYTE next_page)
{
    memset(sb, 0, sizeof(*sb));
    for (BYTE p = 0U; p < PLANE_NUM; p++)
    {
        sb->plane_blocks[p] = plane_blocks[p];
    }
    sb->next_page = next_page;
}

/* 从 from 起找下一个**仍归本 victim 所有**的 plane；没有返回 PLANE_NUM。
 *
 * 只接受 BST state == CLOSED 的 plane：
 *   · 哨兵 plane（退化单 plane 条目）不是本 victim 的物理块；
 *   · 部分 plane 已擦除后 job 中止，重试时那些 plane 已是 FREE/RETIRED/BAD，
 *     再擦一次会重复计磨损 / 复活 BAD；
 *   · 迁移期间被 RR 标 BAD 的 plane 不能再读/擦。 */
static BYTE mig_next_present_plane(const alc_t *alloc, const mig_handle_t *m, BYTE from)
{
    for (BYTE p = from; p < PLANE_NUM; p++)
    {
        if (!super_block_has_plane(&m->victim, p))
        {
            continue;
        }

        const WORD g = ftl_gblock(p, m->victim.plane_blocks[p]);

        if (ftl_block_state(&alloc->blocks[g]) != FTL_BLOCK_STATE_CLOSED)
        {
            continue;
        }

        return p;
    }

    return PLANE_NUM;
}

/* 从 from 起找下一个**本页有有效 CW** 的 plane；没有返回 PLANE_NUM。 */
static BYTE mig_next_data_plane(const mig_handle_t *m, BYTE from)
{
    BYTE p;

    for (p = from; p < PLANE_NUM; p++)
    {
        if (m->cw_bitmap[p] != 0U)
        {
            break;
        }
    }
    return p;
}

/* ── 2. 提交原语 ── */

/* 提交一条已建好的搬移命令：置 waiting、提交、失败回滚。
 * ⚠ stall_ms 只在**提交成功之后**清零：`ftl_op_submit` 自身失败（完成表满 / SQ 满）
 *   也属于"提交不出去"，提前清零会让 deadline 永不触发。 */
static ftl_status_e mig_submit(mig_handle_t *m, fc_command_t *cmd)
{
    m->waiting = true;

    /* GC 与 WL 共用同一服务函数，但 op tag 分开——日志/调试时能看出命令属于哪条路径。 */
    const ftl_op_id_e op_tag = (m->kind == MIG_KIND_GC) ? FTL_OP_GC : FTL_OP_WL;

    if (ftl_op_submit(cmd, op_tag, m) != FTL_OK)
    {
        m->waiting = false;
        (void)fc_command_release(cmd);
        return FTL_AGAIN;
    }

    m->stall_ms    = 0U;
    m->wait_ms     = 0U;
    m->cmd_wait_ms = 0U;
    return FTL_OK;
}

/* 提交一条 victim 页读（整页，4 CW）。数据在完成时由服务函数拷进 m->page_buf
 * ——**读**不像写那样在 prepare 阶段搬数据（见 ftl_async_read_page 的注释）。 */
static ftl_status_e mig_submit_read(mig_handle_t *m)
{
    fc_command_t      *cmd;
    const ftl_status_e ret = ftl_async_read_page(&cmd, m->src[m->plane], FTL_PAGE_CFG_DATA_ANY());
    if (ret != FTL_OK)
    {
        return ret; /* FTL_AGAIN 命令池空 / FTL_ERROR 永久错误，由调用点区分 */
    }

    return mig_submit(m, cmd);
}

/* 提交一条目标页 CW 子集写。只编有效 CW（cw_bitmap 外的 CW 不建链、不编程）——
 * 整页重写会把无效 CW 连同其（陈旧的）META 一起搬过去，污染新页且多搬无用数据。
 * ⚠ 目标页的 plane 位就是 plane（alc_alloc_pair 的契约），故写进去的 META
 *   逐 CW 仍是正确的扇区 LBA。 */
static ftl_status_e mig_submit_write(mig_handle_t *m)
{
    fc_command_t      *cmd;
    const ftl_status_e ret = ftl_async_write_page_cw(&cmd,
                                                     m->dst[m->plane],
                                                     m->page_buf[m->plane],
                                                     m->cw_bitmap[m->plane],
                                                     FTL_PAGE_CFG_DATA(ftl_lba_page(m->base[m->plane])));
    if (ret != FTL_OK)
    {
        return ret; /* FTL_AGAIN 命令池空 / FTL_ERROR 永久错误，由调用点区分 */
    }

    return mig_submit(m, cmd);
}

/* 提交一条 victim 块擦除。 */
static ftl_status_e mig_submit_erase(mig_handle_t *m)
{
    const DWORD block_addr = super_block_page_pba(m->topo, &m->victim, m->plane, 0U);

    fc_command_t      *cmd;
    const ftl_status_e ret = ftl_async_erase_block(&cmd, block_addr);
    if (ret != FTL_OK)
    {
        return ret; /* FTL_AGAIN 命令池空 / FTL_ERROR 永久错误，由调用点区分 */
    }

    return mig_submit(m, cmd);
}

/* ── 3. 启动 ── */

/* --------------------------------------------------------------------
 * mig_start
 *   功能：装配并启动一个搬移 job（SELECT 阶段）。
 *   参数：ftl —— FTL 实例；victim —— 选中的 SB（只读，内部按值拷贝）；
 *         kind —— victim 来源；post —— 收尾方式。三者都是调用者保证的前置条件。
 *   实现要点：**快路径**——各 plane valid_count 之和 == 0 ⇒ 直接进 ERASE（省整块 NAND
 *             读；整块空洞 plane 的根因已由页打包写缓冲消除）。
 *             victim 按值拷贝：closed_sb_list 在搬移期间可能被 allocator 追加/挪动，
 *             持指针会让「本 job 的 victim」随列表漂移（allocator.h 不变量③）。
 * -------------------------------------------------------------------- */
static void mig_start(mig_handle_t        *m,
                      const super_block_t *victim,
                      mig_kind_e           kind,
                      mig_post_e           post,
                      bool                 dst_high_wear)
{
    /* 前置条件（判据 2）：唯一调用者是本文件的 mig_begin_*，各自持有实例与 victim；
     * active 是运行时状态（单飞行），由调用者在选块**之前**判过 ⇒ 这里是死守卫。 */
    ASSERT(!m->active);

    /* job 状态整体复位，但**窄上下文 / 自持节拍 / 单调计数 / 连续失败计数**必须跨 job 保留。 */
    alc_t *const       alloc            = m->alloc;
    l2p_jrn_t *const   journal          = m->journal;
    l2p_cache_t *const cache            = m->cache;
    const DWORD *const total_writes     = m->total_writes;
    const ftl_topo_t   topo             = m->topo;
    const DWORD        gc_eval          = m->gc_eval_last_ms;
    const DWORD        gc_log           = m->gc_log_last_ms;
    const DWORD        wl_eval          = m->wl_eval_last_ms;
    const DWORD        wl_writes        = m->wl_last_writes;
    const DWORD        predict          = m->predict_last_ms;
    const DWORD        reclaimed        = m->reclaimed_count;
    const DWORD        novictim         = m->novictim_count;
    const DWORD        nogain_cnt       = m->nogain_count;
    const BYTE         read_fail_streak = m->read_fail_streak;
    const bool         paused           = m->paused;
    /* GC 收益判据的状态（RISK-GC-13/14）：判据结论/退避/观测必须跨 job 存活。 */
    const DWORD gc_writes_last          = m->gc_writes_last;
    const DWORD gc_write_last_ms        = m->gc_write_last_ms;
    const bool  host_writing            = m->host_writing;
    const bool  gc_no_gain              = m->gc_no_gain;
    const BYTE  nogain_mode             = m->gc_nogain_mode;
    const DWORD nogain_ms               = m->gc_nogain_ms;
    const DWORD nogain_writes           = m->gc_nogain_writes;
    const WORD  nogain_free             = m->gc_nogain_free;
    const WORD  best_valid_pct          = m->gc_best_valid_pct;
    const BYTE  last_mode               = m->gc_last_mode;
    WORD        fail_gblock[PLANE_NUM];
    BYTE        fail_count[PLANE_NUM];

    for (BYTE plane = 0U; plane < PLANE_NUM; plane++)
    {
        fail_gblock[plane] = m->fail_gblock[plane];
        fail_count[plane]  = m->fail_count[plane];
    }

    memset(m, 0, sizeof(*m));

    m->alloc             = alloc;
    m->journal           = journal;
    m->cache             = cache;
    m->total_writes      = total_writes;
    m->topo              = topo;
    m->gc_eval_last_ms   = gc_eval;
    m->gc_log_last_ms    = gc_log;
    m->wl_eval_last_ms   = wl_eval;
    m->wl_last_writes    = wl_writes;
    m->predict_last_ms   = predict;
    m->reclaimed_count   = reclaimed;
    m->novictim_count    = novictim;
    m->nogain_count      = nogain_cnt;
    m->gc_writes_last    = gc_writes_last;
    m->gc_write_last_ms  = gc_write_last_ms;
    m->host_writing      = host_writing;
    m->gc_no_gain        = gc_no_gain;
    m->gc_nogain_mode    = nogain_mode;
    m->gc_nogain_ms      = nogain_ms;
    m->gc_nogain_writes  = nogain_writes;
    m->gc_nogain_free    = nogain_free;
    m->gc_best_valid_pct = best_valid_pct;
    m->gc_last_mode      = last_mode;
    /* 系统性读失败计数必须跨 job 存活：它是「连续多个 victim 都读失败」的判据，
     * 一个 job 最多标 2 块（每 plane 一块）⇒ 若每个 job 都清零，阈值永远到不了。
     * `paused` 同理不能因 memset 丢失：熔断后任何路径误启动新 job 都不得复活 GC。 */
    m->read_fail_streak  = read_fail_streak;
    m->paused            = paused;
    for (BYTE plane = 0U; plane < PLANE_NUM; plane++)
    {
        m->fail_gblock[plane] = fail_gblock[plane];
        m->fail_count[plane]  = fail_count[plane];
    }

    m->victim         = *victim;
    m->victim.is_open = false; /* 本引擎只处理 Closed SB（见 allocator.h） */
    ASSERT(m->victim.next_page <= PAGES_PER_BLOCK);
    m->kind          = kind;
    m->post          = post;
    m->dst_high_wear = dst_high_wear;
    m->active        = true;
    m->error         = (int32_t)FTL_OK;

    /* 快路径：无任何有效 CW ⇒ 无可搬移，直接进擦除。 */
    alc_sb_stats_t st;
    alc_get_sb_stats(m->alloc, &m->victim, &st);
    m->job_valid_cw = st.valid_cw;
    /* 有有效 CW ⇒ 必然写过至少一页。写指针为 0 说明 victim 构造时漏了 next_page，
     * 后果是「一页不读就擦」= 静默丢数据。 */
    ASSERT((st.valid_cw == 0U) || (m->victim.next_page > 0U));
    m->phase = (st.valid_cw == 0U) ? MIG_ERASE : MIG_READ;

    mig_step(m);
}

static const char *mig_gc_mode_name(gc_benefit_mode_e mode)
{
    switch (mode)
    {
        case GC_BENEFIT_IDLE:
            return "idle";
        case GC_BENEFIT_EMERGENCY:
            return "emg";
        default:
            return "off";
    }
}

/* 按触发位与 host 写流选收益判据模式（RISK-GC-13）。
 * T5 磨损（含与 T1 混合）不筛：磨损均衡需要搬冷块，加收益判据会饿死 WL。 */
static gc_benefit_mode_e mig_gc_mode(DWORD triggers, bool host_writing)
{
    if ((triggers & GC_TRIGGER_WEAR) != 0U)
    {
        return GC_BENEFIT_OFF;
    }
    if ((triggers & GC_TRIGGER_EMERGENCY) != 0U)
    {
        return GC_BENEFIT_EMERGENCY;
    }
    if ((triggers & GC_TRIGGER_WATERMARK) != 0U)
    {
        return host_writing ? GC_BENEFIT_OFF : GC_BENEFIT_IDLE;
    }
    return GC_BENEFIT_OFF;
}

/* 记录一次「空间 GC 没有值得搬的 victim」结论：退避 + 写路径 NO_SPACE 判据都依赖它。
 * ⚠ 只由空间 GC（mode != OFF）调用；T5 不产生该结论。 */
static void mig_note_no_gain(mig_handle_t *m, gc_benefit_mode_e mode, WORD best_valid_pct, DWORD now_ms)
{
    const bool first = !m->gc_no_gain;

    m->novictim_count++;
    m->nogain_count++;
    m->gc_best_valid_pct = best_valid_pct;
    m->gc_last_mode      = (BYTE)mode;
    m->gc_no_gain        = true;
    m->gc_nogain_mode    = (BYTE)mode;
    m->gc_nogain_ms      = now_ms;
    m->gc_nogain_writes  = *m->total_writes;
    m->gc_nogain_free    = m->alloc->free_count;

    if (first)
    {
        LOGW("[GC] no-gain mode=%s bestvr=%u free=%u writes=%lu — suspend until writes/emergency\r\n",
             mig_gc_mode_name(mode),
             (unsigned)best_valid_pct,
             (unsigned)m->alloc->free_count,
             *m->total_writes);
    }
}

/* 无收益结论后的复扫门（只挡 closed-list 扫描，不挡 10 ms 的 gc_should_trigger 计数器）。 */
static bool mig_gc_scan_allowed(const mig_handle_t *m, gc_benefit_mode_e mode, DWORD triggers, DWORD now_ms)
{
    if (!m->gc_no_gain)
    {
        return true;
    }
    if (mode == GC_BENEFIT_OFF)
    {
        return true; /* host 在写 / T5：必须重新扫 */
    }
    if (now_ms == 0U)
    {
        return true; /* 时钟缺失：保持「每轮求值」的旧语义 */
    }
    if (((triggers & GC_TRIGGER_EMERGENCY) != 0U) && (m->gc_nogain_mode != (BYTE)GC_BENEFIT_EMERGENCY))
    {
        return true; /* 刚从 IDLE 进入紧急：立即重扫 */
    }
    if (*m->total_writes != m->gc_nogain_writes)
    {
        return true; /* host 有新写入：可能产生了新垃圾 */
    }
    return (now_ms - m->gc_nogain_ms) >= GC_NO_GAIN_BACKOFF_MS;
}

static bool mig_begin_gc(mig_handle_t *m, gc_benefit_mode_e mode, DWORD now_ms)
{
    /* 前置条件（判据 2）：调用者 ftl.c 传 &g_ftl[...]。 */
    if (m->active)
    {
        return false;
    }
    if (jrn_has_write_failure(m->journal))
    {
        return false; /* L2P 尚未 durable：禁止回收，等 CP 清 pending */
    }

    gc_select_out_t   sel;
    const gc_select_e pick = gc_select_victim(m->alloc, mode, &sel);

    m->gc_last_mode      = (BYTE)mode;
    m->gc_best_valid_pct = sel.best_valid_pct;

    if (pick != GC_SELECT_OK)
    {
        if (pick == GC_SELECT_NOT_WORTHWHILE)
        {
            mig_note_no_gain(m, mode, sel.best_valid_pct, now_ms);
        }
        else
        {
            m->novictim_count++; /* 触发但根本没有候选 */
        }
        return false;
    }

    m->gc_no_gain = false;
    mig_start(m, sel.victim, MIG_KIND_GC, MIG_POST_FREE, false);
    return true;
}

static bool mig_begin_wl(mig_handle_t *m)
{
    /* 前置条件（判据 2）：调用者 ftl.c 传 &g_ftl[...]。 */
    if (m->active)
    {
        return false;
    }
    if (jrn_has_write_failure(m->journal))
    {
        return false; /* L2P 尚未 durable：禁止回收，等 CP 清 pending */
    }
    const DWORD total_writes = *m->total_writes;

    if (!wl_should_trigger(m->alloc, total_writes))
    {
        return false;
    }

    /* 容量紧张时把单飞行引擎让给 GC：WL 只是磨损维护，GC 是前台写的前置条件。
     * （FREE_POOL_TARGET 也是 GC T1 的触发线；低于/等于它时不应再启动 WL。） */
    if (m->alloc->free_count <= FREE_POOL_TARGET)
    {
        return false;
    }

    wl_victim_t victim;
    if (!wl_select_victim(m->alloc, &victim))
    {
        return false;
    }

    super_block_t sb;
    mig_victim_from_planes(&sb, victim.plane_blocks, victim.next_page);

    /* 目标块方向：先把 host 的 cold SB 摘掉，让 job 的第一页新开 high-wear SB。 */
    alc_prepare_wl_destination(m->alloc);

    /* 静态 WL 的方向：目标取高擦除空闲块（UBI 模型）。 */
    mig_start(m, &sb, MIG_KIND_WL, MIG_POST_FREE, true);
    return true;
}

static bool mig_begin_predict(mig_handle_t *m)
{
    /* 前置条件（判据 2）：调用者 ftl.c 传 &g_ftl[...]。 */
    if (m->active)
    {
        return false;
    }
    if (jrn_has_write_failure(m->journal))
    {
        return false; /* L2P 尚未 durable：禁止回收，等 CP 清 pending */
    }

    WORD gblock;
    if (!rr_predictive_scan(m->alloc, &gblock))
    {
        return false;
    }

    /* 慢死块必在 closed_sb_list 中（scan 只返回 CLOSED 块）。 */
    const WORD closed_idx   = ftl_block_sb_id(&m->alloc->blocks[gblock]);
    const WORD closed_count = m->alloc->closed_sb_count;

    ASSERT(closed_idx < closed_count);

    /* ⚠ [待上板] release 运行期拒绝（H14）：ASSERT 在 NDEBUG 下裁空，越界会拿垃圾
     * plane_blocks 去擦块 = 静默介质破坏。 */
    if (closed_idx >= closed_count)
    {
        LOGE("[MIG] predict: stale sb_id=%u (closed=%u) — ignored\r\n", (unsigned)closed_idx, (unsigned)closed_count);
        return false;
    }

    const super_block_t *closed = alc_closed_at(m->alloc, closed_idx);
    super_block_t        sb;

    mig_victim_from_planes(&sb, closed->plane_blocks, closed->next_page);

    mig_start(m, &sb, MIG_KIND_PREDICT, MIG_POST_RETIRE, false);
    return true;
}

/* ── 4. 收尾 ── */

/* abort 日志限流（RISK-MIG-05）：坏块介质上 GC 会反复选中不可读 victim、每次
 * 重试都 abort；逐次 WRN 会把有效进展淹没。5 s 一行 + suppressed 计数；
 * `mark BAD` 仍逐块 LOGE（每个坏块只发生一次，是真正的结论）。 */
static DWORD s_abort_log_ms;
static DWORD s_abort_n;

static bool mig_abort_log_due(DWORD *out_suppressed)
{
    const DWORD now_ms = ftl_job_now_ms();

    bool due = (s_abort_n >= 64U);

    if ((!due) && (now_ms != 0U))
    {
        due = (s_abort_log_ms == 0U) ||
              ((now_ms - s_abort_log_ms) >= FTL_JOB_LOG_SLOW_INTERVAL_MS);
    }

    if (!due)
    {
        s_abort_n++;
        return false;
    }

    *out_suppressed = s_abort_n;
    s_abort_n       = 0U;
    s_abort_log_ms  = now_ms;
    return true;
}

/* 中止 job：不擦 victim、不动 L2P、不移除 closed 条目（源数据仍有效，下轮重试）。
 * 默认档限流打一行 abort；逐 plane 源块号属诊断细节，降到 LOGD。 */
static void mig_abort(mig_handle_t *m, const char *reason)
{
    DWORD suppressed = 0U;

    if (mig_abort_log_due(&suppressed))
    {
        char stbuf[48];

        (void)fc_cw_status_str(m->last_fc_status, stbuf, sizeof(stbuf));
        LOGW("[MIG] abort kind=%u phase=%u page=%u plane=%u — %s fc=0x%08lx(%s) supp=%lu\r\n",
             (unsigned)m->kind,
             (unsigned)m->phase,
             (unsigned)m->page,
             (unsigned)m->plane,
             reason,
             m->last_fc_status,
             stbuf,
             suppressed);
    }
    for (BYTE p = 0U; p < PLANE_NUM; p++)
    {
        if (!super_block_has_plane(&m->victim, p))
        {
            continue;
        }
        LOGD("[MIG] victim plane%u block=%u\r\n", (unsigned)p, (unsigned)m->victim.plane_blocks[p]);
    }

    m->aborted = true;
    m->error   = (int32_t)FTL_ERROR; /* status.error：最近一次中止的归因 */
    m->phase   = MIG_DONE;
}

/* 时钟在一个 deadline 起点之后是否已越过 timeout。0 起点 = 刚开始计时。 */
static bool mig_deadline_expired(DWORD *start_ms, DWORD timeout_ms)
{
    const DWORD now = ftl_job_now_ms();

    if (now == 0U)
    {
        return false; /* 平台时钟缺失：deadline 不生效（本板不可达，见 migrate.h） */
    }
    if (*start_ms == 0U)
    {
        *start_ms = now;
        return false;
    }
    return (now - *start_ms) >= timeout_ms;
}

/* 提交不出去超过 FTL_SUBMIT_TIMEOUT_MS ⇒ 中止 job。返回 true = 已中止。 */
static bool mig_stall(mig_handle_t *m)
{
    if (mig_deadline_expired(&m->stall_ms, FTL_SUBMIT_TIMEOUT_MS))
    {
        m->timed_out     = 1U;
        m->timeout_phase = m->phase;
        mig_abort(m, "submit timeout");
        return true;
    }
    return false;
}

/* 等外部模块超过 FTL_WAIT_TIMEOUT_MS ⇒ 中止 job（不擦 victim）。返回 true = 已中止。
 * ⚠ 与 mig_stall 分开：外部命令在飞也计时——"在飞"不等于"会完成"。 */
static bool mig_wait(mig_handle_t *m, const char *reason)
{
    if (mig_deadline_expired(&m->wait_ms, FTL_WAIT_TIMEOUT_MS))
    {
        m->timed_out     = 1U;
        m->timeout_phase = m->phase;
        mig_abort(m, reason);
        return true;
    }
    return false;
}

/* 推进到下一页（清本页状态）。读/写两阶段的页推进都走这里。 */
static void mig_next_page(mig_handle_t *m)
{
    m->page      = (m->page + 1U);
    m->plane     = 0U;
    m->decode_ok = false;
    m->write_ok  = false;
    for (BYTE p = 0U; p < PLANE_NUM; p++)
    {
        m->cw_bitmap[p] = 0U;
        m->base[p]      = 0U;
    }
    m->phase = MIG_READ;
}

/* 当前目标页对里，m->plane 之前是否已有 plane 被成功编程。
 * mig_step_write 只在上一 plane 的 WRITE_SETTLE 成功后才会推进到下一 plane，
 * 所以“前序 plane 的 cw_bitmap 非 0”⇒ 它已编程。 */
static bool mig_dst_pair_has_programmed_plane(const mig_handle_t *m)
{
    for (BYTE p = 0U; p < m->plane; p++)
    {
        if (m->cw_bitmap[p] != 0U)
        {
            return true;
        }
    }
    return false;
}

/* 目标页失败处置（RISK-GC-02）：
 * - 命令从未提交（建链失败 / stall 超时）且本页对没有任何 plane 编程 ⇒ rewind；
 * - 命令已提交/可能半编程，或前序 plane 已编程 ⇒ 立即关闭 Cold SB。
 * 绝不能在同一个 Open SB 上继续写下一页。 */
static void mig_dst_fail_cleanup(mig_handle_t *m, bool command_attempted)
{
    if (command_attempted || mig_dst_pair_has_programmed_plane(m))
    {
        LOGW("[MIG] dst failure plane=%u — close cold SB\r\n", (unsigned)m->plane);
        (void)alc_close_open_sb(m->alloc, FTL_TEMP_COLD);
    }
    else
    {
        LOGW("[MIG] dst failure plane=%u — rewind pair\r\n", (unsigned)m->plane);
        alc_rewind_pair(m->alloc, FTL_TEMP_COLD);
    }
}

/* --------------------------------------------------------------------
 * mig_finish
 *   功能：DONE——移除 closed 条目、记进展计数、复位 job。
 *   参数：ftl —— FTL 实例。
 *   返回：无。
 *   实现要点：定位 victim 在 closed_sb_list 的索引由 allocator 统一完成（比对全部 plane；
 *             Policy B 下退化单 plane 条目的哨兵 plane 相同，只比一个 plane 会命中另一条）。
 *             aborted ⇒ 不做任何列表/计数变更（victim 未被擦，仍有效）。
 * -------------------------------------------------------------------- */
static void mig_finish(mig_handle_t *m)
{
    if (!m->aborted)
    {
        /* 整块迁移成功 ⇒ 该块的历史读失败只是瞬态，清零。
         * 只在**整个 job 完成**时清（所有已编程页都读过），不能在单页读成功或
         * 单 plane 擦成功时清：坏页之后的页永远读不到，按单页成功清零会让标坏阈值
         * 永不触发（RISK-GC-07）；逐 plane 清零会让系统性熔断永不触发（RISK-GC-10）。 */
        m->read_fail_streak = 0U;
        for (BYTE plane = 0U; plane < PLANE_NUM; plane++)
        {
            if (!super_block_has_plane(&m->victim, plane))
            {
                continue;
            }
            if (m->fail_gblock[plane] == ftl_gblock(plane, m->victim.plane_blocks[plane]))
            {
                m->fail_gblock[plane] = UINT16_MAX;
                m->fail_count[plane]  = 0U;
            }
        }

        WORD closed_idx;
        /* 逐 plane 擦除时条目可能已被 alc_closed_drop_plane 摘空并移除
         * （最后一个 plane 摘除即删条目）⇒ 此时按原值查找必然失败。 */
        bool removed = m->closed_removed;

        /* 移除已回收条目（swap-remove）——防 closed_sb_count 只增不减导致列表爆炸
         * + 已回收 SB 被再次选为 victim 的无限 GC 空转。
         * 擦除失败也移除：该 SB 已坏/不可再用，留在列表里只会反复被选。 */
        if ((!removed) && alc_find_closed_sb(m->alloc, &m->victim, &closed_idx))
        {
            alc_remove_closed_sb(m->alloc, closed_idx);
            removed = true;
        }

        /* 进展计数 / 预测日志只在**真正回收成功**时记：擦除失败或条目没找到都不算。 */
        if (removed && (!m->erase_failed))
        {
            if (m->kind == MIG_KIND_GC)
            {
                m->reclaimed_count++; /* 进展计数（见 ftl.h：区分"在推进"与"卡死"） */
            }
            else if (m->kind == MIG_KIND_PREDICT)
            {
                /* 慢死预测命中：整 SB 已搬空并退役，属生命周期跃迁，留痕一次 */
                for (BYTE p = 0U; p < PLANE_NUM; p++)
                {
                    if (!super_block_has_plane(&m->victim, p))
                    {
                        continue;
                    }
                    LOGW("[FTL] predictive relocate retired plane%u=%u\r\n", (unsigned)p, (unsigned)m->victim.plane_blocks[p]);
                }
            }
        }
    }

    /* 只在**真正搬了数据**的 victim 上打收尾行。
     * vcw=0 / pages=0 的“整块已无效、直接擦除归还”在近满盘写负载下每秒几十条，
     * 会把有效日志淹没（多次反馈）；这类空块回收仍计入 reclaimed_count。 */
    if ((!m->aborted) && ((m->migrated != 0U) || (m->job_valid_cw != 0U)))
    {
        const char *kind_s  = (m->kind == MIG_KIND_GC) ? "gc" : ((m->kind == MIG_KIND_WL) ? "wl" : "pred");
        const char *post_s  = ((mig_post_e)m->post == MIG_POST_RETIRE) ? "retire" : "free";
        const DWORD elapsed = (m->start_ms != 0U) ? ftl_job_age_ms(&m->start_ms) : 0U;

        LOGI("[MIG] done kind=%s p0=%u p1=%u vcw=%u pages=%u post=%s err=%u elapsed=%lums\r\n",
             kind_s,
             (unsigned)m->victim.plane_blocks[0],
             (unsigned)m->victim.plane_blocks[1],
             (unsigned)m->job_valid_cw,
             (unsigned)m->page,
             post_s,
             (unsigned)((m->erase_failed != 0U) ? 1U : 0U),
             (unsigned long)elapsed);
    }

    m->active         = false;
    m->phase          = MIG_IDLE;
    m->waiting        = false;
    m->decode_ok      = false;
    m->write_ok       = false;
    m->closed_removed = false;
    m->stall_ms       = 0U;
    m->wait_ms        = 0U;
    m->cmd_wait_ms    = 0U;
    /* fail_gblock[] / fail_count[] 跨 job 保留（RISK-GC-01/07）：只在本 job 正常完成
     * （上方整块迁移成功）或该 plane 标 BAD 时清零，不在这里无条件清。 */
}

/* ── 5. 推进 ── */

/* --------------------------------------------------------------------
 * 各 phase 处理函数
 *   返回 true = 本轮到此为止（提交了一条命令 / 提交失败 / 原地等资源）；
 *   返回 false = 纯计算推进了一步，mig_step 继续处理下一个 phase。
 * -------------------------------------------------------------------- */

static bool mig_step_read(mig_handle_t *m)
{
    /* 只读该块**已编程**的页 [0, next_page)。空洞页物理上是擦除态，读它只会得到
     * BLANK；跳过它们既省命令，也不给「把空洞误判成坏块」留窗口（RISK-GC-11）。
     * next_page == 0 只可能来自异常路径（正常 victim 至少写过 1 页）：保守按整块扫，
     * 读数由 FC 的擦除页识别兜底。 */
    BYTE page_limit = m->victim.next_page;

    /* release 兜底（H14）：next_page 越界时按整块扫——**绝不能**让它把 page 推出
     * PAGES_PER_BLOCK（那会算出非法 PAA）。next_page == 0 只可能来自异常路径。 */
    if ((page_limit == 0U) || (page_limit > PAGES_PER_BLOCK))
    {
        page_limit = PAGES_PER_BLOCK;
    }

    if (m->page >= page_limit)
    {
        m->phase = MIG_FLUSH_JRN; /* 全部已编程页读完 */
        return false;
    }

    /* 找本页下一个有物理块的 plane */
    m->plane = mig_next_present_plane(m->alloc, m, m->plane);

    if (m->plane >= PLANE_NUM)
    {
        /* 本页各 plane 都处理完 ⇒ 决定去向 */
        bool any = false;

        for (BYTE p = 0U; p < PLANE_NUM; p++)
        {
            if (m->cw_bitmap[p] != 0U)
            {
                any = true;
            }
        }
        if (!any)
        {
            mig_next_page(m); /* 空洞 / 全被覆盖 */
            return false;
        }

        /* 成对分配目标页（Cold 流）。池耗尽 ⇒ 中止（不擦 victim，源数据仍在、
         * L2P 未动，下轮 GC 再来）——**不得**先擦后搬。
         * 静态 WL 走 high-wear 变体：目标取擦除最多的空闲块（RISK-WL-08）。 */
        const ftl_status_e dst_st = m->dst_high_wear
                                        ? alc_alloc_pair_high_wear(m->alloc, m->dst)
                                        : alc_alloc_pair(m->alloc, FTL_TEMP_COLD, m->dst);

        if (dst_st != FTL_OK)
        {
            /* 满盘且选出的 victim 也换不到目的页 ⇒ 与「无收益 victim」同义：
             * 没有可回收的空间。必须置 no-gain，否则写路径会一直重试
             * 「选中 → 目的分配失败 → abort」（RISK-GC-14）。 */
            if (m->kind == MIG_KIND_GC)
            {
                mig_note_no_gain(m, GC_BENEFIT_EMERGENCY, m->gc_best_valid_pct, ftl_job_now_ms());
            }
            mig_abort(m, "no free block");
            return false;
        }

        m->plane = 0U;
        m->phase = MIG_WRITE;
        return false;
    }

    /* 本页本 plane 的源地址（页基址）。全局块号不缓存——现算，见 mig_src_gblock。 */
    m->src[m->plane] = super_block_page_pba(m->topo, &m->victim, m->plane, m->page);
    memset(m->page_buf[m->plane], 0, PAGE_MAIN_SIZE);

    const ftl_status_e ret = mig_submit_read(m);
    if (ret == FTL_AGAIN)
    {
        (void)mig_stall(m);
    }
    else if (ret != FTL_OK)
    {
        mig_abort(m, "read command rejected"); /* [待上板] 永久错误不再空转重试 */
    }
    return true;
}

/* 本 CW 的 META LBA 是否属于本实例且可查 L2P。
 * META 来自介质（H29）：未写过 / 越界 / 不属于本实例的条目一律跳过。 */
static bool mig_lba_usable(const mig_handle_t *m, DWORD lba)
{
    return (lba != FTL_LBA_INVALID) &&
           ((QWORD)lba < L2P_GLOBAL_LBA_MAX) &&
           lba_routes_to(lba, m->topo);
}

static bool mig_step_read_settle(mig_handle_t *m)
{
    const BYTE plane = m->plane;

    if (!m->decode_ok)
    {
        /* 空页（无错误 + META 全 FF）：本 plane 无有效 CW。
         * NAND 读错误已在 mig_complete_read 里中止 job，不会走到这里。 */
        m->cw_bitmap[plane] = 0U;
    }
    else
    {
        /* 就绪前置：本页逐 CW 的扇区 LBA 来自页内 META，与刚访问过的 host LBA
         * **无必然联系** ⇒ region miss 现实可达。未驻留 ⇒ 起异步加载并原地返回，
         * 下轮重扫（重扫幂等：ready 命中不记账、不动 access_count）。 */
        for (BYTE cw = 0U; cw < CW_NUM; cw++)
        {
            if (!mig_lba_usable(m, m->cw_lba[plane][cw]))
            {
                continue;
            }
            if (!l2p_is_ready(m->cache, m->cw_lba[plane][cw]))
            {
                /* 加载在飞不计提交卡死，但**必须**计入独立的等待上限：
                 * 加载永久卡住时 job 否则永不中止（RISK-MIG-01）。 */
                (void)mig_wait(m, "l2p load timeout");
                return true;
            }
        }

        m->wait_ms = 0U; /* 等待条件解除 */

        /* 全部就绪 ⇒ 一次性算出本 plane 的位图（不会半途而废） */
        m->cw_bitmap[plane] = 0U;
        for (BYTE cw = 0U; cw < CW_NUM; cw++)
        {
            DWORD cur;

            if (!mig_lba_usable(m, m->cw_lba[plane][cw]))
            {
                continue;
            }
            /* 用**该 CW 自己的 META** 查，仍指向 (本页, 本 CW) 才算有效 */
            if ((l2p_read(m->cache, m->cw_lba[plane][cw], &cur) == FTL_OK) &&
                (cur == pba_with_cw(m->src[plane], cw)))
            {
                m->cw_bitmap[plane] |= ftl_cw_bit(plane, cw);
            }
        }
    }

    m->decode_ok = false;
    m->phase     = MIG_READ;
    m->plane     = (plane + 1U); /* 可能 == PLANE_NUM ⇒ 页结算 */
    return false;
}

static bool mig_step_write(mig_handle_t *m)
{
    /* 找本页下一个有待写 CW 的 plane */
    m->plane = mig_next_data_plane(m, m->plane);

    if (m->plane >= PLANE_NUM)
    {
        mig_next_page(m);
        return false;
    }

    const ftl_status_e ret = mig_submit_write(m);
    if (ret == FTL_AGAIN)
    {
        if (mig_stall(m))
        {
            /* 命令从未提交：无 plane 编程则 rewind，否则关 Cold SB。 */
            mig_dst_fail_cleanup(m, false);
        }
    }
    else if (ret != FTL_OK)
    {
        mig_dst_fail_cleanup(m, false);
        mig_abort(m, "write command rejected"); /* [待上板] 永久错误不再空转重试 */
    }
    return true;
}

static bool mig_step_write_settle(mig_handle_t *m)
{
    const BYTE plane = m->plane;

    if (m->write_ok)
    {
        /* 就绪前置（同 READ_SETTLE）：全部就绪才动账——半途而废会重复记账
         * （valid_count 虚高、journal 重复条目）。 */
        /* ⚠ [待上板] 位图判据必须与 READ_SETTLE 的 `ftl_cw_bit(plane, cw)` 同源：
         * flat 绝对编号下 plane 1 的有效位在 bit 4..7，按 plane 内相对位读会整段漏账。
         * 验证法：各 plane 写满 → 触发 GC → 读回 plane 1 的 LBA（应仍正确）+ 看
         * journal / valid_count 迁到新页。 */
        for (BYTE cw = 0U; cw < CW_NUM; cw++)
        {
            if ((m->cw_bitmap[plane] & ftl_cw_bit(plane, cw)) == 0U)
            {
                continue;
            }
            if (!l2p_is_ready(m->cache, m->cw_lba[plane][cw]))
            {
                (void)mig_wait(m, "l2p load timeout");
                return true;
            }
        }

        /* Journal 容量预留：本 plane 最多 append popcount(cw_bitmap) 条。不足时
         * jrn_reserve 只起异步刷页并返 false，本 job 挂起等下一轮 service；不在
         * migrate 里同步刷 Journal（那会在 service 调用栈里再开完成泵）。
         * ⚠ wait_ms 必须等容量也拿到才清：否则每轮 L2P ready 都会重置等待超时，
         *    Journal 永久无容量时 job 永不中止。 */
        const BYTE n_entries = ftl_cw_bitmap_popcount(m->cw_bitmap[plane]);

        if (!jrn_reserve(m->journal, n_entries))
        {
            (void)mig_wait(m, "journal capacity timeout");
            return true;
        }

        m->wait_ms = 0U; /* L2P 与 Journal 容量均已就绪 */

        for (BYTE cw = 0U; cw < CW_NUM; cw++)
        {
            if ((m->cw_bitmap[plane] & ftl_cw_bit(plane, cw)) == 0U)
            {
                continue;
            }

            const DWORD l = m->cw_lba[plane][cw]; /* 用该 CW 自己的 META（不再 base+cw 推断） */

            if (!mig_lba_usable(m, l))
            {
                continue;
            }

            /* **提交时重查**：READ_SETTLE 到本阶段之间，host 覆盖写 / TRIM 可能已把
             * 该 LBA 改到别处。只有 L2P 仍指向本 victim 的 CW 才能迁移；否则旧数据
             * 覆盖新映射、valid_count 双降。被抢先的 CW 跳过（它已不再引用 victim）。 */
            DWORD cur;

            if (l2p_read(m->cache, l, &cur) != FTL_OK)
            {
                mig_abort(m, "l2p read failed"); /* [待上板] */
                return false;
            }

            const DWORD src_cw = pba_with_cw(m->src[plane], cw);

            if (cur != src_cw)
            {
                continue; /* host 写 / TRIM 已生效：该 CW 不再引用 victim */
            }

            const DWORD dst_cw = pba_with_cw(m->dst[plane], cw);

            /* journal 先于 L2P（与 host 写路径同序）：append 失败 ⇒ 该映射未持久化，
             * 立即中止，victim 不擦。 */
            if (!jrn_append(m->journal, l, src_cw, dst_cw))
            {
                mig_abort(m, "journal append failed"); /* [待上板] */
                return false;
            }
            if (l2p_write(m->cache, l, dst_cw) != FTL_OK)
            {
                mig_abort(m, "l2p write failed"); /* [待上板] */
                return false;
            }

            /* 有效计数（CW 级）：源块 -1，新块 +1。
             * ⚠ 源块可能在迁移期间被 RR 标 BAD（valid_count 已被清 0）；此时不能再
             *   减（debug 下会撞 sub_valid 断言，release 下会回绕）。数据已读到并写进
             *   目的页，L2P 更新仍然正确；只跳过源侧计数。 */
            const WORD src_gblock = ftl_gblock(plane, m->victim.plane_blocks[plane]);
            const WORD dst_gblock = ftl_pba_gblock(dst_cw);
            ftl_block_sub_valid_if_live(&m->alloc->blocks[src_gblock]);
            ftl_block_add_valid(&m->alloc->blocks[dst_gblock]);

            m->migrated++;
        }

        /* GC/WL 的 Journal 追加同样推进水位；漏掉会让 journal 只靠 T2/T3 触发 CP，
         * 大量搬移时可能撞 ring_full（新背压路径会把 host 写停到超时）。 */
        jrn_update_watermark(m->journal);
    }

    m->write_ok = false;
    m->phase    = MIG_WRITE;
    m->plane    = (plane + 1U);
    return false;
}

static bool mig_step_flush_jrn(mig_handle_t *m)
{
    /* Journal 持久性失败 pending（页写失败 ⇒ 条目已丢）：
     * 在 CP 确认当前 L2P 已落盘之前，任何 victim 都不许擦——包括本 job 没有搬移
     * 任何 CW（migrated == 0）的情况，因为上一次 job 的迁移映射可能只在 RAM 里。 */
    if (jrn_has_write_failure(m->journal))
    {
        mig_abort(m, "journal write failure pending"); /* [待上板] */
        return false;
    }

    /* F10 契约：搬移条目先落盘再擦 victim。journal 是 write-back（page_buf
     * 攒满才 flush），不在此强制落盘，掉电落在"条目未落盘 + victim 已擦除"
     * 窗口 ⇒ 恢复回放指向已擦除块 = 数据损坏。
     * 无搬移条目（migrated == 0）⇒ 无需屏障，直接擦。 */
    if (m->migrated == 0U)
    {
        m->phase = MIG_ERASE;
        m->plane = 0U;
        return false;
    }

    {
        /* typed 入口：job 空闲且有待写页 ⇒ 起异步刷页。info 由 migrate 的调用方
         * 在本阶段后续传入；这里位移推进不依赖动态信息。 */
        jrn_status_t jst;
        jrn_handle(m->journal, NULL, NULL, &jst);
    }

    if (!jrn_is_idle(m->journal))
    {
        /* 有命令在飞不计提交卡死，但计入独立的等待上限：Journal job 永久卡住时
         * job 否则永不中止（RISK-MIG-01）。 */
        (void)mig_wait(m, "journal flush timeout");
        return true;
    }
    if ((jrn_has_erase_pending(m->journal)) || (jrn_has_pending_entries(m->journal)))
    {
        /* 想刷却刷不动（命令池空）⇒ 计卡死轮数；超限中止（**不擦 victim**） */
        (void)mig_stall(m);
        return true;
    }

    m->stall_ms = 0U;
    m->wait_ms  = 0U; /* 屏障达成 */
    m->phase    = MIG_ERASE;
    m->plane    = 0U;
    return false;
}

static bool mig_step_erase(mig_handle_t *m)
{
    m->plane = mig_next_present_plane(m->alloc, m, m->plane);

    if (m->plane >= PLANE_NUM)
    {
        m->phase = MIG_DONE;
        return false;
    }

    const ftl_status_e ret = mig_submit_erase(m);
    if (ret == FTL_AGAIN)
    {
        (void)mig_stall(m);
    }
    else if (ret != FTL_OK)
    {
        mig_abort(m, "erase command rejected"); /* [待上板] 永久错误不再空转重试 */
    }
    return true;
}

/* --------------------------------------------------------------------
 * mig_step
 *   功能：状态机主体——一次只提交**一条**命令；纯计算阶段连续推进到下一个
 *         提交阶段或终态。
 *   参数：ftl —— FTL 实例。
 *   返回：无。
 *   实现要点：提交失败（背压）⇒ 原地返回，由 mig_handle 下轮重试同一阶段（幂等）；
 *             连续超过 FTL_SUBMIT_TIMEOUT_MS 无进展 ⇒ 中止。永久错误立即中止。
 * -------------------------------------------------------------------- */
static void mig_step(mig_handle_t *m)
{
    for (;;)
    {
        if (m->waiting)
        {
            /* 完成丢失（例如 FC CQ 满时 CQ entry 被丢）时 waiting 永真，GC/WL 永久卡死。
             * 在飞也计时：超时后放弃本 job；若迟到的完成随后到达，回调的
             * (!active || !waiting) 分支会 release 命令。 */
            if (mig_deadline_expired(&m->cmd_wait_ms, FTL_WAIT_TIMEOUT_MS))
            {
                /* 写命令可能已部分编程：超时后按“已提交”处置，关闭 Cold SB。 */
                if (m->phase == MIG_WRITE)
                {
                    mig_dst_fail_cleanup(m, true);
                }
                m->timed_out     = 1U;
                m->timeout_phase = m->phase;
                m->waiting       = false;
                mig_abort(m, "completion timeout");
            }
            return;
        }

        bool stop;

        switch ((mig_phase_e)m->phase)
        {
            case MIG_READ:
                stop = mig_step_read(m);
                break;
            case MIG_READ_SETTLE:
                stop = mig_step_read_settle(m);
                break;
            case MIG_WRITE:
                stop = mig_step_write(m);
                break;
            case MIG_WRITE_SETTLE:
                stop = mig_step_write_settle(m);
                break;
            case MIG_FLUSH_JRN:
                stop = mig_step_flush_jrn(m);
                break;
            case MIG_ERASE:
                stop = mig_step_erase(m);
                break;
            case MIG_DONE:
                mig_finish(m);
                return;
            case MIG_IDLE:
            default:
                /* 不该发生：active 置位而 phase 非法 */
                ASSERT(!m->active);
                return;
        }

        if (stop)
        {
            return;
        }
    }
}

static void mig_service_step(mig_handle_t *m)
{
    /* 前置条件（判据 2）：调用者 ftl.c 传 &g_ftl[...]。 */

    if (!m->active)
    {
        m->log_ms   = 0U;
        m->start_ms = 0U;
        return;
    }

    /* 心跳（见 ftl_job.h）：搬移最长（64 页 × 2 plane），没有它卡死不可见。
     * ⚠ 只报「真的卡住」：单个 victim 正常 1 s 内完成，逐 victim 的 1 s 心跳在持续
     *   GC 时会变成 1 行/秒。job age ≥ SLOW_AFTER 才打；之后 helper 自动降到 5 s。 */
    if (ftl_job_heartbeat(&m->log_ms, &m->start_ms) &&
        (ftl_job_age_ms(&m->start_ms) >= FTL_JOB_LOG_SLOW_AFTER_MS))
    {
        LOGI("[MIG] job kind=%u phase=%u page=%u plane=%u aborted=%u age=%lums\r\n",
             (unsigned)m->kind,
             (unsigned)m->phase,
             (unsigned)m->page,
             (unsigned)m->plane,
             (unsigned)m->aborted,
             ftl_job_age_ms(&m->start_ms));
    }

    mig_step(m);
}

/* ── 7. typed handle（端口装配 / 触发 / 自持节拍 / 上报）── */

void mig_init(mig_handle_t *m,
              alc_t        *alloc,
              l2p_jrn_t    *journal,
              l2p_cache_t  *cache,
              const DWORD  *total_writes,
              ftl_topo_t    topo)
{
    ASSERT(alloc != NULL);
    ASSERT(journal != NULL);
    ASSERT(cache != NULL);
    ASSERT(total_writes != NULL);

    memset(m, 0, sizeof(*m));
    m->alloc        = alloc;
    m->journal      = journal;
    m->cache        = cache;
    m->total_writes = total_writes;
    m->topo         = topo;
    for (BYTE plane = 0U; plane < PLANE_NUM; plane++)
    {
        m->fail_gblock[plane] = UINT16_MAX;
    }
    m->error = (int32_t)FTL_OK;
}

void mig_trigger(mig_handle_t *m, mig_reason_e reason)
{
    switch (reason)
    {
        case MIGRATE_REASON_GC_URGENT:
            m->urgent_gc = 1U;
            break;
        default:
            /* 值域外：显式忽略（调用者传错原因不应 brick 设备） */
            break;
    }
}

/* GC 求值（自持节拍）：命中时返回 GC_TRIGGER_* 位掩码并打观测日志。 */
static DWORD mig_gc_evaluate(mig_handle_t *m, DWORD now_ms, bool *host_writing)
{
    /* 节拍门。0 作「尚未求值」哨兵：平台时钟缺失时 now_ms 恒 0 ⇒ 每轮求值
     * ——GC 不能因缺时钟而停摆（与定时触发的取舍方向相反）。 */
    if ((m->gc_eval_last_ms != 0U) && ((now_ms - m->gc_eval_last_ms) < GC_EVAL_INTERVAL_MS))
    {
        return 0U;
    }
    m->gc_eval_last_ms = now_ms;

    /* host 写流判定（2 s 窗口，见 gc.h GC_HOST_WRITE_WINDOW_MS）。
     * 时钟缺失（now_ms == 0）按「在写」处理 ⇒ 收益判据 OFF，绝不卡写。 */
    const DWORD writes = *m->total_writes;

    if (writes != m->gc_writes_last)
    {
        m->gc_writes_last   = writes;
        m->gc_write_last_ms = now_ms;
    }
    *host_writing = (now_ms == 0U) ||
                    ((m->gc_write_last_ms != 0U) &&
                     ((now_ms - m->gc_write_last_ms) < GC_HOST_WRITE_WINDOW_MS));
    m->host_writing = *host_writing;

    const DWORD triggers = gc_should_trigger(m->alloc);

    if (triggers == 0U)
    {
        m->gc_log_last_ms = 0U; /* 收敛：下次进入触发态立即打 */
        m->gc_no_gain     = false;
        return 0U;
    }

    /* NO_GAIN 退避：无新写入、非紧急时，避免每 10 ms 全表扫 closed list。 */
    if (!mig_gc_scan_allowed(m, mig_gc_mode(triggers, *host_writing), triggers, now_ms))
    {
        return 0U;
    }

    if ((m->gc_log_last_ms == 0U) || ((now_ms - m->gc_log_last_ms) >= GC_LOG_INTERVAL_MS))
    {
        m->gc_log_last_ms = now_ms;
        LOGD("[GC] trig=0x%02lx free=%u op=%u/%u writes=%lu reclaimed=%lu novictim=%lu nogain=%lu mode=%s hw=%u bestvr=%u\r\n",
             triggers,
             (unsigned)m->alloc->free_count,
             (unsigned)alc_get_op_count(m->alloc, 0U),
             (unsigned)alc_get_op_count(m->alloc, 1U),
             *m->total_writes,
             m->reclaimed_count,
             m->novictim_count,
             m->nogain_count,
             mig_gc_mode_name((gc_benefit_mode_e)m->gc_last_mode),
             (unsigned)(*host_writing ? 1U : 0U),
             (unsigned)m->gc_best_valid_pct);
    }

    return triggers;
}

static void mig_status_fill(const mig_handle_t *m, const bg_info_t *info, mig_status_t *out)
{
    out->base.critical = true; /* 写路径紧急 GC 在 BUSY 档起，不推进即死锁 */
    out->base.progress = m->reclaimed_count;
    out->base.next_ms  = 0U;
    out->base.error    = m->error;

    if (info != NULL)
    {
        out->base.wants_run = m->active || (info->load_level != BG_LEVEL_BUSY);
    }
    else
    {
        out->base.wants_run = m->active;
    }

    if (m->waiting)
    {
        out->base.state = FTL_MODULE_WAITING;
    }
    else if (m->active)
    {
        out->base.state = FTL_MODULE_RUNNING;
    }
    else if (m->error != (int32_t)FTL_OK)
    {
        out->base.state = FTL_MODULE_FAILED;
    }
    else
    {
        out->base.state = FTL_MODULE_IDLE;
    }

    out->phase         = m->phase;
    out->kind          = m->kind;
    out->migrated      = m->migrated;
    out->erased        = m->erased;
    out->aborted       = m->aborted;
    out->erase_failed  = m->erase_failed;
    out->timed_out     = (m->timed_out != 0U);
    out->timeout_phase = m->timeout_phase;
    out->reclaimed     = m->reclaimed_count;
    out->novictim      = m->novictim_count;
    out->nogain        = m->nogain_count;
    out->gc_mode       = m->gc_last_mode;
    out->gc_no_gain    = m->gc_no_gain;
}

void mig_handle(mig_handle_t *m, const bg_info_t *info, mig_status_t *out)
{
    /* ① 在飞：只服务当前 job（提交即返回）。 */
    if (m->active)
    {
        mig_service_step(m);
        mig_status_fill(m, info, out);
        return;
    }

    /* ② 系统性命中熔断：不再启动任何 GC/WL/predict（重启才清）。 */
    if (m->paused)
    {
        mig_status_fill(m, info, out);
        return;
    }

    /* ③ 不在飞：host 饱和档让路，不评估新工作。 */
    if (info->load_level == BG_LEVEL_BUSY)
    {
        mig_status_fill(m, info, out);
        return;
    }

    const DWORD now_ms = info->now_ms;

    /* ④ 紧急/显式触发优先；否则按自持节拍评估 GC → WL → predict。
     *    每轮最多启动一个 job（单飞行）。 */
    if (m->urgent_gc != 0U)
    {
        m->urgent_gc = 0U;

        /* 写路径分配失败 = 紧急：给一次立即扫描；free 仍高于紧急线时按 OFF（不筛），
         * 因为 urgent 的语义是「写不进去」，不是「空闲补池」。 */
        const gc_benefit_mode_e mode = (m->alloc->free_count <= FREE_POOL_EMERGENCY)
                                           ? GC_BENEFIT_EMERGENCY
                                           : GC_BENEFIT_OFF;
        (void)mig_begin_gc(m, mode, now_ms);
    }
    else
    {
        bool        host_writing = false;
        const DWORD trig         = mig_gc_evaluate(m, now_ms, &host_writing);

        if (trig != 0U)
        {
            (void)mig_begin_gc(m, mig_gc_mode(trig, host_writing), now_ms);
        }
        else if ((m->wl_eval_last_ms == 0U) ||
                 ((now_ms - m->wl_eval_last_ms) >= WL_EVAL_INTERVAL_MS))
        {
            m->wl_eval_last_ms = now_ms;

            /* WL 是**后台维护**，只在「上次求值以来 host 没有新编程 + 当前空闲档」时启动。
             * USB BOT 是串行的：pending 在两条命令之间会掉到 0，仅看 load_level 会误判 IDLE
             * ⇒ WL 在 host 持续写盘时启动，并占住单飞行引擎数秒（实测单 job 4~7s），
             * 同时把命令槽让不出去、GC 也起不来。用 total_writes 的窗口增量判断 host 是否在写。 */
            const DWORD writes     = *m->total_writes;
            const bool  host_quiet = (writes == m->wl_last_writes);

            m->wl_last_writes = writes;

            if (host_quiet && (info->load_level == BG_LEVEL_IDLE))
            {
                (void)mig_begin_wl(m);
            }
        }
        else if ((info->load_level == BG_LEVEL_IDLE) &&
                 ((m->predict_last_ms == 0U) || ((now_ms - m->predict_last_ms) >= RR_SCAN_INTERVAL_MS)))
        {
            m->predict_last_ms = now_ms;
            (void)mig_begin_predict(m);
        }
    }

    mig_status_fill(m, info, out);
}

/* ── 6. 完成服务（g_op_callbacks[FTL_OP_GC] / [FTL_OP_WL]）── */

/* MIG_READ 完成：拷数据 + 解 META 得逐 CW 的扇区 LBA；顺带记 ECC 趋势 + read_count
 * （慢死预测闭环）。失败 / 全页空洞 ⇒ 该 plane 无有效 CW，不影响另一 plane。 */
static void mig_complete_read(mig_handle_t              *m,
                              fc_command_t              *cmd,
                              const fc_command_status_t *completion)
{
    const BYTE plane = m->plane;

    m->decode_ok = false;

    /* 读失败与“空页”必须分开：前者页上可能仍有有效数据，擦 victim 会永久丢数据；
     * 后者（无错误 + META 全 FF）只是从未写过的 CW，跳过本页即可。
     * ⚠ 失败原因必须保留到日志：FC 状态错 / 短链读 / META 页基址损坏是三种不同的
     *   故障源，合并成一个 read_ok=false 会让 mark BAD 无法归因（RISK-MIG-06）。 */
    bool        read_ok = true;
    const char *cause   = "";

    m->last_fc_status = completion->status; /* 诊断：abort 日志带上原始状态位 */

    if (fc_cw_status_has_error(completion->status))
    {
        read_ok = false;
        cause   = "fc_err";
    }
    else if (ftl_cmd_read_data(cmd, m->page_buf[plane], PAGE_MAIN_SIZE) != FTL_OK)
    {
        read_ok = false;
        cause   = "short_chain";
    }

    if (read_ok)
    {
        const gc_decode_e dr = gc_decode_cw_lba(cmd, m->src[plane], m->cw_lba[plane], &m->base[plane]);

        if (dr == GC_DECODE_OK)
        {
            const WORD gsrc = ftl_gblock(plane, m->victim.plane_blocks[plane]);
            const BYTE ecc  = fc_get_cmd_ecc_max(cmd);

            /* B4：搬移读记录 ECC 趋势 + read disturb 计数（慢死预测闭环）。
             * 判定只用 ecc_bits 与上一次读数比较，不涉及时间维度。
             * 趋势/read_count 的写入点是 RR owner API，migrate 不直接摸 BST。 */
            rr_record_block_ecc(m->alloc, gsrc, ecc);

            m->decode_ok = true;
        }
        else if (dr == GC_DECODE_CORRUPT)
        {
            /* 页内 META 页基址不一致：不能只跳过部分 CW（它们的 L2P 仍指向本页，
             * 随后擦 victim 会悬空）。整页按读失败中止。 */
            read_ok = false;
            cause   = "decode_corrupt";
        }
        /* GC_DECODE_EMPTY：空页，decode_ok 保持 false，SETTLE 跳过本页。 */
    }

    /* 逐 CW 反查 L2P 改在 MIG_READ_SETTLE（纯计算阶段，可挂起等 region 加载） */
    (void)fc_command_release(cmd);
    m->waiting  = false;
    m->stall_ms = 0U;

    if (!read_ok)
    {
        /* [待上板] 读错误 ⇒ 中止，victim 不擦。空页（decode false）不在此列。
         * 同一源块连续失败达上限 ⇒ 标 BAD 并摘除其 Closed plane（RISK-GC-01）：
         * 否则每轮 GC 都重选同一 victim、永久 abort，容量不再增长。
         * 其他 plane 仍可迁移；数据不因标坏而丢失（坏块本来就读不回来）。
         * ⚠ 计数按 **plane 的源块**分开：同一 victim 的两个 plane 块号不同，共用
         *   一个槽位会让交替失败互相清零。 */
        const WORD gsrc = ftl_gblock(plane, m->victim.plane_blocks[plane]);

        if (m->fail_gblock[plane] != gsrc)
        {
            m->fail_gblock[plane] = gsrc;
            m->fail_count[plane]  = 0U;
        }
        m->fail_count[plane]++;

        if (m->fail_count[plane] >= MIG_VICTIM_READ_FAIL_MAX)
        {
            /* gblk 是全局块号（plane*BLOCKS_PER_PLANE+block），与 mig_abort 的
             * 「plane%u block=%u」不是同一坐标，故字段名写 gblk（RISK-MIG-06）。 */
            LOGW("[MIG] victim read failed gblk=%u x%u cause=%s — mark BAD\r\n", (unsigned)gsrc, (unsigned)m->fail_count[plane], cause);
            (void)bbt_mark_bad(m->alloc, gsrc);
            if (alc_closed_drop_plane(m->alloc, plane, m->victim.plane_blocks[plane]))
            {
                m->closed_removed = true;
            }
            m->fail_count[plane]  = 0U;
            m->fail_gblock[plane] = UINT16_MAX;

            /* 系统性读失败熔断：连续多个不同 victim 都被标 BAD ⇒ 不是单块坏。
             * 继续 GC 会耗尽 Free/OP 并毁掉 BAD 图；停止后台搬移直到重启。 */
            if (m->read_fail_streak < UINT8_MAX)
            {
                m->read_fail_streak++;
            }
            if (!m->paused && (m->read_fail_streak >= MIG_SYSTEMIC_READ_FAIL_MAX))
            {
                m->paused = true;
                LOGE("[MIG] systemic victim read failures (%u consecutive) — suspend GC until reboot\r\n",
                     (unsigned)m->read_fail_streak);
            }
        }

        mig_abort(m, cause); /* 三种原因直传日志，见 RISK-MIG-06 */
    }
    else
    {
        /* 单页读成功**不**清零失败计数（RISK-GC-07）：坏页之前的页每轮都会成功读一遍，
         * 若按单页成功清零，第 2 次重试永远从 0 开始 ⇒ MIG_VICTIM_READ_FAIL_MAX 不可达。
         * 计数只在整个 job 完成（mig_finish）或该块标 BAD 时清。 */
        m->phase = MIG_READ_SETTLE;
    }

    mig_step(m);
}

/* MIG_WRITE 完成：只记成败，L2P/Journal/有效计数的改动在 MIG_WRITE_SETTLE。
 * 写失败 ⇒ **中止 job，victim 不擦**（源数据仍有效，下轮再搬）；绝不能继续到 ERASE。 */
static void mig_complete_write(mig_handle_t              *m,
                               fc_command_t              *cmd,
                               const fc_command_status_t *completion)
{
    const bool write_ok = !fc_cw_status_has_error(completion->status);

    (void)fc_command_release(cmd);
    m->waiting  = false;
    m->stall_ms = 0U;

    if (!write_ok)
    {
        /* [待上板] 写错误 ⇒ 中止，victim 不擦。
         * 命令已提交，目的页可能已半编程 ⇒ 不能 rewind；立即关闭 Cold SB
         * （RISK-GC-02）。关闭后失败页留在 SB 尾部，下一次 GC 换新的目的块；
         * 目的块可能已有成功迁移的页，**不能**标 BAD，旧块的已写页仍可读。 */
        mig_dst_fail_cleanup(m, true);
        mig_abort(m, "write failed");
        mig_step(m);
        return;
    }

    m->write_ok = true;
    m->phase    = MIG_WRITE_SETTLE;
    mig_step(m);
}

/* MIG_ERASE 完成：成功 ⇒ erase_count++ / last_pe_ts / 清 valid，
 * 然后按 post 二选一（FREE 入池 / RETIRE 退役）；失败 ⇒ bbt_mark_bad。 */
static void mig_complete_erase(mig_handle_t              *m,
                               fc_command_t              *cmd,
                               const fc_command_status_t *completion)
{
    const BYTE  plane        = m->plane;
    const WORD  gsrc         = ftl_gblock(plane, m->victim.plane_blocks[plane]);
    const DWORD total_writes = *m->total_writes;

    if (!fc_cw_status_has_error(completion->status))
    {
        m->erased++;
        if (ftl_block_is_bad(&m->alloc->blocks[gsrc]))
        {
            /* 迁移期间被 RR/BBT 标 BAD：擦成功也不能复活或入池。 */
            m->erase_failed = true;
            LOGW("[MIG] erase ok but block %u became BAD — not freed/retired\r\n", (unsigned)gsrc);
        }
        else
        {
            /* ⚠ 不在这里清 `read_fail_streak`：擦除是**逐 plane** 的，一个 victim
             * 常常一侧 plane 健康（读完→擦成功）另一侧读失败；在此清零会让熔断
             * 计数每个 job 都归零，阈值永不触发（RISK-GC-10）。清零只在
             * mig_finish 的「整块迁移成功」分支。 */

            /* 擦除成功：统一走 allocator 的擦除记账（含 !bad 断言 + 清 valid）。 */
            alc_note_erase(m->alloc, gsrc, total_writes);

            /* 去向二选一——**退役块不得先进池**：
             * 退役直接 RETIRED，与「state == FREE ⟺ 在池」不变量一致（allocator.h ②）。 */
            if ((mig_post_e)m->post == MIG_POST_RETIRE)
            {
                /* 慢死预测：搬空的块**退役**（RETIRED，不再分配新数据、不标坏、仍可读）。
                 * GC / 静态 WL 走 FREE 分支，块重新进入磨损轮转。
                 * victim 正常是 CLOSED、从未入池 ⇒ remove_from_free 是文档化 no-op
                 * （allocator.h 判据 4）；保留它是为了让「退役块绝不留在池里」不依赖调用顺序。
                 * rr_retire_block 内部把 read_count 归零（read disturb 重计）。 */
                alc_remove_from_free(m->alloc, gsrc);
                rr_retire_block_at(m->alloc, gsrc);
            }
            else
            {
                alc_free_block(m->alloc, gsrc);
            }
        }
    }
    else
    {
        m->erase_failed = true;

        /* 擦除失败：标记坏块（BBT 单数据源，后台持久化）。
         * ⚠ 已 BAD 时不再重复标记：bbt_mark_bad 会再次 bad_count++ 并从 Spare 补池。
         * ⚠ 必须逐块打一行：否则运行期 BAD 只留下游的 `[FTL] dangling`，无法归因到
         *   「GC 源块擦失败」还是「host 写/读失败」。 */
        if (!ftl_block_is_bad(&m->alloc->blocks[gsrc]))
        {
            LOGW("[MIG] erase failed gblk=%u — mark BAD\r\n", (unsigned)gsrc);
            (void)bbt_mark_bad(m->alloc, gsrc);
        }
        else
        {
            LOGW("[MIG] erase failed on already-BAD block %u — not re-marked\r\n", (unsigned)gsrc);
        }
    }

    /* 逐 plane 摘除已处理的块：擦完后该块已归还 Free Pool（或已标坏），旧 Closed 条目
     * 不得继续引用它——否则块被重新分配并再次 Closed 后会被旧条目误认（RISK-GC-04）。
     * 条目在最后一个 plane 摘除时自动移除。 */
    if (alc_closed_drop_plane(m->alloc, plane, m->victim.plane_blocks[plane]))
    {
        m->closed_removed = true;
    }

    (void)fc_command_release(cmd);
    m->waiting  = false;
    m->stall_ms = 0U;
    m->plane    = (plane + 1U);
    mig_step(m);
}

/* --------------------------------------------------------------------
 * ftl_migrate_callback
 *   功能：搬移命令的完成服务函数——按 job.phase 判完成结果、改 phase、release 命令，
 *         再交回 mig_step 续跑。
 *   参数：ftl —— 实例；completion —— 完成状态；ctx —— `mig_handle_t *`（= m）。
 *   返回：无。命令的 release 归本函数（见 ftl_op.h 的所有权表）。
 * -------------------------------------------------------------------- */
void ftl_migrate_callback(struct ftl_context *ftl, const fc_command_status_t *completion, void *ctx)
{
    /* 形参由完成派发签名固定；release 下 ASSERT 裁空 ⇒ 显式丢弃（H30）。 */
    (void)ftl;
    mig_handle_t *m = (mig_handle_t *)ctx;

    fc_command_t *cmd = fc_command_id_to_command(completion->command_id);

    /* 旁表项缺失（ctx == NULL）= 重复/迟到的完成：只 release 命令，不解引用 m。
     * 对齐 read_recovery.c 的同族守卫；release 下 ASSERT 裁空，必须先判空。 */
    if (m == NULL)
    {
        LOGE("[MIG] late completion cmd=%u status=0x%02x — released\r\n", (unsigned)completion->command_id, (unsigned)completion->status);
        (void)fc_command_release(cmd);
        return;
    }

    /* 实例由 ctx 反推（job 就长在实例里）；ftl 形参是 dispatch 解析出来的归属实例，
     * 二者必须一致——不一致说明派发路由错了。 */
    ASSERT(ftl->mig_handle == m);

    /* 孤儿 / 重复完成：job 已收尾或当前没有在飞命令 ⇒ 只释放命令，不改状态。
     * 否则迟到的完成会按新 job 的 phase 被误解释（同 read_recovery.c 的守卫）。 */
    if ((!m->active) || (!m->waiting))
    {
        LOGW("[MIG] orphan completion phase=%u active=%u waiting=%u — ignored\r\n",
             (unsigned)m->phase,
             (unsigned)m->active,
             (unsigned)m->waiting);
        (void)fc_command_release(cmd);
        return;
    }

    switch ((mig_phase_e)m->phase)
    {
        case MIG_READ:
            mig_complete_read(m, cmd, completion);
            return;

        case MIG_WRITE:
            mig_complete_write(m, cmd, completion);
            return;

        case MIG_ERASE:
            mig_complete_erase(m, cmd, completion);
            return;

        case MIG_IDLE:
        case MIG_READ_SETTLE:
        case MIG_WRITE_SETTLE:
        case MIG_FLUSH_JRN:
        case MIG_DONE:
        default:
            /* 不该发生：一条命令对应一个提交阶段（READ / WRITE / ERASE）。 */
            LOGE("[MIG] unexpected completion phase=%u\r\n", (unsigned)m->phase);
            (void)fc_command_release(cmd);
            m->waiting = false;
            mig_abort(m, "unexpected completion");
            mig_step(m);
            return;
    }
}
