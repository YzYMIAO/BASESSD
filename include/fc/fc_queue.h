#ifndef FC_QUEUE_H
#define FC_QUEUE_H

#include "fc_command.h"

#include <stdint.h>

/*
 * fc_queue.h — SQ/HSQ/CQ 环形队列
 *
 * SQ/HSQ 都是 FC 命令的 FIFO 环形队列。
 * SQ/HSQ 只保存 FC Command Pool 的槽位编号，不复制命令内容。
 * 因而命令、Descriptor 和 CQ 上下文都能使用同一个稳定的 Command Handle 关联。
 *
 * CQ 保存每条命令的完成结果。完成项中的 command 和 failed_descriptor 用于
 * FTL 的错误分析、Read Retry 和 RAID 恢复，不能在 FTL 取走 CQ 前失效。
 *
 * storage 由 FC 内部固定提供，队列不分配也不释放内存。
 */

typedef struct
{
    fc_command_id_t *entries;
    uint16_t         depth;
    uint16_t         head;
    uint16_t         tail;
    uint16_t         count;
} fc_submission_queue_t;

/*
 * CQ 保存每条命令的完成结果。完成项中的 command 和 failed_descriptor 用于
 * FTL 的错误分析、Read Retry 和 RAID 恢复，不能在 FTL 取走 CQ 前失效。
 */
typedef struct
{
    fc_command_status_t *entries;
    uint16_t             depth;
    uint16_t             head;
    uint16_t             tail;
    uint16_t             count;
} fc_completion_queue_t;

/* ── 初始化：storage 由调用方提供（固定数组），队列不分配内存 ── */
fc_status_t fc_submission_queue_init(fc_submission_queue_t *queue, fc_command_id_t *storage, uint16_t depth);
fc_status_t fc_completion_queue_init(fc_completion_queue_t *queue, fc_command_status_t *storage, uint16_t depth);

/* 入队满时返回 FC_STATUS_QUEUE_FULL；出队空时返回 FC_STATUS_QUEUE_EMPTY。
 * push 的 command_id 若为 FC_COMMAND_ID_INVALID（哨兵）属调用者 bug → ASSERT。 */
fc_status_t fc_submission_queue_push(fc_submission_queue_t *queue, fc_command_id_t command_id);
fc_status_t fc_submission_queue_pop(fc_submission_queue_t *queue, fc_command_id_t *command_id);

/* Peek 第 offset 条待出队命令（不消耗），offset=0 即队头。 */
fc_command_id_t fc_submission_queue_peek(const fc_submission_queue_t *queue, uint16_t offset);

fc_status_t fc_completion_queue_push(fc_completion_queue_t *queue, const fc_command_status_t *completion);
fc_status_t fc_completion_queue_pop(fc_completion_queue_t *queue, fc_command_status_t *completion);

#endif /* FC_QUEUE_H */
