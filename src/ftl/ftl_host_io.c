/*
 * ftl_host_io.c — host I/O 提交 / group drain / 子操作下发 / 完成入口
 *
 * 职责：
 *   · host 读 / 写请求的入口校验、建组与快路径下发（read / write 共用一份提交骨架）；
 *   · 一个完成组 → 多个 FC 子操作的拆分与连续下发（pair 窗口夹取 / L2P 分组收缩）；
 *   · FC 完成入口：按 {读,写}×{成功,失败} 结算子操作并推进组聚合；
 *   · 顺序流检测与 L2P 预取（host 读路径）。
 *
 * 关键机制：
 *   · 进度单位是 LBA 偏移（g->next / g->total），子操作数由 drain 现场决定；
 *   · 三态返回：FTL_OK / FTL_AGAIN（瞬态背压，下轮补发）/ FTL_ERROR（映射损坏等永久失败）；
 *   · drain 的 stall 判据只认「有确定完成时刻的进展」，预算耗尽才判真卡死；
 *   · 顺序流检测器决定 cache 读/写序列能否开启；预取用 generation 防重入失效。
 *
 * 与其他模块：
 *   · 上游：HIL request / controller 调 ftl_submit_read|write；ftl_sched 调 ftl_host_group_drain；
 *   · 下游：pending（组/子操作账本）、fc_interface（命令池 / 建链）、wbuf / cache_batch
 *     （写打包 / cache 序列）、l2p_cache（映射与预取）；
 *   · 完成路径：FC → ftl_op_drain → ftl_host_io_op_callback；成败分支实现在 ftl_confirm.c。
 */

#include "assertion.h"
#include "bbt.h"
#include "cache_batch.h"
#include "fc_interface.h"
#include "ftl.h"
#include "ftl_confirm.h"
#include "ftl_fc_interface.h"
#include "ftl_op.h"
#include "l2p_cache.h"
#include "log.h"
#include "migrate.h"
#include "pending.h"
#include "ssd_compiler.h"
#include "timer.h"
#include "wbuf.h"

/* 写失败透明重试次数上限（H4：只重试一次覆盖「遇到一个新坏块」；第二次仍失败
 * 说明介质 / 通道有问题，如实返回 IO_ERROR，不掩盖故障）。 */
#define FTL_WRITE_RETRY_MAX (1U)

/* 文件级变量 */
static log_ratelimit_s s_cache_rd_err_log; /* cache 读命令失败（1 s） */

static log_ratelimit_s s_lost_log; /* 降级 region 读错误（5 s） */

static log_ratelimit_s s_nospace_log; /* 无空间归因（5 s） */

static log_ratelimit_s s_subop_fail_log; /* subop 永久失败（5 s） */

/* ── stream ── */

/**
 * @brief 复位一个方向的顺序流检测器。
 *
 * @param det 检测器（cache_stream.read / cache_stream.write）
 */
static void host_stream_reset(ftl_stream_det_s *det)
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
static void host_stream_note(ftl_stream_det_s *det, DWORD lba, DWORD count)
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

/* ── seq ── */

/**
 * @brief 复位顺序读检测状态，并使已算出的预取计划失效。
 *
 * @param ftl 实例（cache_stream 的预取字段由本函数更新）
 *
 * @note 写 / flush / trim 入口显式调用：它们会打断顺序读流。
 */
void ftl_seq_reset(ftl_ctx_s *ftl)
{
    ftl->cache_stream.prefetch_last_end = FTL_LBA_INVALID; /* 与 stream 检测器同一空哨兵 */
    ftl->cache_stream.prefetch_run      = 0U;
    ftl->cache_stream.prefetch_valid    = false;
    ftl->cache_stream.prefetch_target   = 0U;
    ftl->cache_stream.prefetch_generation++;
}

/**
 * @brief 只更新检测状态并算出预取目标，不在这里起加载（host 读应先拿资源）。
 *
 * @param ftl 实例（检测字段与 seq_prefetch_target 由本函数更新）
 * @param req 刚受理的读请求（只读 lba / count）
 */
static void host_seq_note_read(ftl_ctx_s *ftl, const ftl_io_req_s *req)
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
static void host_seq_prefetch_pending(ftl_ctx_s *ftl, DWORD generation)
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

