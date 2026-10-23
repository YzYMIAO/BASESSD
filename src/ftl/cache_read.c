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

/* 收集可连续 cache 读的 pair；返回 pair 数（0..FTL_CACHE_READ_MAX_PAIRS）。
 * *defer = 某次 l2p_read 遇到 LOADING（已起加载），调用者可下轮重试。 */
static DWORD cache_read_collect(ftl_ctx_t *ftl, DWORD lba, DWORD remain, DWORD pba[FTL_CACHE_READ_MAX_PAIRS][PLANE_NUM], bool *defer)
{
    DWORD max_pairs = remain / FTL_LBA_PER_PAIR;

    if (max_pairs > FTL_CACHE_READ_MAX_PAIRS)
    {
        max_pairs = FTL_CACHE_READ_MAX_PAIRS;
    }

    DWORD n_pairs = 0U;

    for (DWORD j = 0U; j < max_pairs; j++)
    {
        const DWORD base = lba + (j * FTL_LBA_PER_PAIR);
        DWORD       pair_pba[PLANE_NUM];
        bool        pair_ok = true;

        for (BYTE p = 0U; p < PLANE_NUM; p++)
        {
            pair_pba[p] = NAND_PBA_INVALID;
        }

        for (BYTE k = 0U; k < FTL_LBA_PER_PAIR; k++)
        {
            const DWORD l = base + k;

            if (wb_has(ftl->wbuf, l))
            {
                pair_ok = false; /* 写缓冲未落盘：走普通读的 wbuf 快路径 */
                break;
            }

            DWORD              p  = NAND_PBA_INVALID;
            const ftl_status_e st = l2p_read(ftl->cache, l, &p);

            if (st == FTL_AGAIN)
            {
                *defer  = true;
                pair_ok = false;
                break;
            }
            if ((st != FTL_OK) || (p == NAND_PBA_INVALID) || (!pba_is_valid(p)))
            {
                pair_ok = false;
                break;
            }

            const BYTE pl = pba_to_plane(p);
            if ((pl >= PLANE_NUM) || (pl != ftl_lba_plane(l)) ||
                (pba_to_ch(p) != ftl->topo.ch) || (pba_to_ce(p) != ftl->topo.ce) ||
                (pba_to_lun(p) != ftl->topo.lun))
            {
                pair_ok = false; /* 映射表损坏/跨实例：交给普通读路径报错 */
                break;
            }

            if (pair_pba[pl] != NAND_PBA_INVALID)
            {
                if ((!pba_same_page(pair_pba[pl], p)) || (pba_to_block(pair_pba[pl]) != pba_to_block(p)))
                {
                    pair_ok = false;
                    break;
                }
            }
            pair_pba[pl] = p;
        }

        if (!pair_ok)
        {
            break;
        }

        if ((pair_pba[0] == NAND_PBA_INVALID) || (pair_pba[1] == NAND_PBA_INVALID) ||
            (pba_page(pair_pba[0]) != pba_page(pair_pba[1])))
        {
            break; /* 两 plane 不齐/不同页：不是完整 pair */
        }

        if (j > 0U)
        {
            for (BYTE p = 0U; p < PLANE_NUM; p++)
            {
                if ((pba_page(pair_pba[p]) != (pba_page(pba[j - 1U][p]) + 1U)) ||
                    (pba_to_block(pair_pba[p]) != pba_to_block(pba[j - 1U][p])))
                {
                    pair_ok = false;
                    break;
                }
            }
            if (!pair_ok)
            {
                break; /* 物理不连续：只提交已收集的前缀 */
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

static ftl_cache_read_result_e cache_read_submit(ftl_ctx_t *ftl, WORD gi, DWORD lba, void *buffer, DWORD n_pairs, DWORD pba[FTL_CACHE_READ_MAX_PAIRS][PLANE_NUM], DWORD *used)
{
    fc_command_t *cmds[FTL_CACHE_READ_MAX_PAIRS]  = {0};
    pend_req_t   *pends[FTL_CACHE_READ_MAX_PAIRS] = {0};
    DWORD         submitted                       = 0U;

    for (DWORD i = 0U; i < n_pairs; i++)
    {
        cmds[i] = fc_command_alloc();

        if (cmds[i] == NULL)
        {
            goto build_fail;
        }

        fc_command_t *cmd = cmds[i];

        if (i == 0U)
        {
            cmd->config.operation = OP_READ_PAGE_CACHE_START_SEQ_MP;
        }
        else if (i == (n_pairs - 1U))
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
        cmd->meta               = ftl_lba_pair_base(lba + (i * FTL_LBA_PER_PAIR));

        if (ftl_cmd_setup_ppa_pair_cw(cmd, pba[i], FTL_CACHE_FULL_CW) != FTL_OK)
        {
            goto build_fail;
        }
    }

    for (DWORD i = 0U; i < n_pairs; i++)
    {
        pends[i] = pend_alloc(&ftl->pending);

        if (pends[i] == NULL)
        {
            goto build_fail;
        }

        pend_req_t *pending = pends[i];

        pending->command_id         = ftl_cmd_id(cmds[i]);
        pending->owners             = ftl_group_bit(gi);
        pending->op                 = PENDING_OP_READ_CACHE;
        pending->n_cw               = FTL_LBA_PER_PAIR;
        pending->cw_bitmap          = 0U;
        pending->lba                = ftl_lba_pair_base(lba + (i * FTL_LBA_PER_PAIR));
        pending->buffer             = (BYTE *)buffer + (i * FTL_LBA_PER_PAIR * FTL_IU_BYTES);
        pending->new_pba            = NAND_PBA_INVALID;
        pending->new_pba1           = NAND_PBA_INVALID;
        pending->cache_intermediate = 0U;
        pend_validate(pending);
    }

    /* 记账必须早于 submit：完成可能在本轮 drain 之后立刻被泵处理。 */
    for (DWORD i = 0U; i < n_pairs; i++)
    {
        pend_group_add_subop(&ftl->pending, gi);
    }

    for (DWORD i = 0U; i < n_pairs; i++)
    {
        if (ftl_op_submit(cmds[i], FTL_OP_HOST_IO, NULL) != FTL_OK)
        {
            /* 已提交的成员保留并等待完成；未提交的成员回滚。 */
            for (DWORD k = i; k < n_pairs; k++)
            {
                pend_remove(&ftl->pending, ftl_cmd_id(cmds[k]));
                (void)fc_command_release(cmds[k]);
                pend_group_drop_subop(&ftl->pending, gi);
            }

            ftl->cache_read_fallbacks++;

            if (submitted == 0U)
            {
                return FTL_CACHE_READ_AGAIN;
            }

            return FTL_CACHE_READ_ERROR;
        }

        submitted++;
    }

    *used = n_pairs * FTL_LBA_PER_PAIR;
    ftl->cache_read_batches++;
    ftl->cache_read_pairs += n_pairs;
    return FTL_CACHE_READ_OK;

build_fail:
    for (DWORD i = 0U; i < n_pairs; i++)
    {
        if (pends[i] != NULL)
        {
            pend_remove(&ftl->pending, pends[i]->command_id);
        }
        if (cmds[i] != NULL)
        {
            (void)fc_command_release(cmds[i]);
        }
    }

    ftl->cache_read_fallbacks++;
    return FTL_CACHE_READ_AGAIN;
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

    DWORD pba[FTL_CACHE_READ_MAX_PAIRS][PLANE_NUM];
    bool  defer   = false;
    DWORD n_pairs = cache_read_collect(ftl, lba, remain, pba, &defer);

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

    return cache_read_submit(ftl, gi, lba, buffer, n_pairs, pba, used);
}
