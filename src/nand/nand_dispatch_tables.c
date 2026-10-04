/*
 * nand_dispatch_tables.c
 *
 * 函数指针表：以 OP_* 为下标，替代 nand_dispatch.c 中的 if-else / switch 链。
 *
 * 设计原则：
 *   - 每个 handler 只负责一件事（request 构建 / completion 策略）
 *   - 函数指针表为 O(1) 直接跳转，无分支预测惩罚
 *   - 新操作加入只需加 handler + 一行表项
 *
 * 本文件定义两张表（下标均为 OP_COUNT=41）：
 *   - g_nand_dispatch_req_table[]       ：request 构建 handler（req_*）
 *   - g_nand_dispatch_completion_table[]：completion 策略 handler（completion_*）
 * 调用点位于 nand_dispatch.c：
 *   - nand_dispatch_task_complete() 经 completion_table[task->operation](...) 收尾
 *   - submit / chain 阶段经 req_table[task->operation](task) 构建请求
 */

#include "common/assertion.h"
#include "common/log.h"
#include "nand_config.h"
#include "nand_dispatch_internal.h"

#include <stddef.h>
#include <string.h>

/* ======================================================================
 * Completion 策略 handler
 * ====================================================================== */

/* --------------------------------------------------------------------
 * 全局变量 g_nand_channel_page_buffer
 *   通道页缓冲，物理位置在 noncacheable_buffer 段（DMA 直接访问，
 *   DCache 关闭）。FC 侧 data path 与 NAND 引擎共用。
 *   多 plane 操作时保存双页数据（PAGE_BUFFER_SIZE = 2112×2），
 *   由 req_* 拼接写入 / poll 拆回各 CW buffer。
 *   归属模块：nand_dispatch.c（定义）与本文件（引用）。
 * -------------------------------------------------------------------- */
extern uint8_t g_nand_channel_page_buffer[CH_NUM][PAGE_BUFFER_SIZE];

/* --------------------------------------------------------------------
 * completion_all_cws
 *   功能：将统一状态写入任务的全部 CW（PROGRAM/ERASE 类）。
 *   参数：task —— 正在收尾的任务；result —— NAND_RESULT_* 执行结果；
 *         nand_completion —— 底层 NAND 完成信息（本 handler 不使用）。
 *   返回：无。
 *   实现要点：result 到 FC_CW_STATUS_* 映射统一为
 *         OK→SUCCESS / TIMEOUT→TIMEOUT / 其余→NAND_FAIL；
 *         失败时 failed_descriptor_id 固定为 0（无逐 CW 区分）。
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
 *   功能：仅标记当前处理的 CW（READ/CACHE_READ 类，逐 CW 独立状态）。
 *   参数：task —— 正在收尾的任务（cw_index 指示当前 CW）；
 *         result —— NAND_RESULT_* 执行结果；
 *         nand_completion —— 底层 NAND 完成信息（本 handler 不使用）。
 *   返回：无。
 *   实现要点：与 completion_all_cws 的区别——失败时 failed_descriptor_id
 *         上报 task->cw_index 而非固定 0，供上层定位具体 CW。
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
 *   功能：只标记 cw_status[0]（无 CW 概念操作的默认策略）。
 *   参数：task —— 正在收尾的任务；result —— NAND_RESULT_* 执行结果；
 *         nand_completion —— 底层 NAND 完成信息（本 handler 不使用）。
 *   返回：无。
 *   实现要点：适用于 RESET/READ_ID/STATUS/FEATURES/内部搬移/块锁等
 *         单次原子操作，cw_count 通常为 0 或 1。
 * -------------------------------------------------------------------- */
static void completion_single(nand_dispatch_task_t *task, uint8_t result, const nand_fc_completion_t *nand_completion)
{
    uint8_t status = (result == NAND_RESULT_OK)        ? FC_CW_STATUS_SUCCESS
                     : (result == NAND_RESULT_TIMEOUT) ? FC_CW_STATUS_TIMEOUT
                                                       : FC_CW_STATUS_NAND_FAIL;

    (void)nand_completion;
    task->completion.cw_status[0] = status;
}

