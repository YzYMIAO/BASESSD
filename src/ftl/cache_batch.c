/*
 * cache_batch.c — FTL cache 流式序列（读 / 写，本仓自定）
 *
 * 两个方向共用同一模块，但各自保留独立的条件、命令序列与结果枚举：
 *   · cache read：连续 pair 页映射驻留且物理连续时，按窗口提交
 *     START_SEQ_MP/SEQ_MP/LAST_MP；序列状态（ftl->cache_stream.seq）跨窗口、跨 host 传输段
 *     保持，START 只在顺序流头，LAST 只在断点（物理/L2P 块/命令尾/D4）；
 *   · cache program：首窗口首条 op49、中间 op44（15h）、**断点**末条 op48（10h）；
 *     窗口上限 4 个 pair 页，序列同样跨窗口、跨传输段（逐页确认见 ftl.c）。
 *     顺序写确认进 Journal 顺序映射页（JSEG），到配额主动 END 落元数据
 *     （见 journal.h 与 docs/design/ftl_design.md §3.16.5）。
 *
 * 是否进入序列由 ftl->cache_stream.rd/wr 顺序流检测器判定（连续 LBA 阈值），**host 传输段
 * 边界不是断点**。两者都按窗口标 FC chain，经 ftl_op_submit_batch 原子入队；条件
 * 不满足一律回退普通读 / wbuf 路径。
 */

#include "cache_batch.h"

#include "assertion.h"
#include "fc.h"
#include "ftl.h"
#include "ftl_fc_interface.h"
#include "ftl_op.h"
#include "journal.h"
#include "l2p_cache.h"
#include "migrate.h"
#include "pending.h"
#include "wbuf.h"

#include <string.h>

/* cache read 单条 run 的 pair 上限：**由 FC 资源决定，不是 FIFO 深度**。
 * 每 pair 占 FTL_LBA_PER_PAIR（8）个 desc / buffer，FC buffer 池 =
 * FC_BUFFER_POOL_DEPTH ⇒ pair 数 ≤ FC_BUFFER_POOL_DEPTH / FTL_LBA_PER_PAIR。
 * 实际批深再由 cache_batch_budget_ok() 逐级收缩（还要给后台留预留量）。 */
#define FTL_CACHE_READ_MAX_PAIRS (FC_BUFFER_POOL_DEPTH / FTL_LBA_PER_PAIR)

/* ── 资源判据 ── */

/**
 * @brief host cache batch（读/写）共享的资源判据。
 *
 * = FC 命令 / desc / buffer / SQ / HSQ 余量（ftl_fc_host_budget_ok_n）+
 *   pending 表余量。读 / 写 batch 都走这一个入口，避免两份逐字重复的资源检查。
 *
 * @param ftl 实例（提供 pending 表；非 NULL）。
 * @param n   本次 batch 计划提交的子操作数。
 *
 * @return true = 两类资源都够；false = 至少一类不足（调用者应拒绝或降级）。
 */
static BOOL cache_batch_budget_ok(const ftl_ctx_s *ftl, DWORD n)
{
    ASSERT(ftl != NULL);

    return ftl_fc_host_budget_ok_n(n) && (((DWORD)pend_slot_get_count(&ftl->pending) + n) <= FTL_PENDING_MAX);
}

/**
 * @brief 本请求所属 host 命令的结束 LBA（exclusive）。
 *
 * = 本请求剩余 + 同一命令在本请求之后还剩的 LBA（`cmd_remaining`，来自控制器）。
 * 只用于 cache 序列 END/LAST 的落点判定：器件要求收尾命令落在最后一个数据页/读命令上。
 *
 * @param g      当前完成组（提供 cmd_remaining）
 * @param lba    本请求内的当前位置
 * @param remain 本组剩余 LBA 数（走到这里时 = 请求结束 - lba）
 * @return 命令结束 LBA（exclusive）
 */
static DWORD cache_batch_cmd_end(const pend_group_s *g, DWORD lba, DWORD remain)
{
    return lba + remain + g->cmd_remaining;
}

/* ── cache read ── */

// cache read 一次调用的公共参数；避免内部 helper 逐个传 6 个实参。
typedef struct CACHE_READ_REQUEST_S
{
    ftl_ctx_s *ftl;
    WORD       gi;
    DWORD      lba;
    void      *buffer;
    DWORD      max_pairs; /* 本窗口上限（min(remain, 资源)） */
    DWORD     *used;
} cache_read_req_s;

// 构建中的命令/pending 集合；只在 cache_read_submit 内使用。
typedef struct CACHE_READ_BUILD_S
{
    fc_cmd_s   *cmds[FTL_CACHE_READ_MAX_PAIRS];
    pend_req_s *pends[FTL_CACHE_READ_MAX_PAIRS];
    DWORD       n_pairs;
} cache_read_build_s;

/**
 * @brief 校验一个 pair 的 L2P 映射、plane/拓扑与页内一致性。
 *
 * @param ftl      实例（只读）。
 * @param base     本 pair 的起始 LBA（对齐到 FTL_LBA_PER_PAIR）。
 * @param pair_pba 出参：每 plane 的 PBA；未映射的 plane 填 NAND_PBA_INVALID。
 * @param defer    出参：置 true 表示某次 l2p_read 遇 LOADING（已起加载），调用者可下轮重试。
 *
 * @return true = 该 pair 可整对 cache 读；false = 不可（wbuf 未落盘 / 未映射 /
 *         拓扑不符 / 非同一页）。
 */
static BOOL cache_read_probe_pair(const ftl_ctx_s *ftl, DWORD base, DWORD pair_pba[PLANE_NUM], BOOL *defer)
{
    for (BYTE p = 0U; p < PLANE_NUM; p++)
    {
        pair_pba[p] = NAND_PBA_INVALID;
    }

    for (BYTE k = 0U; k < FTL_LBA_PER_PAIR; k++)
    {
        const DWORD l      = base + k;
        DWORD       wr_pba = NAND_PBA_INVALID;

        if (wb_has(ftl->wbuf, l))
        {
            return false; /* 写缓冲未落盘：走普通读的 wbuf 快路径 */
        }
        if (ftl_cache_wr_lookup(ftl, l, &wr_pba) == true)
        {
            return false; /* 已编程未确认（RISK-CACHE-27）：真值不在 L2P，走普通读 */
        }
        if (ftl_cache_wr_failed_covers(ftl, l) == true)
        {
            return false; /* 已判失败未撤销（RISK-CACHE-28）：交给普通读报 IO_ERROR */
        }

        DWORD              p  = NAND_PBA_INVALID;
        const ftl_status_e st = l2p_read(ftl->cache, l, &p);

        if (st == FTL_AGAIN)
        {
            *defer = true;
            return false;
        }

        const BOOL pba_ok = (st == FTL_OK) && (p != NAND_PBA_INVALID) && pba_is_valid(p);
        if (pba_ok == false)
        {
            return false;
        }

        const BYTE pl         = pba_to_plane(p);
        const BOOL same_plane = (pl < PLANE_NUM) && (pl == ftl_lba_plane(l));
        const BOOL same_topo  = (pba_to_ch(p) == ftl->topo.ch) && (pba_to_ce(p) == ftl->topo.ce) && (pba_to_lun(p) == ftl->topo.lun);

        if ((same_plane == false) || (same_topo == false))
        {
            return false; /* 映射表损坏/跨实例：交给普通读路径报错 */
        }

        if (pair_pba[pl] != NAND_PBA_INVALID)
        {
            const BOOL same_page  = pba_same_page(pair_pba[pl], p);
            const BOOL same_block = (pba_to_block(pair_pba[pl]) == pba_to_block(p));

            if ((same_page == false) || (same_block == false))
            {
                return false;
            }
        }
        pair_pba[pl] = p;
    }

    return true;
}

