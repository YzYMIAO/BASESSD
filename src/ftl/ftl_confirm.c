/*
 * ftl_confirm.c — 写确认 / 待入账 / 透明重试 / dangling 守卫 / host I/O 完成分支
 *
 * 职责：
 *   · 写确认事务：Journal → L2P → BST 的落账顺序，两遍式（就绪前置 + 动账）保证不重复记账；
 *   · 待入账（confirm park）：region 未就绪时命令脱钩、组不结算，由 service 按完成源预算推进；
 *   · host I/O 完成的四个分支：读成功拷出 + ECC 记账 / 写成功确认 / 读失败托管 RR / 写失败标坏；
 *   · 透明重试：失败页读回 → 新块建链 → 重发；dangling 守卫：覆盖写前旧块 0 有效的现场。
 *
 * 关键机制：
 *   · confirm 只在确认时刻重查 L2P（不存 old_pba），先 Journal 后 L2P；
 *   · 待入账超时预算只按「有确定完成时刻的进展」延长，全无进展才结算成写失败；
 *   · 四个 host I/O 分支不改变组结算权：托管 / 待入账都保持组 pending（命令脱钩）。
 *
 * 与其他模块：
 *   · 上游：ftl_host_io_op_callback 调四个分支；ftl_service 调 ftl_confirm_service；
 *   · 下游：l2p_cache（行读写 / 就绪判定）、journal（reserve/append）、BST/allocator
 *     （valid_count 与 trace）、bbt（标坏）、read_recovery（读失败托管）。
 */

#include "assertion.h"
#include "bbt.h"
#include "ftl.h"
#include "ftl_fc_interface.h"
#include "ftl_op.h"
#include "log.h"
#include "read_recovery.h"

#include <string.h>

/* 文件级宏 / 类型 / 变量 */
// dangling 守卫日志：
// · `st=BAD` 是**预期降级**（旧块已退役，数据本不可读）⇒ 每个旧 gblock 只打一行，
// 其余 LBA 只累计到该槽位的 lbas（一个块 256 个 LBA，逐条打必刷屏）。
// · `st=FREE/LOG/NONE` 是**非预期缺陷** ⇒ 仍用限流 ERROR，保留完整现场。
#define FTL_DANGLING_SLOTS (8U)

// 透明重试日志节流：第 1 次立即打印，此后每 N 次一条（program-fail 风暴不刷屏）。
#define FTL_WRITE_RETRY_LOG_EVERY (64U)

typedef struct FTL_DANGLING_ENTRY_S
{
    WORD  g;         /* 旧 gblock；lbas==0 表示空槽 */
    DWORD first_lba; /* 该块第一个命中的 LBA */
    DWORD lbas;      /* 该块累计命中的 LBA 数 */
} ftl_dangling_entry_s;

static log_ratelimit_s s_dangling_log; /* BAD 退役块的 dangling 聚合（1 s） */

static log_ratelimit_s s_confirm_log; /* 待入账超时（5 s） */

static DWORD s_write_retry_logged;

static ftl_dangling_entry_s s_dangling[FTL_DANGLING_SLOTS];

/* dangling 聚合槽的轮转游标：BAD 退役块超过槽数时循环覆盖。 */
static BYTE s_dangling_next;

/* ── dangling ── */

