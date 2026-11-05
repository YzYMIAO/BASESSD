/*
 * fc_nand_dispatch_tables.c
 *
 * 函数指针表：以 OP_* 为下标，替代 fc_nand_dispatch.c 中的 if-else / switch 链。
 *
 * 设计原则：
 *   - 每个 handler 只负责一件事（request 构建 / completion 策略）
 *   - 函数指针表为 O(1) 直接跳转，无分支预测惩罚
 *   - 新操作加入只需加 handler + 一行表项
 *
 * 本文件定义两张表（下标均为 OP_COUNT）：
 *   - g_fc_nand_dispatch_req_table[]       ：request 构建 handler（req_*）
 *   - g_fc_nand_dispatch_completion_table[]：completion 策略 handler（completion_*）
 * 调用点位于 fc_nand_dispatch.c：
 *   - fc_nand_dispatch_task_complete() 经 completion_table[task->operation](...) 收尾
 *   - submit / chain 阶段经 req_table[task->operation](task) 构建请求
 */

#include "assertion.h"
#include "fc_nand_dispatch_internal.h"
#include "nand_config.h"

#include <stddef.h>
#include <string.h>

/* ======================================================================
 * Completion 策略 handler
 * ====================================================================== */

/* --------------------------------------------------------------------
 * 全局变量 g_nand_channel_page_buffer
 *   页缓冲，物理位置在 noncacheable_buffer 段（DMA 直接访问，
 *   DCache 关闭）。FC 侧 data path 与 NAND 引擎共用。
 *   按 (ch, ce, lun) 各持一块：同一通道的多个 LUN 由 Phase 1 轮流驱动
 *   （fc_nand_dispatch_poll 的 ce/lun 双层循环），而 request.data 一旦指向本缓冲
 *   就跨多次 poll 存活（如 program ISSUE 先被 70h 忙轮询挡下，数据已 memcpy
 *   进缓冲却要等下一轮才 DATA_IN）。若全通道共用一块，后驱动的 LUN 会把先
 *   驱动的 LUN 尚未搬走的数据覆盖掉——故必须带 ce/lun 维度。
 *   多 plane 操作时保存双页数据（PAGE_BUFFER_SIZE = 2112×2），
 *   由 req_* 拼接写入 / poll 拆回各 CW buffer。
 *   归属模块：fc_nand_dispatch.c（定义）与本文件（引用）。
 * -------------------------------------------------------------------- */
extern BYTE g_nand_channel_page_buffer[CH_NUM][CE_NUM][LUN_NUM][PAGE_BUFFER_SIZE];

/**
 * @brief 清零 request 并填非行地址字段
 *
 * row[] 由各 handler 按 plane 填写；未用到的 plane 保持 0。
 *
 * @param task        任务
 * @param column      列地址
 * @param data        数据缓冲
 * @param data_length 数据长度
 */
static void req_init(fc_nand_dispatch_task_t *task, WORD column, BYTE *data, WORD data_length)
{
    memset(&task->request, 0, sizeof(task->request));
    task->request.column      = column;
    task->request.data        = data;
    task->request.data_length = data_length;
}

/**
 * @brief 把统一状态写入任务的全部选中 CW（PROGRAM/ERASE 类）
 *
 * 掩码为空时写 cw_status[0]（ERASE 的位图合法为空集）。
 *
 * @param task           正在收尾的任务
 * @param result         NAND_RESULT_* 执行结果
 * @param nand_completion 底层完成信息（未使用）
 */
static void completion_all_cws(fc_nand_dispatch_task_t *task, BYTE result, const fc_nand_completion_t *nand_completion)
{
    BYTE status = (result == NAND_RESULT_OK) ? FC_CW_STATUS_UNUSED : FC_CW_STATUS_FAIL;
    BYTE n;

    (void)nand_completion;
    if (task->cw_bitmap == 0U)
    {
        task->completion.cw_status[0] = status;
        if (result != NAND_RESULT_OK)
        {
            task->completion.failed_descriptor_id = 0U;
        }
        return;
    }
    for (n = fc_nand_dispatch_cw_next(task->cw_bitmap, 0U); n < CW_TOTAL_MAX; n = fc_nand_dispatch_cw_next(task->cw_bitmap, (n + 1U)))
    {
        task->completion.cw_status[n] = status;
    }
    if (result != NAND_RESULT_OK)
    {
        task->completion.failed_descriptor_id = 0U;
    }
}

