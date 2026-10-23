/*
 * cache_read.c — FTL two-plane cache read 批次（本仓自定）
 *
 * 一次 host 读请求内，若连续 pair 页的 L2P 映射已驻留且物理页连续，就把
 * 2~4 个 pair 用 START_SEQ_MP/SEQ_MP/LAST_MP 一次性提交。命令不依赖 FC chain：
 * 4 条命令在同一 SQ 内连续提交，FTL 当前全部命令 high_priority=0，后续命令
 * 只会排在 sequence 之后。
 *
 * 任何条件不满足（未对齐/未驻留/未映射/物理不连续/资源不足）一律回退现有
 * 单 pair 普通读；失败不启动 RR（见 ftl_host_io_callback）。
 */

#include "cache_read.h"

#include "assertion.h"
#include "fc.h"
#include "ftl.h"
#include "ftl_fc_interface.h"
#include "ftl_op.h"
#include "l2p_cache.h"
#include "pending.h"
#include "wbuf.h"

#include <string.h>

/* cache read 一次调用的公共参数；避免内部 helper 逐个传 6 个实参。 */
typedef struct
{
    ftl_ctx_t *ftl;
    WORD       gi;
    DWORD      lba;
    void      *buffer;
    DWORD      remain;
    DWORD     *used;
} cache_read_req_t;

/* 构建中的命令/pending 集合；只在 cache_read_submit 内使用。 */
typedef struct
{
    fc_command_t *cmds[FTL_CACHE_READ_MAX_PAIRS];
    pend_req_t   *pends[FTL_CACHE_READ_MAX_PAIRS];
    DWORD         n_pairs;
} cache_read_build_t;

/* 校验一个 pair 的 L2P 映射、plane/拓扑与页内一致性。
 * *defer = 某次 l2p_read 遇到 LOADING（已起加载），调用者可下轮重试。 */
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
        if (!pba_ok)
        {
            return false;
        }

        const BYTE pl         = pba_to_plane(p);
        const bool same_plane = (pl < PLANE_NUM) && (pl == ftl_lba_plane(l));
        const bool same_topo  = (pba_to_ch(p) == ftl->topo.ch) &&
                               (pba_to_ce(p) == ftl->topo.ce) &&
                               (pba_to_lun(p) == ftl->topo.lun);

        if ((!same_plane) || (!same_topo))
        {
            return false; /* 映射表损坏/跨实例：交给普通读路径报错 */
        }

        if (pair_pba[pl] != NAND_PBA_INVALID)
        {
            const bool same_page  = pba_same_page(pair_pba[pl], p);
            const bool same_block = (pba_to_block(pair_pba[pl]) == pba_to_block(p));

            if ((!same_page) || (!same_block))
            {
                return false;
            }
        }
        pair_pba[pl] = p;
    }

    return true;
}

/* 两 plane 是否都有效且指向同一页。 */
static bool cache_read_pair_complete(const DWORD pair_pba[PLANE_NUM])
{
    for (BYTE p = 0U; p < PLANE_NUM; p++)
    {
        if (pair_pba[p] == NAND_PBA_INVALID)
        {
            return false;
        }
    }

    for (BYTE p = 1U; p < PLANE_NUM; p++)
    {
        if (pba_page(pair_pba[p]) != pba_page(pair_pba[0]))
        {
            return false;
        }
    }

    return true;
}

/* 当前 pair 是否紧接上一 pair 的物理页。 */
static bool cache_read_pairs_contiguous(const DWORD prev[PLANE_NUM], const DWORD cur[PLANE_NUM])
{
    for (BYTE p = 0U; p < PLANE_NUM; p++)
    {
        const bool next_page  = (pba_page(cur[p]) == (pba_page(prev[p]) + 1U));
        const bool same_block = (pba_to_block(cur[p]) == pba_to_block(prev[p]));

        if ((!next_page) || (!same_block))
        {
            return false;
        }
    }
    return true;
}

/* 收集可连续 cache 读的 pair；返回 pair 数（0..FTL_CACHE_READ_MAX_PAIRS）。 */
static DWORD cache_read_collect(const cache_read_req_t *req,
                                DWORD                   pba[FTL_CACHE_READ_MAX_PAIRS][PLANE_NUM],
                                bool                   *defer)
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

        if (!cache_read_probe_pair(req->ftl, base, pair_pba, defer))
        {
            break;
        }
        if (!cache_read_pair_complete(pair_pba))
        {
            break; /* 两 plane 不齐/不同页：不是完整 pair */
        }
        if ((j > 0U) && (!cache_read_pairs_contiguous(pba[j - 1U], pair_pba)))
        {
            break; /* 物理不连续：只提交已收集的前缀 */
        }

        for (BYTE p = 0U; p < PLANE_NUM; p++)
        {
            pba[j][p] = pair_pba[p];
        }
        n_pairs++;
    }

    return n_pairs;
}

