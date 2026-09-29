#include "ftl/ftl.h"

#include "fc/fc.h"
#include "ftl/bbt.h"
#include "ftl/checkpoint.h"
#include "ftl/completion_table.h"
#include "ftl/ftl_fc_interface.h"
#include "ftl/pending.h"
#include "ftl/power_failure_recovery.h"
#include "ftl/wear_leveling.h"
#include "nand/onfi_1_0.h"

#include <string.h>

/* ── 内部辅助 ── */

/*
 * ftl_handle_completion - 处理 FC 命令完成回调（默认 handler）
 *
 * 作为 completion_table 的默认 handler 注册，所有未注册精确路由
 * 的 completion 都走此函数。
 *
 * 根据 completion.module 路由：
 *   FTL      → 查 pending 表 → 读数据 / release
 *   METADATA → release（同步操作完成后释放）
 *   GC       → release
 *
 * @param completion  FC completion status
 * @param ctx         ftl_context_t 指针
 */
static void ftl_handle_completion(const fc_command_status_t *completion,
                                  void                      *ctx)
{
    ftl_context_t *ftl      = (ftl_context_t *)ctx;
    uint16_t       cmd_id   = (uint16_t)completion->command_id;
    fc_command_t  *cmd;

    (void)ftl;

    cmd = fc_command_id_to_command(cmd_id);
    if (cmd == NULL)
    {
        return;
    }

    switch (completion->module)
    {
        case FC_COMMAND_MODULE_FTL:
            /* 用户数据 I/O 完成 */
            {
                ftl_pending_req_t *pending = pending_lookup(cmd_id);

                if (pending != NULL)
                {
                    uint32_t data_size = FTL_READ_USER_SIZE * ((pending->pages > 0U) ? pending->pages : 1U);

                    if ((completion->status & FC_CW_STATUS_NAND_FAIL) == 0U)
                    {
                        if (pending->buffer != NULL)
                        {
                            /* 读成功：拷贝数据到 caller buffer */
                            (void)ftl_cmd_read_data(cmd, pending->buffer,
                                                    data_size);
                            if (pending->status_out != NULL)
                            {
                                *pending->status_out = SSD_STATUS_OK;
                            }
                        }
                        else if (pending->cache_intermediate == 0U)
                        {
                            /* 写成功（非 cache 中间页）：确认 Journal + L2P（modify-after-confirm） */
                            journal_append(&pending->ftl->journal,
                                           pending->lba,
                                           pending->old_pba,
                                           pending->new_pba);
                            l2p_cache_write(&pending->ftl->cache,
                                            pending->lba,
                                            pending->new_pba);
                            checkpoint_journal_watermark(&pending->ftl->journal);
                            pending->ftl->total_writes++;
                            if (pending->status_out != NULL)
                            {
                                *pending->status_out = SSD_STATUS_OK;
                            }
                        }
                        /* cache_intermediate 页：跳过 L2P/Journal 更新，等最终 10h 确认 */
                    }
                    else
                    {
                        if (pending->buffer != NULL)
                        {
                            /* 读失败：返回全零 */
                            memset(pending->buffer, 0, data_size);
                        }
                        else
                        {
                            /* 写失败：标记坏块（RAM only，后台 bbt_flush 持久化） */
                            uint16_t block = pba_to_block(pending->new_pba);
                            bbt_mark_bad(&pending->ftl->alloc, block);
                            /* L2P 不动 → 自动回滚到 old_pba，旧数据完好 */
                            /* Super Block 空洞由 GC 扫描时自然跳过 */
                        }
                        if (pending->status_out != NULL)
                        {
                            *pending->status_out = SSD_STATUS_IO_ERROR;
                        }
                    }

                    /* 通知等待方 */
                    if (pending->complete_flag != NULL)
                    {
                        *pending->complete_flag = 1U;
                    }

                    /* Controller 子请求完成回调（聚合计数器减 1） */
                    if (pending->on_complete != NULL)
                    {
                        pending->on_complete(pending->on_complete_ctx);
                    }

                    pending_remove(cmd_id);
                }

                (void)fc_command_release(cmd);
            }
            break;

        case FC_COMMAND_MODULE_METADATA:
        case FC_COMMAND_MODULE_GC:
        case FC_COMMAND_MODULE_CHECKPOINT:
        case FC_COMMAND_MODULE_BBT:
            /* 元数据类操作完成后 release
             * 如果同步等待 handler 已提前 release，此处 fc_command_release
             * 会返回错误，忽略即可 */
            if (cmd->command_id != FC_COMMAND_ID_INVALID)
            {
                (void)fc_command_release(cmd);
            }
            break;

        default:
            if (cmd->command_id != FC_COMMAND_ID_INVALID)
            {
                (void)fc_command_release(cmd);
            }
            break;
    }
}