/**
 * @brief 纯探针：一个 pair 的 8 条 L2P 是否全部驻留（**不触发加载**）。
 *
 * @param ftl  实例（只读）。
 * @param base pair 起始 LBA。
 *
 * @return true = 8 条全部驻留；false = 至少一条缺失/正在加载。
 *
 * @note 序列打开时不能调 `l2p_read`：它起的加载读会被 FTL 门控挡住（同通道门控），
 *       本函数因此只查驻留状态。驻留 = CLEAN/DIRTY/FLUSHING，`l2p_read` 不会再起加载。
 */
static BOOL cache_read_pair_resident(const ftl_ctx_s *ftl, DWORD base)
{
    for (BYTE k = 0U; k < FTL_LBA_PER_PAIR; k++)
    {
        if (l2p_is_resident(ftl->cache, base + k) == false)
        {
            return false;
        }
    }

    return true;
}

/**
 * @brief `next` 这一 pair 是否是 `prev` 在器件语义下的物理下一对。
 *
 * 两种合法形态（READ PAGE CACHE SEQUENTIAL 31h **不带地址**，由器件内部自增取下一页）：
 *   · 同 block 且 page + 1；
 *   · 每个 plane 都到 block + 1 且 page == 0——31h 在 block 末页之后进入下一个
 *     logical block（`docs/reference/datasheet_mt29f_cache_ops.md` §2）。
 * two-plane 的 31h 让「每个已寻址 plane」各前进一页 ⇒ 两 plane 必须**同时**成立。
 * 跨 die/LUN 由 probe 的同拓扑检查挡住（31h 禁止跨 die）。
 *
 * @param prev 上一对 PBA（每 plane 一个，已通过 probe）
 * @param next 待判的一对 PBA（每 plane 一个，已通过 probe）
 *
 * @return true = 可以用 SEQ(46h) 继续；false = 必须在上一对用 LAST(47h) 收尾。
 */
static BOOL cache_read_pair_contiguous(const DWORD prev[PLANE_NUM], const DWORD next[PLANE_NUM])
{
    for (BYTE p = 0U; p < PLANE_NUM; p++)
    {
        const WORD prev_block = pba_to_block(prev[p]);
        const BYTE prev_page  = pba_to_page(prev[p]);
        const WORD next_block = pba_to_block(next[p]);
        const BYTE next_page  = pba_to_page(next[p]);

        if ((next_block == prev_block) && (next_page == (BYTE)(prev_page + 1U)))
        {
            continue;
        }

        if ((next_page == 0U) && (next_block == (WORD)(prev_block + 1U)))
        {
            continue;
        }

        return false;
    }

    return true;
}

/**
 * @brief 收集可连续 cache 读的 pair，并在窗口末尾做一对 lookahead。
 *
 * @param req       本次 cache read 的公共参数（max_pairs 由调用者按资源/run 上限收紧）。
 * @param pba       出参：已收集的 pair 物理地址（每 pair 每 plane）。
 * @param defer     出参：收集过程中遇到 L2P 加载（已起加载），调用者应下轮重试。
 * @param run_ended 出参：true = 下一页是物理/L2P/块/total 边界（本窗口末条应 LAST）；
 *                  false = 收集到窗口上限且下一页可续（末条 MORE）。
 *
 * @return 收集到的完整且连续 pair 数（0..max_pairs）。
 *
 * @note lookahead 只用纯驻留探针 `l2p_is_resident()`；**不能** `l2p_read`——
 * 序列打开时它起的加载读会被 FTL 门控挡住，造成死锁。
 */
static DWORD cache_read_collect(const cache_read_req_s *req, DWORD pba[FTL_CACHE_READ_MAX_PAIRS][PLANE_NUM], BOOL *defer, BOOL *run_ended)
{
    const DWORD max_pairs = req->max_pairs;

    DWORD n_pairs = 0U;

    *run_ended = false;

    for (DWORD j = 0U; j < max_pairs; j++)
    {
        const DWORD base = req->lba + (j * FTL_LBA_PER_PAIR);
        DWORD       pair_pba[PLANE_NUM];

        if (cache_read_probe_pair(req->ftl, base, pair_pba, defer) == false)
        {
            *run_ended = true;
            break;
        }

        // 两 plane 必须都有效且页号相同：这是 multi-plane 命令的器件约束
        // （ONFI 3.2 / Micron datasheet 要求同一页地址），不是本仓的特化。
        BOOL complete = true;
        for (BYTE p = 0U; p < PLANE_NUM; p++)
        {
            if (pair_pba[p] == NAND_PBA_INVALID)
            {
                complete = false;
                break;
            }
        }
        // 跨 plane 必须比**页号字段**：`pba_page()` 保留 plane/block 位，两 plane
        // 的同一页号取出来必然不等（Policy B 下两 plane 块号也可能不同）。
        for (BYTE p = 1U; complete && (p < PLANE_NUM); p++)
        {
            complete = (pba_to_page(pair_pba[p]) == pba_to_page(pair_pba[0]));
        }
        if (complete == false)
        {
            *run_ended = true;
            break;
        }

        // 当前 pair 必须紧接上一 pair 的物理页（同 block 的 page+1，或跨到 block+1 的
        // page 0）；否则只提交已收集的前缀。见 cache_read_pair_contiguous。
        if ((j > 0U) && (cache_read_pair_contiguous(pba[j - 1U], pair_pba) == false))
        {
            *run_ended = true;
            break;
        }

        for (BYTE p = 0U; p < PLANE_NUM; p++)
        {
            pba[j][p] = pair_pba[p];
        }
        n_pairs++;
    }

    // 断点 lookahead：窗口收满后探一对（不提交），决定本窗口末条 LAST 还是 MORE。
    if ((n_pairs == max_pairs) && (n_pairs > 0U))
    {
        const DWORD next_base = req->lba + (n_pairs * FTL_LBA_PER_PAIR);

        if (((QWORD)next_base + FTL_LBA_PER_PAIR) > (QWORD)L2P_GLOBAL_LBA_MAX)
        {
            *run_ended = true; /* 键空间到头：本实例不会再有后续数据 */
        }
        else if (lba_routes_to(next_base, req->ftl->topo) == false)
        {
            *run_ended = true; /* 下一段路由到别的实例：本序列到此为止 */
        }
        else if (cache_read_pair_resident(req->ftl, next_base) == false)
        {
            *run_ended = true; /* 8 条 L2P 未全驻留：下一个序列预加载后再来 */
        }
        else
        {
            DWORD look_pba[PLANE_NUM];
            BOOL  look_defer = false;

            if (cache_read_probe_pair(req->ftl, next_base, look_pba, &look_defer) == false)
            {
                *run_ended = true; /* 下一页不可 cache 读（未映射/物理/拓扑/wbuf） */
            }
            else
            {
                // probe 已保证两 plane 有效、同页；这里只判物理连续性
                // （同 block 的 page+1，或跨到 block+1 的 page 0）。
                *run_ended = (cache_read_pair_contiguous(pba[n_pairs - 1U], look_pba) == false);
            }
        }
    }

    return n_pairs;
}

