/*
 * cache_batch.c — FTL cache 流式序列（读 / 写，本仓自定）
 *
 * 两个方向共用同一模块，但各自保留独立的条件、命令序列与结果枚举：
 *   · cache read：连续 pair 页映射驻留且物理连续时，按窗口提交
 *     START_SEQ_MP/SEQ_MP/LAST_MP；序列状态（ftl->cache_seq）跨窗口、跨 chunk 保持，
 *     START 只在 run 头，LAST 只在断点（物理/L2P/块/命令尾/D4）；
 *   · cache program：首窗口首条 op49、中间 op44（15h）、**断点**末条 op48（10h）；
 *     窗口上限 4 个 pair 页，序列同样跨窗口、跨 chunk（逐页确认见 ftl.c）。
 *
 * 两者都按窗口标 FC chain，经 ftl_op_submit_batch 原子入队；条件不满足一律回退
 * 普通读 / wbuf 路径。
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
static bool cache_batch_budget_ok(const ftl_ctx_t *ftl, DWORD n)
{
    ASSERT(ftl != NULL);

    return ftl_fc_host_budget_ok_n(n) && (((DWORD)pend_slot_get_count(&ftl->pending) + n) <= FTL_PENDING_MAX);
}

/* ── cache read ── */

// cache read 一次调用的公共参数；避免内部 helper 逐个传 6 个实参。
typedef struct
{
    ftl_ctx_t *ftl;
    WORD       gi;
    DWORD      lba;
    void      *buffer;
    DWORD      max_pairs; /* 本窗口上限（min(remain, run 剩余, 资源)） */
    DWORD      run_limit; /* 本命令/run 的结束（exclusive） */
    DWORD     *used;
} cache_read_req_t;

