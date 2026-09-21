/*
 * nand_dispatch_tables.c
 *
 * 函数指针表：以 OP_* 为下标，替代 nand_dispatch.c 中的 if-else / switch 链。
 *
 * 设计原则：
 *   - 每个 handler 只负责一件事（request 构建 / completion 策略）
 *   - 函数指针表为 O(1) 直接跳转，无分支预测惩罚
 *   - 新操作加入只需加 handler + 一行表项
 */

#include "common/ssd_log.h"
#include "nand_config.h"
#include "nand_dispatch_internal.h"

#include <stddef.h>
#include <string.h>

/* ======================================================================
 * Completion 策略 handler
 * ====================================================================== */

/* 全局 page_buffer 引用（与 nand_dispatch.c 中相同） */
extern uint8_t              g_nand_channel_page_buffer[CH_NUM][PAGE_BUFFER_SIZE];
extern nand_dispatch_task_t g_nand_dispatch_tasks[CH_NUM][CE_NUM][LUN_NUM];

/* --------------------------------------------------------------------
 * completion_all_cws
 *   PROGRAM / ERASE：所有 CW 共享同一完成状态
 * -------------------------------------------------------------------- */
static void completion_all_cws(nand_dispatch_task_t *task, uint8_t result, const nand_fc_completion_t *nand_completion)
{
    uint8_t status = (result == NAND_RESULT_OK)        ? FC_CW_STATUS_SUCCESS
                     : (result == NAND_RESULT_TIMEOUT) ? FC_CW_STATUS_TIMEOUT
                                                       : FC_CW_STATUS_NAND_FAIL;
    uint8_t cw;

    (void)nand_completion;
    for (cw = 0U; cw < task->cw_count; cw++)
    {
        task->completion.cw_status[cw] = status;
    }
    if (result != NAND_RESULT_OK)
    {
        task->completion.failed_descriptor_id = 0U;
    }
}

/* --------------------------------------------------------------------
 * completion_per_cw
 *   READ / CACHE_READ：每 CW 独立完成状态
 * -------------------------------------------------------------------- */
static void completion_per_cw(nand_dispatch_task_t *task, uint8_t result, const nand_fc_completion_t *nand_completion)
{
    uint8_t status = (result == NAND_RESULT_OK)        ? FC_CW_STATUS_SUCCESS
                     : (result == NAND_RESULT_TIMEOUT) ? FC_CW_STATUS_TIMEOUT
                                                       : FC_CW_STATUS_NAND_FAIL;

    (void)nand_completion;
    task->completion.cw_status[task->cw_index] = status;
    if (result != NAND_RESULT_OK)
    {
        task->completion.failed_descriptor_id = task->cw_index;
    }
}

/* --------------------------------------------------------------------
 * completion_single
 *   默认：单 CW 状态
 * -------------------------------------------------------------------- */
static void completion_single(nand_dispatch_task_t *task, uint8_t result, const nand_fc_completion_t *nand_completion)
{
    uint8_t status = (result == NAND_RESULT_OK)        ? FC_CW_STATUS_SUCCESS
                     : (result == NAND_RESULT_TIMEOUT) ? FC_CW_STATUS_TIMEOUT
                                                       : FC_CW_STATUS_NAND_FAIL;

    (void)nand_completion;
    task->completion.cw_status[0] = status;
}

/* ======================================================================
 * Request 构建 handler
 * ====================================================================== */

/* --------------------------------------------------------------------
 * req_program_page
 *   NORMAL_PROGRAM_PAGE / OTP_DATA_PROGRAM / PROGRAM_PAGE_CACHE
 *   将各 CW buffer 拼接到 page_buffer 中，一次性写入
 * -------------------------------------------------------------------- */
static uint8_t req_program_page(nand_dispatch_task_t *task)
{
    uint8_t  cw;
    uint8_t *page_buffer = g_nand_channel_page_buffer[task->ch];

    memset(page_buffer, 0xFF, PAGE_SIZE);
    for (cw = 0U; cw < task->cw_count; cw++)
    {
        if (task->cw_buffers[0][cw] == NULL)
        {
            return NAND_RESULT_INVALID_ARGUMENT;
        }
        memcpy(&page_buffer[cw * (PAGE_SIZE / 4U)], task->cw_buffers[0][cw], PAGE_SIZE / 4U);
    }
    task->request = (nand_fc_request_t){.column = 0U, .row = {paa_to_nand_row(task->cw_paa[0][0])}, .data = page_buffer, .data_length = PAGE_SIZE};
    return NAND_RESULT_OK;
}

