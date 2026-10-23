/*
 * cache_write.c — host 16 KiB cache program 批次（本仓自拟）
 *
 * 一次 host 16 KiB 写 = 4 个 pair 页。条件满足时一次提交 4 条命令：
 *   op49（首页 15h，只查 FAIL） + op44 × 2（中间页 15h） + op48（末页 10h）
 * 中间页只释放命令，不确认映射；末页成功后统一确认（见 ftl_host_io_callback）。
 *
 * 不依赖 FC chain：4 条命令经 ftl_op_submit_batch()（→ fc_submit_batch）一次全进 SQ，
 * 后续命令只排在其后。
 * 目的页必须是同一 Open SB 内连续的 4 页，且首成员页号 ≥1（page0 只允许普通单条写）。
 */

#include "cache_write.h"

#include "assertion.h"
#include "fc.h"
#include "ftl.h"
#include "ftl_fc_interface.h"
#include "ftl_op.h"
#include "migrate.h"
#include "pending.h"
#include "wbuf.h"

typedef struct
{
    DWORD         pba[FTL_CACHE_WRITE_MAX_PAGES][PLANE_NUM];
    fc_command_t *cmds[FTL_CACHE_WRITE_MAX_PAGES];
    BYTE          n_alloc;
    BYTE          n_cmd;
    DWORD         n_pending;
} cache_wb_build_t;

/* 撤销尚未提交的构建：释放命令/pending + 归还页对。调用后 batch 缓冲/组未动。 */
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
        alc_rewind_pair(&ftl->alloc, FTL_TEMP_HOT);
    }

    ftl->cache_wb.active = false;
}

/* 分配 4 个页对、建 4 条命令、登记 4 个 pending（尚未提交）。 */
static ftl_status_e cache_wb_build(ftl_ctx_t *ftl, WORD gi, DWORD lba, const void *buffer, cache_wb_build_t *b)
{
    for (BYTE j = 0U; j < FTL_CACHE_WRITE_MAX_PAGES; j++)
    {
        if (alc_alloc_pair(&ftl->alloc, FTL_TEMP_HOT, b->pba[j]) != FTL_OK)
        {
            mig_trigger(ftl->mig_handle, MIGRATE_REASON_GC_URGENT);
            cache_wb_discard(ftl, b);
            return FTL_AGAIN;
        }
        b->n_alloc++;

        const DWORD           page_lba = lba + ((DWORD)j * FTL_LBA_PER_PAIR);
        const BYTE           *src      = (const BYTE *)buffer + ((DWORD)j * FTL_LBA_PER_PAIR * FTL_IU_BYTES);
        const ftl_page_cfg_t *cfg      = FTL_PAGE_CFG_DATA(ftl_lba_page(page_lba));

        const ftl_status_e st = ftl_async_write_pair_cmd(&b->cmds[j],
                                                         b->pba[j],
                                                         src,
                                                         FTL_CACHE_FULL_CW,
                                                         ftl_cache_prog_op(j, FTL_CACHE_WRITE_MAX_PAGES),
                                                         cfg);

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

    ASSERT(b->n_cmd == FTL_CACHE_WRITE_MAX_PAGES);
    ASSERT(b->n_pending == FTL_CACHE_WRITE_MAX_PAGES);
    return FTL_OK;
}

ftl_cache_write_result_e ftl_cache_write_try(ftl_ctx_t *ftl,
                                             WORD       gi,
                                             DWORD      lba,
                                             void      *buffer,
                                             DWORD      remain,
                                             DWORD     *used)
{
    ASSERT(ftl != NULL);
    ASSERT(used != NULL);

    /* 条件不满足 = 正常回退，不是错误。
     * alc_open_pair_room>=4 已蕴含：SB 开着、所有 plane 的 page0 已成功、剩余 ≥4 页。 */
    if (ftl->cache_wb.active || (remain < FTL_CACHE_WRITE_LBAS) ||
        ((lba & (FTL_LBA_PER_PAIR - 1U)) != 0U) || (!wb_is_idle(ftl->wbuf)) ||
        (alc_open_pair_room(&ftl->alloc, FTL_TEMP_HOT) < FTL_CACHE_WRITE_MAX_PAGES) ||
        (!ftl_cache_budget_ok(ftl, FTL_CACHE_WRITE_MAX_PAGES)))
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