// 构建中的命令/pending 集合；只在 cache_read_submit 内使用。
typedef struct
{
    fc_command_t *cmds[FTL_CACHE_READ_MAX_PAIRS];
    pend_req_t   *pends[FTL_CACHE_READ_MAX_PAIRS];
    DWORD         n_pairs;
} cache_read_build_t;

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
static bool cache_read_probe_pair(const ftl_ctx_t *ftl, DWORD base, DWORD pair_pba[PLANE_NUM], bool *defer)
{
    for (BYTE p = 0U; p < PLANE_NUM; p++)
    {
        pair_pba[p] = NAND_PBA_INVALID;
    }

    for (BYTE k = 0U; k < FTL_LBA_PER_PAIR; k++)
    {
        const DWORD l = base + k;

        if (wb_has(ftl->wbuf, l))
        {
            return false; /* 写缓冲未落盘：走普通读的 wbuf 快路径 */
        }

        DWORD              p  = NAND_PBA_INVALID;
        const ftl_status_e st = l2p_read(ftl->cache, l, &p);

        if (st == FTL_AGAIN)
        {
            *defer = true;
            return false;
        }

        const bool pba_ok = (st == FTL_OK) && (p != NAND_PBA_INVALID) && pba_is_valid(p);
        if (pba_ok == false)
        {
            return false;
        }

        const BYTE pl         = pba_to_plane(p);
        const bool same_plane = (pl < PLANE_NUM) && (pl == ftl_lba_plane(l));
        const bool same_topo  = (pba_to_ch(p) == ftl->topo.ch) && (pba_to_ce(p) == ftl->topo.ce) && (pba_to_lun(p) == ftl->topo.lun);

        if ((same_plane == false) || (same_topo == false))
        {
            return false; /* 映射表损坏/跨实例：交给普通读路径报错 */
        }

        if (pair_pba[pl] != NAND_PBA_INVALID)
        {
            const bool same_page  = pba_same_page(pair_pba[pl], p);
            const bool same_block = (pba_to_block(pair_pba[pl]) == pba_to_block(p));

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
static bool cache_read_pair_resident(const ftl_ctx_t *ftl, DWORD base)
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
static DWORD cache_read_collect(const cache_read_req_t *req, DWORD pba[FTL_CACHE_READ_MAX_PAIRS][PLANE_NUM], bool *defer, bool *run_ended)
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
        bool complete = true;
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

        // 当前 pair 必须紧接上一 pair 的物理页；否则只提交已收集的前缀。
        // 同 block 是本仓的保守选择（不跨块），跨块直接回退普通读——不是器件强制。
        if (j > 0U)
        {
            bool contiguous = true;
            for (BYTE p = 0U; contiguous && (p < PLANE_NUM); p++)
            {
                contiguous = (pba_to_page(pair_pba[p]) == (pba_to_page(pba[j - 1U][p]) + 1U)) && (pba_to_block(pair_pba[p]) == pba_to_block(pba[j - 1U][p]));
            }
            if (contiguous == false)
            {
                *run_ended = true;
                break;
            }
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

        if (next_base >= req->run_limit)
        {
            *run_ended = true; /* 命令 total 到头 */
        }
        else if (cache_read_pair_resident(req->ftl, next_base) == false)
        {
            *run_ended = true; /* 8 条 L2P 未全驻留：下一个序列预加载后再来 */
        }
        else
        {
            DWORD look_pba[PLANE_NUM];
            bool  look_defer = false;
            bool  contiguous = true;

            if (cache_read_probe_pair(req->ftl, next_base, look_pba, &look_defer) == false)
            {
                *run_ended = true; /* 下一页不可 cache 读（未映射/物理/拓扑/wbuf） */
            }
            else
            {
                for (BYTE p = 0U; p < PLANE_NUM; p++)
                {
                    if ((look_pba[p] == NAND_PBA_INVALID) || (pba_to_page(look_pba[p]) != pba_to_page(look_pba[0])) ||
                        (pba_to_page(look_pba[p]) != (pba_to_page(pba[n_pairs - 1U][p]) + 1U)) || (pba_to_block(look_pba[p]) != pba_to_block(pba[n_pairs - 1U][p])))
                    {
                        contiguous = false;
                        break;
                    }
                }
                *run_ended = (contiguous == false);
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
static bool cache_read_build(cache_read_build_t *b, const cache_read_req_t *req, DWORD pba[FTL_CACHE_READ_MAX_PAIRS][PLANE_NUM], bool start_seq, bool end_seq)
{
    for (DWORD i = 0U; i < b->n_pairs; i++)
    {
        b->cmds[i] = fc_command_alloc();

        if (b->cmds[i] == NULL)
        {
            goto build_fail;
        }

        fc_command_t *cmd = b->cmds[i];

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
        cmd->config.cw          = CW_NUM - 1U;
        cmd->config.crc         = FC_CRC_ENABLE;
        cmd->config.random      = FC_RANDOM_ENABLE;
        cmd->config.ldpc        = FC_ECC_MODE_BCH;
        // 整条 run 是一条 FC chain：只有末条 END。FC 据此在跨填充 pass 续批时
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
        b->pends[i] = pend_alloc(&req->ftl->pending);

        if (b->pends[i] == NULL)
        {
            goto build_fail;
        }

        pend_req_t *pending = b->pends[i];

        pending->command_id = ftl_cmd_id(b->cmds[i]);
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
            pend_remove(&req->ftl->pending, b->pends[i]->command_id);
        }
        if (b->cmds[i] != NULL)
        {
            (void)fc_command_release(b->cmds[i]);
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
static ftl_cache_read_result_e cache_read_submit(const cache_read_req_t *req, DWORD n_pairs, DWORD pba[FTL_CACHE_READ_MAX_PAIRS][PLANE_NUM], bool start_seq, bool end_seq)
{
    cache_read_build_t b;

    memset(&b, 0, sizeof(b));
    b.n_pairs = n_pairs;

    if (cache_read_build(&b, req, pba, start_seq, end_seq) == false)
    {
        req->ftl->cache_read_fallbacks++;
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
    const bool owner_prev = req->ftl->cache_seq.owner_submit;

    req->ftl->cache_seq.owner_submit = true;
    const ftl_status_e submit_status = ftl_op_submit_batch(b.cmds, b.n_pairs, FTL_OP_HOST_IO, NULL);
    req->ftl->cache_seq.owner_submit = owner_prev;

    if (submit_status != FTL_OK)
    {
        // 整批未入队：命令、pending、组子操作计数全部回滚。
        for (DWORD i = 0U; i < b.n_pairs; i++)
        {
            pend_remove(&req->ftl->pending, ftl_cmd_id(b.cmds[i]));
            (void)fc_command_release(b.cmds[i]);
            pend_group_drop_subop(&req->ftl->pending, req->gi);
        }

        req->ftl->cache_read_fallbacks++;

        // 瞬态背压可重试；链契约违反等编程错误是永久失败（H38）。
        return (submit_status == FTL_AGAIN) ? FTL_CACHE_READ_AGAIN : FTL_CACHE_READ_ERROR;
    }

    *req->used = b.n_pairs * FTL_LBA_PER_PAIR;
    req->ftl->cache_read_batches++;
    req->ftl->cache_read_pairs += b.n_pairs;
    return FTL_CACHE_READ_OK;
}

ftl_cache_read_result_e ftl_cache_read_try(ftl_ctx_t *ftl, WORD gi, DWORD lba, void *buffer, DWORD remain, DWORD *used)
{
    ASSERT(ftl != NULL);
    ASSERT(used != NULL);

    if ((lba & (FTL_LBA_PER_PAIR - 1U)) != 0U)
    {
        return FTL_CACHE_READ_DECLINED;
    }

    // 命令上下文按**组**保存：同一 SCSI 命令的后续 chunk 是不同组，各自的
    // cmd_start/cmd_total 来自 CBW；序列的持有权由 owner_gi 区分。
    if ((gi >= FTL_GROUP_MAX) || (ftl->pending.groups[gi].used == false))
    {
        return FTL_CACHE_READ_DECLINED;
    }

    const pend_group_t *g         = &ftl->pending.groups[gi];
    const DWORD         cmd_start = g->cmd_start;
    const DWORD         cmd_total = g->cmd_total;

    ftl_cache_seq_t *seq = &ftl->cache_seq;

    // 序列只跨**同一 SCSI 命令**的 chunk。上下文不同 / 续接点不一致（乱序、空洞）
    // ⇒ 这条序列已不可能由本组续；若仍 active 必须先释放状态，否则普通读回退会被
    // 门控永久挡回（owner 槽位可能已被复用，idle 守卫也等不到）。
    if (seq->active == true)
    {
        if (seq->kind != FTL_CACHE_SEQ_READ)
        {
            return FTL_CACHE_READ_DECLINED; /* 写序列由写路径收尾 */
        }

        const bool same_cmd = (seq->cmd_start == cmd_start) && (seq->cmd_total == cmd_total);

        if ((same_cmd == false) || (seq->next_lba != lba))
        {
            ftl_cache_seq_release(ftl);
            return FTL_CACHE_READ_DECLINED;
        }
    }

    if ((cmd_total == 0U) || (lba < cmd_start) || (lba >= (cmd_start + cmd_total)))
    {
        return FTL_CACHE_READ_DECLINED; /* 没有命令上下文：回退普通读 */
    }

    const DWORD run_limit = cmd_start + cmd_total;

    // 新序列至少 2 个 pair；**续序列允许 1 个 pair 的尾巴**（单条 LAST 收尾）。
    const bool  starting   = (seq->active == false);
    const DWORD min_pairs  = (starting == true) ? FTL_CACHE_READ_MIN_PAIRS : 1U;
    const DWORD run_remain = run_limit - lba;

    if ((remain < (min_pairs * FTL_LBA_PER_PAIR)) || (run_remain < (min_pairs * FTL_LBA_PER_PAIR)))
    {
        if (seq->active == true)
        {
            ftl_cache_seq_release(ftl); /* 本组没有可续的完整 pair：不能留着序列挡门控 */
        }
        return FTL_CACHE_READ_DECLINED;
    }

    // 新序列：预加载 L2P region（region = 512 条 entry；最多探 512 LBA）。
    if (starting == true)
    {
        // run 的物理跨度 ≤ 一个 block = 512 LBA（64 页 × 8 LBA）⇒ 最多跨 2 个 L2P
        // region。cache 行整 region 一条（512 entry），2 行不会互相驱逐。
        DWORD probe_end = lba + 512U;

        if (probe_end > run_limit)
        {
            probe_end = run_limit;
        }

        if ((probe_end - lba) >= (FTL_CACHE_READ_MIN_PAIRS * FTL_LBA_PER_PAIR))
        {
            const DWORD last_base = (probe_end - 1U) & ~(FTL_LBA_PER_PAIR - 1U);
            DWORD       p0        = NAND_PBA_INVALID;
            DWORD       p1        = NAND_PBA_INVALID;

            const ftl_status_e s0 = l2p_read(ftl->cache, lba, &p0);
            const ftl_status_e s1 = l2p_read(ftl->cache, last_base, &p1);

            if ((s0 == FTL_AGAIN) || (s1 == FTL_AGAIN))
            {
                ftl->l2p_defer = true; // 有确定完成的后台进展，drain 不计 stall
                ftl->cache_read_declines++;
                return FTL_CACHE_READ_AGAIN;
            }
        }
    }

    DWORD max_pairs = remain / FTL_LBA_PER_PAIR;

    if (max_pairs > FTL_CACHE_READ_MAX_PAIRS)
    {
        max_pairs = FTL_CACHE_READ_MAX_PAIRS;
    }
    if (max_pairs > (run_remain / FTL_LBA_PER_PAIR))
    {
        max_pairs = run_remain / FTL_LBA_PER_PAIR;
    }

    if (max_pairs < min_pairs)
    {
        return FTL_CACHE_READ_DECLINED;
    }

    cache_read_req_t req;

    req.ftl       = ftl;
    req.gi        = gi;
    req.lba       = lba;
    req.buffer    = buffer;
    req.max_pairs = max_pairs;
    req.run_limit = run_limit;
    req.used      = used;

    DWORD pba[FTL_CACHE_READ_MAX_PAIRS][PLANE_NUM];
    bool  defer     = false;
    bool  run_ended = false;
    DWORD collected = cache_read_collect(&req, pba, &defer, &run_ended);

    if (collected < min_pairs)
    {
        ftl->cache_read_declines++;
        if (defer == true)
        {
            ftl->l2p_defer = true;
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
    const bool  at_run_limit = (after >= run_limit);
    // 本组剩余不足一个 pair（命令尾 1..7 LBA）：其余 LBA 只能走普通读，
    // 必须在这里 LAST 收尾，否则普通读会被序列门控永久挡回。
    const DWORD remain_after = remain - (collected * FTL_LBA_PER_PAIR);
    const DWORD tail_lbas    = run_limit - after;
    bool        end_seq      = ((run_ended == true) || (at_run_limit == true) || (seq->break_requested == true) ||
                    ((remain_after > 0U) && (remain_after < FTL_LBA_PER_PAIR)) ||
                    ((tail_lbas > 0U) && (tail_lbas < FTL_LBA_PER_PAIR)));

    DWORD n = collected;

    while ((n > min_pairs) && (cache_batch_budget_ok(ftl, n) == false))
    {
        n--;
    }

    if (cache_batch_budget_ok(ftl, n) == false)
    {
        ftl->cache_read_fallbacks++;
        // 序列打开时不能回退普通读（会劈开设备序列）⇒ 重试等资源。
        return (seq->active == true) ? FTL_CACHE_READ_AGAIN : FTL_CACHE_READ_DECLINED;
    }

    if (n < collected)
    {
        end_seq = false; // 只提交前缀：run 未到终点
    }

    const bool                    start_seq = (starting == true);
    const ftl_cache_read_result_e r         = cache_read_submit(&req, n, pba, start_seq, end_seq);

    if (r == FTL_CACHE_READ_OK)
    {
        if ((end_seq == true) && (seq->break_requested == true))
        {
            ftl->cache_seq_breaks++; /* D4：后台/其他提交触发的断点 */
        }

        seq->kind     = FTL_CACHE_SEQ_READ;
        seq->owner_gi = gi;
        seq->next_lba = lba + (n * FTL_LBA_PER_PAIR);
        seq->idle_ms  = 0U;

        if (starting == true)
        {
            seq->cmd_start = cmd_start;
            seq->cmd_total = cmd_total;
        }

        if (end_seq == true)
        {
            seq->active          = false;
            seq->draining        = true; /* 命令还在飞：门控保持关闭直到排空 */
            seq->idle_ms         = 0U;
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
typedef struct
{
    fc_command_t *cmds[FTL_CACHE_WRITE_MAX_PAGES];
    DWORD         pba[FTL_CACHE_WRITE_MAX_PAGES][PLANE_NUM];
    BYTE          pend_idx[FTL_CACHE_WRITE_MAX_PAGES];
    BYTE          n_cmd;
    BYTE          n_alloc;
    BYTE          n_pend;
} cache_wr_build_t;

/**
 * @brief 撤销尚未提交的写窗口：释放命令/pending + 归还已分配的页对。
 *
 * @param ftl 实例。
 * @param b   构建上下文（按 n_cmd / n_pend / n_alloc 逐项回收）。
 */
static void cache_wr_discard(ftl_ctx_t *ftl, cache_wr_build_t *b)
{
    for (BYTE i = 0U; i < b->n_cmd; i++)
    {
        if (b->cmds[i] != NULL)
        {
            (void)fc_command_release(b->cmds[i]);
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
 * @param start_seq true = 本窗口是序列首窗口（首条 op49）。
 * @param end_seq   true = 本窗口是序列末窗口（末条 op48）。
 * @param b        出参：本窗口的页对 / 命令 / pending。
 *
 * @return FTL_OK；FTL_AGAIN（页对 / pending / 资源不足，已回滚）；其他建链错误。
 */
static ftl_status_e cache_wr_build(ftl_ctx_t *ftl, WORD gi, DWORD lba, const void *buffer, BYTE n_pages, bool start_seq, bool end_seq, cache_wr_build_t *b)
{
    for (BYTE j = 0U; j < n_pages; j++)
    {
        if (alc_super_open_alloc_pair(&ftl->alloc, ALC_SUPER_ALLOC_HOT, b->pba[j]) != FTL_OK)
        {
            mig_gc_request_urgent(ftl->mig_handle);
            cache_wr_discard(ftl, b);
            return FTL_AGAIN;
        }
        b->n_alloc++;

        const DWORD           page_lba = lba + ((DWORD)j * FTL_LBA_PER_PAIR);
        const BYTE           *src      = (const BYTE *)buffer + ((DWORD)j * FTL_LBA_PER_PAIR * FTL_IU_BYTES);
        const ftl_page_cfg_t *cfg      = FTL_PAGE_CFG_DATA(ftl_lba_page(page_lba));

        BYTE              op;
        pend_cache_role_e role;

        if ((start_seq == true) && (j == 0U))
        {
            op   = OP_TWO_PLANE_PROGRAM_CACHE_FIRST;
            role = PEND_CACHE_ROLE_FIRST;
        }
        else if ((j + 1U == n_pages) && (end_seq == true))
        {
            op   = OP_TWO_PLANE_PROGRAM_CACHE_END;
            role = PEND_CACHE_ROLE_END;
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

        pend_req_t *pending = pend_alloc(&ftl->pending);

        if (pending == NULL)
        {
            cache_wr_discard(ftl, b);
            return FTL_AGAIN;
        }

        pending->command_id  = ftl_cmd_id(b->cmds[j]);
        pending->owners      = ftl_group_bit(gi);
        pending->op          = PENDING_OP_WRITE_CACHE;
        pending->n_cw        = FTL_LBA_PER_PAIR;
        pending->cw_bitmap   = (BYTE)((1U << FTL_LBA_PER_PAIR) - 1U);
        pending->lba         = page_lba;
        pending->buffer      = NULL;
        pending->new_pba     = b->pba[j][0];
        pending->new_pba1    = b->pba[j][1];
        pending->cache_role  = role;
        pending->retry_count = 0U;
        pend_validate(pending);

        b->pend_idx[j] = (BYTE)(pending - ftl->pending.slots);
        b->n_pend++;
    }

    return FTL_OK;
}

ftl_cache_write_result_e ftl_cache_write_try(ftl_ctx_t *ftl, WORD gi, DWORD lba, void *buffer, DWORD remain, DWORD *used)
{
    ASSERT(ftl != NULL);
    ASSERT(used != NULL);

    if ((lba & (FTL_LBA_PER_PAIR - 1U)) != 0U)
    {
        ftl->cache_wb_declines++;
        return FTL_CACHE_WRITE_DECLINED;
    }

    if ((gi >= FTL_GROUP_MAX) || (ftl->pending.groups[gi].used == false))
    {
        return FTL_CACHE_WRITE_DECLINED;
    }

    const pend_group_t *g         = &ftl->pending.groups[gi];
    const DWORD         cmd_start = g->cmd_start;
    const DWORD         cmd_total = g->cmd_total;

    if ((cmd_total == 0U) || (lba < cmd_start) || (lba >= (cmd_start + cmd_total)))
    {
        return FTL_CACHE_WRITE_DECLINED; /* 没有命令上下文：回退 wbuf */
    }

    const DWORD      cmd_end = cmd_start + cmd_total;
    ftl_cache_seq_t *seq     = &ftl->cache_seq;

    if (seq->active == true)
    {
        if (seq->kind != FTL_CACHE_SEQ_WRITE)
        {
            /* 读序列占用设备管线：写组等它收尾（门控会挡住普通写），不能回退。 */
            return FTL_CACHE_WRITE_AGAIN;
        }
        if ((seq->cmd_start != cmd_start) || (seq->cmd_total != cmd_total) || (seq->next_lba != lba))
        {
            // 别的命令 / 续接点不一致：本序列不可能再续。未确认页按错误结算，
            // 否则组永远等不到确认、控制器也不会有下一 chunk。
            ftl_cache_wr_abort(ftl);
            return FTL_CACHE_WRITE_DECLINED;
        }
    }

    const bool starting      = (seq->active == false);
    // 新序列 ≥4 页（16 KiB）；续序列允许 1 页尾巴。
    const DWORD min_pages    = (starting == true) ? FTL_CACHE_WRITE_MAX_PAGES : 1U;
    const DWORD remain_pages = remain / FTL_LBA_PER_PAIR;

    if ((remain_pages < min_pages) || ((cmd_end - lba) < (min_pages * FTL_LBA_PER_PAIR)))
    {
        if (seq->active == true)
        {
            /* 本组没有可续的完整页：不能留着写序列挡住门控（正常路径会被挡回）。 */
            ftl_cache_seq_release(ftl);
        }
        ftl->cache_wb_declines++;
        return FTL_CACHE_WRITE_DECLINED;
    }

    if ((starting == true) && (wb_is_idle(ftl->wbuf) == false))
    {
        ftl->cache_wb_declines++;
        return FTL_CACHE_WRITE_DECLINED;
    }

    const DWORD room = alc_super_open_get_room(&ftl->alloc, FTL_TEMP_HOT);

    if (room == 0U)
    {
        if (seq->active == true)
        {
            /* 当前 block 已被关闭/写满：序列无法续。中止它（未确认页按错误结算），
             * 否则后组只能走普通写而被门控永久挡回。 */
            ftl_cache_wr_abort(ftl);
        }
        ftl->cache_wb_declines++;
        return FTL_CACHE_WRITE_DECLINED;
    }

    if ((starting == true) && (room < FTL_CACHE_WRITE_MAX_PAGES))
    {
        ftl->cache_wb_declines++;
        return FTL_CACHE_WRITE_DECLINED;
    }

    // 新序列：先预加载本窗口覆盖的 L2P region（序列打开后加载读会被门控挡住，
    // 而确认映射需要该 region 就绪）。最多探首/尾两点，覆盖 4 页窗口的跨 region 情况。
    if (starting == true)
    {
        const DWORD probe_last = ((cmd_end - lba) > (FTL_CACHE_WRITE_MAX_PAGES * FTL_LBA_PER_PAIR)) ? (lba + (FTL_CACHE_WRITE_MAX_PAGES * FTL_LBA_PER_PAIR) - 1U) : (cmd_end - 1U);
        DWORD       p0         = NAND_PBA_INVALID;
        DWORD       p1         = NAND_PBA_INVALID;

        const ftl_status_e s0 = l2p_read(ftl->cache, lba, &p0);
        const ftl_status_e s1 = l2p_read(ftl->cache, probe_last, &p1);

        if ((s0 == FTL_AGAIN) || (s1 == FTL_AGAIN))
        {
            ftl->l2p_defer = true; // 有确定完成的后台进展：drain 不计 stall
            ftl->cache_wb_declines++;
            return FTL_CACHE_WRITE_AGAIN;
        }
    }

    // 窗口 = min(组剩余, 命令剩余, block 剩余, 窗口上限, 确认 FIFO 余量)。
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

    const BYTE ring_free = (BYTE)(FTL_CACHE_SEQ_RING - seq->wr.count);

    if (n_pages > ring_free)
    {
        n_pages = ring_free;
    }

    if (n_pages < min_pages)
    {
        if (starting == false)
        {
            ftl->cache_wb_declines++;
            return FTL_CACHE_WRITE_AGAIN; /* 确认 FIFO 满：等完成泵结算 */
        }
        ftl->cache_wb_declines++;
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

    // 断点：命令到头 / 当前 block 写到头 / D4 / **命令尾不足一个 pair**（尾 1..7 LBA
    // 只能走普通写，必须在本窗口 END，否则它们会被门控挡回且序列永不收尾）。
    // **chunk 边界不是断点**——下一 chunk 的组以相同命令上下文接手同一序列
    // （跨 chunk 流式的全部意义）。
    const DWORD after     = lba + (n_pages * FTL_LBA_PER_PAIR);
    const bool  block_end = (after >= (lba + (room * FTL_LBA_PER_PAIR)));
    const DWORD tail_lbas = cmd_end - after;
    bool        end_seq   = ((after >= cmd_end) || (block_end == true) || (seq->break_requested == true) ||
                    ((tail_lbas > 0U) && (tail_lbas < FTL_LBA_PER_PAIR)));

    // Journal 容量：本窗口页的条目先预留。失败意味着确认会停泊，而 journal 刷页在
    // 序列打开时被门控挡住 ⇒ 本窗口必须 END：序列在这里收尾，刷页随后能跑，停泊的
    // 确认才能推进。（只按窗口预留：一页 journal 装不下整个确认 FIFO。）
    _Static_assert((FTL_CACHE_WRITE_MAX_PAGES * FTL_LBA_PER_PAIR) < JRN_ENTRIES_PER_PAGE, "journal reserve window must fit one page");

    if (jrn_reserve(ftl->journal, (WORD)(n_pages * FTL_LBA_PER_PAIR)) == false)
    {
        end_seq = true;
    }

    // 断点 lookahead：下一窗口覆盖的 L2P region 不驻留 ⇒ 本窗口 END，让下一窗口以
    // 新序列预加载（序列打开时不能起加载读）。与读路径的 lookahead 同源。
    if (end_seq == false)
    {
        const DWORD next_last = ((cmd_end - after) > (FTL_CACHE_WRITE_MAX_PAGES * FTL_LBA_PER_PAIR)) ? (after + (FTL_CACHE_WRITE_MAX_PAGES * FTL_LBA_PER_PAIR) - 1U) : (cmd_end - 1U);

        if ((l2p_is_resident(ftl->cache, after) == false) || (l2p_is_resident(ftl->cache, next_last) == false))
        {
            end_seq = true;
        }
    }

    cache_wr_build_t b;

    memset(&b, 0, sizeof(b));

    ftl_status_e st = cache_wr_build(ftl, gi, lba, buffer, (BYTE)n_pages, starting, end_seq, &b);

    if (st == FTL_OK)
    {
        // 记账必须早于 submit：完成可能在本轮 drain 之后立刻被泵处理。
        for (BYTE j = 0U; j < b.n_pend; j++)
        {
            pend_group_add_subop(&ftl->pending, gi);
        }

        // 序列所有者通道：门控对本批旁路（其他提交会被延迟并置 break_requested）。
        const bool owner_prev = ftl->cache_seq.owner_submit;

        ftl->cache_seq.owner_submit = true;
        st                          = ftl_op_submit_batch(b.cmds, b.n_cmd, FTL_OP_HOST_IO, NULL);
        ftl->cache_seq.owner_submit = owner_prev;

        if (st != FTL_OK)
        {
            for (BYTE j = 0U; j < b.n_pend; j++)
            {
                pend_release_index(&ftl->pending, b.pend_idx[j]);
                pend_group_drop_subop(&ftl->pending, gi);
            }
            for (BYTE j = 0U; j < b.n_cmd; j++)
            {
                (void)fc_command_release(b.cmds[j]);
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

    // 提交成功：按提交顺序把每页挂到未确认 FIFO（下一条命令的 FAILC 才能结算它）。
    for (BYTE j = 0U; j < b.n_pend; j++)
    {
        if (ftl_cache_wr_ring_push(ftl, b.pend_idx[j]) == false)
        {
            ASSERT(false); /* 窗口大小已按 ring_free 收紧，不应溢出 */
            break;
        }
    }

    if ((end_seq == true) && (seq->break_requested == true))
    {
        ftl->cache_seq_breaks++; /* D4：后台/其他提交触发的断点 */
    }

    seq->kind     = FTL_CACHE_SEQ_WRITE;
    seq->owner_gi = gi;
    seq->next_lba = lba + (n_pages * FTL_LBA_PER_PAIR);
    seq->idle_ms  = 0U;

    if (starting == true)
    {
        seq->cmd_start = cmd_start;
        seq->cmd_total = cmd_total;
    }

    if (end_seq == true)
    {
        seq->active          = false;
        seq->break_requested = false;
    }
    else
    {
        seq->active = true;
    }

    ftl->cache_wb_batches++;
    ftl->cache_wb_pages += n_pages;
    *used = n_pages * FTL_LBA_PER_PAIR;
    return FTL_CACHE_WRITE_OK;
}