/* --------------------------------------------------------------------
 * req_read_page
 *   NORMAL_READ_PAGE / OTP_DATA_READ
 *   首次 CW0 读取（后续 CW 由 poll() 中的 chaining 循环通过
 *   OP_RANDOM_DATA_READ 处理）
 * -------------------------------------------------------------------- */
static uint8_t req_read_page(nand_dispatch_task_t *task)
{
    uint8_t *cw_buffer = task->cw_buffers[0][0];
    if (cw_buffer == NULL)
    {
        return NAND_RESULT_INVALID_ARGUMENT;
    }
    task->request = (nand_fc_request_t){.column = 0U, .row = {paa_to_nand_row(task->cw_paa[0][0])}, .data = cw_buffer, .data_length = (uint16_t)(PAGE_SIZE / 4U)};
    return NAND_RESULT_OK;
}

/* --------------------------------------------------------------------
 * req_cache_read
 *   READ_PAGE_CACHE_SEQ / READ_PAGE_CACHE_RANDOM / READ_PAGE_CACHE_LAST
 *   一次性读 full page 到 page_buffer（后续由 poll() 拆回各 CW buffer）
 * -------------------------------------------------------------------- */
static uint8_t req_cache_read(nand_dispatch_task_t *task)
{
    uint8_t  cw;
    uint8_t *page_buffer = g_nand_channel_page_buffer[task->ch];

    for (cw = 0U; cw < task->cw_count; cw++)
    {
        if (task->cw_buffers[0][cw] == NULL)
        {
            return NAND_RESULT_INVALID_ARGUMENT;
        }
    }
    memset(page_buffer, 0x00, PAGE_SIZE);
    task->request = (nand_fc_request_t){.column = 0U, .row = {paa_to_nand_row(task->cw_paa[0][0])}, .data = page_buffer, .data_length = PAGE_SIZE};
    return NAND_RESULT_OK;
}

/* --------------------------------------------------------------------
 * req_erase
 *   ERASE_BLOCK
 * -------------------------------------------------------------------- */
static uint8_t req_erase(nand_dispatch_task_t *task)
{
    task->request = (nand_fc_request_t){.column = 0U, .row = {paa_to_nand_row(task->cw_paa[0][0])}, .data = NULL, .data_length = 0U};
    return NAND_RESULT_OK;
}

/* --------------------------------------------------------------------
 * req_feature
 *   SET_FEATURES / GET_FEATURES
 *   column = feature_addr, data = cw_buffers[0]（已由 FC 重定向到
 *   command->parameter0..3）
 * -------------------------------------------------------------------- */
static uint8_t req_feature(nand_dispatch_task_t *task)
{
    task->request = (nand_fc_request_t){.column = (uint16_t)task->feature_addr, .row = {0U}, .data = task->cw_buffers[0][0], .data_length = 4U};
    return NAND_RESULT_OK;
}

/* --------------------------------------------------------------------
 * req_single
 *   固定长度数据传输（读/写），包括 READ_ID、READ_PARAMETER_PAGE、
 *   RANDOM_DATA_READ、RANDOM_DATA_INPUT、READ_STATUS 等。
 *   长度从 g_nand_operation_map[].data_length 获取。
 * -------------------------------------------------------------------- */
static uint8_t req_single(nand_dispatch_task_t *task)
{
    uint16_t data_length = g_nand_operation_map[task->operation].data_length;

    task->request = (nand_fc_request_t){
        .column = 0U,
        .row = {(task->cw_count != 0U) ? paa_to_nand_row(task->cw_paa[0][0]) : 0U},
        .data = task->cw_buffers[0][0],
        .data_length = data_length};
    return NAND_RESULT_OK;
}

/* --------------------------------------------------------------------
 * req_no_data
 *   RESET、READ_MODE、BLOCK_LOCK、BLOCK_LOCK_TIGHT、BLOCK_UNLOCK 等
 *   无数据传输，纯地址操作
 * -------------------------------------------------------------------- */
static uint8_t req_no_data(nand_dispatch_task_t *task)
{
    task->request = (nand_fc_request_t){
        .column = 0U,
        .row = {(task->cw_count != 0U) ? paa_to_nand_row(task->cw_paa[0][0]) : 0U},
        .data = NULL,
        .data_length = 0U};
    return NAND_RESULT_OK;
}

/* --------------------------------------------------------------------
 * req_multi_plane_read
 *   OP_READ_PAGE_MULTI_PLANE
 *   将双页数据一次性读入 page_buffer（后续由 poll 拆回各 plane 各 CW buffer）
 * -------------------------------------------------------------------- */
