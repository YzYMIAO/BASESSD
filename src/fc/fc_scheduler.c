/*
 * fc_scheduler.c — FC 调度器状态
 *
 * 职责：承载调度器状态（SQ/HSQ 绑定）与命令链描述类型。
 *
 * 历史：本文件原实现 select/select_chain + per-LUN busy 表 + 读饥饿保护。
 * 改为 per-LUN FIFO 模型后全部删除：
 *   - busy 表是死代码（生产零调用），"忙"改由 NAND 侧 nand_dispatch_lun_free() 表达；
 *   - 选择逻辑搬进 fc.c 的填充 pass（fc_poll_fill）——按 LUN 保序、HSQ 优先、
 *     一次扫描把能放下的命令（含链）提交进 NAND per-LUN FIFO；
 *   - 读饥饿保护（consecutive_write_count）随选择逻辑一并移除。
 */

#include "fc_scheduler.h"

#include "assertion.h"

/* --------------------------------------------------------------------
 * fc_scheduler_init
 *   功能：初始化调度器（绑定 SQ/HSQ）。
 *   参数：scheduler —— 调度器；sq/hsq —— 队列指针。
 *   返回：FC_STATUS_OK。
 *   实现要点：入参前置条件由调用者保证（fc.c 传入本池的调度器与两条队列）。
 * -------------------------------------------------------------------- */
fc_status_e fc_scheduler_init(fc_scheduler_t *scheduler, fc_submission_queue_t *sq, fc_submission_queue_t *hsq)
{
    ASSERT(scheduler != NULL);
    ASSERT(sq != NULL);
    ASSERT(hsq != NULL);

    scheduler->sq  = sq;
    scheduler->hsq = hsq;
    return FC_STATUS_OK;
}