/* ── 三级后台服务 ── */

static void ftl_service_idle(ftl_context_t *ftl)
{
    /* flush 所有 dirty cache */
    l2p_cache_flush_all(&ftl->cache);

    /* 补充 Pre-conditioned Pool */
    if (ftl->alloc.pre_cond_count < FREE_POOL_TARGET)
    {
        uint32_t       target = FREE_POOL_TARGET - ftl->alloc.pre_cond_count;
        super_block_t *victim;

        while (target > 0U)
        {
            if (!gc_select_victim(&ftl->alloc, &victim))
            {
                break;
            }
            (void)gc_migrate_sb(&ftl->alloc, &ftl->cache, &ftl->journal, victim);
            gc_erase_and_free(&ftl->alloc, victim);
            target -= 2U;
        }
    }

    /* 磨损均衡（IDLE 全力执行） */
    (void)wl_execute(ftl);

    /* 强制写 Journal buffer */
    journal_flush_page(&ftl->journal);

    /* BBT 持久化（攒批写 NAND BBT） */
    (void)bbt_flush(&ftl->alloc);

    /* Checkpoint 检查 */
    if (checkpoint_check_trigger(ftl, 0U))
    {
        (void)checkpoint_create(ftl);
    }
}

static void ftl_service_normal(ftl_context_t *ftl)
{
    /* 后台 flush：最多 4 条 */
    uint16_t flushed = 0U;
    for (uint16_t i = 0U; (i < L2P_CACHE_SIZE) && (flushed < BG_NORMAL_FLUSH_COUNT); i++)
    {
        if (ftl->cache.lines[i].state == L2P_CACHE_DIRTY)
        {
            (void)l2p_cache_flush_one(&ftl->cache, (int)i);
            flushed++;
        }
    }

    /* 检查 GC */
    if (gc_check_trigger(&ftl->alloc, ftl->total_writes, 500U))
    {
        super_block_t *victim;
        if (gc_select_victim(&ftl->alloc, &victim))
        {
            (void)gc_migrate_sb(&ftl->alloc, &ftl->cache, &ftl->journal, victim);
            gc_erase_and_free(&ftl->alloc, victim);
        }
    }

    /* 磨损均衡（NORMAL 模式按需执行） */
    if (wl_should_trigger(ftl))
    {
        (void)wl_execute(ftl);
    }

    /* BBT 持久化（NORMAL 模式周期性刷入） */
    (void)bbt_flush(&ftl->alloc);

    /* Checkpoint 检查（NORMAL 模式也执行） */
    if (checkpoint_check_trigger(ftl, 0U))
    {
        (void)checkpoint_create(ftl);
    }
}

static void ftl_service_busy(ftl_context_t *ftl)
{
    /* 最少后台：只 flush 1 条防 evict 阻塞 */
    for (uint16_t i = 0U; i < L2P_CACHE_SIZE; i++)
    {
        if (ftl->cache.lines[i].state == L2P_CACHE_DIRTY)
        {
            (void)l2p_cache_flush_one(&ftl->cache, (int)i);
            break;
        }
    }

    /* 紧急 Checkpoint：Journal ≥ 95% 时 BUSY 模式也强制执行 */
    if (checkpoint_is_emergency(ftl))
    {
        (void)checkpoint_create(ftl);
    }
}

/* ── 对外接口 ── */

ftl_status_t ftl_init(ftl_context_t *ftl,
                      ftl_topology_t topo,
                      uint16_t      *free_blocks,
                      uint16_t       free_count,
                      uint16_t      *bad_blocks,
                      uint32_t      *erase_counts,
                      super_block_t *closed_sb_list)
{
    if (ftl == NULL)
    {
        return FTL_ERROR;
    }

    ftl->topo = topo;

    /* ├─ Completion 路由表 ── */
    completion_table_init();

    /* ├─ 注册默认 handler（= ftl_handle_completion）── */
    completion_table_set_default(ftl_handle_completion, ftl);

    /* ├─ Pending Request 表 ── */
    pending_init();

    /* ├─ L2P Cache ── */
    l2p_cache_init(&ftl->cache);

    /* ├─ Allocator ── */
    allocator_init(&ftl->alloc,
                   topo.ch, topo.ce,
                   free_blocks, free_count,
                   bad_blocks, erase_counts, closed_sb_list);

    /* ├─ 连接 L2P Cache → Metadata Directory + Allocator ── */
    /* 此时 Metadata Directory 尚未加载，先设 NULL，后续 bbt_init/md_load 后更新 */
    l2p_cache_set_md_dir(&ftl->cache, NULL, &ftl->alloc);

    /* ├─ Journal ── */
    journal_init(&ftl->journal, ftl->topo);

    /* ├─ Metadata Directory ── */
    ftl->metadata_dir.topo = ftl->topo;
    {
        int md_ret = metadata_dir_load(&ftl->metadata_dir);
        if (md_ret != 0)
        {
            metadata_dir_init(&ftl->metadata_dir);
        }
    }

    /* ├─ 重新连接 L2P Cache（此时 md_dir 已就绪）── */
    l2p_cache_set_md_dir(&ftl->cache, &ftl->metadata_dir, &ftl->alloc);

    /* ├─ BBT 初始化（从 NAND 加载或出厂扫描）── */
    (void)bbt_init(&ftl->alloc);

    /* ├─ Checkpoint 计时 ── */
    ftl->cp_timer_start = 0U;
    ftl->cp_sequence    = 0U;

    /* ├─ GC 计时 ── */
    ftl->gc_last_run_writes = 0U;
    ftl->gc_timer_start     = 0U;

    /* ├─ 统计 ── */
    ftl->total_writes = 0U;

    return FTL_OK;
}