/**
 * @brief 仅标记当前处理的 CW（READ/CACHE_READ 类）
 *
 * 失败时 failed_descriptor_id 上报当前 CW 的 flat 下标。
 *
 * @param task           正在收尾的任务
 * @param result         NAND_RESULT_* 执行结果
 * @param nand_completion 底层完成信息（未使用）
 */
static void completion_per_cw(fc_nand_dispatch_task_t *task, BYTE result, const fc_nand_completion_t *nand_completion)
{
    BYTE status = (result == NAND_RESULT_OK) ? FC_CW_STATUS_UNUSED : FC_CW_STATUS_FAIL;

    (void)nand_completion;
    task->completion.cw_status[task->cw_index] = status;
    if (result != NAND_RESULT_OK)
    {
        task->completion.failed_descriptor_id = task->cw_index;
    }
}

/**
 * @brief 单次原子操作的默认完成策略
 *
 * 掩码为空时写 cw_status[0]（RESET/STATUS 等无 CW 概念）；非空时逐位置位。
 *
 * @param task           正在收尾的任务
 * @param result         NAND_RESULT_* 执行结果
 * @param nand_completion 底层完成信息（未使用）
 */
static void completion_single(fc_nand_dispatch_task_t *task, BYTE result, const fc_nand_completion_t *nand_completion)
{
    BYTE status = (result == NAND_RESULT_OK) ? FC_CW_STATUS_UNUSED : FC_CW_STATUS_FAIL;
    BYTE n;

    (void)nand_completion;
    if (task->cw_bitmap == 0U)
    {
        task->completion.cw_status[0] = status;
        return;
    }
    for (n = fc_nand_dispatch_cw_next(task->cw_bitmap, 0U); n < CW_TOTAL_MAX; n = fc_nand_dispatch_cw_next(task->cw_bitmap, (n + 1U)))
    {
        task->completion.cw_status[n] = status;
    }
}

/**
 * @brief 加载型 op 的完成策略
 *
 * 成功时不写 cw_status（由后续逐 CW 取数填）；失败时刷满整张表。
 *
 * @param task           正在收尾的任务
 * @param result         NAND_RESULT_* 执行结果
 * @param nand_completion 底层完成信息（未使用）
 */
static void completion_load(fc_nand_dispatch_task_t *task, BYTE result, const fc_nand_completion_t *nand_completion)
{
    BYTE status;
    BYTE i;

    (void)nand_completion;
    if (result == NAND_RESULT_OK)
    {
        return;
    }
    status = FC_CW_STATUS_FAIL;
    for (i = 0U; i < (CW_NUM * PLANE_NUM); i++)
    {
        task->completion.cw_status[i] = status;
    }
    task->completion.failed_descriptor_id = 0U;
}

/* ======================================================================
 * Request 构建 handler
 * ====================================================================== */

/**
 * @brief 把各 CW buffer 拼进页缓冲，构建单 plane 页写请求
 *
 * 拼接偏移 = cw * CW_SIZE；未填充 CW 保持 0xFF 擦除态。
 *
 * @param task 预解析完成的任务
 */
static void req_program_page(fc_nand_dispatch_task_t *task)
{
    BYTE  n;
    BYTE *page_buffer = g_nand_channel_page_buffer[task->ch][task->ce][task->lun];

    memset(page_buffer, 0xFF, PAGE_SIZE);
    for (n = fc_nand_dispatch_cw_next(task->cw_bitmap, 0U); n < CW_TOTAL_MAX; n = fc_nand_dispatch_cw_next(task->cw_bitmap, (n + 1U)))
    {
        /* CW 缓冲由 FC 数据路径填满，掩码内的指针恒非空 */
        ASSERT(task->cw_buffers[n] != NULL);
        /* 页缓冲内的偏移 = CW 号 × CW_SIZE；CW 号就是 n % CW_NUM
         * （未写到的 CW 保持上面的 0xFF 擦除态）。 */
        memcpy(&page_buffer[(n % CW_NUM) * CW_SIZE], task->cw_buffers[n], CW_SIZE);
    }
    req_init(task, 0U, page_buffer, PAGE_SIZE);
    task->request.row[0] = paa_to_nand_row(task->plane_paa[task->plane]);
}