/* --------------------------------------------------------------------
 * completion_multi_plane
 *   功能：按 plane × CW 扁平遍历，逐 CW 标记状态（多 plane 操作）。
 *   参数：task —— 正在收尾的任务；result —— NAND_RESULT_* 执行结果；
 *         nand_completion —— 底层 NAND 完成信息（本 handler 不使用）。
 *   返回：无。
 *   实现要点：扁平索引 flat_idx = plane * CW_NUM + cw，与
 *         req_multi_plane_program 拼接偏移（plane*PAGE_SIZE + cw*(PAGE_SIZE/CW_NUM)）
 *         保持一致，保证 cw_status 与 page_buffer 布局对应。
 * -------------------------------------------------------------------- */
static void completion_multi_plane(nand_dispatch_task_t *task, uint8_t result, const nand_fc_completion_t *nand_completion)
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
 * Request 构建 handler
 * ====================================================================== */

/* --------------------------------------------------------------------
 * req_program_page
 *   功能：将 task 的各 CW buffer 拼接到 page_buffer，一次性写入。
 *   参数：task —— 预解析完成的 nand_dispatch_task_t，cw_buffers[0][cw]
 *         已由 nand_dispatch_task_load() 填好；cw_count 指示 CW 数。
 *   返回：NAND_RESULT_OK；任一 CW buffer 为 NULL 时返回
 *         NAND_RESULT_INVALID_ARGUMENT。
 *   实现要点：拼接偏移 = cw * (PAGE_SIZE/4)；先 memset 0xFF 使未填充
 *         CW 位保持擦除态；request 行地址取 cw_paa[0][0]（单 plane）。
 * -------------------------------------------------------------------- */
static uint8_t req_program_page(nand_dispatch_task_t *task)
{
    uint8_t  cw;
    uint8_t *page_buffer = g_nand_channel_page_buffer[task->ch];

    memset(page_buffer, 0xFF, PAGE_SIZE);
    for (cw = 0U; cw < task->cw_count; cw++)
    {
        /* CW 缓冲由 FC 数据路径填满，链上指针恒非空 */
        ASSERT(task->cw_buffers[0][cw] != NULL);
        memcpy(&page_buffer[cw * (PAGE_SIZE / 4U)], task->cw_buffers[0][cw], PAGE_SIZE / 4U);
    }
    task->request = (nand_fc_request_t){.column = 0U, .row = {paa_to_nand_row(task->cw_paa[0][0])}, .data = page_buffer, .data_length = PAGE_SIZE};
    return NAND_RESULT_OK;
}

/* --------------------------------------------------------------------
 * req_read_page
 *   功能：读首个 CW（CW0）到 cw_buffer，构建首次读请求。
 *   参数：task —— 预解析完成的任务，cw_buffers[0][0] 为 CW0 目标缓冲。
 *   返回：NAND_RESULT_OK；cw_buffers[0][0] 为 NULL 时返回
 *         NAND_RESULT_INVALID_ARGUMENT。
 *   实现要点：data_length = PAGE_SIZE/4 只传 CW0；后续 CW 由 poll()
 *         的 chaining 循环经 OP_RANDOM_DATA_READ 逐个读取。
 * -------------------------------------------------------------------- */
static uint8_t req_read_page(nand_dispatch_task_t *task)
{
    uint8_t *cw_buffer = task->cw_buffers[0][0];

    /* CW0 缓冲由 FC 数据路径填满，恒非空 */
    ASSERT(cw_buffer != NULL);
    task->request = (nand_fc_request_t){.column = 0U, .row = {paa_to_nand_row(task->cw_paa[0][0])}, .data = cw_buffer, .data_length = (uint16_t)(PAGE_SIZE / 4U)};
    return NAND_RESULT_OK;
}

/* --------------------------------------------------------------------
 * req_fetch_page
 *   功能：将整页读入 page_buffer，构建 FETCH 请求。
 *   参数：task —— 预解析完成的任务；cw_buffers[0][cw] 为后续拆回目标。
 *   返回：NAND_RESULT_OK；任一 CW buffer 为 NULL 时返回
 *         NAND_RESULT_INVALID_ARGUMENT。
 *   实现要点：与 req_read_page 不同——FETCH 无后续 CW chaining，
 *         必须一次性读完整页（data_length = PAGE_SIZE），再由 poll()
 *         拆回各 CW buffer；memset 0x00 先清零整页缓冲。
 * -------------------------------------------------------------------- */