static uint8_t req_multi_plane_read(nand_dispatch_task_t *task)
{
    uint8_t *page_buffer = g_nand_channel_page_buffer[task->ch];

    memset(page_buffer, 0x00, PAGE_BUFFER_SIZE);

    task->request = (nand_fc_request_t){
        .column = 0U,
        .row = {paa_to_nand_row(task->cw_paa[0][0]),
                paa_to_nand_row(task->cw_paa[1][0])},
        .data = page_buffer,
        .data_length = PAGE_BUFFER_SIZE};
    return NAND_RESULT_OK;
}

/* --------------------------------------------------------------------
 * req_multi_plane_program
 *   OP_PROGRAM_PAGE_MULTI_PLANE
 *   各 plane 各 CW buffer 拼接到 page_buffer，一次性写入
 * -------------------------------------------------------------------- */
static uint8_t req_multi_plane_program(nand_dispatch_task_t *task)
{
    uint8_t  plane, cw;
    uint8_t *page_buffer = g_nand_channel_page_buffer[task->ch];

    memset(page_buffer, 0xFF, PAGE_BUFFER_SIZE);
    for (plane = 0U; plane < PLANE_NUM; plane++)
    {
        for (cw = 0U; cw < task->cw_count; cw++)
        {
            if (task->cw_buffers[plane][cw] == NULL)
            {
                return NAND_RESULT_INVALID_ARGUMENT;
            }
            memcpy(&page_buffer[plane * PAGE_SIZE + cw * (PAGE_SIZE / CW_NUM)],
                   task->cw_buffers[plane][cw], PAGE_SIZE / CW_NUM);
        }
    }

    task->request = (nand_fc_request_t){
        .column = 0U,
        .row = {paa_to_nand_row(task->cw_paa[0][0]),
                paa_to_nand_row(task->cw_paa[1][0])},
        .data = page_buffer,
        .data_length = PAGE_BUFFER_SIZE};
    return NAND_RESULT_OK;
}

/* --------------------------------------------------------------------
 * req_multi_plane_erase
 *   OP_ERASE_BLOCK_MULTI_PLANE
 *   两个 plane 行地址，无数据
 * -------------------------------------------------------------------- */
static uint8_t req_multi_plane_erase(nand_dispatch_task_t *task)
{
    task->request = (nand_fc_request_t){
        .column = 0U,
        .row = {paa_to_nand_row(task->cw_paa[0][0]),
                paa_to_nand_row(task->cw_paa[1][0])},
        .data = NULL,
        .data_length = 0U};
    return NAND_RESULT_OK;
}

/* --------------------------------------------------------------------
 * completion_multi_plane
 *   多 plane 操作 completion：遍历 plane × CW，逐 CW 设置状态
 * -------------------------------------------------------------------- */
static void completion_multi_plane(nand_dispatch_task_t *task, uint8_t result,
                                    const nand_fc_completion_t *nand_completion)
{
    uint8_t status = (result == NAND_RESULT_OK)        ? FC_CW_STATUS_SUCCESS
                     : (result == NAND_RESULT_TIMEOUT) ? FC_CW_STATUS_TIMEOUT
                                                       : FC_CW_STATUS_NAND_FAIL;
    uint8_t plane, cw, flat_idx;

    (void)nand_completion;
    for (plane = 0U; plane < PLANE_NUM; plane++)
    {
        for (cw = 0U; cw < task->cw_count; cw++)
        {
            flat_idx = plane * CW_NUM + cw;
            if (flat_idx < (CW_NUM * PLANE_NUM))
            {
                task->completion.cw_status[flat_idx] = status;
            }
        }
    }
    if (result != NAND_RESULT_OK)
    {
        task->completion.failed_descriptor_id = 0U;
    }
}

/* ======================================================================
 * 函数指针表定义
 * ====================================================================== */