/**
 * @brief 构建整页加载请求
 *
 * 同时服务 WITHOUT_DATA_OUT（纯加载）与 WITH_DATA_OUT/OTP（旧语义）；
 * 空掩码 = 无 desc 的纯加载，data 置 NULL/0。
 *
 * @param task 预解析完成的任务
 */
static void req_read_page(fc_nand_dispatch_task_t *task)
{
    BYTE *cw_buffer;

    /* 空掩码 = 无 desc 的「纯加载」op（如 op 33 直接下发）：uop 无 DATA_OUT，data 不被读，
     * 且 cw_buffers[] 是上一次占用该 slot 的残留值（见 fc_nand_dispatch_internal.h 的契约注释）
     * ⇒ 显式置 NULL/0，不把残留指针挂到请求上。 */
    if (task->cw_bitmap == 0U)
    {
        req_init(task, 0U, NULL, 0U);
        task->request.row[0] = paa_to_nand_row(task->plane_paa[task->plane]);
        return;
    }

    /* 掩码最低位 = 本 plane 第一个要访问的 CW（其缓冲由 FC 数据路径填满，恒非空） */
    cw_buffer = task->cw_buffers[fc_nand_dispatch_cw_first(task->cw_bitmap)];

    ASSERT(cw_buffer != NULL);
    req_init(task, 0U, cw_buffer, CW_SIZE);
    task->request.row[0] = paa_to_nand_row(task->plane_paa[task->plane]);
}

/**
 * @brief 构建单 plane CACHE 读请求
 *
 * 整页读入 page_buffer（data_length = PAGE_SIZE），随后由 poll 拆回各 CW。
 *
 * @param task 预解析完成的任务
 */
static void req_cache_read(fc_nand_dispatch_task_t *task)
{
    BYTE  n;
    BYTE *page_buffer = g_nand_channel_page_buffer[task->ch][task->ce][task->lun];

    for (n = fc_nand_dispatch_cw_next(task->cw_bitmap, 0U); n < CW_TOTAL_MAX; n = fc_nand_dispatch_cw_next(task->cw_bitmap, (n + 1U)))
    {
        /* CW 缓冲由 FC 数据路径填满，掩码内的指针恒非空 */
        ASSERT(task->cw_buffers[n] != NULL);
    }
    memset(page_buffer, 0x00, PAGE_SIZE);
    req_init(task, 0U, page_buffer, PAGE_SIZE);
    task->request.row[0] = paa_to_nand_row(task->plane_paa[task->plane]);
}

/**
 * @brief 构建单 plane 块擦除请求（无数据传输）
 *
 * @param task 预解析完成的任务
 */
static void req_erase(fc_nand_dispatch_task_t *task)
{
    req_init(task, 0U, NULL, 0U);
    task->request.row[0] = paa_to_nand_row(task->plane_paa[task->plane]);
}

/**
 * @brief 构建 SET/GET_FEATURES 请求
 *
 * column = feature_addr，data_length 固定 4 字节；Feature 是 die 级，固定用本
 * plane 的 CW0 那一格。
 *
 * @param task 预解析完成的任务
 */
static void req_feature(fc_nand_dispatch_task_t *task)
{
    req_init(task, task->feature_addr, task->cw_buffers[(task->plane * CW_NUM)], 4U);
}

/**
 * @brief 构建 META 只读回读请求（OP_READ_META）
 *
 * 列地址 = paa_to_cw(plane_paa[plane]) * CW_SIZE；4 字节落本 plane 的 CW0 格。
 *
 * @param task 预解析完成的任务
 */
static void req_meta_read(fc_nand_dispatch_task_t *task)
{
    const BYTE plane = task->plane;

    req_init(task, (paa_to_cw(task->plane_paa[plane]) * CW_SIZE), task->cw_buffers[(plane * CW_NUM)], 4U);
    task->request.row[0] = paa_to_nand_row(task->plane_paa[plane]);
}

/**
 * @brief 构建按列读请求（OP_READ_COLUMN）
 *
 * 列地址来自 task->column（可够到 spare 区），行地址取 plane_paa。
 *
 * @param task 预解析完成的任务
 */
static void req_read_column(fc_nand_dispatch_task_t *task)
{
    const BYTE plane = task->plane;

    req_init(task, task->column, task->cw_buffers[(plane * CW_NUM)], 4U);
    task->request.row[0] = paa_to_nand_row(task->plane_paa[plane]);
}