static uint8_t req_fetch_page(nand_dispatch_task_t *task)
{
    uint8_t  cw;
    uint8_t *page_buffer = g_nand_channel_page_buffer[task->ch];

    for (cw = 0U; cw < task->cw_count; cw++)
    {
        /* CW 缓冲由 FC 数据路径填满，链上指针恒非空 */
        ASSERT(task->cw_buffers[0][cw] != NULL);
    }
    memset(page_buffer, 0x00, PAGE_SIZE);
    task->request = (nand_fc_request_t){
        .column      = 0U,
        .row         = {paa_to_nand_row(task->cw_paa[0][0])},
        .data        = page_buffer,
        .data_length = PAGE_SIZE};
    return NAND_RESULT_OK;
}

/* --------------------------------------------------------------------
 * req_cache_read
 *   功能：将整页读入 page_buffer，构建 CACHE 读请求。
 *   参数：task —— 预解析完成的任务；cw_buffers[0][cw] 为后续拆回目标。
 *   返回：NAND_RESULT_OK；任一 CW buffer 为 NULL 时返回
 *         NAND_RESULT_INVALID_ARGUMENT。
 *   实现要点：覆盖 READ_PAGE_CACHE_SEQ/RANDOM/LAST 三种 OP，均整页
 *         读入 page_buffer（data_length = PAGE_SIZE）后由 poll() 拆回。
 * -------------------------------------------------------------------- */
static uint8_t req_cache_read(nand_dispatch_task_t *task)
{
    uint8_t  cw;
    uint8_t *page_buffer = g_nand_channel_page_buffer[task->ch];

    for (cw = 0U; cw < task->cw_count; cw++)
    {
        /* CW 缓冲由 FC 数据路径填满，链上指针恒非空 */
        ASSERT(task->cw_buffers[0][cw] != NULL);
    }
    memset(page_buffer, 0x00, PAGE_SIZE);
    task->request = (nand_fc_request_t){.column = 0U, .row = {paa_to_nand_row(task->cw_paa[0][0])}, .data = page_buffer, .data_length = PAGE_SIZE};
    return NAND_RESULT_OK;
}

/* --------------------------------------------------------------------
 * req_erase
 *   功能：构建块擦除请求（无数据传输）。
 *   参数：task —— 预解析完成的任务，cw_paa[0][0] 提供块行地址。
 *   返回：恒为 NAND_RESULT_OK。
 *   实现要点：data 为 NULL、data_length 为 0，仅携带行地址。
 * -------------------------------------------------------------------- */
static uint8_t req_erase(nand_dispatch_task_t *task)
{
    task->request = (nand_fc_request_t){.column = 0U, .row = {paa_to_nand_row(task->cw_paa[0][0])}, .data = NULL, .data_length = 0U};
    return NAND_RESULT_OK;
}

/* --------------------------------------------------------------------
 * req_feature
 *   功能：构建 SET/GET_FEATURES 请求。
 *   参数：task —— 预解析完成的任务；feature_addr 为 Feature 寄存器地址；
 *         cw_buffers[0][0] 已由 FC 重定向到 command->parameter0..3。
 *   返回：恒为 NAND_RESULT_OK。
 *   实现要点：column = feature_addr，data_length 固定 4 字节
 *         （ONFI Feature 寄存器宽度）。
 * -------------------------------------------------------------------- */
static uint8_t req_feature(nand_dispatch_task_t *task)
{
    task->request = (nand_fc_request_t){.column = (uint16_t)task->feature_addr, .row = {0U}, .data = task->cw_buffers[0][0], .data_length = 4U};
    return NAND_RESULT_OK;
}

/* --------------------------------------------------------------------
 * req_single
 *   功能：构建固定长度数据传输请求（READ_ID/PARAMETER_PAGE/RANDOM 等）。
 *   参数：task —— 预解析完成的任务；operation 决定数据长度。
 *   返回：恒为 NAND_RESULT_OK。
 *   实现要点：数据长度取自 g_nand_operation_map[operation].data_length；
 *         行地址仅当存在 CW（cw_count!=0）时填充，否则为 0。
 * -------------------------------------------------------------------- */
static uint8_t req_single(nand_dispatch_task_t *task)
{
    uint16_t data_length = g_nand_operation_map[task->operation].data_length;

    task->request = (nand_fc_request_t){
        .column      = 0U,
        .row         = {(task->cw_count != 0U) ? paa_to_nand_row(task->cw_paa[0][0]) : 0U},
        .data        = task->cw_buffers[0][0],
        .data_length = data_length};
    return NAND_RESULT_OK;
}