const nand_dispatch_completion_handler_t g_nand_dispatch_completion_table[OP_COUNT] = {
    [OP_RESET]                          = completion_single,
    [OP_READ_ID]                        = completion_single,
    [OP_READ_PARAMETER_PAGE]            = completion_single,
    [OP_READ_UNIQUE_ID]                 = completion_single,
    [OP_GET_FEATURES]                   = completion_single,
    [OP_SET_FEATURES]                   = completion_single,
    [OP_READ_STATUS]                    = completion_single,
    [OP_READ_STATUS_ENHANCED]           = completion_single,
    [OP_RANDOM_DATA_READ]               = completion_single,
    [OP_RANDOM_DATA_INPUT]              = completion_single,
    [OP_PROGRAM_FOR_INTERNAL_DATA_MOVE] = completion_single,
    [OP_READ_MODE]                      = completion_single,
    [OP_NORMAL_READ_PAGE_WITH_DATA_OUT]    = completion_per_cw,
    [OP_NORMAL_READ_PAGE_WITHOUT_DATA_OUT]  = completion_per_cw,
    [OP_READ_PAGE_CACHE_SEQ]            = completion_per_cw,
    [OP_READ_PAGE_CACHE_RANDOM]         = completion_per_cw,
    [OP_READ_PAGE_CACHE_LAST]           = completion_per_cw,
    [OP_NORMAL_PROGRAM_PAGE]            = completion_all_cws,
    [OP_PROGRAM_PAGE_CACHE]             = completion_all_cws,
    [OP_ERASE_BLOCK]                    = completion_all_cws,
    [OP_READ_FOR_INTERNAL_DATA_MOVE]    = completion_single,
    [OP_PROGRAM_FOR_INTERNAL_MOVE]      = completion_single,
    [OP_BLOCK_UNLOCK_LOW]               = completion_single,
    [OP_BLOCK_UNLOCK_HIGH]              = completion_single,
    [OP_BLOCK_LOCK]                     = completion_single,
    [OP_BLOCK_LOCK_TIGHT]               = completion_single,
    [OP_BLOCK_LOCK_STATUS]              = completion_single,
    [OP_OTP_DATA_LOCK]                  = completion_single,
    [OP_OTP_DATA_PROGRAM]               = completion_all_cws,
    [OP_OTP_DATA_READ]                  = completion_per_cw,
    [OP_READ_PAGE_MULTI_PLANE_WITHOUT_DATA_OUT] = completion_multi_plane,
    [OP_READ_PAGE_MULTI_PLANE_WITH_DATA_OUT]    = completion_multi_plane,
    [OP_PROGRAM_PAGE_MULTI_PLANE]       = completion_multi_plane,
    [OP_ERASE_BLOCK_MULTI_PLANE]        = completion_multi_plane,
};

const nand_dispatch_req_handler_t g_nand_dispatch_req_table[OP_COUNT] = {
    [OP_RESET]                          = req_no_data,
    [OP_READ_ID]                        = req_single,
    [OP_READ_PARAMETER_PAGE]            = req_single,
    [OP_READ_UNIQUE_ID]                 = req_single,
    [OP_GET_FEATURES]                   = req_feature,
    [OP_SET_FEATURES]                   = req_feature,
    [OP_READ_STATUS]                    = req_single,
    [OP_READ_STATUS_ENHANCED]           = req_single,
    [OP_RANDOM_DATA_READ]               = req_single,
    [OP_RANDOM_DATA_INPUT]              = req_single,
    [OP_PROGRAM_FOR_INTERNAL_DATA_MOVE] = req_single,
    [OP_READ_MODE]                      = req_no_data,
    [OP_NORMAL_READ_PAGE_WITH_DATA_OUT]    = req_read_page,
    [OP_NORMAL_READ_PAGE_WITHOUT_DATA_OUT]  = req_read_page,
    [OP_READ_PAGE_CACHE_SEQ]            = req_cache_read,
    [OP_READ_PAGE_CACHE_RANDOM]         = req_cache_read,
    [OP_READ_PAGE_CACHE_LAST]           = req_cache_read,
    [OP_NORMAL_PROGRAM_PAGE]            = req_program_page,
    [OP_PROGRAM_PAGE_CACHE]             = req_program_page,
    [OP_ERASE_BLOCK]                    = req_erase,
    [OP_READ_FOR_INTERNAL_DATA_MOVE]    = req_no_data,
    [OP_PROGRAM_FOR_INTERNAL_MOVE]      = req_single,
    [OP_BLOCK_UNLOCK_LOW]               = req_no_data,
    [OP_BLOCK_UNLOCK_HIGH]              = req_no_data,
    [OP_BLOCK_LOCK]                     = req_no_data,
    [OP_BLOCK_LOCK_TIGHT]               = req_no_data,
    [OP_BLOCK_LOCK_STATUS]              = req_single,
    [OP_OTP_DATA_LOCK]                  = req_no_data,
    [OP_OTP_DATA_PROGRAM]               = req_program_page,
    [OP_OTP_DATA_READ]                  = req_read_page,
    [OP_READ_PAGE_MULTI_PLANE_WITHOUT_DATA_OUT] = req_multi_plane_read,
    [OP_READ_PAGE_MULTI_PLANE_WITH_DATA_OUT]    = req_multi_plane_read,
    [OP_PROGRAM_PAGE_MULTI_PLANE]       = req_multi_plane_program,
    [OP_ERASE_BLOCK_MULTI_PLANE]        = req_multi_plane_erase,
};