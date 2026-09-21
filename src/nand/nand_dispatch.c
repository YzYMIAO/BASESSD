#include "common/ssd_log.h"
#include "fc/fc_command.h"
#include "nand_config.h"
#include "nand_dispatch_internal.h"
#include "nand_mt29f4g08abadawp.h"
#include "onfi_1_0.h"

#include <stddef.h>
#include <string.h>

/* ======================================================================
 * 通道队列及全局状态
 * ====================================================================== */

typedef struct
{
    uint16_t entries[NAND_CHANNEL_QUEUE_DEPTH];
    uint16_t head;
    uint16_t tail;
    uint16_t count;
} nand_channel_queue_t;

static nand_channel_queue_t       g_nand_channel_queues[CH_NUM];
static uint8_t                    g_nand_channel_cursor;
static uint8_t                    g_nand_device_ready;

/* 每 LUN 链状态：原 g_nand_dispatch_tasks[ch][ce][lun] 的一维扩展。
 * 单命令：count=1，tasks[0] 预加载。
 * 链式命令：count=N，tasks[0..N-1] 预加载，poll 从 cursor=0 推进到 N-1。 */
static nand_dispatch_lun_chain_t  g_nand_dispatch_chains[CH_NUM][CE_NUM][LUN_NUM];
static uint8_t                    g_nand_channel_page_buffer[CH_NUM][PAGE_BUFFER_SIZE];

/* ======================================================================
 * 辅助函数
 * ====================================================================== */

static void nand_dispatch_completion_init(nand_dispatch_completion_t *completion, uint16_t command_id)
{
    uint8_t i;

    *completion                      = (nand_dispatch_completion_t){0};
    completion->command_id           = command_id;
    completion->failed_descriptor_id = FC_DESCRIPTOR_ID_INVALID;
    for (i = 0U; i < CW_TOTAL_MAX; i++)
    {
        completion->cw_status[i]     = FC_CW_STATUS_UNUSED;
    }
}

static void nand_dispatch_task_complete(nand_dispatch_task_t *task, uint8_t result,
                                         const nand_fc_completion_t *nand_completion)
{
    task->completion.execution_status = result;
    task->completion.nand_status      = nand_completion->nand_status;

    g_nand_dispatch_completion_table[task->operation](task, result, nand_completion);
}

/* 将 dispatch_request 预加载到指定的 task slot。
 * 被 submit / submit_chain 共用。 */
static void nand_dispatch_task_load(nand_dispatch_task_t *task, const nand_dispatch_request_t *request)
{
    uint8_t cw;

    nand_dispatch_completion_init(&task->completion, request->command_id);
    task->command_id   = request->command_id;
    task->ch           = request->ch;
    task->ce           = request->ce;
    task->lun          = request->lun;
    task->operation    = request->operation;
    task->page_type    = request->page_type;
    task->cw_index     = 0U;
    task->cw_count     = request->cw_count;
    task->plane_count  = request->plane_count;
    task->started      = 0U;
    task->parameter[0] = request->parameter[0];
    task->parameter[1] = request->parameter[1];
    task->parameter[2] = request->parameter[2];
    task->parameter[3] = request->parameter[3];
    task->feature_addr = request->feature_addr;
    for (cw = 0U; cw < request->cw_count; cw++)
    {
        task->cw_paa[0][cw]     = request->cws[0][cw].paa;
        task->cw_buffers[0][cw] = request->cws[0][cw].buffer;
        task->cw_paa[1][cw]     = request->cws[1][cw].paa;
        task->cw_buffers[1][cw] = request->cws[1][cw].buffer;
    }
}

/* ======================================================================
 * 初始化 / 就绪
 * ====================================================================== */

uint8_t nand_dispatch_init(void)
{
    uint8_t channel;
    uint8_t ce;
    uint8_t lun;

    memset(g_nand_channel_queues, 0, sizeof(g_nand_channel_queues));
    memset(g_nand_dispatch_chains, 0, sizeof(g_nand_dispatch_chains));
    g_nand_channel_cursor = 0U;
    g_nand_device_ready   = 0U;
    return NAND_RESULT_OK;
}

void nand_dispatch_set_device_ready(uint8_t ready)
{
    g_nand_device_ready = (ready != 0U) ? 1U : 0U;
}

/* ======================================================================
 * submit — 单命令入队
 * ====================================================================== */

uint8_t nand_dispatch_submit(const nand_dispatch_request_t *request)
{
    nand_channel_queue_t      *queue;
    nand_dispatch_lun_chain_t *chain;

    if ((g_nand_device_ready == 0U) || (request == NULL) || (request->ch >= CH_NUM) ||
        (request->ce >= CE_NUM) || (request->lun >= LUN_NUM) ||
        (request->cw_count > CW_NUM))
    {
        return NAND_RESULT_INVALID_ARGUMENT;
    }

    chain = &g_nand_dispatch_chains[request->ch][request->ce][request->lun];
    if (chain->count != 0U)
    {
        return NAND_RESULT_NOT_READY; /* LUN 忙 */
    }

    queue = &g_nand_channel_queues[request->ch];
    if (queue->count == NAND_CHANNEL_QUEUE_DEPTH)
    {
        return NAND_RESULT_NOT_READY;
    }

    /* 预加载到 tasks[0] */
    nand_dispatch_task_load(&chain->tasks[0U], request);
    chain->count  = 1U;
    chain->cursor = 0U;

    /* 入队 command_id */
    queue->entries[queue->tail] = (uint16_t)request->command_id;
    queue->tail                 = (uint16_t)((queue->tail + 1U) % NAND_CHANNEL_QUEUE_DEPTH);
    queue->count++;
    return NAND_RESULT_OK;
}