/**
 * @brief 构建命令与 pending；失败时在 build_fail 统一回收。
 *
 * @param b   构建上下文：n_pairs 为入参，cmds / pends 为出参。
 * @param req 本次 cache read 的公共参数。
 * @param pba 每个 pair 每 plane 的物理地址。
 *
 * @return true = 全部命令与 pending 建好；false = 建链或 pending 不足，已回滚。
 */
static BOOL cache_read_build(cache_read_build_s *b, const cache_read_req_s *req, DWORD pba[FTL_CACHE_READ_MAX_PAIRS][PLANE_NUM], BOOL start_seq, BOOL end_seq)
{
    fc_cmd_id_t cmd_ids[FTL_CACHE_READ_MAX_PAIRS];

    for (DWORD i = 0U; i < b->n_pairs; i++)
    {
        b->cmds[i] = fc_command_alloc();

        if (b->cmds[i] == NULL)
        {
            goto build_fail;
        }
        cmd_ids[i] = b->cmds[i]->cmd_id;

        fc_cmd_s *cmd = b->cmds[i];

        // 单页尾（续序列的最后一个 pair）也走 LAST；新序列不允许单页（调用者保证 ≥2）。
        if ((i == (b->n_pairs - 1U)) && (end_seq == true))
        {
            cmd->config.operation = OP_READ_PAGE_CACHE_LAST_MP;
        }
        else if (i == 0U)
        {
            cmd->config.operation = (start_seq == true) ? OP_READ_PAGE_CACHE_START_SEQ_MP : OP_READ_PAGE_CACHE_SEQ_MP;
        }
        else
        {
            cmd->config.operation = OP_READ_PAGE_CACHE_SEQ_MP;
        }

        cmd->config.multi_plane = PLANE_NUM;
        cmd->config.cw_count    = CW_NUM - 1U;
        cmd->config.crc         = FC_CRC_ENABLE;
        cmd->config.random      = FC_RANDOM_ENABLE;
        cmd->config.ldpc        = FC_ECC_MODE_NORMAL;
        // 整条 run 是一条 FC chain：只有末条 END。FC 据此在跨填充轮次续批时
        // 锁住该 LUN，另一队列（HSQ/SQ）的命令回推，序列不会被劈开。
        cmd->config.chain       = (i + 1U < b->n_pairs) ? FC_CHAIN_MORE : ((end_seq == true) ? FC_CHAIN_END : FC_CHAIN_MORE);
        cmd->config.page_type   = FC_PAGE_TYPE_LP;
        cmd->meta               = ftl_lba_pair_base(req->lba + (i * FTL_LBA_PER_PAIR));

        if (ftl_cmd_setup_ppa_pair_cw(cmd, pba[i], FTL_CACHE_FULL_CW) != FTL_OK)
        {
            goto build_fail;
        }
    }

    for (DWORD i = 0U; i < b->n_pairs; i++)
    {
        b->pends[i] = pend_alloc(&req->ftl->pending, cmd_ids[i]);

        if (b->pends[i] == NULL)
        {
            goto build_fail;
        }

        pend_req_s *pending = b->pends[i];

        pending->owners     = ftl_group_bit(req->gi);
        pending->op         = PENDING_OP_READ_CACHE;
        pending->n_cw       = FTL_LBA_PER_PAIR;
        pending->cw_bitmap  = 0U;
        pending->lba        = ftl_lba_pair_base(req->lba + (i * FTL_LBA_PER_PAIR));
        pending->buffer     = (BYTE *)req->buffer + (i * FTL_LBA_PER_PAIR * FTL_IU_BYTES);
        pending->new_pba    = NAND_PBA_INVALID;
        pending->new_pba1   = NAND_PBA_INVALID;
        pending->cache_role = PEND_CACHE_ROLE_NONE;
        pend_validate(pending);
    }

    return true;

build_fail:
    for (DWORD i = 0U; i < b->n_pairs; i++)
    {
        if (b->pends[i] != NULL)
        {
            pend_remove(&req->ftl->pending, b->pends[i]->cmd_id);
        }
        if (b->cmds[i] != NULL)
        {
            (void)fc_command_release(b->cmds[i]->cmd_id);
        }
    }
    return false;
}

/**
 * @brief 建命令与 pending 并提交 N 个 cache 读 pair。
 *
 * @param req     本次 cache read 的公共参数。
 * @param n_pairs 已收集的 pair 数（新序列 ≥ FTL_CACHE_READ_MIN_PAIRS；续序列尾可为 1）。
 * @param pba     每个 pair 每 plane 的物理地址。
 *
 * @return FTL_CACHE_READ_OK 整批已提交；FTL_CACHE_READ_AGAIN 建链失败或整批入队遇
 *         瞬态背压（全批回滚，可下轮重试）；FTL_CACHE_READ_ERROR 永久提交失败。
 */
static ftl_cache_read_result_e cache_read_submit(const cache_read_req_s *req, DWORD n_pairs, DWORD pba[FTL_CACHE_READ_MAX_PAIRS][PLANE_NUM], BOOL start_seq, BOOL end_seq)
{
    cache_read_build_s b;

    memset(&b, 0, sizeof(b));
    b.n_pairs = n_pairs;

    if (cache_read_build(&b, req, pba, start_seq, end_seq) == false)
    {
        req->ftl->cache_stream.read_fallbacks++;
        return FTL_CACHE_READ_AGAIN;
    }

    // 记账必须早于 submit：完成可能在本轮 drain 之后立刻被泵处理。
    for (DWORD i = 0U; i < b.n_pairs; i++)
    {
        pend_group_add_subop(&req->ftl->pending, req->gi);
    }

    // 整条 run 原子入队：FC 的 chain 机制要求链成员在同一队列**连续**入队；
    // 逐条提交在资源不足时会留下半条链（前半已入队、后半没进），链语义被破坏。
    // 序列所有者通道：门控对本批旁路（其他提交会被延迟并置 break_requested）。
    const BOOL owner_prev = req->ftl->cache_stream.seq.owner_submit;

    req->ftl->cache_stream.seq.owner_submit = true;
    const ftl_status_e submit_status        = ftl_op_submit_batch(b.cmds, b.n_pairs, FTL_OP_HOST_IO, NULL);
    req->ftl->cache_stream.seq.owner_submit = owner_prev;

    if (submit_status != FTL_OK)
    {
        // 整批未入队：命令、pending、组子操作计数全部回滚。
        for (DWORD i = 0U; i < b.n_pairs; i++)
        {
            pend_remove(&req->ftl->pending, ftl_cmd_id(b.cmds[i]));
            (void)fc_command_release(b.cmds[i]->cmd_id);
            pend_group_drop_subop(&req->ftl->pending, req->gi);
        }

        req->ftl->cache_stream.read_fallbacks++;

        // 瞬态背压可重试；链契约违反等编程错误是永久失败（H38）。
        return (submit_status == FTL_AGAIN) ? FTL_CACHE_READ_AGAIN : FTL_CACHE_READ_ERROR;
    }

    *req->used = b.n_pairs * FTL_LBA_PER_PAIR;
    req->ftl->cache_stream.read_batches++;
    req->ftl->cache_stream.read_pairs += b.n_pairs;
    return FTL_CACHE_READ_OK;
}

/**
 * @brief host 读的 cache 命中尝试
 *
 * pair 对齐 / 组有效 / 序列方向与接续点任一不满足即 DECLINED，交调用者回退
 * 普通读。
 *
 * @param ftl    实例
 * @param gi     组下标
 * @param lba    起始 LBA
 * @param buffer 输出缓冲
 * @param remain 剩余 LBA 数
 * @param used   输出：消费的 LBA 数
 * @return OK / DECLINED / AGAIN / ERROR
 */