/**
 * @brief 覆盖/解除映射前的**常驻守卫**：L2P 指向的旧块在 BST 里 0 有效时留现场。
 *
 * 触发条件：`valid_count == 0` 却仍被 L2P 引用。此后每次覆盖写该 LBA 都会撞
 * `bst.h` 的 `valid_count > 0U` 断言；运行期被 `sub_valid_if_live` 静默夹住，
 * 但 journal 条目照样落盘 ⇒ 下次开机回放 `pfr_valid_remove` 判**下溢** ⇒ `ctx_ok=0`
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
void ftl_dangling_check(ftl_ctx_s *ftl, DWORD l, DWORD old_pba, WORD new_g)
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
    // 名称转换放在实参里：限流抑制时完全不求值（本函数在写确认热路径上）。
    LOGE_RL(&s_dangling_log, LOG_TAG_FTL "dangling lba=%lu old=%08lx g=%hu st=%s sb=%hu newg=%hu nst=%s", l, old_pba, gob, ftl_block_state_name(old_st), ftl->alloc.blocks[gob].sb_id, new_g, ftl_block_state_name(ftl_block_state(&ftl->alloc.blocks[new_g])));
}

/* ── confirm ── */

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
static BOOL ftl_confirm_progress_is_pending(const ftl_ctx_s *ftl)
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
ftl_status_e ftl_confirm_write(ftl_ctx_s *ftl, const ftl_page_desc_s *d, BOOL *l2p_blocked)
{
    if (l2p_blocked != NULL)
    {
        *l2p_blocked = false;
    }

    // 三个调用点（host 完成路径 / cache 写确认步 / 写确认服务）都传非空 ftl 与描述符
    // ⇒ 不在本函数复检（§十附 / §二十六）。
    // 同理 d->lba 恒为**窗口基址**（唯一写点是 wb_handle(FLUSH) 的 wbuf.base）——
    // 下面用 ftl_lba_plane/cw(l) 取代"从位下标反解坐标"就依赖这条。

    // 页对基址：8 个 LBA 属于同一 region ⇒ 就绪只需探一次（原来逐 CW 8 次）。
    ASSERT(ftl_lba_is_pair_aligned(d->lba));

    const DWORD mask = d->cw_bitmap & ((1U << FTL_LBA_PER_PAIR) - 1U);

    if ((mask != 0U) && (l2p_entries_is_ready(ftl->cache, d->lba, mask) == false))
    {
        // 返回 AGAIN 交由待入账重试：预算是否延长由 `ftl_confirm_service` 按
        // 「确有可推进的工作」判定。加载**起不动**
        // （`load_inflight==0`）时该判据不成立 ⇒ 仍保留 100 ms 的有界失败。
        if (l2p_blocked != NULL)
        {
            *l2p_blocked = true; /* LOADING / FLUSHING / CMT 写回容量不足 */
        }
        return FTL_AGAIN; /* 已起加载：本轮**一条都不动** */
    }

    // Journal 容量/坐标在动账前判定：不足或页型不符时只起异步刷页并返回 false，
    // 本函数直接待入账重试，绝不在这里同步刷 Journal（那会重入完成派发）。
    // 按最坏情况（mask 置位数）预留并留 1 个空槽 ⇒ 随后的 append 不会触发页满分支。
    // 两种页型走各自的入口：顺序写确认 = jrn_seq_is_ready + jrn_append_seq（4B 隐式
    // LBA），其余（wbuf / GC / page0）= jrn_reserve + jrn_append（16B 显式 LBA）。
    const BYTE n_entries = ftl_cw_bitmap_popcount(mask);
    // cache program 页（cache_role != NONE）属于顺序写流 ⇒ Journal 走顺序映射页
    // （4B/条、LBA 由页头基准 + 下标隐式给出）；其余（wbuf / GC / page0）走通用页。
    const BOOL seq_page  = d->seq_page;

    if (n_entries != 0U)
    {
        const BOOL reserved = (seq_page == true) ? jrn_seq_is_ready(ftl->journal, d->lba, n_entries)
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
        if (l2p_blocked != NULL)
        {
            *l2p_blocked = true; /* 就绪检查与批量读之间行转为 FLUSHING */
        }
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
            // 顺序页只记 new_pba；坐标/容量已由 jrn_seq_is_ready 全批判过 ⇒ 这里的
            // 失败是结构性簿记错误。丢弃条目会让已确认的写在掉电后丢映射，
            // 故 debug 留现场（C7：传错 lba 即可构造失败），release 也必须留下日志
            // （H14：ASSERT 与运行期留痕不是二选一）。
            const BOOL appended = jrn_append_seq(ftl->journal, l, newp[bit]);

            LOGE_IF(appended == false, LOG_TAG_FTL "seq journal append failed lba=%lu — mapping not persisted\r\n", l);
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
 * @brief 初始化本模块的日志限流对象（NOLOAD，上电/软复位需显式复位）。
 */
void ftl_confirm_init(void)
{
    log_ratelimit_init(&s_dangling_log, LOG_RATE_INTERVAL_MS);
    log_ratelimit_init(&s_confirm_log, LOG_RATE_SLOW_INTERVAL_MS);
    s_write_retry_logged = 0U;
    memset(s_dangling, 0, sizeof(s_dangling));
    s_dangling_next = 0U;
}

/**
 * @brief 写确认服务：推进**待入账**的写确认（ftl_service 的 [svc] 步）。
 *
 * @param ftl 实例。
 *
 * @note 循环在 FTL（控制器写管线的形态：映射更新由 FTL 执行），pending 只提供
 *       表原语；结算顺序不变量由 pend_confirm_settle 维护。
 */
void ftl_confirm_service(ftl_ctx_s *ftl)
{
    const DWORD now_ms = platform_now_ms();

    if (pend_confirm_count(&ftl->pending) == 0U)
    {
        ftl_cache_demand_window_clear(ftl); /* 没有 parked 确认：窗口上下文结束 */
    }

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

        BOOL               l2p_blocked = false;
        const ftl_status_e st          = ftl_confirm_write(ftl, &d, &l2p_blocked);

        if (st == FTL_AGAIN)
        {
            // confirm 被 L2P 阻塞（LOADING / FLUSHING / CMT 写回容量不足）且没有在飞
            // load ⇒ 开独立提交窗口。不能用 `l2p_is_resident` 判断：它对 FLUSHING
            // 返回 true，而 `l2p_entries_is_ready` 对 FLUSHING 返回 false，会漏掉正是
            // “写回被门控挡住”的失败路径。`l2p_blocked` 由 ftl_confirm_write 精确回传。
            if ((l2p_blocked != false) && (ftl->cache->inflight.load == 0U) &&
                (ftl_cache_demand_window_open(ftl, now_ms) != false))
            {
                pending->park_ms = now_ms; /* 窗口期间不算 confirm 超时 */
                continue;
            }

            // 预算按「无进展时长」计：确有完成源在推进
            // （region 加载或写回在飞、Journal 页写或换块擦、任一非待入账 pending 项）⇒
            // 刷新起点；全都没有才是结构性卡死，让 100 ms 到期暴露成一次写失败。
            if (ftl_confirm_progress_is_pending(ftl) == true)
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
            // 日志必须报「同时有多少条在等」——只打扫描到的第一条（常是 idx=0）会被
            // 误读成「只有一个 region 卡住」。计数放进实参，只在真正打印时统计。
            LOGE_RL(&s_confirm_log, LOG_TAG_FTL "confirm park timeout lba=%lu idx=%hhu parked=%hu", pending->lba, i, pend_confirm_count(&ftl->pending));
        }
        else if (st != FTL_OK)
        {
            LOGE(LOG_TAG_FTL "confirm failed lba=%lu idx=%hhu\r\n", pending->lba, i);
        }

        if (st == FTL_OK)
        {
            ftl_cache_demand_window_clear(ftl); /* 本确认已推进，窗口目的达成 */
        }

        const ssd_status_e status = (st == FTL_OK) ? SSD_STATUS_OK : SSD_STATUS_IO_ERROR;

        pend_confirm_settle(&ftl->pending, i, status);
    }
}

/* ── host_io ── */

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
ssd_status_e ftl_host_io_read_ok(ftl_ctx_s *ftl, fc_cmd_s *cmd, pend_req_s *pending, WORD cmd_id)
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
    const BYTE          nblocks = ftl_cmd_block_ecc_collect(cmd, blocks, PLANE_NUM);

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
BOOL ftl_host_io_write_ok(ftl_ctx_s *ftl, pend_req_s *pending)
{
    ftl_page_desc_s d;

    d.lba       = pending->lba;
    d.new_pba   = pending->new_pba;
    d.new_pba1  = pending->new_pba1;
    d.cw_bitmap = pending->cw_bitmap;
    d.seq_page  = (pending->cache_role != PEND_CACHE_ROLE_NONE);

    if (ftl_confirm_write(ftl, &d, NULL) != FTL_AGAIN)
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
void ftl_host_io_read_fail(ftl_ctx_s *ftl, pend_req_s *pending)
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
void ftl_host_io_write_fail(ftl_ctx_s *ftl, pend_req_s *pending, const fc_cmd_status_s *completion)
{
    // 失败 plane 掩码：逐 desc 取 CW 状态 + desc->paa 的 plane。
    // FC 把 NAND per-CW FAIL 并进 desc（program 与 read 都并），plane1 的失败不再
    // 等到 GC 擦除才暴露。
    DWORD bad_plane_mask = 0U;

    fc_cmd_s *cmd = fc_command_lookup(completion->cmd_id);
    WORD      did = cmd->desc_id;

    while (did != FC_DESC_ID_INVALID)
    {
        const fc_desc_s *d = fc_descriptor_lookup(did);

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

/* ── write ── */

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
ftl_status_e ftl_write_retry(ftl_ctx_s *ftl, pend_req_s *pending, fc_cmd_s *old_cmd)
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
        if (bit_mask_any(pending->cw_bitmap, ftl_cw_plane_mask(p)) && (pba_to_page(pba[p]) > 0U) && (alc_super_open_pba_state(&ftl->alloc, FTL_TEMP_HOT, pba[p]) != ALC_SUPER_PBA_OPEN_PAGE0_DONE))
        {
            alc_super_open_rewind_pair(&ftl->alloc, FTL_TEMP_HOT);
            return FTL_AGAIN;
        }
    }

    // host 写重试命令走后台额度：不得吃掉 L2P demand load 的 CRIT 预留槽。
    fc_cmd_s *cmd = ftl_cmd_alloc_class(FC_CMD_CLASS_BG);

    if (cmd == NULL)
    {
        alc_super_open_rewind_pair(&ftl->alloc, FTL_TEMP_HOT);
        return FTL_AGAIN;
    }
    const fc_cmd_id_t cmd_id = cmd->cmd_id;

    cmd->config = old_cmd->config;
    cmd->meta   = pending->lba;

    ftl_status_e st = ftl_cmd_build_ppa_pair_cw(cmd, pba, pending->cw_bitmap);

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