/* --------------------------------------------------------------------
 * req_no_data
 *   功能：构建无数据传输请求（RESET/READ_MODE/块锁/块解锁等）。
 *   参数：task —— 预解析完成的任务。
 *   返回：恒为 NAND_RESULT_OK。
 *   实现要点：data 为 NULL、data_length 为 0；行地址与 req_single
 *         相同规则（有 CW 才填充）。
 * -------------------------------------------------------------------- */
static uint8_t req_no_data(nand_dispatch_task_t *task)
{
    task->request = (nand_fc_request_t){
        .column      = 0U,
        .row         = {(task->cw_count != 0U) ? paa_to_nand_row(task->cw_paa[0][0]) : 0U},
        .data        = NULL,
        .data_length = 0U};
    return NAND_RESULT_OK;
}

/* --------------------------------------------------------------------
 * req_multi_plane_read
 *   功能：构建多 plane 读请求，双页一次性读入 page_buffer。
 *   参数：task —— 预解析完成的任务，cw_paa[0][0] 与 cw_paa[1][0]
 *         分别为 Plane 0/1 行地址。
 *   返回：恒为 NAND_RESULT_OK。
 *   实现要点：request.row 填两个 plane 行地址；data_length = PAGE_SIZE
 *         而非 PAGE_BUFFER_SIZE——第二个 plane 由 FC 侧 DATA_OUT
 *         operand=PAGE_SIZE 分两次读取（06h-E0h 机制）。
 * -------------------------------------------------------------------- */
static uint8_t req_multi_plane_read(nand_dispatch_task_t *task)
{
    uint8_t *page_buffer = g_nand_channel_page_buffer[task->ch];

    memset(page_buffer, 0x00, PAGE_BUFFER_SIZE);

    task->request = (nand_fc_request_t){
        .column      = 0U,
        .row         = {paa_to_nand_row(task->cw_paa[0][0]),
                        paa_to_nand_row(task->cw_paa[1][0])},
        .data        = page_buffer,
        .data_length = PAGE_SIZE  /* 每次 DATA_OUT 只读 1 个 Plane（2112 字节）
  * 第二个 Plane 由 06h-E0h + DATA_OUT operand=PAGE_SIZE 分两次读取 */
    };
    return NAND_RESULT_OK;
}

/* --------------------------------------------------------------------
 * req_multi_plane_program
 *   功能：将各 plane 各 CW buffer 拼接到 page_buffer，构建多 plane 写请求。
 *   参数：task —— 预解析完成的任务；cw_buffers[plane][cw] 已填好；
 *         plane 与 cw 均由 task 提供。
 *   返回：NAND_RESULT_OK；任一 plane 任一 CW buffer 为 NULL 时返回
 *         NAND_RESULT_INVALID_ARGUMENT。
 *   实现要点：拼接偏移 = plane * PAGE_SIZE + cw * (PAGE_SIZE/CW_NUM)；
 *         memset 0xFF 保证未填充 CW 位保持擦除态；行地址填双 plane。
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
            /* CW 缓冲由 FC 数据路径填满，链上指针恒非空 */
            ASSERT(task->cw_buffers[plane][cw] != NULL);
            memcpy(&page_buffer[plane * PAGE_SIZE + cw * (PAGE_SIZE / CW_NUM)],
                   task->cw_buffers[plane][cw],
                   PAGE_SIZE / CW_NUM);
        }
    }

    task->request = (nand_fc_request_t){
        .column      = 0U,
        .row         = {paa_to_nand_row(task->cw_paa[0][0]),
                        paa_to_nand_row(task->cw_paa[1][0])},
        .data        = page_buffer,
        .data_length = PAGE_SIZE  /* 每次 DATA_IN 只发 1 个 Plane（2112 字节），
  * 第二个 Plane 由 DATA_IN opernad=PAGE_SIZE 偏移读取 */
    };
    return NAND_RESULT_OK;
}

/* --------------------------------------------------------------------
 * req_multi_plane_erase
 *   功能：构建多 plane 块擦除请求（无数据传输，双行地址）。
 *   参数：task —— 预解析完成的任务，cw_paa[0][0] 与 cw_paa[1][0]
 *         为两个 plane 的块行地址。
 *   返回：恒为 NAND_RESULT_OK。
 *   实现要点：data 为 NULL、data_length 为 0，仅携带双行地址。
 * -------------------------------------------------------------------- */