ftl_cache_read_result_e ftl_cache_read_try(ftl_ctx_s *ftl, WORD gi, DWORD lba, void *buffer, DWORD remain, DWORD *used)
{
    ASSERT(ftl != NULL);
    ASSERT(used != NULL);

    if (ftl_lba_is_pair_aligned(lba) == false)
    {
        return FTL_CACHE_READ_DECLINED;
    }

    if ((gi >= FTL_GROUP_MAX) || (ftl->pending.groups[gi].used == false))
    {
        return FTL_CACHE_READ_DECLINED;
    }

    const pend_group_s *g   = &ftl->pending.groups[gi];
    ftl_cache_seq_s    *seq = &ftl->cache_stream.seq;

    // 续序列：方向与接续点必须一致（host 传输段边界不是断点）。
    // 上下文不同 / 续接点不一致（乱序、空洞）⇒ 这条序列已不可能由本组续；若仍
    // active 必须先释放状态，否则普通读回退会被门控永久挡回（owner 槽位可能已被
    // 复用，idle 守卫也等不到）。
    if (seq->active == true)
    {
        if (seq->kind != FTL_CACHE_SEQ_READ)
        {
            return FTL_CACHE_READ_DECLINED; /* 写序列由写路径收尾 */
        }
        if (seq->next_lba != lba)
        {
            ftl_cache_seq_release(ftl);
            return FTL_CACHE_READ_DECLINED;
        }
    }
    else if (ftl->cache_stream.rd.active == false)
    {
        return FTL_CACHE_READ_DECLINED; /* FTL 顺序流检测器未判为顺序读 */
    }

    // 本 host 命令的结束（= 本请求之后仍属于同一命令的 LBA 也在内）：只用于 END 落点。
    const DWORD cmd_end = cache_batch_cmd_end(g, lba, remain);

    // 新序列至少 2 个 pair；**续序列允许 1 个 pair 的尾巴**（单条 LAST 收尾）。
    const BOOL starting = (seq->active == false);

    // 同写路径：起点未 pair 对齐的 host 段不开序列（跨段续接点必然对不上）。
    if ((starting == true) && (ftl_lba_is_pair_aligned((DWORD)g->lba) == false))
    {
        return FTL_CACHE_READ_DECLINED;
    }

    const DWORD min_pairs    = (starting == true) ? FTL_CACHE_READ_MIN_PAIRS : 1U;
    const DWORD remain_pages = remain / FTL_LBA_PER_PAIR;

    if (remain_pages < min_pairs)
    {
        if (seq->active == true)
        {
            ftl_cache_seq_release(ftl); /* 本组没有可续的完整 pair：不能留着序列挡门控 */
        }
        return FTL_CACHE_READ_DECLINED;
    }

    // 新序列：预加载 L2P region（一个 region = L2P_ENTRIES_PER_REGION 条 entry）。
    if (starting == true)
    {
        // START 的探测范围只看当前一个 region 的跨度（起点不对齐时最多跨 2 个
        // L2P region）；序列单段上限 128 pair = 2 个 region，预取只覆盖下一个
        // region，再下一个靠 lookahead 驻留检查安全降级。cache 行整 region 一条，
        // 两行不会互相驱逐。
        DWORD probe_end = lba + L2P_ENTRIES_PER_REGION;

        if (probe_end > cmd_end)
        {
            probe_end = cmd_end;
        }

        if ((probe_end - lba) >= (FTL_CACHE_READ_MIN_PAIRS * FTL_LBA_PER_PAIR))
        {
            const DWORD last_base = ftl_lba_pair_base(probe_end - 1U);
            DWORD       p0        = NAND_PBA_INVALID;
            DWORD       p1        = NAND_PBA_INVALID;

            const ftl_status_e s0 = l2p_read(ftl->cache, lba, &p0);
            const ftl_status_e s1 = l2p_read(ftl->cache, last_base, &p1);

            // 与写路径同口径：预加载本实例的下一个 region，让序列能跨 region 边界。
            const DWORD next_region = lba_next_region_base(lba);

            if ((next_region != FTL_LBA_INVALID) && ((QWORD)next_region < FTL_LBA_MAX) && lba_routes_to(next_region, ftl->topo))
            {
                // 与写路径同口径的 best-effort 预取（边界 lookahead 会安全降级）。
                (void)l2p_region_prefetch(ftl->cache, lba_to_global_region(next_region));
            }

            if ((s0 == FTL_AGAIN) || (s1 == FTL_AGAIN))
            {
                // 只认**在飞命令**（启动失败 ≠ 有进展，见 RISK-HOST-14）
                ftl->flags.l2p_defer = l2p_cache_any_inflight(ftl->cache);
                ftl->cache_stream.read_declines++;
                return FTL_CACHE_READ_AGAIN;
            }
        }
    }

    DWORD max_pairs = remain_pages;

    if (max_pairs > FTL_CACHE_READ_MAX_PAIRS)
    {
        max_pairs = FTL_CACHE_READ_MAX_PAIRS;
    }

    // 单段上限：跨 block 续接后序列仍需要一个与器件无关的收尾点——读序列期间该 die
    // 的 cache 寄存器被占用（ARDY = 0），程序命令发不进去，断点就是后台元数据在本实例
    // 上的让步点（见 cache_batch.h 的 FTL_CACHE_READ_MAX_RUN_PAIRS）。
    const DWORD run_used = (starting == true) ? 0U : seq->run_pairs;

    if (run_used >= FTL_CACHE_READ_MAX_RUN_PAIRS)
    {
        // 不变量：吃到上限的那一窗口必须已带 LAST 收尾 ⇒ 这里不应再看到 active。
        ASSERT_MSG(run_used < FTL_CACHE_READ_MAX_RUN_PAIRS, "cache read run over max: run=%lu", run_used);
        ftl_cache_seq_release(ftl);
        return FTL_CACHE_READ_DECLINED;
    }

    const DWORD run_left = FTL_CACHE_READ_MAX_RUN_PAIRS - run_used;

    if (max_pairs > run_left)
    {
        max_pairs = run_left;
    }

    if (max_pairs < min_pairs)
    {
        return FTL_CACHE_READ_DECLINED;
    }

    cache_read_req_s req;

    req.ftl       = ftl;
    req.gi        = gi;
    req.lba       = lba;
    req.buffer    = buffer;
    req.max_pairs = max_pairs;
    req.used      = used;

    DWORD pba[FTL_CACHE_READ_MAX_PAIRS][PLANE_NUM];
    BOOL  defer     = false;
    BOOL  run_ended = false;
    DWORD collected = cache_read_collect(&req, pba, &defer, &run_ended);

    if (collected < min_pairs)
    {
        ftl->cache_stream.read_declines++;
        if (defer == true)
        {
            ftl->flags.l2p_defer = l2p_cache_any_inflight(ftl->cache);
        }
        if (seq->active == true)
        {
            // 续窗口连第一个 pair 都收不到：无法给已打开的序列发 LAST。
            // 放弃序列状态，下轮以新 START 重来（设备侧未收尾的序列由 START 重置，
            // RISK-IF-09）。不放弃的话普通读会被门控永久挡回。
            ftl_cache_seq_release(ftl);
        }
        return (defer == true) ? FTL_CACHE_READ_AGAIN : FTL_CACHE_READ_DECLINED;
    }

    const DWORD after        = lba + (collected * FTL_LBA_PER_PAIR);
    // 本组剩余不足一个 pair（命令尾 1..7 LBA）：其余 LBA 只能走普通读，
    // 必须在这里 LAST 收尾，否则普通读会被序列门控永久挡回。
    const DWORD remain_after = remain - (collected * FTL_LBA_PER_PAIR);
    const DWORD tail_lbas    = cmd_end - after;
    // 命令边界**不是**断点：序列跨 host 命令保持打开（START 只在 LBA 流头）。
    // 断点 = 物理/region 不连续 / D4 / 命令尾不足一对。
    BOOL end_seq             = ((run_ended == true) || (seq->break_requested == true) ||
                    ((remain_after > 0U) && (remain_after < FTL_LBA_PER_PAIR)) ||
                    ((tail_lbas > 0U) && (tail_lbas < FTL_LBA_PER_PAIR)));

    DWORD n = collected;

    while ((n > min_pairs) && (cache_batch_budget_ok(ftl, n) == false))
    {
        n--;
    }

    if (cache_batch_budget_ok(ftl, n) == false)
    {
        ftl->cache_stream.read_fallbacks++;
        // 序列打开时不能回退普通读（会劈开设备序列）⇒ 重试等资源。
        return (seq->active == true) ? FTL_CACHE_READ_AGAIN : FTL_CACHE_READ_DECLINED;
    }

    if (n < collected)
    {
        end_seq = false; // 只提交前缀：run 未到终点
    }

    // 本窗口吃到单段上限 ⇒ 必须在本窗口最后一对发 LAST（下一段重新 START）。
    if ((run_used + n) >= FTL_CACHE_READ_MAX_RUN_PAIRS)
    {
        end_seq = true;
    }

    const BOOL                    start_seq = (starting == true);
    const ftl_cache_read_result_e r         = cache_read_submit(&req, n, pba, start_seq, end_seq);

    if (r == FTL_CACHE_READ_OK)
    {
        if ((end_seq == true) && (seq->break_requested == true))
        {
            ftl->cache_stream.seq_breaks++; /* D4：后台/其他提交触发的断点 */
        }

        seq->kind      = FTL_CACHE_SEQ_READ;
        seq->owner_gi  = gi;
        seq->next_lba  = lba + (n * FTL_LBA_PER_PAIR);
        seq->idle_ms   = 0U;
        seq->run_pairs = (starting == true) ? n : (seq->run_pairs + n);

        if (starting == true)
        {
            ftl->cache_stream.read_starts++; /* 观测：batches / starts = 平均段长 */
        }

        if (end_seq == true)
        {
            seq->active          = false;
            seq->draining        = true; /* 命令还在飞：门控保持关闭直到排空 */
            seq->idle_ms         = 0U;
            seq->run_pairs       = 0U;
            seq->break_requested = false;
        }
        else
        {
            seq->active   = true;
            seq->draining = false;
        }
    }

    return r;
}
/* ── cache program（流式写序列） ──
 *
 * 写序列的 START（49h FIRST）只在序列头，END（48h）只在断点；窗口之间全 44h。
 * 页的**最终结论滞后一页**：第 i 页由第 i+1 条命令的 FAIL_CACHE 确认，最后一页
 * 由 END 的 FAIL 确认（见 ftl.c 的 ftl_cache_wr_member_done）。
 */