/* ── host_group ── */

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
static void SSD_CODE_FAST __attribute__((noinline)) host_group_stall_dump(const ftl_ctx_s *ftl, WORD gi)
{
    const ftl_cache_seq_s *seq = &ftl->cache_stream.seq;
    const jrn_handle_s    *jrn = &ftl->journal->handle;

    LOGE(LOG_TAG_FTL "stall: seq kind=%d act=%d drain=%d close=%d brk=%d wr=%hu next=%lu\r\n", (int)seq->kind, (int)seq->active, (int)seq->draining, (int)seq->close_requested, (int)seq->break_requested, seq->write.count, seq->next_lba);

    LOGE(LOG_TAG_FTL "stall: jrn phase=%d erase=%d full=%d kind=%d ent=%hu seq=%lu jf=%lu\r\n", (int)jrn->job.phase, (int)jrn->job.need_erase, (int)jrn->state.ring_full, (int)jrn->state.page_kind, jrn->state.current_entry, jrn->state.sequence_counter, jrn_page_writes(ftl->journal));

    {
        fc_resource_stats_s rs;
        WORD                fpmin = 0U;

        memset(&rs, 0, sizeof(rs));
        (void)fc_resource_stats_get(&rs);

        // per-plane 最小空闲块数（成对分配看的是最饿的那个 plane）。
        for (BYTE p = 0U; p < PLANE_NUM; p++)
        {
            const WORD c = alc_free_get_count(&ftl->alloc, p);

            if ((p == 0U) || (c < fpmin))
            {
                fpmin = c;
            }
        }

        LOGE(LOG_TAG_FTL "stall: gi=%hu next=%lu/%lu wb_busy=%d pend=%hu free=%hu fpmin=%hu sb_open=%d np=%hhu p0=%02hhx fc cmd=%hu/%hu desc=%hu/%hu buf=%hu/%hu sq=%hu/%hu cq=%hu/%hu\r\n", gi, ftl->pending.groups[gi].next, ftl->pending.groups[gi].total, (int)(wb_is_idle(ftl->wbuf) == false), pend_slot_count(&ftl->pending), ftl->alloc.free_count, fpmin, (int)ftl->alloc.hot_sb.is_open, ftl->alloc.hot_sb.next_page, ftl->alloc.hot_sb.page0_programmed, rs.command_used, (WORD)FC_COMMAND_POOL_DEPTH, rs.descriptor_used, (WORD)FC_DESCRIPTOR_POOL_DEPTH, rs.buffer_used, (WORD)FC_BUFFER_POOL_DEPTH, rs.sq_used, (WORD)FC_SQ_DEPTH, rs.cq_used, (WORD)FC_CQ_DEPTH);
    }
}

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
 * @note ⚠ 三态分类是全局枢纽（见文件头的三态说明）。
 * · 读：逐 LBA 查 L2P，按 (物理 plane, 页) 分组——未映射、或同 plane 换了页
 * 即收缩；首个 LBA 就未映射则同步零填 1 个 LBA 并消费它。
 * · 写：按窗口内 LBA 段算 flat 位图（每个 LBA 落自己 plane 位的页 + 自己的
 * CW），一次 alc_super_open_alloc_pair 拿两个 plane 的页。
 */
