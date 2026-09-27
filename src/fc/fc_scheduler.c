#include "fc_scheduler.h"

#include "fc.h"
#include "onfi_1_0.h"

#include <stddef.h>
#include <string.h>

/*
 * 判断 operation 是否为读操作。
 * 读操作包括：标准 Page Read、Multi-Plane Read、Cache Read、
 * Random Data Read、Internal Data Move Read、READ_ID、
 * READ_PARAMETER_PAGE、READ_STATUS、READ_MODE 等。
 * PROGRAM/ERASE/SET_FEATURES 等非读操作受读饥饿保护约束。
 */
static uint8_t fc_scheduler_op_is_read(uint8_t operation)
{
    switch (operation)
    {
        case OP_NORMAL_READ_PAGE_WITH_DATA_OUT:
        case OP_NORMAL_READ_PAGE_WITHOUT_DATA_OUT:
        case OP_READ_PAGE_MULTI_PLANE_WITH_DATA_OUT:
        case OP_READ_PAGE_MULTI_PLANE_WITHOUT_DATA_OUT:
        case OP_READ_PAGE_CACHE_SEQ:
        case OP_READ_PAGE_CACHE_RANDOM:
        case OP_READ_PAGE_CACHE_LAST:
        case OP_RANDOM_DATA_READ:
        case OP_READ_FOR_INTERNAL_DATA_MOVE:
        case OP_READ_ID:
        case OP_READ_PARAMETER_PAGE:
        case OP_READ_UNIQUE_ID:
        case OP_READ_STATUS:
        case OP_READ_STATUS_ENHANCED:
        case OP_BLOCK_LOCK_STATUS:
        case OP_READ_PAGE_LOAD:             /* Phase V: LOAD 是 ISSUE 段，算读 */
        case OP_READ_PAGE_FETCH:            /* Phase V: FETCH 是读完成段 */
            return 1U;
        default:
            return 0U;
    }
}

fc_status_t fc_scheduler_init(fc_scheduler_t *scheduler, fc_submission_queue_t *sq, fc_submission_queue_t *hsq)
{
    if ((scheduler == NULL) || (sq == NULL) || (hsq == NULL))
    {
        return FC_STATUS_INVALID_ARGUMENT;
    }
    scheduler->sq                      = sq;
    scheduler->hsq                     = hsq;
    scheduler->consecutive_write_count = 0U;
    memset(scheduler->busy, 0, sizeof(scheduler->busy));
    return FC_STATUS_OK;
}

static uint8_t fc_scheduler_target_is_busy(const fc_scheduler_t *scheduler, fc_command_id_t command_id)
{
    const fc_command_t *command;

    command = fc_command_id_to_command(command_id);
    if (command == NULL)
    {
        return 1U;
    }
    return scheduler->busy[command->config.ch][command->config.ce][command->config.lun];
}

/* ── 原单命令 select 接口，保留兼容性 ── */

fc_status_t fc_scheduler_select(fc_scheduler_t *scheduler, fc_command_id_t *command_id)
{
    uint16_t index;

    if ((scheduler == NULL) || (command_id == NULL))
    {
        return FC_STATUS_INVALID_ARGUMENT;
    }
    /* HSQ 优先 */
    if (scheduler->hsq->count != 0U)
    {
        for (index = 0U; index < scheduler->hsq->count; index++)
        {
            fc_command_id_t head_id = scheduler->hsq->entries[scheduler->hsq->head];
            if (fc_scheduler_target_is_busy(scheduler, head_id) == 0U)
            {
                return fc_submission_queue_pop(scheduler->hsq, command_id);
            }
            /* 目标忙，移到队尾继续找 */
            fc_command_id_t skipped_id;
            (void)fc_submission_queue_pop(scheduler->hsq, &skipped_id);
            (void)fc_submission_queue_push(scheduler->hsq, skipped_id);
        }
        return FC_STATUS_NOT_READY;
    }
    /* 普通 SQ */
    for (index = 0U; index < scheduler->sq->count; index++)
    {
        fc_command_id_t head_id = scheduler->sq->entries[scheduler->sq->head];
        if (fc_scheduler_target_is_busy(scheduler, head_id) == 0U)
        {
            return fc_submission_queue_pop(scheduler->sq, command_id);
        }
        fc_command_id_t skipped;
        (void)fc_submission_queue_pop(scheduler->sq, &skipped);
        (void)fc_submission_queue_push(scheduler->sq, skipped);
    }
    return (scheduler->sq->count != 0U) ? FC_STATUS_NOT_READY : FC_STATUS_QUEUE_EMPTY;
}

/* ── 链式 select 接口 ── */