// 一个写窗口的构建上下文；只在 ftl_cache_write_try 内使用。
typedef struct CACHE_WR_BUILD_S
{
    fc_cmd_s *cmds[FTL_CACHE_WRITE_MAX_PAGES];
    DWORD     pba[FTL_CACHE_WRITE_MAX_PAGES][PLANE_NUM];
    BYTE      pend_idx[FTL_CACHE_WRITE_MAX_PAGES];
    BYTE      n_cmd;
    BYTE      n_alloc;
    BYTE      n_pend;
} cache_wr_build_s;

/**
 * @brief 撤销尚未提交的写窗口：释放命令/pending + 归还已分配的页对。
 *
 * @param ftl 实例。
 * @param b   构建上下文（按 n_cmd / n_pend / n_alloc 逐项回收）。
 */
static void cache_wr_discard(ftl_ctx_s *ftl, cache_wr_build_s *b)
{
    for (BYTE i = 0U; i < b->n_cmd; i++)
    {
        if (b->cmds[i] != NULL)
        {
            (void)fc_command_release(b->cmds[i]->cmd_id);
        }
    }

    for (BYTE i = 0U; i < b->n_pend; i++)
    {
        pend_release_index(&ftl->pending, b->pend_idx[i]);
    }

    for (BYTE i = 0U; i < b->n_alloc; i++)
    {
        alc_super_open_rewind_pair(&ftl->alloc, FTL_TEMP_HOT);
    }
}

/**
 * @brief 建一个写窗口（N 页）：分配 hot SB 页对、建命令、登记 pending（尚未提交）。
 *
 * @param ftl      实例。
 * @param gi       本窗口归属的组下标。
 * @param lba      窗口起始 LBA（pair 对齐）。
 * @param buffer   窗口源数据（N × 4 KiB）。
 * @param n_pages  窗口页数（1..FTL_CACHE_WRITE_MAX_PAGES）。
 * @param start_seq true = 本窗口是序列首窗口（首条 op49；**1 页窗口同时是首/末页时
 *                  发 op48 END**——末页结论只能由 END 给出，见函数内优先级注释）。
 * @param end_seq   true = 本窗口是序列末窗口（末条 op48）。
 * @param b        出参：本窗口的页对 / 命令 / pending。
 *
 * @return FTL_OK；FTL_AGAIN（页对 / pending / 资源不足，已回滚）；其他建链错误。
 */