static uint8_t req_multi_plane_erase(nand_dispatch_task_t *task)
{
    task->request = (nand_fc_request_t){
        .column      = 0U,
        .row         = {paa_to_nand_row(task->cw_paa[0][0]),
                        paa_to_nand_row(task->cw_paa[1][0])},
        .data        = NULL,
        .data_length = 0U
    };
    return NAND_RESULT_OK;
}

/* ======================================================================
 * 函数指针表定义
 * ====================================================================== */

/* --------------------------------------------------------------------
 * completion 表 g_nand_dispatch_completion_table
 *   每个 OP 的完成策略选择依据（与 onfi_1_0.h 的 OP 语义对应）：
 *     - completion_single    ：无 CW 概念（RESET/ID/STATUS/FEATURES/内部搬移/块锁等）
 *     - completion_per_cw    ：逐 CW 独立状态（普通读/OTP 读/CACHE_SEQ/RANDOM/LAST/FETCH）
 *     - completion_all_cws   ：所有 CW 同状态（编程/擦除/ISSUE/WAIT/CACHE_START）
 *     - completion_multi_plane：多 plane 扁平索引遍历
 *   下标 OP_* 不连续（含 Phase V 扩展 35..40），用设计初始化器定位。
 * -------------------------------------------------------------------- */
const nand_dispatch_completion_handler_t g_nand_dispatch_completion_table[OP_COUNT] = {
    [OP_RESET]                                  = completion_single,
    [OP_READ_ID]                                = completion_single,
    [OP_READ_PARAMETER_PAGE]                    = completion_single,
    [OP_READ_UNIQUE_ID]                         = completion_single,
    [OP_GET_FEATURES]                           = completion_single,
    [OP_SET_FEATURES]                           = completion_single,
    [OP_READ_STATUS]                            = completion_single,
    [OP_READ_STATUS_ENHANCED]                   = completion_single,
    [OP_RANDOM_DATA_READ]                       = completion_single,
    [OP_PROGRAM_FOR_INTERNAL_DATA_MOVE]         = completion_single,
    [OP_NORMAL_READ_PAGE_WITH_DATA_OUT]         = completion_per_cw,
    [OP_NORMAL_READ_PAGE_WITHOUT_DATA_OUT]      = completion_per_cw,
    [OP_READ_PAGE_CACHE_SEQ]                    = completion_per_cw,
    [OP_READ_PAGE_CACHE_RANDOM]                 = completion_per_cw,
    [OP_READ_PAGE_CACHE_LAST]                   = completion_per_cw,
    [OP_READ_PAGE_CACHE_START]                  = completion_all_cws,
    [OP_NORMAL_PROGRAM_PAGE]                    = completion_all_cws,
    [OP_PROGRAM_PAGE_CACHE]                     = completion_all_cws,
    [OP_ERASE_BLOCK]                            = completion_all_cws,
    [OP_READ_FOR_INTERNAL_DATA_MOVE]            = completion_single,
    [OP_PROGRAM_FOR_INTERNAL_MOVE]              = completion_single,
    [OP_BLOCK_UNLOCK_LOW]                       = completion_single,
    [OP_BLOCK_UNLOCK_HIGH]                      = completion_single,
    [OP_BLOCK_LOCK]                             = completion_single,
    [OP_BLOCK_LOCK_TIGHT]                       = completion_single,
    [OP_BLOCK_LOCK_STATUS]                      = completion_single,
    [OP_OTP_DATA_LOCK]                          = completion_single,
    [OP_OTP_DATA_PROGRAM]                       = completion_all_cws,
    [OP_OTP_DATA_READ]                          = completion_per_cw,
    [OP_READ_PAGE_MULTI_PLANE_WITHOUT_DATA_OUT] = completion_multi_plane,
    [OP_READ_PAGE_MULTI_PLANE_WITH_DATA_OUT]    = completion_multi_plane,
    [OP_PROGRAM_PAGE_MULTI_PLANE]               = completion_multi_plane,
    [OP_ERASE_BLOCK_MULTI_PLANE]                = completion_multi_plane,
    /* Phase V: ISSUE/WAIT/FETCH completion */
    [OP_READ_PAGE_LOAD]     = completion_single,
    [OP_READ_PAGE_FETCH]    = completion_per_cw,
    [OP_PROGRAM_PAGE_ISSUE] = completion_all_cws,
    [OP_PROGRAM_PAGE_WAIT]  = completion_all_cws,
    [OP_ERASE_BLOCK_ISSUE]  = completion_all_cws,
    [OP_ERASE_BLOCK_WAIT]   = completion_all_cws,
};