void ftl_service(ftl_context_t *ftl)
{
    uint32_t submitted = 0U;

    if (ftl == NULL)
    {
        return;
    }

    /* Phase 1: fc_service 推进 NAND */
    for (uint8_t i = 0U; i < NAND_SUBMIT_BUDGET; i++)
    {
        if (fc_service() != FC_STATUS_OK)
        {
            break;
        }
        submitted++;
    }

    /* Phase 2: 统一完成泵 — 替代原有 fc_poll_completion 循环 */
    completion_table_drain();

    /* Phase 3: 三级后台模式 */
    if (submitted == 0U)
    {
        ftl_service_idle(ftl);
    }
    else if (submitted <= (NAND_SUBMIT_BUDGET / 2U))
    {
        ftl_service_normal(ftl);
    }
    else
    {
        ftl_service_busy(ftl);
    }
}

ftl_status_t ftl_submit_read(ftl_context_t *ftl, uint64_t lba, void *buffer,
                             uint32_t pages,
                             volatile uint8_t *complete_flag,
                             volatile ssd_status_t *status_out,
                             pending_cb_t on_complete, void *on_complete_ctx)
{
    fc_command_t *cmd;
    nand_pba_t    pba;
    uint32_t      data_size;
    int           ret;
    lba_t         internal_lba;

    if ((ftl == NULL) || (buffer == NULL) || (pages == 0U) || (pages > 2U))
    {
        return FTL_ERROR;
    }
    data_size = FTL_READ_USER_SIZE * pages;

    /* LBA 越界检查 */
    if (lba >= FTL_LBA_MAX)
    {
        return FTL_ERROR;
    }
    internal_lba = (lba_t)lba;

    pba = l2p_cache_read(&ftl->cache, internal_lba);
    if (pba == FTL_PBA_INVALID)
    {
        memset(buffer, 0, data_size);

        /* 通知等待方：虽无 NAND 操作，但仍需发完成信号 */
        if (status_out != NULL)
        {
            *status_out = SSD_STATUS_OK;
        }
        if (complete_flag != NULL)
        {
            *complete_flag = 1U;
        }
        if (on_complete != NULL)
        {
            on_complete(on_complete_ctx);
        }

        return FTL_OK;
    }

    /* ② 分配 FC 命令 */
    cmd = fc_command_alloc();
    if (cmd == NULL)
    {
        return FTL_ERROR;
    }

    /* ③ 配置读命令 — 根据 pages 选择单 Plane 或 Multi-Plane */
    if (pages == 1U)
    {
        cmd->config.operation   = OP_NORMAL_READ_PAGE_WITH_DATA_OUT;
        cmd->config.multi_plane = 0U;
    }
    else
    {
        cmd->config.operation   = OP_READ_PAGE_MULTI_PLANE_WITH_DATA_OUT;
        cmd->config.multi_plane = 2U;
    }
    cmd->config.cw          = 3U;
    cmd->config.crc         = FC_CRC_ENABLE;
    cmd->config.random      = FC_RANDOM_ENABLE;
    cmd->config.ldpc        = FC_ECC_MODE_BCH;
    cmd->config.chain       = FC_CHAIN_END;
    cmd->config.page_type   = FC_PAGE_TYPE_LP;
    cmd->module             = FC_COMMAND_MODULE_FTL;
    cmd->meta               = (DWORD)internal_lba;

    /* ④ 配置 PBA + desc 链 */
    ret = ftl_cmd_setup_ppa(cmd, pba);
    if (ret != 0)
    {
        (void)fc_command_release(cmd);
        return FTL_ERROR;
    }

    /* ⑤ 注册 pending（必须早于 submit，避免竞态） */
    ret = pending_add(cmd->command_id, internal_lba, buffer,
                      on_complete, on_complete_ctx, complete_flag,
                      status_out,
                      NAND_PBA_INVALID, NAND_PBA_INVALID, NULL, 0U, pages);
    if (ret != 0)
    {
        (void)fc_command_release(cmd);
        return FTL_ERROR;
    }

    /* ⑥ 异步提交 */
    if (fc_submit(cmd) != FC_STATUS_OK)
    {
        pending_remove(cmd->command_id);
        (void)fc_command_release(cmd);
        return FTL_ERROR;
    }

    /* ⑦ 立即返回，数据在 completion_table_drain 中拷贝到 buffer */
    return FTL_OK;
}