/* 构建命令：分配 + 配置 + PPA/CW。失败时由 release_build 统一回收。 */
static bool cache_read_build_commands(cache_read_build_t     *b,
                                      const cache_read_req_t *req,
                                      DWORD                   pba[FTL_CACHE_READ_MAX_PAIRS][PLANE_NUM])
{
    for (DWORD i = 0U; i < b->n_pairs; i++)
    {
        b->cmds[i] = fc_command_alloc();

        if (b->cmds[i] == NULL)
        {
            return false;
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
            return false;
        }
    }

    return true;
}

/* 构建 pending：命令已全部建好后才登记，保证失败回收顺序固定。 */
static bool cache_read_build_pending(cache_read_build_t *b, const cache_read_req_t *req)
{
    for (DWORD i = 0U; i < b->n_pairs; i++)
    {
        b->pends[i] = pend_alloc(&req->ftl->pending);

        if (b->pends[i] == NULL)
        {
            return false;
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
        pending->cache_intermediate = 0U;
        pend_validate(pending);
    }

    return true;
}

/* 构建失败的统一回收：与旧 build_fail 标签逐项等价。 */
static void cache_read_release_build(ftl_ctx_t *ftl, cache_read_build_t *b)
{
    for (DWORD i = 0U; i < b->n_pairs; i++)
    {
        if (b->pends[i] != NULL)
        {
            pend_remove(&ftl->pending, b->pends[i]->command_id);
        }
        if (b->cmds[i] != NULL)
        {
            (void)fc_command_release(b->cmds[i]);
        }
    }
}

/* 提交已建好的命令；保留原“已提交保留、未提交回滚”的组计数语义。 */
static ftl_cache_read_result_e cache_read_submit_commands(cache_read_build_t *b, const cache_read_req_t *req)
{
    ftl_ctx_t *ftl = req->ftl;

    /* 记账必须早于 submit：完成可能在本轮 drain 之后立刻被泵处理。 */
    for (DWORD i = 0U; i < b->n_pairs; i++)
    {
        pend_group_add_subop(&ftl->pending, req->gi);
    }

    DWORD submitted = 0U;

    for (DWORD i = 0U; i < b->n_pairs; i++)
    {
        if (ftl_op_submit(b->cmds[i], FTL_OP_HOST_IO, NULL) != FTL_OK)
        {
            /* 已提交的成员保留并等待完成；未提交的成员回滚。 */
            for (DWORD k = i; k < b->n_pairs; k++)
            {
                pend_remove(&ftl->pending, ftl_cmd_id(b->cmds[k]));
                (void)fc_command_release(b->cmds[k]);
                pend_group_drop_subop(&ftl->pending, req->gi);
            }

            ftl->cache_read_fallbacks++;

            return (submitted == 0U) ? FTL_CACHE_READ_AGAIN : FTL_CACHE_READ_ERROR;
        }

        submitted++;
    }

    *req->used = b->n_pairs * FTL_LBA_PER_PAIR;
    ftl->cache_read_batches++;
    ftl->cache_read_pairs += b->n_pairs;
    return FTL_CACHE_READ_OK;
}

static ftl_cache_read_result_e cache_read_submit(const cache_read_req_t *req,
                                                 DWORD                   n_pairs,
                                                 DWORD                   pba[FTL_CACHE_READ_MAX_PAIRS][PLANE_NUM])
{
    cache_read_build_t b;

    memset(&b, 0, sizeof(b));
    b.n_pairs = n_pairs;

    if (!cache_read_build_commands(&b, req, pba))
    {
        cache_read_release_build(req->ftl, &b);
        req->ftl->cache_read_fallbacks++;
        return FTL_CACHE_READ_AGAIN;
    }

    if (!cache_read_build_pending(&b, req))
    {
        cache_read_release_build(req->ftl, &b);
        req->ftl->cache_read_fallbacks++;
        return FTL_CACHE_READ_AGAIN;
    }

    return cache_read_submit_commands(&b, req);
}

ftl_cache_read_result_e ftl_cache_read_try(ftl_ctx_t *ftl, WORD gi, DWORD lba, void *buffer, DWORD remain, DWORD *used)
{
    ASSERT(ftl != NULL);
    ASSERT(used != NULL);

    if ((remain < (FTL_CACHE_READ_MIN_PAIRS * FTL_LBA_PER_PAIR)) ||
        ((lba & (FTL_LBA_PER_PAIR - 1U)) != 0U))
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

    if (!ftl_cache_budget_ok(ftl, n_pairs))
    {
        ftl->cache_read_fallbacks++;
        return FTL_CACHE_READ_DECLINED;
    }

    return cache_read_submit(&req, n_pairs, pba);
}