/**
 * @brief 构建 ONFI 签名读请求（90h + 地址 20h）
 *
 * 列地址恒 0x20；4 字节落本 plane 的 CW0 格。
 *
 * @param task 预解析完成的任务
 */
static void req_onfi_signature(fc_nand_dispatch_task_t *task)
{
    req_init(task, NAND_READ_ID_ADDR_ONFI_SIGNATURE, task->cw_buffers[(task->plane * CW_NUM)], 4U);
}

/**
 * @brief 构建固定长度数据传输请求（READ_ID / PARAMETER_PAGE / RANDOM 等）
 *
 * 数据长度取自 g_nand_operation_map；缓冲取掩码最低位那一格。
 *
 * @param task 预解析完成的任务
 */
static void req_single(fc_nand_dispatch_task_t *task)
{
    WORD data_length = g_nand_operation_map[task->operation].data_length;

    req_init(task, 0U, task->cw_buffers[fc_nand_dispatch_cw_first(task->cw_bitmap)], data_length);
    task->request.row[0] = paa_to_nand_row(task->plane_paa[task->plane]);
}

/**
 * @brief 构建页缓冲随机读请求（05h-E0h / 06h-E0h）
 *
 * 列地址 = (n % CW_NUM) * CW_SIZE；行地址取 plane_paa[n / CW_NUM]。
 *
 * @param task 预解析完成的任务
 */
static void req_random_read(fc_nand_dispatch_task_t *task)
{
    BYTE n = fc_nand_dispatch_cw_first(task->cw_bitmap);

    req_init(task, ((n % CW_NUM) * CW_SIZE), task->cw_buffers[n], CW_SIZE);
    task->request.row[0] = paa_to_nand_row(task->plane_paa[n / CW_NUM]);
}

/**
 * @brief 构建无数据传输请求（RESET / 块锁 / 块解锁等）
 *
 * @param task 预解析完成的任务
 */
static void req_no_data(fc_nand_dispatch_task_t *task)
{
    req_init(task, 0U, NULL, 0U);
    task->request.row[0] = paa_to_nand_row(task->plane_paa[task->plane]);
}

/**
 * @brief 构建多 plane 整页加载请求
 *
 * row[] 按 plane 填 PLANE_NUM 个行地址；数据落各自 plane 页缓冲。
 *
 * @param task 预解析完成的任务
 */
static void req_multi_plane_read(fc_nand_dispatch_task_t *task)
{
    BYTE  plane;
    BYTE *page_buffer = g_nand_channel_page_buffer[task->ch][task->ce][task->lun];

    memset(page_buffer, 0x00, PAGE_BUFFER_SIZE);

    /* 每次 DATA_OUT 只读 1 个 Plane（PAGE_SIZE）；第二个 Plane 由 06h-E0h +
     * DATA_OUT operand=PAGE_SIZE 分两次读取。 */
    req_init(task, 0U, page_buffer, PAGE_SIZE);

    /* ⚠ 上界写死 PLANE_NUM，不看 task->plane_count：本 handler 只由 multi-plane OP
     *   选中，那条路径上 plane_count 恒 = PLANE_NUM。改 plane 语义时两处要一起动。
     * row[PLANE_NUM..NAND_PLANE_MAX-1] 由 req_init 的 memset 零填充。 */
    for (plane = 0U; plane < PLANE_NUM; plane++)
    {
        task->request.row[plane] = paa_to_nand_row(task->plane_paa[plane]);
    }
}

/**
 * @brief 构建多 plane 写请求
 *
 * 拼接偏移 = (n / CW_NUM) * PAGE_SIZE + (n % CW_NUM) * CW_SIZE；
 * 未填充 CW 保持 0xFF；行地址按 plane 填 PLANE_NUM 个。
 *
 * @param task 预解析完成的任务
 */
