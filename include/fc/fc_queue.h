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
 * CQ 保存每条命令的完成结果。完成项中的 command_id / failed_cw / failed_desc_id
 * 用于 FTL 的错误分析、Read Retry 和 RAID 恢复，不能在 FTL 取走 CQ 前失效。
 *
 * storage 由 FC 内部固定提供，队列不分配也不释放内存。
 */

typedef struct FC_SUBMISSION_QUEUE_S
{
    fc_cmd_id_t *entries; /* 调用方提供的固定存储 */
    WORD         depth;   /* 容量（非 0） */
    WORD         head;    /* 出队游标 */
    WORD         tail;    /* 入队游标 */
    WORD         count;   /* 当前条数（0..depth） */
} fc_sub_queue_s;

typedef struct FC_COMPLETION_QUEUE_S
{
    fc_cmd_status_s *entries; /* 调用方提供的固定存储 */
    WORD             depth;   /* 容量（非 0） */
    WORD             head;    /* 出队游标 */
    WORD             tail;    /* 入队游标 */
    WORD             count;   /* 当前条数（0..depth） */
} fc_comp_queue_s;

/* ── 初始化：storage 由调用方提供（固定数组），队列不分配内存 ── */

/**
 * @brief 初始化 SQ/HSQ 环形队列（storage 由调用方提供）。
 *
 * @param queue 队列
 * @param storage 命令 ID 数组（长度 depth）
 * @param depth 容量
 *
 * @note 空指针 / depth=0 由 ASSERT 拦截；本函数无运行期失败路径。
 */
void fc_submission_queue_init(fc_sub_queue_s *queue, fc_cmd_id_t *storage, WORD depth);

/**
 * @brief 初始化 CQ 环形队列（storage 由调用方提供）。
 *
 * @param queue 队列
 * @param storage 完成项数组
 * @param depth 容量。
 *
 * @note 空指针 / depth=0 由 ASSERT 拦截；本函数无运行期失败路径。
 */
void fc_completion_queue_init(fc_comp_queue_s *queue, fc_cmd_status_s *storage, WORD depth);

/**
 * @brief 把命令 ID 入队尾。
 *
 * @param queue 队列
 * @param command_id 命令 ID，须为 FC 自铸的合法句柄
 *
 * @return FC_STATUS_OK
 *         FC_STATUS_QUEUE_FULL（队列已满）
 *
 * @note command_id 由 fc_submit / submit_batch 的命令级校验保证非 INVALID。
 */
fc_status_e fc_submission_queue_push(fc_sub_queue_s *queue, fc_cmd_id_t command_id);

/**
 * @brief 从队头弹出命令 ID。
 *
 * @param queue 队列
 * @param command_id 输出
 *
 * @return FC_STATUS_OK
 *         FC_STATUS_QUEUE_EMPTY（队列为空）
 */
fc_status_e fc_submission_queue_pop(fc_sub_queue_s *queue, fc_cmd_id_t *command_id);

/**
 * @brief Peek 第 offset 条待出队命令（不消耗），offset=0 即队头。
 *
 * @param queue 队列
 * @param offset 相对队头的偏移
 *
 * @return 命令 ID；offset 越界返回 FC_CMD_ID_INVALID（调用方据此判链尾）。
 */
fc_cmd_id_t fc_submission_queue_peek(const fc_sub_queue_s *queue, WORD offset);

/**
 * @brief 完成项入队尾（整个 fc_cmd_status_s 拷贝）。
 *
 * @param queue 队列
 * @param completion 待入队完成项
 *
 * @return FC_STATUS_OK
 *         FC_STATUS_QUEUE_FULL（队列已满）
 */
fc_status_e fc_completion_queue_push(fc_comp_queue_s *queue, const fc_cmd_status_s *completion);

/**
 * @brief 从队头弹出完成项。
 *
 * @param queue 队列
 * @param completion 输出
 *
 * @return FC_STATUS_OK
 *         FC_STATUS_QUEUE_EMPTY（队列为空）
 */
fc_status_e fc_completion_queue_pop(fc_comp_queue_s *queue, fc_cmd_status_s *completion);

#endif /* FC_QUEUE_H */