fc_status_t fc_scheduler_select_chain(fc_scheduler_t *scheduler, fc_scheduler_chain_t *chain)
{
    fc_command_id_t head_id;
    fc_command_t   *cmd;
    uint8_t         ch;
    uint8_t         ce;
    uint8_t         lun;
    uint16_t        index;
    uint8_t         chain_len;

    if ((scheduler == NULL) || (chain == NULL))
    {
        return FC_STATUS_INVALID_ARGUMENT;
    }
    *chain = (fc_scheduler_chain_t){0};

    /* ── Phase 1: HSQ 优先 ──
     * HSQ 命令不参与链式，每次独立弹出一条。 */
    if (scheduler->hsq->count != 0U)
    {
        for (index = 0U; index < scheduler->hsq->count; index++)
        {
            head_id = scheduler->hsq->entries[scheduler->hsq->head];
            cmd     = fc_command_id_to_command(head_id);
            if (cmd == NULL)
            {
                fc_command_id_t null_id;
                (void)fc_submission_queue_pop(scheduler->hsq, &null_id);
                continue;
            }
            if (fc_scheduler_target_is_busy(scheduler, head_id) == 0U)
            {
                (void)fc_submission_queue_pop(scheduler->hsq, &head_id);
                chain->ids[0U] = head_id;
                chain->count   = 1U;
                return FC_STATUS_OK;
            }
            /* 目标忙：旋转到队尾再找 */
            (void)fc_submission_queue_pop(scheduler->hsq, &head_id);
            (void)fc_submission_queue_push(scheduler->hsq, head_id);
        }
        return FC_STATUS_NOT_READY;
    }

    /* ── Phase 2: 普通 SQ ── */

    /*
     * 读饥饿保护前置检查：扫描 SQ 是否有读命令。
     * 如果 SQ 内完全没有读命令，不触发读饥饿保护（避免死锁）。
     */
    uint8_t  sq_has_read = 0U;
    uint16_t scan;

    if (scheduler->consecutive_write_count >= FC_SCHEDULER_READ_PRIORITY_WRITE_BUDGET)
    {
        for (scan = 0U; scan < scheduler->sq->count; scan++)
        {
            fc_command_id_t scan_id  = scheduler->sq->entries[(uint16_t)(scheduler->sq->head + scan) % scheduler->sq->depth];
            fc_command_t   *scan_cmd = fc_command_id_to_command(scan_id);
            if ((scan_cmd != NULL) && (fc_scheduler_op_is_read((uint8_t)scan_cmd->config.operation) != 0U))
            {
                sq_has_read = 1U;
                break;
            }
        }
    }

    for (index = 0U; index < scheduler->sq->count; index++)
    {
        head_id = scheduler->sq->entries[scheduler->sq->head];
        cmd     = fc_command_id_to_command(head_id);
        if (cmd == NULL)
        {
            (void)fc_submission_queue_pop(scheduler->sq, &head_id);
            continue;
        }
        if (fc_scheduler_target_is_busy(scheduler, head_id))
        {
            /* 目标忙：旋转到队尾 */
            (void)fc_submission_queue_pop(scheduler->sq, &head_id);
            (void)fc_submission_queue_push(scheduler->sq, head_id);
            continue;
        }

        /*
         * 读饥饿保护：连续非读命令超过预算时，将非读命令旋转到队尾，
         * 优先调度读命令。
         * 仅在 SQ 中确实存在读命令时才触发，避免死锁。
         */
        if ((scheduler->consecutive_write_count >= FC_SCHEDULER_READ_PRIORITY_WRITE_BUDGET) &&
            (fc_scheduler_op_is_read((uint8_t)cmd->config.operation) == 0U) &&
            (sq_has_read != 0U))
        {
            (void)fc_submission_queue_pop(scheduler->sq, &head_id);
            (void)fc_submission_queue_push(scheduler->sq, head_id);
            continue;
        }

        /* 弹出链头命令 */
        (void)fc_submission_queue_pop(scheduler->sq, &head_id);
        chain->ids[0U] = head_id;
        chain->count   = 1U;

        /* 更新连续写计数 */
        if (fc_scheduler_op_is_read((uint8_t)cmd->config.operation) != 0U)
        {
            scheduler->consecutive_write_count = 0U;
        }
        else
        {
            scheduler->consecutive_write_count++;
        }

        /* 检测 chain 标志：0 = 单命令 */
        if (cmd->config.chain == 0U)
        {
            return FC_STATUS_OK;
        }

        /* chain == 1: 收集后续连续命令 */
        ch        = cmd->config.ch;
        ce        = cmd->config.ce;
        lun       = cmd->config.lun;
        chain_len = 1U;

        while (chain_len < FC_SCHEDULER_CHAIN_MAX)
        {
            fc_command_id_t next_id = fc_submission_queue_peek(scheduler->sq, 0U);
            if (next_id == FC_COMMAND_ID_INVALID)
            {
                break;
            }
            cmd = fc_command_id_to_command(next_id);
            if (cmd == NULL)
            {
                break;
            }

            /* 连续性校验：必须同一 target */
            if ((cmd->config.ch != ch) || (cmd->config.ce != ce) || (cmd->config.lun != lun))
            {
                break;
            }

            /* 弹出并加入链 */
            (void)fc_submission_queue_pop(scheduler->sq, &chain->ids[chain_len]);
            chain_len++;

            if (cmd->config.chain == 0U)
            {
                break; /* chain=0：链结束 */
            }
            /* chain=1：继续收集 */
        }

        chain->count = chain_len;
        return FC_STATUS_OK;
    }

    return (scheduler->sq->count != 0U) ? FC_STATUS_NOT_READY : FC_STATUS_QUEUE_EMPTY;
}

void fc_scheduler_set_busy(fc_scheduler_t *scheduler, uint8_t ch, uint8_t ce, uint8_t lun)
{
    if ((scheduler != NULL) && (ch < CH_NUM) && (ce < CE_NUM) && (lun < LUN_NUM))
    {
        scheduler->busy[ch][ce][lun] = 1U;
    }
}

void fc_scheduler_clear_busy(fc_scheduler_t *scheduler, uint8_t ch, uint8_t ce, uint8_t lun)
{
    if ((scheduler != NULL) && (ch < CH_NUM) && (ce < CE_NUM) && (lun < LUN_NUM))
    {
        scheduler->busy[ch][ce][lun] = 0U;
    }
}