static ftl_status_e cache_wr_build(ftl_ctx_s *ftl, WORD gi, DWORD lba, const void *buffer, BYTE n_pages, BOOL start_seq, BOOL end_seq, cache_wr_build_s *b)
{
    for (BYTE j = 0U; j < n_pages; j++)
    {
        const ftl_status_e alc_st = alc_super_open_alloc_pair(&ftl->alloc, ALC_SUPER_ALLOC_HOT, b->pba[j]);

        if (alc_st != FTL_OK)
        {
            if (alc_st == FTL_AGAIN)
            {
                mig_gc_request_urgent(ftl->mig_handle); /* 瞬态：请求 GC 补池 */
            }

            cache_wr_discard(ftl, b);
            /* H38：永久失败（如 Closed 列表满）不能伪装成瞬态，否则 drain 会重试到 stall
             * 才报 IO_ERROR（板端 RISK-ALLOC-09 的次生现象）。 */
            return (alc_st == FTL_AGAIN) ? FTL_AGAIN : FTL_ERROR;
        }
        b->n_alloc++;

        // 本窗口自己打开了新块（分配到的第一页是 page0）：page0 的写屏障由本序列
        // 承担——成功结算时记 page0_programmed；失败时序列中止、其后页不确认。
        if (pba_to_page(b->pba[j][0]) == 0U)
        {
            ftl->cache_stream.seq.own_page0 = true;
        }

        const DWORD           page_lba = lba + ((DWORD)j * FTL_LBA_PER_PAIR);
        const BYTE           *src      = (const BYTE *)buffer + ((DWORD)j * FTL_LBA_PER_PAIR * FTL_IU_BYTES);
        const ftl_page_cfg_s *cfg      = FTL_PAGE_CFG_DATA(ftl_lba_page(page_lba));

        BYTE              op;
        pend_cache_role_e role;

        // 优先级：**END 高于 FIRST**。1 页窗口同时是首/末页时（块尾 1 页开序列）必须发
        // END：末页的结论由 END 的 FAIL 给出（ftl_cache_wr_member_done）；只发 FIRST
        // 会让该页永远留在未确认 FIFO 里，映射永不应用、后续序列也开不了。
        if ((j + 1U == n_pages) && (end_seq == true))
        {
            op   = OP_TWO_PLANE_PROGRAM_CACHE_END;
            role = PEND_CACHE_ROLE_END;
        }
        else if ((start_seq == true) && (j == 0U))
        {
            op   = OP_TWO_PLANE_PROGRAM_CACHE_FIRST;
            role = PEND_CACHE_ROLE_FIRST;
        }
        else
        {
            op   = OP_TWO_PLANE_PROGRAM_CACHE;
            role = PEND_CACHE_ROLE_INTERMEDIATE;
        }

        const ftl_status_e st = ftl_async_write_pair_cmd(&b->cmds[j], b->pba[j], src, FTL_CACHE_FULL_CW, op, cfg);

        if (st != FTL_OK)
        {
            cache_wr_discard(ftl, b);
            return st;
        }
        b->n_cmd++;

        // 窗口只是一条更长序列的一段：断点末条 END，否则 MORE。
        b->cmds[j]->config.chain = (j + 1U < n_pages) ? FC_CHAIN_MORE : ((end_seq == true) ? FC_CHAIN_END : FC_CHAIN_MORE);

        pend_req_s *pending = pend_alloc(&ftl->pending, ftl_cmd_id(b->cmds[j]));

        if (pending == NULL)
        {
            cache_wr_discard(ftl, b);
            return FTL_AGAIN;
        }

        pending->owners      = ftl_group_bit(gi);
        pending->op          = PENDING_OP_WRITE_CACHE;
        pending->n_cw        = FTL_LBA_PER_PAIR;
        pending->cw_bitmap   = (BYTE)((1U << FTL_LBA_PER_PAIR) - 1U);
        pending->lba         = page_lba;
        pending->buffer      = NULL;
        pending->new_pba     = b->pba[j][0];
        pending->new_pba1    = b->pba[j][1];
        pending->cache_role  = role;
        pending->cache_gen   = ftl->cache_stream.seq.gen;
        pending->retry_count = 0U;
        pend_validate(pending);

        b->pend_idx[j] = (BYTE)(pending - ftl->pending.slots);
        b->n_pend++;
    }

    return FTL_OK;
}

/**
 * @brief host 写的 cache 命中尝试
 *
 * 写序列被读序列占用时返回 AGAIN（不回退）；其余不满足即 DECLINED。
 *
 * @param ftl    实例
 * @param gi     组下标
 * @param lba    起始 LBA
 * @param buffer 源缓冲
 * @param remain 剩余 LBA 数
 * @param used   输出：消费的 LBA 数
 * @return OK / DECLINED / AGAIN / ERROR
 */