static void req_multi_plane_program(fc_nand_dispatch_task_t *task)
{
    BYTE  n, plane;
    BYTE *page_buffer = g_nand_channel_page_buffer[task->ch][task->ce][task->lun];

    memset(page_buffer, 0xFF, PAGE_BUFFER_SIZE);
    for (n = fc_nand_dispatch_cw_next(task->cw_bitmap, 0U); n < CW_TOTAL_MAX; n = fc_nand_dispatch_cw_next(task->cw_bitmap, (n + 1U)))
    {
        /* CW 缓冲由 FC 数据路径填满，掩码内的指针恒非空 */
        ASSERT(task->cw_buffers[n] != NULL);
        memcpy(&page_buffer[(n / CW_NUM) * PAGE_SIZE + (n % CW_NUM) * CW_SIZE], task->cw_buffers[n], CW_SIZE);
    }

    /* 每次 DATA_IN 只发 1 个 Plane（PAGE_SIZE），第二个 Plane 由 DATA_IN
     * operand=PAGE_SIZE 偏移读取。 */
    req_init(task, 0U, page_buffer, PAGE_SIZE);

    /* ⚠ 上界写死 PLANE_NUM，理由同上。row[PLANE_NUM..] 由 req_init 的 memset 零填充。 */
    for (plane = 0U; plane < PLANE_NUM; plane++)
    {
        task->request.row[plane] = paa_to_nand_row(task->plane_paa[plane]);
    }
}

/**
 * @brief 构建多 plane 块擦除请求（无数据传输，双行地址）
 *
 * @param task 预解析完成的任务
 */
static void req_multi_plane_erase(fc_nand_dispatch_task_t *task)
{
    BYTE plane;

    req_init(task, 0U, NULL, 0U);

    /* ⚠ 上界写死 PLANE_NUM，不看 task->plane_count：本 handler 只由 multi-plane OP
     *   选中，那条路径上 plane_count 恒 = PLANE_NUM。改 plane 语义时两处要一起动。
     * row[PLANE_NUM..NAND_PLANE_MAX-1] 由 req_init 的 memset 零填充。 */
    for (plane = 0U; plane < PLANE_NUM; plane++)
    {
        task->request.row[plane] = paa_to_nand_row(task->plane_paa[plane]);
    }
}

/* ======================================================================
 * 函数指针表定义
 * ====================================================================== */

/* --------------------------------------------------------------------
 * completion 表 g_fc_nand_dispatch_completion_table
 *   每个 OP 的完成策略选择依据（与 onfi_1_0.h 的 OP 语义对应）：
 *     - completion_single    ：无 CW 概念（RESET/ID/STATUS/FEATURES/内部搬移/块锁/META 回读等）
 *     - completion_per_cw    ：逐 CW 独立状态（普通读/OTP 读/单 plane CACHE_SEQ/RANDOM/LAST）
 *     - completion_load      ：加载型 op 不写状态，失败时刷满
 *                              （WITHOUT_DATA_OUT 两种 + Phase V 的 LOAD/WAIT）
 *     - completion_all_cws   ：所有选中 CW 同状态（编程/擦除/ISSUE/WAIT/CACHE_START_SEQ，
 *                              单 plane 与 multi-plane 共用——索引空间统一后两者同构）
 *   下标 OP_* 不连续（含 Phase V 扩展 35..43），用设计初始化器定位。
 * -------------------------------------------------------------------- */
