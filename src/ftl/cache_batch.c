/*
 * cache_batch.c — FTL cache read / cache program 批次（本仓自定）
 *
 * 两个方向共用同一模块，但各自保留独立的条件、命令序列与结果枚举：
 *   · cache read：连续 pair 页映射驻留且物理连续时，2~4 个 pair 用
 *     START_SEQ_MP/SEQ_MP/LAST_MP 一次提交；
 *   · cache program：host 16 KiB 写 = 4 个 pair 页，按 op49/op44×2/op48 提交。
 *
 * 两者都不依赖 FC chain；条件不满足一律回退普通读 / wbuf 路径。
 */

#include "cache_batch.h"

#include "assertion.h"
#include "fc.h"
#include "ftl.h"
#include "ftl_fc_interface.h"
#include "ftl_op.h"
#include "l2p_cache.h"
#include "migrate.h"
#include "pending.h"
#include "wbuf.h"

#include <string.h>

/* ── cache read ── */

// cache read 一次调用的公共参数；避免内部 helper 逐个传 6 个实参。
typedef struct
{
    ftl_ctx_t *ftl;
    WORD       gi;
    DWORD      lba;
    void      *buffer;
    DWORD      remain;
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
 * @brief 收集可连续 cache 读的 pair。
 *
 * @param req   本次 cache read 的公共参数。
 * @param pba   出参：已收集的 pair 物理地址（每 pair 每 plane）。
 * @param defer 出参：见 cache_read_probe_pair。
 *
 * @return 收集到的完整且连续 pair 数（0..FTL_CACHE_READ_MAX_PAIRS）。
 */
static DWORD cache_read_collect(const cache_read_req_t *req, DWORD pba[FTL_CACHE_READ_MAX_PAIRS][PLANE_NUM], bool *defer)
{
    DWORD max_pairs = req->remain / FTL_LBA_PER_PAIR;

    if (max_pairs > FTL_CACHE_READ_MAX_PAIRS)
    {
        max_pairs = FTL_CACHE_READ_MAX_PAIRS;
    }

    DWORD n_pairs = 0U;

    for (DWORD j = 0U; j < max_pairs; j++)
    {
        const DWORD base = req->lba + (j * FTL_LBA_PER_PAIR);
        DWORD       pair_pba[PLANE_NUM];

        if (cache_read_probe_pair(req->ftl, base, pair_pba, defer) == false)
        {
            break;
        }

        // 两 plane 必须都有效且指向同一页；否则不是完整 pair。
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
        // 多平面 cache 命令只要求两 plane 页号一致，不要求块号相同。
        for (BYTE p = 1U; complete && (p < PLANE_NUM); p++)
        {
            complete = (pba_to_page(pair_pba[p]) == pba_to_page(pair_pba[0]));
        }
        if (complete == false)
        {
            break;
        }

        // 当前 pair 必须紧接上一 pair 的物理页；否则只提交已收集的前缀。
        if (j > 0U)
        {
            bool contiguous = true;
            for (BYTE p = 0U; contiguous && (p < PLANE_NUM); p++)
            {
                // 页号 +1（不能对 `pba_page()` 加 1：那会加到 CW 位），且同一块。
                contiguous = (pba_to_page(pair_pba[p]) == (pba_to_page(pba[j - 1U][p]) + 1U)) && (pba_to_block(pair_pba[p]) == pba_to_block(pba[j - 1U][p]));
            }
            if (contiguous == false)
            {
                break;
            }
        }

        for (BYTE p = 0U; p < PLANE_NUM; p++)
        {
            pba[j][p] = pair_pba[p];
        }
        n_pairs++;
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
static bool cache_read_build(cache_read_build_t *b, const cache_read_req_t *req, DWORD pba[FTL_CACHE_READ_MAX_PAIRS][PLANE_NUM])
{
    for (DWORD i = 0U; i < b->n_pairs; i++)
    {
        b->cmds[i] = fc_command_alloc();

        if (b->cmds[i] == NULL)
        {
            goto build_fail;
        }

        fc_command_t *cmd = b->cmds[i];

        if (i == 0U)
        {
            cmd->config.operation = OP_READ_PAGE_CACHE_START_SEQ_MP;
        }
        else if (i == (b->n_pairs - 1U))
        {
            cmd->config.operation = OP_READ_PAGE_CACHE_LAST_MP;
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
        cmd->config.chain       = FC_CHAIN_END;
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

        pending->command_id         = ftl_cmd_id(b->cmds[i]);
        pending->owners             = ftl_group_bit(req->gi);
        pending->op                 = PENDING_OP_READ_CACHE;
        pending->n_cw               = FTL_LBA_PER_PAIR;
        pending->cw_bitmap          = 0U;
        pending->lba                = ftl_lba_pair_base(req->lba + (i * FTL_LBA_PER_PAIR));
        pending->buffer             = (BYTE *)req->buffer + (i * FTL_LBA_PER_PAIR * FTL_IU_BYTES);
        pending->new_pba            = NAND_PBA_INVALID;
        pending->new_pba1           = NAND_PBA_INVALID;
        pending->cache_intermediate = false;
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
 * @param n_pairs 已收集的 pair 数（≥ FTL_CACHE_READ_MIN_PAIRS）。
 * @param pba     每个 pair 每 plane 的物理地址。
 *
 * @return FTL_CACHE_READ_OK 全部提交；FTL_CACHE_READ_AGAIN 建链失败或首批提交失败
 *         （整批回滚或可安全重试）；FTL_CACHE_READ_ERROR 部分提交后失败
 *         （已提交成员保留，组报 IO_ERROR）。
 */
static ftl_cache_read_result_e cache_read_submit(const cache_read_req_t *req, DWORD n_pairs, DWORD pba[FTL_CACHE_READ_MAX_PAIRS][PLANE_NUM])
{
    cache_read_build_t b;

    memset(&b, 0, sizeof(b));
    b.n_pairs = n_pairs;

    if (cache_read_build(&b, req, pba) == false)
    {
        req->ftl->cache_read_fallbacks++;
        return FTL_CACHE_READ_AGAIN;
    }

    // 记账必须早于 submit：完成可能在本轮 drain 之后立刻被泵处理。
    for (DWORD i = 0U; i < b.n_pairs; i++)
    {
        pend_group_add_subop(&req->ftl->pending, req->gi);
    }

    DWORD submitted = 0U;

    for (DWORD i = 0U; i < b.n_pairs; i++)
    {
        if (ftl_op_submit(b.cmds[i], FTL_OP_HOST_IO, NULL) != FTL_OK)
        {
            // 已提交的成员保留并等待完成；未提交的成员回滚。
            for (DWORD k = i; k < b.n_pairs; k++)
            {
                pend_remove(&req->ftl->pending, ftl_cmd_id(b.cmds[k]));
                (void)fc_command_release(b.cmds[k]);
                pend_group_drop_subop(&req->ftl->pending, req->gi);
            }

            req->ftl->cache_read_fallbacks++;

            return (submitted == 0U) ? FTL_CACHE_READ_AGAIN : FTL_CACHE_READ_ERROR;
        }

        submitted++;
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

    if ((remain < (FTL_CACHE_READ_MIN_PAIRS * FTL_LBA_PER_PAIR)) || ((lba & (FTL_LBA_PER_PAIR - 1U)) != 0U))
    {
        return FTL_CACHE_READ_DECLINED;
    }

    cache_read_req_t req;

    req.ftl    = ftl;
    req.gi     = gi;
    req.lba    = lba;
    req.buffer = buffer;
    req.remain = remain;
    req.used   = used;

    DWORD pba[FTL_CACHE_READ_MAX_PAIRS][PLANE_NUM];
    bool  defer   = false;
    DWORD n_pairs = cache_read_collect(&req, pba, &defer);

    if (n_pairs < FTL_CACHE_READ_MIN_PAIRS)
    {
        ftl->cache_read_declines++;
        return defer ? FTL_CACHE_READ_AGAIN : FTL_CACHE_READ_DECLINED;
    }

    if (ftl_cache_budget_ok(ftl, n_pairs) == false)
    {
        ftl->cache_read_fallbacks++;
        return FTL_CACHE_READ_DECLINED;
    }

    return cache_read_submit(&req, n_pairs, pba);
}
/* ── cache program ── */

typedef struct
{
    DWORD         pba[FTL_CACHE_WRITE_MAX_PAGES][PLANE_NUM];
    fc_command_t *cmds[FTL_CACHE_WRITE_MAX_PAGES];
    BYTE          n_alloc;
    BYTE          n_cmd;
    DWORD         n_pending;
} cache_wb_build_t;

/**
 * @brief 撤销尚未提交的构建：释放命令/pending + 归还页对。调用后 batch 缓冲/组未动。
 *
 * @param ftl 实例。
 * @param b   构建上下文（按 n_cmd / n_pending / n_alloc 逐项回收）。
 */
static void cache_wb_discard(ftl_ctx_t *ftl, cache_wb_build_t *b)
{
    for (BYTE i = 0U; i < b->n_cmd; i++)
    {
        if (b->cmds[i] != NULL)
        {
            (void)fc_command_release(b->cmds[i]);
        }
    }

    for (DWORD i = 0U; i < b->n_pending; i++)
    {
        pend_release_index(&ftl->pending, ftl->cache_wb.pending_idx[i]);
    }

    for (BYTE i = 0U; i < b->n_alloc; i++)
    {
        alc_super_open_rewind_pair(&ftl->alloc, FTL_TEMP_HOT);
    }

    ftl->cache_wb.active = false;
}

/**
 * @brief 分配 4 个页对、建 4 条命令、登记 4 个 pending（尚未提交）。
 *
 * @param ftl    实例。
 * @param gi     本子操作归属的组下标。
 * @param lba    本批起始 LBA（FTL_LBA_PER_PAIR 对齐）。
 * @param buffer 16 KiB 源数据（4 个 pair 页）。
 * @param b      出参：本批的页对 PBA / 命令 / pending 记录。
 *
 * @return FTL_OK 全部建好；FTL_AGAIN 页对或 pending 不足（已回滚）；
 *         其他 = 建链失败的状态码（已回滚）。
 */
static ftl_status_e cache_wb_build(ftl_ctx_t *ftl, WORD gi, DWORD lba, const void *buffer, cache_wb_build_t *b)
{
    for (BYTE j = 0U; j < FTL_CACHE_WRITE_MAX_PAGES; j++)
    {
        if (alc_super_open_alloc_pair(&ftl->alloc, ALC_SUPER_ALLOC_HOT, b->pba[j]) != FTL_OK)
        {
            mig_gc_request_urgent(ftl->mig_handle);
            cache_wb_discard(ftl, b);
            return FTL_AGAIN;
        }
        b->n_alloc++;

        const DWORD           page_lba = lba + ((DWORD)j * FTL_LBA_PER_PAIR);
        const BYTE           *src      = (const BYTE *)buffer + ((DWORD)j * FTL_LBA_PER_PAIR * FTL_IU_BYTES);
        const ftl_page_cfg_t *cfg      = FTL_PAGE_CFG_DATA(ftl_lba_page(page_lba));

        const ftl_status_e st = ftl_async_write_pair_cmd(&b->cmds[j], b->pba[j], src, FTL_CACHE_FULL_CW, ftl_cache_prog_op(j, FTL_CACHE_WRITE_MAX_PAGES), cfg);

        if (st != FTL_OK)
        {
            cache_wb_discard(ftl, b);
            return st;
        }
        b->n_cmd++;

        pend_req_t *pending = pend_alloc(&ftl->pending);

        if (pending == NULL)
        {
            cache_wb_discard(ftl, b);
            return FTL_AGAIN;
        }

        pending->command_id         = ftl_cmd_id(b->cmds[j]);
        pending->owners             = ftl_group_bit(gi);
        pending->op                 = PENDING_OP_WRITE_CACHE;
        pending->n_cw               = FTL_LBA_PER_PAIR;
        pending->cw_bitmap          = (BYTE)((1U << FTL_LBA_PER_PAIR) - 1U);
        pending->lba                = page_lba;
        pending->buffer             = NULL;
        pending->new_pba            = b->pba[j][0];
        pending->new_pba1           = b->pba[j][1];
        pending->cache_intermediate = (j + 1U < FTL_CACHE_WRITE_MAX_PAGES);
        pend_validate(pending);

        ftl->cache_wb.pending_idx[j] = (BYTE)(pending - ftl->pending.slots);
        b->n_pending++;
    }

    return FTL_OK;
}

ftl_cache_write_result_e ftl_cache_write_try(ftl_ctx_t *ftl, WORD gi, DWORD lba, void *buffer, DWORD remain, DWORD *used)
{
    ASSERT(ftl != NULL);
    ASSERT(used != NULL);

    // 条件不满足 = 正常回退，不是错误。
    // alc_super_open_get_room>=4 已蕴含：SB 开着、所有 plane 的 page0 已成功、剩余 ≥4 页。
    const bool batch_active = ftl->cache_wb.active;
    const bool enough_lbas  = (remain >= FTL_CACHE_WRITE_LBAS);
    const bool lba_aligned  = ((lba & (FTL_LBA_PER_PAIR - 1U)) == 0U);
    const bool wbuf_idle    = wb_is_idle(ftl->wbuf);
    const bool room_ok      = (alc_super_open_get_room(&ftl->alloc, FTL_TEMP_HOT) >= FTL_CACHE_WRITE_MAX_PAGES);
    const bool budget_ok    = ftl_cache_budget_ok(ftl, FTL_CACHE_WRITE_MAX_PAGES);

    if (batch_active || (enough_lbas == false) || (lba_aligned == false) || (wbuf_idle == false) || (room_ok == false) || (budget_ok == false))
    {
        ftl->cache_wb_declines++;
        return FTL_CACHE_WRITE_DECLINED;
    }

    cache_wb_build_t b;

    memset(&b, 0, sizeof(b));

    ftl_status_e st = cache_wb_build(ftl, gi, lba, buffer, &b);

    if (st == FTL_OK)
    {
        st = ftl_op_submit_batch(b.cmds, FTL_CACHE_WRITE_MAX_PAGES, FTL_OP_HOST_IO, NULL);
        if (st != FTL_OK)
        {
            cache_wb_discard(ftl, &b);
        }
    }

    if (st != FTL_OK)
    {
        return (st == FTL_AGAIN) ? FTL_CACHE_WRITE_AGAIN : FTL_CACHE_WRITE_ERROR;
    }

    ftl->cache_wb.active      = true;
    ftl->cache_wb.abort       = false;
    ftl->cache_wb.final_done  = false;
    ftl->cache_wb.n           = FTL_CACHE_WRITE_MAX_PAGES;
    ftl->cache_wb.done_mask   = 0U;
    ftl->cache_wb.outstanding = FTL_CACHE_WRITE_MAX_PAGES;
    ftl->cache_wb.lba         = lba;

    for (BYTE j = 0U; j < FTL_CACHE_WRITE_MAX_PAGES; j++)
    {
        pend_group_add_subop(&ftl->pending, gi);
    }

    ftl->cache_wb_batches++;
    ftl->cache_wb_pages += FTL_CACHE_WRITE_MAX_PAGES;
    *used = FTL_CACHE_WRITE_LBAS;
    return FTL_CACHE_WRITE_OK;
}