/* ======================================================================
 * submit_chain — 链式命令原子入队
 *
 * 数据完整性保证：
 *   1. 队列剩余空间不足整链时返回 NOT_READY，无副作用
 *   2. LUN chain buffer 非空闲时返回 NOT_READY，无副作用
 *   3. 入队后链中命令按序执行，HSQ 无法插入
 * ====================================================================== */

uint8_t nand_dispatch_submit_chain(const nand_dispatch_request_t *requests, uint8_t count)
{
    nand_channel_queue_t      *queue;
    nand_dispatch_lun_chain_t *chain;
    uint8_t                    i;

    if ((requests == NULL) || (count == 0U) || (count > NAND_DISPATCH_CHAIN_DEPTH))
    {
        return NAND_RESULT_INVALID_ARGUMENT;
    }
    if ((requests[0U].ch >= CH_NUM) || (requests[0U].ce >= CE_NUM) || (requests[0U].lun >= LUN_NUM))
    {
        return NAND_RESULT_INVALID_ARGUMENT;
    }

    chain = &g_nand_dispatch_chains[requests[0U].ch][requests[0U].ce][requests[0U].lun];
    if (chain->count != 0U)
    {
        return NAND_RESULT_NOT_READY; /* LUN 链正忙 */
    }

    queue = &g_nand_channel_queues[requests[0U].ch];

    /* 原子性检查：空间必须容纳整链 */
    if ((uint16_t)(queue->count + count) > NAND_CHANNEL_QUEUE_DEPTH)
    {
        return NAND_RESULT_NOT_READY;
    }

    /* 预加载整链到 tasks[0..count-1] */
    for (i = 0U; i < count; i++)
    {
        nand_dispatch_task_load(&chain->tasks[i], &requests[i]);
        queue->entries[queue->tail] = requests[i].command_id;
        queue->tail                 = (uint16_t)((queue->tail + 1U) % NAND_CHANNEL_QUEUE_DEPTH);
        queue->count++;
    }

    chain->count  = count;
    chain->cursor = 0U;
    return NAND_RESULT_OK;
}

/* ======================================================================
 * poll — 推进各 LUN 活跃事务 + 从通道队列出队新命令
 *
 * 两阶段设计：
 *   Phase 1 — 遍历各 Channel/CE/LUN 的 chain buffer，step 活跃事务。
 *              当前命令完成后自动推进 cursor，不清 LUN busy。
 *              整链结束（cursor+1==count）才返回 completion。
 *   Phase 2 — 从通道队列出队 command_id，启动 LUN 的第一个命令。
 * ====================================================================== */