static ftl_status_e host_group_subop_issue(ftl_ctx_s *ftl, WORD gi, DWORD internal_lba, void *buffer, BYTE *n_cw)
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
        // 回填窗口夹住后的实际消费数——调用者（ftl_host_group_drain）据此推进 g->next。
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

            if (wb_has_lba(ftl->wbuf, l))
            {
                break; /* 还在写缓冲（未编程）：收缩到此为止，下一轮同步从缓冲取 */
            }

            // cache 写序列里**已编程、尚未确认**的页：L2P 仍是旧映射，必须读刚编程的
            // 页。命中即用它的 PBA，跳过 L2P。
            if (ftl_cache_write_lookup(ftl, l, &p) == false)
            {
                // 已判失败、撤销还没走完的页：L2P 可能仍指着编程失败的 PBA
                // ⇒ 必须报错，不能把它的内容当数据返回。
                if (ftl_cache_write_lba_is_failed(ftl, l) == true)
                {
                    LOGE_RL(&s_lost_log, LOG_TAG_FTL "rd lba=%lu: cache page failed (FAIL_CACHE) — IO_ERROR", l);
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
                LOGE_RL(&s_lost_log, LOG_TAG_FTL "rd lba=%lu: mapping lost (degraded L2P region) — IO_ERROR", internal_lba);
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
    // cw_bitmap 非空由 host_group_subop_issue 的拆分子操作保证（至少 1 个 CW）。
    BYTE nplanes = ftl_cw_bitmap_plane_count(cw_bitmap);

    // 资源次序：**先查 pending 表余量，再配 desc 链**。末尾要用一个 pending 槽
    // 登记本子操作；表满却先建好链，只能整条回滚——白配一场。
    // ⚠ 必须在 wbuf 命中分支**之后**：那条路径同步完成、不占 pending 槽，不该被表满挡住。
    if (pend_slot_count(&ftl->pending) >= FTL_PENDING_MAX)
    {
        return FTL_AGAIN;
    }

    // host 读也必须走预算：否则读突发会把给后台留的命令/buffer 吃光，
    // CP/收尾命令申请不到槽 ⇒ confirm park timeout。
    if (ftl_fc_host_budget_ok() == false)
    {
        return FTL_AGAIN;
    }

    // 分配 FC 命令槽（池空 = 瞬态）；host 读走后台额度，保留 L2P demand load 的 CRIT 槽。
    fc_cmd_s *cmd = ftl_cmd_alloc_class(FC_CMD_CLASS_BG);
    if (cmd == NULL)
    {
        return FTL_AGAIN;
    }
    const fc_cmd_id_t cmd_id = cmd->cmd_id;

    // 配置命令头（multi_plane 决定 plane 段，必须早于 ftl_cmd_build_ppa_pair_cw——
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
    ftl_status_e ret = ftl_cmd_build_ppa_pair_cw(cmd, pba, cw_bitmap);
    if (ret != FTL_OK)
    {
        fc_command_release(cmd_id);
        return ret; /* FTL_AGAIN（池空，瞬态）/ FTL_ERROR 原样上抛 */
    }

    // 注册 pending 子操作（必须早于 submit，避免竞态）。读：owners = 单组位、写专用字段传
    // INVALID、cw_bitmap 传 0；lba 传**全局** LBA（L2P/Journal/META 均按全局索引）。
    pend_req_s *pending = pend_alloc(&ftl->pending, cmd_id);

    if (pending == NULL)
    {
        fc_command_release(cmd_id);
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
        fc_command_release(cmd_id);
        pend_group_drop_subop(&ftl->pending, gi);
        return FTL_AGAIN; /* SQ 满：瞬态 */
    }

    return FTL_OK;
}

/**
 * @brief 连续下发一个组的待下发子操作，直到全部下发完 / 遇瞬态背压（FTL_AGAIN）/遇永久失败（FTL_ERROR）。
 *
 * @param ftl 实例
 * @param gi 组下标。
 *
 * @note **进度单位是 LBA 偏移**（g->next / g->total 都以 LBA 计），不是子操作序号：
 * 一个子操作消费 1..FTL_LBA_PER_PAIR（8）个 LBA，取决于 FTL 现场判断
 * （读路径按 L2P 分组收缩、写路径按 pair 窗口夹住，见 host_group_subop_issue）——
 * "怎么拆"完全由 FTL 决定，调用者不必预先算出子操作数。每次迭代**重取** g
 * （下发可能改变组状态甚至释放本组）；全部子操作都在本函数外异步完成，
 * 故循环末尾统一试触发组完成通知——覆盖"整段未映射读"这种从不进 outstanding、
 * 无人触发完成的组。
 */
void ftl_host_group_drain(ftl_ctx_s *ftl, WORD gi)
{
    ASSERT(ftl != NULL);
    ASSERT(gi < FTL_GROUP_MAX);

    pend_group_s *g = &ftl->pending.groups[gi];

    while ((g->used == true) && (g->next < g->total))
    {
        const DWORD off    = g->next; /* 已下发到的 LBA 偏移（g->next 以 LBA 计，非子操作序号） */
        const DWORD remain = g->total - off;
        DWORD       glba   = (g->lba + off);
        void       *buffer = (BYTE *)g->completion.buffer + (off * FTL_IU_BYTES);
        // 本窗口内可提供的最大 LBA 数（≤ 一个 pair 窗口）。实际消费几个由
        // host_group_subop_issue 回填——读路径按 L2P 分组收缩，写路径按窗口边界收缩。
        BYTE n_cw          = (remain >= FTL_LBA_PER_PAIR) ? FTL_LBA_PER_PAIR : remain;

        ftl->flags.l2p_defer = false; /* 每次下发前清零：本轮的背压原因由本次下发重新判定 */

        ftl_status_e st         = FTL_OK;
        BOOL         use_normal = true;

        // 连续 pair 的 two-plane cache read：是否进入序列由 FTL 顺序流检测器判
        // （cache_stream.read），序列所有者必须每轮尝试续窗口（窗口消费量不一定落在固定
        // 节拍上）。随机读只在检测器判为顺序流后才尝试，避免每次先付一轮 L2P 查找。
        const BOOL seq_owner = (ftl->cache_stream.seq.active == true) && (ftl->cache_stream.seq.owner_gi == gi);

        if (pend_op_is_read(g->op) && (seq_owner || (ftl->cache_stream.read.active == true)))
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
            st = host_group_subop_issue(ftl, gi, glba, buffer, &n_cw);
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
            const BOOL other_seq          = (ftl->cache_stream.seq.active == true) && (ftl->cache_stream.seq.owner_gi != gi);
            // 写序列所有者被确认 FIFO / 资源暂时挡回时，队列里的页命令一定有完成
            // （NAND 每命令有超时），属**有确定完成时刻的进展**，不计 stall。
            // 注意与 cache_stream.seq.write_inflight（在飞成员计数）区分：本项只看「本组的页还在页表里」。
            const BOOL write_ring_pending = (ftl->cache_stream.seq.kind == FTL_CACHE_SEQ_WRITE) && (ftl->cache_stream.seq.owner_gi == gi) &&
                                            ((ftl->cache_stream.seq.write.count > 0U) || (ftl->cache_stream.seq.write_inflight > 0U));
            // 读序列所有者的在飞子操作同理：NAND 每命令有超时 ⇒ 必有完成。
            // 没有它，读窗口一次占满 FC 池时后续窗口的预算等待会被误判成卡死。
            const BOOL rd_inflight    = (ftl->cache_stream.seq.kind == FTL_CACHE_SEQ_READ) && (ftl->cache_stream.seq.owner_gi == gi) && (g->outstanding > 0U);
            // 序列正在有序收尾（padding END 已提交或在途）：这是确定性进展，等它完成。
            const BOOL seq_closing    = (ftl->cache_stream.seq.closing == true) || (ftl->cache_stream.seq.close_requested == true) || (ftl->cache_stream.seq.break_requested == true) || (ftl->cache_stream.seq.draining == true);
            // host 正在给后台让行：后台提交成功或 20ms 超时后必然恢复，属确定性等待。
            const BOOL seq_yield      = (ftl->cache_stream.seq.yield_pending == true);
            // Journal 页写 / 换块擦在飞：NAND 命令有超时 ⇒ 必有完成，属确定性等待。
            // 少这一条，序列边界触发的页刷若跨过 8 轮预算，正常的写会被误判成卡死。
            const BOOL jrn_inflight   = (jrn_is_idle(ftl->journal) == false) || (jrn_has_erase_pending(ftl->journal) != false);
            // 待入账的写确认：要么 region 就绪后结算，要么 100 ms 后超时释放（pending.c）
            // ⇒ 确定性等待。待入账本身不占 drain 的进展，不能算卡死。
            const BOOL confirm_parked = pend_find_op(&ftl->pending, PENDING_OP_WRITE_CONFIRM, NULL);
            // 搬移引擎：job 在飞，或 job 已 abort 但命令仍在 FC 侧（新 job 被 inflight 门控
            // 拒绝）——后者同样是确定性等待（NAND 保证每条命令有完成），不能计成卡死。
            const BOOL mig_inflight   = (mig_is_busy(ftl->mig_handle) != false);
            /* wbuf 有打开窗口/页在飞也是**确定性进展**：它必然由 NAND 编程超时或
             * 后续 flush 收尾。speed 的 drain 循环远快于 600 µs tPROG，漏掉这一项
             * 会在 8 轮内把「wbuf 正在编程」误判成 IO_ERROR（板端 stall 现场
             * `wb_busy=1 cmd=2 desc=8 buf=8`）。 */
            const BOOL wb_busy        = (wb_is_idle(ftl->wbuf) == false);
            const BOOL bg_progress    = (ftl->flags.l2p_defer == true) || mig_inflight || other_seq || write_ring_pending || rd_inflight || seq_closing || seq_yield || jrn_inflight || confirm_parked || wb_busy;

            if (bg_progress == false)
            {
                if (++g->stalls > FTL_DRAIN_STALL_MAX)
                {
                    // 归因：GC 已判定「无收益 victim」时，这不是普通超时，是真正的无空间
                    // （free 见底且搬不动）。打印区分行，避免与瞬态背压混淆。
                    // 无空间是终止态、host 会持续重试 ⇒ 用限流器，避免逐请求刷屏。
                    // 两行共享一次限流判据：不能用 LOGx_RATE 调两次（第二次会被判 suppressed）。
                    const DWORD suppressed = s_nospace_log.suppressed;

                    if (log_ratelimit_due(&s_nospace_log) != false)
                    {
                        if (mig_gc_no_gain(ftl->mig_handle) != false)
                        {
                            LOGE(LOG_TAG_FTL "no space: GC no-gain (free=%hu) — IO_ERROR suppressed=%lu\r\n", ftl->alloc.free_count, suppressed);
                        }

                        LOGE(LOG_TAG_FTL "drain stalled gi=%hhu next=%lu/%lu suppressed=%lu\r\n", gi, g->next, g->total, suppressed);
                        host_group_stall_dump(ftl, gi);
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
        LOGE_RL(&s_subop_fail_log, LOG_TAG_FTL "subop failed gi=%hhu next=%lu/%lu", gi, g->next, g->total);
        g->agg_status = SSD_STATUS_IO_ERROR;
        g->next       = g->total;
        ftl_cache_seq_close(ftl, gi);
        break;
    }

    pend_group_maybe_finish(&ftl->pending, gi);
}

/* ── take ── */

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
    if (has != false)
    {
        STATS_INC(ftl->stats.deferred_taken);
    }
    return has;
}

/* ── submit ── */

/**
 * @brief 读/写共用的提交骨架——入参校验 + 建组 + 快路径下发。
 *
 * @param ftl 实例
 * @param req 请求（count = 连续 LBA 数，buffer 非空）
 * @param op PENDING_OP_READ / PENDING_OP_WRITE（决定组类型与日志标签）。
 *
 * @return FTL_OK    已建组（可能仍有余量待 host 补发步补发）
 *         FTL_AGAIN host 写被 admission 暂缓 / 组槽不足
 *         FTL_ERROR 参数非法 / 越界 / 延迟写错误
 *
 * @note 两个公开入口的流程**逐字相同**（差异只有 op 与日志文案）⇒ 收敛到本函数
 * 一份，避免两处各自漂移（H6：注释与代码不一致即缺陷，重复实现更甚）。
 */
static ftl_status_e host_submit_common(ftl_ctx_s *ftl, const ftl_io_req_s *req, pend_op_e op)
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

    // host 写 admission：CMT dirty / free 水位高时不启动新写。USB 前端已在
    // WAIT_GATE 侧暂缓；这里覆盖 CLI / 其他直接走 FTL 的 host 写入口。
    if ((op == PENDING_OP_WRITE) && (ftl_sched_host_write_is_admitted(ftl) == false))
    {
        return FTL_AGAIN;
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
        host_stream_note(&ftl->cache_stream.read, (DWORD)req->lba, count);
        host_stream_reset(&ftl->cache_stream.write);
    }
    else
    {
        host_stream_note(&ftl->cache_stream.write, (DWORD)req->lba, count);
        host_stream_reset(&ftl->cache_stream.read);
    }

    // 建完成组（一条 host 请求 = 一个组）。通知字段必须**早于任何下发**填入：
    // 下发可能当场完成整组，届时通知就要读它们。
    // 先回收已完成组的 scheduler item：group 槽会被复用，不能让旧 item 留在新组上。
    ftl_sched_host_reap(ftl);

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

    if (ftl_sched_host_begin(&ftl->sched, gi, (op == PENDING_OP_READ)) == false)
    {
        pend_group_release(&ftl->pending, gi);
        return FTL_AGAIN; /* scheduler host 上限 / item 池满：瞬态 */
    }

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
        host_seq_note_read(ftl, req);
        seq_gen = ftl->cache_stream.prefetch_generation;
    }

    // 快路径：只推进**本组**，不扫其它 host 组。批量派发留给 ftl_service；
    // 否则一次 submit 会把所有组的 stall 计数推进一遍，无时间流逝却可能判卡死。
    ftl_sched_host_kick(ftl, gi);

    if (op == PENDING_OP_READ)
    {
        const pend_group_s *g_done = &ftl->pending.groups[gi];

        // 本组还有未下发的子操作（背压）⇒ 不给预取抢资源；等本组下发完再预取。
        if (g_done->next >= g_done->total)
        {
            host_seq_prefetch_pending(ftl, seq_gen);
        }
    }

    return FTL_OK;
}

/**
 * @brief host 读提交
 *
 * 与写共用 host_submit_common，只差 pending 的 op 类型。
 *
 * @param ftl 实例
 * @param req 请求
 * @return FTL_OK；FTL_AGAIN 背压；FTL_ERROR 参数/状态错误
 */
ftl_status_e ftl_submit_read(ftl_ctx_s *ftl, const ftl_io_req_s *req)
{
    return host_submit_common(ftl, req, PENDING_OP_READ);
}

/**
 * @brief host 写提交
 *
 * 与读共用 host_submit_common，只差 pending 的 op 类型。
 *
 * @param ftl 实例
 * @param req 请求
 * @return FTL_OK；FTL_AGAIN 背压；FTL_ERROR 参数/状态错误
 */
ftl_status_e ftl_submit_write(ftl_ctx_s *ftl, const ftl_io_req_s *req)
{
    return host_submit_common(ftl, req, PENDING_OP_WRITE);
}

/* ── host_io ── */

/**
 * @brief host IO 命令完成入口
 *
 * 按 {读,写}×{成功,失败} 分派到四个分支；组结算见各分支。
 *
 * @param ftl        实例
 * @param completion FC 完成状态
 * @param ctx        恒 NULL（本函数自己 pend_find）
 */
void ftl_host_io_op_callback(ftl_ctx_s *ftl, const fc_cmd_status_s *completion, void *ctx)
{
    (void)ctx; /* host I/O 不登记request 表 */

    // cmd_id 由 NAND 从本命令回填；公共壳 fc_command_lookup 已断言
    // ID 在池内，恒返回非空指针。
    WORD      cmd_id = completion->cmd_id;
    fc_cmd_s *cmd    = fc_command_lookup(cmd_id);

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
            ftl_cache_write_member_finish(ftl, pending, completion);
            fc_command_release(cmd->cmd_id);
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
#if SSD_STATS_PERSIST_ENABLE
                if (sub_status == SSD_STATUS_OK)
                {
                    ftl_op_lifetime_note_host_io(ftl, pending, false);
                }
#endif
            }
            else
            {
                // 普通写 / page0 写：完成后确认映射（cache program 成员已在上面分流）。
                if (ftl_host_io_write_ok(ftl, pending))
                {
#if SSD_STATS_PERSIST_ENABLE
                    ftl_op_lifetime_note_host_io(ftl, pending, true);
#endif
                    fc_command_release(cmd->cmd_id);
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
                LOGE_RL(&s_cache_rd_err_log, LOG_TAG_FTL "cache read cmd failed lba=%lu cmd=%hu st=0x%02lX pba=%08lX", pending->lba, cmd_id, (DWORD)completion->status, ftl_cmd_pba(cmd));
                sub_status = SSD_STATUS_IO_ERROR;
            }
            else
            {
                ftl_host_io_read_fail(ftl, pending);
                fc_command_release(cmd->cmd_id);
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
                fc_command_release(cmd->cmd_id);
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
        pend_group_subop_finish_multi(&ftl->pending, owners, sub_status);
    }
    fc_command_release(cmd->cmd_id);
}

/**
 * @brief 初始化 host I/O 的日志限流对象（NOLOAD，显式复位）。
 */
void ftl_host_io_init(void)
{
    log_ratelimit_init(&s_cache_rd_err_log, LOG_RATE_INTERVAL_MS);
    log_ratelimit_init(&s_lost_log, LOG_RATE_SLOW_INTERVAL_MS);
    log_ratelimit_init(&s_nospace_log, LOG_RATE_SLOW_INTERVAL_MS);
    log_ratelimit_init(&s_subop_fail_log, LOG_RATE_SLOW_INTERVAL_MS);
}