const fc_nand_dispatch_completion_handler_t g_fc_nand_dispatch_completion_table[OP_COUNT] = {
    [OP_RESET]                                  = completion_single,
    [OP_READ_ID]                                = completion_single,
    [OP_READ_PARAMETER_PAGE]                    = completion_single,
    [OP_READ_UNIQUE_ID]                         = completion_single,
    [OP_GET_FEATURES]                           = completion_single,
    [OP_SET_FEATURES]                           = completion_single,
    [OP_READ_STATUS]                            = completion_single,
    [OP_READ_STATUS_ENHANCED]                   = completion_single,
    [OP_RANDOM_DATA_READ]                       = completion_per_cw,
    [OP_RANDOM_DATA_READ_MULTI_PLANE]           = completion_per_cw,
    [OP_READ_ONFI_SIGNATURE]                    = completion_single,
    [OP_PROGRAM_FOR_INTERNAL_DATA_MOVE]         = completion_single,
    [OP_READ_META]                              = completion_single,
    [OP_NORMAL_READ_PAGE_WITH_DATA_OUT]         = completion_per_cw,
    [OP_NORMAL_READ_PAGE_WITHOUT_DATA_OUT]      = completion_load,
    [OP_READ_PAGE_CACHE_SEQ]                    = completion_per_cw,
    [OP_READ_PAGE_CACHE_RANDOM]                 = completion_per_cw,
    [OP_READ_PAGE_CACHE_LAST]                   = completion_per_cw,
    [OP_READ_PAGE_CACHE_START_SEQ]              = completion_all_cws,
    [OP_TWO_PLANE_PROGRAM_CACHE]                = completion_all_cws,
    [OP_TWO_PLANE_PROGRAM_CACHE_END]            = completion_all_cws,
    [OP_TWO_PLANE_PROGRAM_CACHE_FIRST]          = completion_all_cws,
    [OP_READ_PAGE_CACHE_START_SEQ_MP]           = completion_all_cws,
    [OP_READ_PAGE_CACHE_SEQ_MP]                 = completion_all_cws,
    [OP_READ_PAGE_CACHE_LAST_MP]                = completion_all_cws,
    [OP_NORMAL_PROGRAM_PAGE]                    = completion_all_cws,
    [OP_PROGRAM_PAGE_CACHE]                     = completion_all_cws,
    [OP_ERASE_BLOCK]                            = completion_all_cws,
    [OP_READ_FOR_INTERNAL_DATA_MOVE]            = completion_single,
    [OP_BLOCK_UNLOCK_LOW]                       = completion_single,
    [OP_BLOCK_UNLOCK_HIGH]                      = completion_single,
    [OP_BLOCK_LOCK]                             = completion_single,
    [OP_BLOCK_LOCK_TIGHT]                       = completion_single,
    [OP_BLOCK_LOCK_STATUS]                      = completion_single,
    [OP_OTP_DATA_LOCK]                          = completion_single,
    [OP_OTP_DATA_PROGRAM]                       = completion_all_cws,
    [OP_OTP_DATA_READ]                          = completion_per_cw,
    [OP_READ_PAGE_MULTI_PLANE_WITHOUT_DATA_OUT] = completion_load,
    [OP_READ_PAGE_MULTI_PLANE_WITH_DATA_OUT]    = completion_all_cws,
    [OP_PROGRAM_PAGE_MULTI_PLANE]               = completion_all_cws,
    [OP_ERASE_BLOCK_MULTI_PLANE]                = completion_all_cws,
    /* Phase V: ISSUE/WAIT read completion —— 读的 CW 状态统一由逐 CW 取数写，
     * 所以 LOAD/WAIT 都用 completion_load（成功不写、失败刷满）。 */
    [OP_READ_PAGE_LOAD]                         = completion_load,
    [OP_READ_PAGE_WAIT]                         = completion_load,
    [OP_READ_PAGE_MULTI_PLANE_LOAD]             = completion_load,
    [OP_PROGRAM_PAGE_ISSUE]                     = completion_all_cws,
    [OP_PROGRAM_PAGE_WAIT]                      = completion_all_cws,
    [OP_ERASE_BLOCK_ISSUE]                      = completion_all_cws,
    [OP_ERASE_BLOCK_WAIT]                       = completion_all_cws,
    [OP_READ_COLUMN]                            = completion_single,
};

/* --------------------------------------------------------------------
 * req 表 g_fc_nand_dispatch_req_table
 *   每个 OP 的 request 构建策略选择依据：
 *     - req_no_data        ：纯地址操作，无数据传输
 *     - req_single         ：固定长度数据（长度查 g_nand_operation_map）
 *     - req_random_read    ：页缓冲随机读单 CW（列地址 = paa_to_cw(PAA)×CW_SIZE）
 *     - req_read_page      ：读 CW0 到 cw_buffer（后续 CW 走 RANDOM_DATA_READ chaining）
 *     - req_cache_read     ：START_SEQ/SEQ/RANDOM/LAST 单 plane 全页读入 page_buffer
 *     - req_program_page   ：单 plane 多 CW 拼接后一次写
 *     - req_multi_plane_read/program/erase：双 plane 行地址 / 双 plane 拼接写
 *     - req_feature        ：SET/GET_FEATURES（column=feature_addr, 4B）
 *     - req_meta_read      ：META 只读回读（column = CW 号×CW_SIZE, 4B, 无 desc）
 *     - req_onfi_signature ：ONFI 签名读（90h + 地址 20h, 4B, 无 desc）
 *     - req_read_column    ：按显式列地址读（列来自 task->column, 4B, 无 desc）
 *     - req_erase          ：擦除（无数据）
 *   下标 OP_* 不连续（含 Phase V 扩展 35..43），用设计初始化器定位。
 * -------------------------------------------------------------------- */