uint8_t nand_dispatch_poll(nand_dispatch_completion_t *completion)
{
    uint8_t channel;
    uint8_t count;

    if (completion == NULL)
    {
        return NAND_RESULT_INVALID_ARGUMENT;
    }

    for (count = 0U; count < CH_NUM; count++)
    {
        channel                     = (uint8_t)((g_nand_channel_cursor + count) % CH_NUM);
        nand_channel_queue_t *queue = &g_nand_channel_queues[channel];
        uint8_t               ce;
        uint8_t               lun_idx;

        /* ── Phase 1: Step 各 LUN 活跃事务 ── */
        for (ce = 0U; ce < CE_NUM; ce++)
        {
            for (lun_idx = 0U; lun_idx < LUN_NUM; lun_idx++)
            {
                nand_dispatch_lun_chain_t *chain = &g_nand_dispatch_chains[channel][ce][lun_idx];
                nand_fc_completion_t       nand_completion;
                uint8_t                    result;

                if (chain->count == 0U)
                {
                    continue;
                }

                /* 内层 while：单个链命令完成 → CW chaining → 链推进 → 继续 step */
                while (1)
                {
                    nand_dispatch_task_t *task = &chain->tasks[chain->cursor];

                    if (task->started == 0U)
                    {
                        break; /* 等 Phase 2 启动第一个命令 */
                    }

                    result = nand_fc_step(&task->transaction, &nand_completion);
                    if (result == NAND_RESULT_NOT_READY)
                    {
                        break;
                    }

                    /* ── CW 级 chaining（多 CW READ 的 RANDOM_DATA_READ 链） ── */
                    for (;;)
                    {
                        nand_dispatch_task_complete(task, result, &nand_completion);
                        if ((result != NAND_RESULT_OK) ||
                            ((task->operation != OP_NORMAL_READ_PAGE_WITH_DATA_OUT) && (task->operation != OP_NORMAL_READ_PAGE_WITHOUT_DATA_OUT) && (task->operation != OP_OTP_DATA_READ)) ||
                            (task->cw_index + 1U >= task->cw_count))
                        {
                            break;
                        }
                        task->cw_index++;
                        task->operation = OP_RANDOM_DATA_READ;
                        task->started   = 1U;
                        {
                            nand_paa_t cw_paa    = task->cw_paa[0][task->cw_index];
                            uint8_t   *cw_buffer = task->cw_buffers[0][task->cw_index];
                            uint16_t   cw_column = (uint16_t)(task->cw_index * (PAGE_SIZE / 4U));
                            task->request        = (nand_fc_request_t){.column = cw_column, .row = {paa_to_nand_row(cw_paa)}, .data = cw_buffer, .data_length = (uint16_t)(PAGE_SIZE / 4U)};
                        }
                        result = nand_fc_start(&task->transaction, task->operation, &task->request);
                        if (result == NAND_RESULT_NOT_READY)
                        {
                            break;
                        }
                        nand_completion.execution_status = result;
                        nand_completion.nand_status      = task->transaction.completion.nand_status;
                    }
                    if (result == NAND_RESULT_NOT_READY)
                    {
                        break;
                    }

                    /* Cache reads: 把 page_buffer 拆回各 CW buffer */
                    if ((task->operation == OP_READ_PAGE_CACHE_SEQ) ||
                        (task->operation == OP_READ_PAGE_CACHE_RANDOM) ||
                        (task->operation == OP_READ_PAGE_CACHE_LAST))
                    {
                        uint8_t cache_cw;
                        for (cache_cw = 0U; cache_cw < task->cw_count; cache_cw++)
                        {
                            memcpy(task->cw_buffers[0][cache_cw],
                                   &g_nand_channel_page_buffer[channel][cache_cw * (PAGE_SIZE / 4U)],
                                   PAGE_SIZE / 4U);
                        }
                    }

                    /* ── 判断链是否还有下一命令 ── */
                    if (chain->cursor + 1U < chain->count)
                    {
                        /* 不清 LUN busy，自动推进到下一命令 */
                        chain->cursor++;
                        task = &chain->tasks[chain->cursor];

                        result = g_nand_dispatch_req_table[task->operation](task);
                        if (result != NAND_RESULT_OK)
                        {
                            chain->count = 0U;
                            return NAND_RESULT_INVALID_ARGUMENT;
                        }
                        result = nand_fc_start(&task->transaction, task->operation, &task->request);
                        task->started = 1U;
                        if (result == NAND_RESULT_NOT_READY)
                        {
                            break;
                        }
                        /* 下个命令同步完成 —> while(1) 继续 step */
                        nand_completion.execution_status = result;
                        nand_completion.nand_status      = task->transaction.completion.nand_status;
                        continue;
                    }

                    /* 整链结束 → 返回 completion */
                    *completion       = task->completion;
                    chain->count      = 0U;
                    g_nand_channel_cursor = (uint8_t)((channel + 1U) % CH_NUM);
                    return result;
                }
            }
        }

        /* ── Phase 2: 从通道队列出队新命令 ── */
        if (queue->count != 0U)
        {
            uint16_t                  command_id = queue->entries[queue->head];
            nand_dispatch_lun_chain_t *chain;
            nand_dispatch_task_t      *task;
            nand_fc_completion_t       nand_completion;
            uint8_t                    result;

            /* 从 command_id 反查 ch/ce/lun（队列中所有命令在同一 channel，但 ce/lun 不同）*/
            /* 当前为单 CE/LUN 简化版：CE=0, LUN=0 */
            chain = &g_nand_dispatch_chains[channel][0U][0U];
            if (chain->count != 0U)
            {
                continue; /* LUN busy */
            }

            queue->head = (uint16_t)((queue->head + 1U) % NAND_CHANNEL_QUEUE_DEPTH);
            queue->count--;

            task = &chain->tasks[0U];
            /* task 已由 submit/submit_chain 预加载 */

            result = g_nand_dispatch_req_table[task->operation](task);
            if (result != NAND_RESULT_OK)
            {
                chain->count = 0U;
                return NAND_RESULT_INVALID_ARGUMENT;
            }

            SSD_LOGD("nand: FC dispatch op=%u buffer=%p length=%u column=0x%04X row=0x%06lX\r\n",
                     task->operation,
                     (void *)task->request.data,
                     task->request.data_length,
                     task->request.column,
                     (unsigned long)task->request.row);

            result        = nand_fc_start(&task->transaction, task->operation, &task->request);
            task->started = 1U;
            if (result == NAND_RESULT_NOT_READY)
            {
                continue;
            }
            nand_completion.execution_status = result;
            nand_completion.nand_status      = task->transaction.completion.nand_status;
            nand_dispatch_task_complete(task, result, &nand_completion);

            /* 单命令立即完成 — 检查是否在链中（count=1 表示单命令） */
            if (chain->count > 1U)
            {
                /* 链中还有后续命令，由 Phase 1 继续推进 */
                continue;
            }
            *completion  = task->completion;
            chain->count = 0U;
            return result;
        }
    }
    return NAND_RESULT_NOT_READY;
}