ftl_status_t ftl_submit_write(ftl_context_t *ftl, uint64_t lba, const void *data,
                              uint32_t pages,
                              volatile uint8_t *complete_flag,
                              volatile ssd_status_t *status_out,
                              pending_cb_t on_complete, void *on_complete_ctx)
{
    fc_command_t *cmd;
    nand_pba_t    new_pba;
    nand_pba_t    old_pba;
    uint32_t      data_size;
    int           ret;
    lba_t         internal_lba;

    if ((ftl == NULL) || (data == NULL) || (pages == 0U) || (pages > 2U))
    {
        return FTL_ERROR;
    }
    data_size = FTL_READ_USER_SIZE * pages;

    /* LBA 越界检查 */
    if (lba >= FTL_LBA_MAX)
    {
        return FTL_ERROR;
    }
    internal_lba = (lba_t)lba;

    /* ① Allocator 分配新 PBA */
    new_pba = allocator_alloc(&ftl->alloc, FTL_TEMP_HOT);
    if (new_pba == NAND_PBA_INVALID)
    {
        return FTL_ERROR;
    }

    /* ② 读旧 PBA（journal 记录用） */
    old_pba = l2p_cache_read(&ftl->cache, internal_lba);

    /* ③ 配置 FC 写命令 — 根据 pages 选择单 Plane 或 Multi-Plane */
    cmd = fc_command_alloc();
    if (cmd == NULL)
    {
        return FTL_ERROR;
    }

    if (pages == 1U)
    {
        cmd->config.operation   = OP_NORMAL_PROGRAM_PAGE;
        cmd->config.multi_plane = 0U;
    }
    else
    {
        cmd->config.operation   = OP_PROGRAM_PAGE_MULTI_PLANE;
        cmd->config.multi_plane = 2U;
    }
    cmd->config.cw          = 3U;
    cmd->config.crc         = FC_CRC_ENABLE;
    cmd->config.random      = FC_RANDOM_ENABLE;
    cmd->config.ldpc        = FC_ECC_MODE_BCH;
    cmd->config.chain       = FC_CHAIN_END;
    cmd->config.page_type   = FC_PAGE_TYPE_LP;
    cmd->module             = FC_COMMAND_MODULE_FTL;
    cmd->meta               = (DWORD)internal_lba;

    /* ④ 配置 PBA + desc 链 + 写入数据 */
    ret = ftl_cmd_setup_ppa(cmd, new_pba);
    if (ret != 0)
    {
        (void)fc_command_release(cmd);
        return FTL_ERROR;
    }

    ret = ftl_cmd_write_data(cmd, data, data_size);
    if (ret != 0)
    {
        (void)fc_command_release(cmd);
        return FTL_ERROR;
    }

    /* ⑤ 注册 pending（complete_flag + old/new_pba + ftl 用于写完成确认） */
    ret = pending_add(cmd->command_id, internal_lba, NULL,
                      on_complete, on_complete_ctx, complete_flag,
                      status_out,
                      old_pba, new_pba, ftl, 0U, pages);
    if (ret != 0)
    {
        (void)fc_command_release(cmd);
        return FTL_ERROR;
    }

    /* ⑥ 异步提交到 NAND */
    if (fc_submit(cmd) != FC_STATUS_OK)
    {
        pending_remove(cmd->command_id);
        (void)fc_command_release(cmd);
        return FTL_ERROR;
    }

    /* ⑦ Journal + L2P 在 completion handler 中确认 NAND PROGRAM 成功后执行 */
    return FTL_OK;
}

/* ── UART CLI 便利包装（非阻塞，仅用于 pending 表跟踪）── */

void ftl_trim(ftl_context_t *ftl, uint64_t lba)
{
    if (ftl != NULL)
    {
        l2p_cache_write(&ftl->cache, (lba_t)lba, FTL_PBA_INVALID);
    }
}