ftl_cache_write_result_e ftl_cache_write_try(ftl_ctx_s *ftl, WORD gi, DWORD lba, void *buffer, DWORD remain, DWORD *used)
{
    ASSERT(ftl != NULL);
    ASSERT(used != NULL);

    if (ftl_lba_is_pair_aligned(lba) == false)
    {
        ftl->cache_stream.wb_declines++;
        return FTL_CACHE_WRITE_DECLINED;
    }

    if ((gi >= FTL_GROUP_MAX) || (ftl->pending.groups[gi].used == false))
    {
        return FTL_CACHE_WRITE_DECLINED;
    }

    const pend_group_s *g   = &ftl->pending.groups[gi];
    ftl_cache_seq_s    *seq = &ftl->cache_stream.seq;

    // 本 host 命令的结束（= 本请求之后仍属于同一命令的 LBA 也在内）：只用于 END 落点。
    const DWORD cmd_end = cache_batch_cmd_end(g, lba, remain);

    // 续序列：方向与接续点必须一致（host 传输段边界不是断点）。
    if (seq->active == true)
    {
        if (seq->kind != FTL_CACHE_SEQ_WRITE)
        {
            /* 读序列占用设备管线：写组等它收尾（门控会挡住普通写），不能回退。 */
            return FTL_CACHE_WRITE_AGAIN;
        }
        if (seq->next_lba != lba)
        {
            // 续接点不一致：本序列不能再由本窗口续。**不能 abort**——设备序列已
            // 提前回执，未确认页还没结算；交给 service 用 padding END 有序收尾，
            // 本组下轮重试（确定性等待，不计 stall）。
            seq->close_requested = true; /* 进展由 seq_closing 表达（bg_progress 已含） */
            return FTL_CACHE_WRITE_AGAIN;
        }
    }
    else if (ftl->cache_stream.wr.active == false)
    {
        ftl->cache_stream.wb_declines++;
        return FTL_CACHE_WRITE_DECLINED; /* FTL 顺序流检测器未判为顺序写 */
    }

    const BOOL starting = (seq->active == false);

    // 序列只在 **pair 对齐的 host 段**上开启：段长（16 KiB = 32 LBA）是 pair 的整数倍，
    // 命令起点的对齐相位整条命令不变。起点未对齐时，序列的 next_lba（pair 对齐）与
    // 下一段起点（同相位未对齐）必然对不上；而写序列没有数据页就无法 END ⇒ 不开启，
    // 整段走 wbuf（随机/未对齐写本来也不值得 cache program 序列）。
    if ((starting == true) && (ftl_lba_is_pair_aligned((DWORD)g->lba) == false))
    {
        ftl->cache_stream.wb_declines++;
        return FTL_CACHE_WRITE_DECLINED;
    }

    // 序列允许 1 页窗口（含开新序列）：Journal 顺序页配额（63 pair）与物理 block
    // （64 页）不同长，块尾 1..3 页必须能开序列才能续上——写 cache program 的
    // 44/48h 每条自带地址，序列跨 block 合法（`docs/reference/datasheet_mt29f_cache_ops.md`
    // §5/§6）。少于 4 页的窗口只有块尾/命令尾两种情况，不增加 START 频次。
    const DWORD min_pages    = 1U;
    const DWORD remain_pages = remain / FTL_LBA_PER_PAIR;

    if ((remain_pages < min_pages) || ((cmd_end - lba) < (min_pages * FTL_LBA_PER_PAIR)))
    {
        if (seq->active == true)
        {
            // 本段没有可续的完整页（命令尾 / 低于最小窗口）：设备序列保持打开，
            // 交给 service 用 padding END 有序收尾；本组下轮重试。
            seq->close_requested = true; /* 进展由 seq_closing 表达（bg_progress 已含） */
            ftl->cache_stream.wb_declines++;
            return FTL_CACHE_WRITE_AGAIN;
        }
        ftl->cache_stream.wb_declines++;
        return FTL_CACHE_WRITE_DECLINED;
    }

    if ((starting == true) && (wb_is_idle(ftl->wbuf) == false))
    {
        ftl->cache_stream.wb_declines++;
        return FTL_CACHE_WRITE_DECLINED;
    }

    // 开序列前必须没有**非顺序页**的写确认在飞（wbuf / GC 的普通写）：
    // 顺序页不携带 LBA，普通条目混进来会破坏隐式坐标；等它们确认完再开（下个窗口重试）。
    if ((starting == true) && pend_has_foreign_write(&ftl->pending))
    {
        ftl->cache_stream.wb_declines++;
        return FTL_CACHE_WRITE_DECLINED;
    }

    // 开新序列时要求上一段的 cache program 页全部确认完：新序列的 START 可能需要
    // 刷出旧顺序页（LBA 不连续时），迟到的确认会落到新页、破坏其隐式坐标/容量。
    // 待入账的确认同样算"未排空"（它尚未 append，LBA 也未知先后）。
    if ((starting == true) && ((seq->wr.count != 0U) || (seq->wr_inflight != 0U) || (pend_find_op(&ftl->pending, PENDING_OP_WRITE_CONFIRM, NULL) == true)))
    {
        ftl->cache_stream.wb_declines++;
        return FTL_CACHE_WRITE_DECLINED;
    }

    // 余量用 cache 序列专用口：新块允许由本序列自己打开（page0 由本序列写），
    // 从而消掉「每个 block 边界回退 wbuf、wbuf 再开块」的写放大。
    const DWORD room = alc_super_open_get_room_seq(&ftl->alloc, FTL_TEMP_HOT, (seq->active == true) && (seq->own_page0 == true));

    if (room == 0U)
    {
        if (seq->active == true)
        {
            // 当前 block 已被关闭/写满：本窗口无法续。写序列已提前回执 ⇒ **不能
            // abort**；请 service 用 padding END 收尾（padding 自己会开新块）。
            seq->close_requested = true; /* 进展由 seq_closing 表达（bg_progress 已含） */
            ftl->cache_stream.wb_declines++;
            return FTL_CACHE_WRITE_AGAIN;
        }
        ftl->cache_stream.wb_declines++;
        return FTL_CACHE_WRITE_DECLINED;
    }

    // 新序列：先把 Journal 当前页刷出并切到一张空白顺序页（END → 元数据落盘 →
    // 新 START 的落盘次序；顺序序列占用每个 plane 的 cache 寄存器期间元数据写会被
    // 门控挡住，所以元数据只能在序列边界落盘）。
    if (starting == true)
    {
        seq->own_page0 = false; /* 新序列：page0 归属由本窗口的分配结果决定 */

        // 预留量 = 首窗口实际页数（≤4，且不超过本块余量）；按 4 页预留会在块尾
        // 把还有空间的 Journal 页提前刷掉。
        const DWORD first_pages = (room < FTL_CACHE_WRITE_MAX_PAGES) ? room : FTL_CACHE_WRITE_MAX_PAGES;

        if (jrn_seq_begin(ftl->journal, lba, (WORD)(first_pages * FTL_LBA_PER_PAIR)) == false)
        {
            ftl->cache_stream.wb_declines++;
            return FTL_CACHE_WRITE_DECLINED; /* 页写起不来：本窗口回退 wbuf */
        }
    }

    // 新序列：预加载本窗口覆盖的 L2P region（序列打开后加载读会被门控挡住，
    // 而确认映射需要该 region 就绪）。最多探首/尾两点，覆盖 4 页窗口的跨 region 情况。
    if (starting == true)
    {
        const DWORD probe_last = ((cmd_end - lba) > (FTL_CACHE_WRITE_MAX_PAGES * FTL_LBA_PER_PAIR)) ? (lba + (FTL_CACHE_WRITE_MAX_PAGES * FTL_LBA_PER_PAIR) - 1U) : (cmd_end - 1U);
        DWORD       p0         = NAND_PBA_INVALID;
        DWORD       p1         = NAND_PBA_INVALID;

        const ftl_status_e s0 = l2p_read(ftl->cache, lba, &p0);
        const ftl_status_e s1 = l2p_read(ftl->cache, probe_last, &p1);

        // 顺序流**跨 region** 的配套：把本实例的下一个 region 也在 START 前加载。
        // 序列期间 L2P 加载被门控，只能在开序列前做；一段序列 ≤ JSEG 页容量
        // （63 pair < 64 pair/region）⇒ 最多跨一个 region，预加载一个就够。
        // 预加载失败/被驱逐时，窗口末的 lookahead 仍会照常 END（安全降级）。
        const DWORD next_region = lba_next_region_base(lba);

        if ((next_region != FTL_LBA_INVALID) && ((QWORD)next_region < FTL_LBA_MAX) && lba_routes_to(next_region, ftl->topo))
        {
            // **best-effort 预取**（l2p_region_prefetch 自带节流：thrashing / 在飞上限 /
            // FC 预算；不踢脏行写回、不挤 host load）。失败时窗口末 lookahead 照常 END，
            // 下一次 START 再受益；只有当前 region 就绪才必须等。
            (void)l2p_region_prefetch(ftl->cache, lba_to_global_region(next_region));
        }

        if ((s0 == FTL_AGAIN) || (s1 == FTL_AGAIN))
        {
            // 只认**在飞命令**（启动失败 ≠ 有进展，见 RISK-HOST-14）
            ftl->flags.l2p_defer = l2p_cache_any_inflight(ftl->cache);
            ftl->cache_stream.wb_declines++;
            return FTL_CACHE_WRITE_AGAIN;
        }
    }

    // 窗口 = min(组剩余, 命令剩余, block 剩余, 窗口上限, 确认 FIFO 余量, 顺序页可用)。
    DWORD n_pages = remain_pages;

    if (n_pages > ((cmd_end - lba) / FTL_LBA_PER_PAIR))
    {
        n_pages = (cmd_end - lba) / FTL_LBA_PER_PAIR;
    }
    if (n_pages > room)
    {
        n_pages = room;
    }
    if (n_pages > FTL_CACHE_WRITE_MAX_PAGES)
    {
        n_pages = FTL_CACHE_WRITE_MAX_PAGES;
    }

    // Journal 顺序页可用空间：页可以跨 host 命令续用，**页满才是主动断点**
    // （508 条按整 pair 用到 504 条 = 63 pair ≈252 KiB）。
    // ⚠ 必须减去**在飞未确认页**的条目：`current_entry` 只计已 append 的条目，
    // 确认滞后于提交；不减就会把页算满后再追加，触发序列期间的页刷被门控
    // （确认待入账 / 1 s 中止）。每个在飞页确认时追加 FTL_LBA_PER_PAIR 条。
    // 在飞 = 已进页表（wr.count）+ 已下发未完成（wr_inflight）。页在**完成时**才进
    // 页表（早回执），只减 wr.count 会把还在编程的页当成"没有"，窗口就会被放大到
    // 超过环深/顺序页容量——完成后集中入表即溢出（环满 ASSERT / 顺序页超发）。
    const DWORD wr_pages_pending = seq->wr.count + seq->wr_inflight;
    const DWORD page_free        = jrn_seq_room(ftl->journal);
    const DWORD inflight         = wr_pages_pending * FTL_LBA_PER_PAIR;
    const DWORD page_usable      = (page_free > inflight) ? (page_free - inflight) : 0U;
    const DWORD page_room_pairs  = page_usable / FTL_LBA_PER_PAIR;

    if (n_pages > page_room_pairs)
    {
        n_pages = page_room_pairs;
    }

    const BYTE ring_free = (wr_pages_pending < FTL_CACHE_SEQ_RING) ? (BYTE)(FTL_CACHE_SEQ_RING - wr_pages_pending) : 0U;

    if (n_pages > ring_free)
    {
        n_pages = ring_free;
    }

    if (n_pages < min_pages)
    {
        if (starting == false)
        {
            /* 没有可下发数据页时不能只返回 AGAIN：确认 FIFO 满（wr.count+inflight 达
             * FTL_CACHE_SEQ_RING）或 Journal 顺序页无空间后，旧页的 FAILC 证明必须靠
             * **后续 cache 命令 / padding END** 推进；只等完成泵会形成环形等待——
             * 组等 ring 空，ring 等下一命令，idle guard 又因组仍 issuing 不请求收尾。
             * 这里显式请求 padding END，让旧页走完证明后组再继续。 */
            if (n_pages == 0U)
            {
                seq->close_requested = true;
            }
            ftl->cache_stream.wb_declines++;
            return FTL_CACHE_WRITE_AGAIN; /* 确认 FIFO 满：等完成泵结算 */
        }
        ftl->cache_stream.wb_declines++;
        return FTL_CACHE_WRITE_DECLINED;
    }

    while ((n_pages > min_pages) && (cache_batch_budget_ok(ftl, n_pages) == false))
    {
        n_pages--;
    }

    if (cache_batch_budget_ok(ftl, n_pages) == false)
    {
        return (starting == true) ? FTL_CACHE_WRITE_DECLINED : FTL_CACHE_WRITE_AGAIN;
    }

    // 断点：**Journal 顺序页写满** / D4 / 命令尾不足一个 pair（尾 1..7 LBA 只能走普通写，
    // 用 cmd_remaining 提前在数据页上 END）。**host 传输段、host 命令与物理 block 边界
    // 都不是断点**——写 cache program 的 44/48h 每条自带地址，序列跨 block 合法
    // （`docs/reference/datasheet_mt29f_cache_ops.md` §5/§6）；窗口按 Open SB 剩余 clamp
    // （一次构建不跨 SB，保 `alc_super_open_rewind_pair` 的回滚前提），下一个窗口由
    // `alloc_pair` 关旧块、开新块后继续同一序列。没有数据页时由 padding END 收尾。
    const DWORD after     = lba + (n_pages * FTL_LBA_PER_PAIR);
    const DWORD tail_lbas = cmd_end - after;
    // 页满断点：本窗口 + 在飞页之后，顺序页再也放不下一个 pair ⇒ 主动 END。
    const BOOL page_end   = ((page_usable - (n_pages * FTL_LBA_PER_PAIR)) < FTL_LBA_PER_PAIR);
    BOOL       end_seq    = ((seq->break_requested == true) || (page_end == true) ||
                    ((tail_lbas > 0U) && (tail_lbas < FTL_LBA_PER_PAIR)));

    // 跨 block 续写准入：本窗口吃完当前 SB 时，下一个窗口必须开得出新块。否则序列会停在
    // 「设备序列打开、没有可提交的数据页」——门控挡住 GC，padding END 也无页可分配
    // （RISK-CACHE-15 的盘满例外会中止一条**已回执**的序列）。此时按旧语义在本窗口
    // 最后一页 END，把设备序列关掉再让 GC 跑。
    if ((n_pages == room) && (alc_super_can_open(&ftl->alloc, ALC_SUPER_ALLOC_HOT) == false))
    {
        end_seq = true;
    }

    // 顺序页预留：失败 ⇒ 本窗口 END（序列收尾后刷页能跑，待入账的确认才能推进）。
    _Static_assert((FTL_CACHE_WRITE_MAX_PAGES * FTL_LBA_PER_PAIR) < JRN_SEG_ENTRIES_PER_PAGE, "journal seq window must fit one page");

    if (jrn_seq_reserve(ftl->journal, (WORD)(n_pages * FTL_LBA_PER_PAIR)) == false)
    {
        end_seq = true;
    }

    // 断点 lookahead：下一窗口覆盖的 L2P region 不驻留 ⇒ 本窗口 END，让下一窗口以
    // 新序列预加载（序列打开时不能起加载读）。与读路径的 lookahead 同源。
    if (end_seq == false)
    {
        const DWORD next_last = ((cmd_end - after) > (FTL_CACHE_WRITE_MAX_PAGES * FTL_LBA_PER_PAIR)) ? (after + (FTL_CACHE_WRITE_MAX_PAGES * FTL_LBA_PER_PAIR) - 1U) : (cmd_end - 1U);

        if ((lba_routes_to(after, ftl->topo) == false) || (l2p_is_resident(ftl->cache, after) == false) ||
            (l2p_is_resident(ftl->cache, next_last) == false))
        {
            end_seq = true;
        }
    }

    cache_wr_build_s b;

    memset(&b, 0, sizeof(b));

    ftl_status_e st = cache_wr_build(ftl, gi, lba, buffer, (BYTE)n_pages, starting, end_seq, &b);

    if (st == FTL_OK)
    {
        // 记账必须早于 submit：完成可能在本轮 drain 之后立刻被泵处理。
        for (BYTE j = 0U; j < b.n_pend; j++)
        {
            pend_group_add_subop(&ftl->pending, gi);
        }
        // 在飞成员数：完成入口（ftl_cache_wr_member_done）逐个递减，flush 屏障与
        // 「开新序列」都等它归零。页在完成时才进页表，这个计数补上「已下发未完成」的洞。
        ftl->cache_stream.seq.wr_inflight = (BYTE)(ftl->cache_stream.seq.wr_inflight + b.n_pend);

        // 序列所有者通道：门控对本批旁路（其他提交会被延迟并置 break_requested）。
        const BOOL owner_prev = ftl->cache_stream.seq.owner_submit;

        ftl->cache_stream.seq.owner_submit = true;
        st                                 = ftl_op_submit_batch(b.cmds, b.n_cmd, FTL_OP_HOST_IO, NULL);
        ftl->cache_stream.seq.owner_submit = owner_prev;

        if (st != FTL_OK)
        {
            ftl->cache_stream.seq.wr_inflight = (BYTE)(ftl->cache_stream.seq.wr_inflight - b.n_pend);

            for (BYTE j = 0U; j < b.n_pend; j++)
            {
                pend_release_index(&ftl->pending, b.pend_idx[j]);
                pend_group_drop_subop(&ftl->pending, gi);
            }
            for (BYTE j = 0U; j < b.n_cmd; j++)
            {
                (void)fc_command_release(b.cmds[j]->cmd_id);
            }
            for (BYTE j = 0U; j < b.n_alloc; j++)
            {
                alc_super_open_rewind_pair(&ftl->alloc, FTL_TEMP_HOT);
            }
        }
    }

    if (st != FTL_OK)
    {
        return (st == FTL_AGAIN) ? FTL_CACHE_WRITE_AGAIN : FTL_CACHE_WRITE_ERROR;
    }

    // 页在**完成**时进入「已编程页表」（ftl_cache_wr_member_done，早回执）——
    // 提交时**不**压 pending 下标：页在完成时入表（RISK-CACHE-27）。

    if ((end_seq == true) && (seq->break_requested == true))
    {
        ftl->cache_stream.seq_breaks++; /* D4：后台/其他提交触发的断点 */
    }

    seq->kind     = FTL_CACHE_SEQ_WRITE;
    seq->owner_gi = gi;
    seq->next_lba = lba + (n_pages * FTL_LBA_PER_PAIR);
    seq->idle_ms  = 0U;

    if (starting == true)
    {
        ftl->cache_stream.wb_starts++;
    }

    if (page_end == true)
    {
        ftl->cache_stream.wb_page_ends++; /* 观测：约每 63 pair / 252 KiB 一次 */
    }

    if (end_seq == true)
    {
        seq->active          = false;
        seq->break_requested = false;
        seq->own_page0       = false; /* 序列结束：下一段重新判定 page0 归属 */
    }
    else
    {
        seq->active = true;
    }

    ftl->cache_stream.wb_batches++;
    ftl->cache_stream.wb_pages += n_pages;
    *used = n_pages * FTL_LBA_PER_PAIR;
    return FTL_CACHE_WRITE_OK;
}