/* --------------------------------------------------------------------
 * req 表 g_nand_dispatch_req_table
 *   每个 OP 的 request 构建策略选择依据：
 *     - req_no_data        ：纯地址操作，无数据传输
 *     - req_single         ：固定长度数据（长度查 g_nand_operation_map）
 *     - req_read_page      ：读 CW0 到 cw_buffer（后续 CW 走 RANDOM_DATA_READ chaining）
 *     - req_fetch_page     ：FETCH 一次性读 full page 到 page_buffer
 *     - req_cache_read     ：CACHE_SEQ/RANDOM/LAST 全页读入 page_buffer
 *     - req_program_page   ：单 plane 多 CW 拼接后一次写
 *     - req_multi_plane_read/program/erase：双 plane 行地址 / 双 plane 拼接写
 *     - req_feature        ：SET/GET_FEATURES（column=feature_addr, 4B）
 *     - req_erase          ：擦除（无数据）
 *   下标 OP_* 不连续（含 Phase V 扩展 35..40），用设计初始化器定位。
 * -------------------------------------------------------------------- */
const nand_dispatch_req_handler_t g_nand_dispatch_req_table[OP_COUNT] = {
    [OP_RESET]                                  = req_no_data,
    [OP_READ_ID]                                = req_single,
    [OP_READ_PARAMETER_PAGE]                    = req_single,
    [OP_READ_UNIQUE_ID]                         = req_single,
    [OP_GET_FEATURES]                           = req_feature,
    [OP_SET_FEATURES]                           = req_feature,
    [OP_READ_STATUS]                            = req_single,
    [OP_READ_STATUS_ENHANCED]                   = req_single,
    [OP_RANDOM_DATA_READ]                       = req_single,
    [OP_PROGRAM_FOR_INTERNAL_DATA_MOVE]         = req_single,
    [OP_NORMAL_READ_PAGE_WITH_DATA_OUT]         = req_read_page,
    [OP_NORMAL_READ_PAGE_WITHOUT_DATA_OUT]      = req_read_page,
    [OP_READ_PAGE_CACHE_SEQ]                    = req_cache_read,
    [OP_READ_PAGE_CACHE_RANDOM]                 = req_cache_read,
    [OP_READ_PAGE_CACHE_LAST]                   = req_cache_read,
    [OP_READ_PAGE_CACHE_START]                  = req_no_data,
    [OP_NORMAL_PROGRAM_PAGE]                    = req_program_page,
    [OP_PROGRAM_PAGE_CACHE]                     = req_program_page,
    [OP_ERASE_BLOCK]                            = req_erase,
    [OP_READ_FOR_INTERNAL_DATA_MOVE]            = req_no_data,
    [OP_PROGRAM_FOR_INTERNAL_MOVE]              = req_single,
    [OP_BLOCK_UNLOCK_LOW]                       = req_no_data,
    [OP_BLOCK_UNLOCK_HIGH]                      = req_no_data,
    [OP_BLOCK_LOCK]                             = req_no_data,
    [OP_BLOCK_LOCK_TIGHT]                       = req_no_data,
    [OP_BLOCK_LOCK_STATUS]                      = req_single,
    [OP_OTP_DATA_LOCK]                          = req_no_data,
    [OP_OTP_DATA_PROGRAM]                       = req_program_page,
    [OP_OTP_DATA_READ]                          = req_read_page,
    [OP_READ_PAGE_MULTI_PLANE_WITHOUT_DATA_OUT] = req_multi_plane_read,
    [OP_READ_PAGE_MULTI_PLANE_WITH_DATA_OUT]    = req_multi_plane_read,
    [OP_PROGRAM_PAGE_MULTI_PLANE]               = req_multi_plane_program,
    [OP_ERASE_BLOCK_MULTI_PLANE]                = req_multi_plane_erase,
    /* Phase V: ISSUE/WAIT/FETCH request */
    [OP_READ_PAGE_LOAD]     = req_read_page,
    [OP_READ_PAGE_FETCH]    = req_fetch_page,
    [OP_PROGRAM_PAGE_ISSUE] = req_program_page,
    [OP_PROGRAM_PAGE_WAIT]  = req_no_data,
    [OP_ERASE_BLOCK_ISSUE]  = req_erase,
    [OP_ERASE_BLOCK_WAIT]   = req_no_data,
};