const fc_nand_dispatch_req_handler_t g_fc_nand_dispatch_req_table[OP_COUNT] = {
    [OP_RESET]                                  = req_no_data,
    [OP_READ_ID]                                = req_single,
    [OP_READ_PARAMETER_PAGE]                    = req_single,
    [OP_READ_UNIQUE_ID]                         = req_single,
    [OP_GET_FEATURES]                           = req_feature,
    [OP_SET_FEATURES]                           = req_feature,
    [OP_READ_STATUS]                            = req_single,
    [OP_READ_STATUS_ENHANCED]                   = req_single,
    [OP_RANDOM_DATA_READ]                       = req_random_read,
    [OP_RANDOM_DATA_READ_MULTI_PLANE]           = req_random_read,
    [OP_READ_ONFI_SIGNATURE]                    = req_onfi_signature,
    [OP_PROGRAM_FOR_INTERNAL_DATA_MOVE]         = req_single,
    [OP_READ_META]                              = req_meta_read,
    [OP_NORMAL_READ_PAGE_WITH_DATA_OUT]         = req_read_page,
    [OP_NORMAL_READ_PAGE_WITHOUT_DATA_OUT]      = req_read_page,
    [OP_READ_PAGE_CACHE_SEQ]                    = req_cache_read,
    [OP_READ_PAGE_CACHE_RANDOM]                 = req_cache_read,
    [OP_READ_PAGE_CACHE_LAST]                   = req_cache_read,
    [OP_READ_PAGE_CACHE_START_SEQ]              = req_cache_read,
    [OP_TWO_PLANE_PROGRAM_CACHE]                = req_multi_plane_program,
    [OP_TWO_PLANE_PROGRAM_CACHE_END]            = req_multi_plane_program,
    [OP_TWO_PLANE_PROGRAM_CACHE_FIRST]          = req_multi_plane_program,
    [OP_READ_PAGE_CACHE_START_SEQ_MP]           = req_multi_plane_read,
    [OP_READ_PAGE_CACHE_SEQ_MP]                 = req_multi_plane_read,
    [OP_READ_PAGE_CACHE_LAST_MP]                = req_multi_plane_read,
    [OP_NORMAL_PROGRAM_PAGE]                    = req_program_page,
    [OP_PROGRAM_PAGE_CACHE]                     = req_program_page,
    [OP_ERASE_BLOCK]                            = req_erase,
    [OP_READ_FOR_INTERNAL_DATA_MOVE]            = req_no_data,
    [OP_BLOCK_UNLOCK_LOW]                       = req_no_data,
    [OP_BLOCK_UNLOCK_HIGH]                      = req_no_data,
    [OP_BLOCK_LOCK]                             = req_no_data,
    [OP_BLOCK_LOCK_TIGHT]                       = req_no_data,
    [OP_BLOCK_LOCK_STATUS]                      = req_no_data,
    [OP_OTP_DATA_LOCK]                          = req_no_data,
    [OP_OTP_DATA_PROGRAM]                       = req_program_page,
    [OP_OTP_DATA_READ]                          = req_read_page,
    [OP_READ_PAGE_MULTI_PLANE_WITHOUT_DATA_OUT] = req_multi_plane_read,
    [OP_READ_PAGE_MULTI_PLANE_WITH_DATA_OUT]    = req_multi_plane_read,
    [OP_PROGRAM_PAGE_MULTI_PLANE]               = req_multi_plane_program,
    [OP_ERASE_BLOCK_MULTI_PLANE]                = req_multi_plane_erase,
    /* Phase V: ISSUE/WAIT read request */
    [OP_READ_PAGE_LOAD]                         = req_read_page,
    [OP_READ_PAGE_WAIT]                         = req_no_data,
    [OP_READ_PAGE_MULTI_PLANE_LOAD]             = req_multi_plane_read,
    [OP_PROGRAM_PAGE_ISSUE]                     = req_program_page,
    [OP_PROGRAM_PAGE_WAIT]                      = req_no_data,
    [OP_ERASE_BLOCK_ISSUE]                      = req_erase,
    [OP_ERASE_BLOCK_WAIT]                       = req_no_data,
    [OP_READ_COLUMN]                            = req_read_column,
};
