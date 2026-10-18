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
 * 本文件定义两张表（下标均为 OP_COUNT）：
 *   - g_nand_dispatch_req_table[]       ：request 构建 handler（req_*）
 *   - g_nand_dispatch_completion_table[]：completion 策略 handler（completion_*）
 * 调用点位于 nand_dispatch.c：
 *   - nand_dispatch_task_complete() 经 completion_table[task->operation](...) 收尾
 *   - submit / chain 阶段经 req_table[task->operation](task) 构建请求
 */

#include "assertion.h"
#include "log.h"
#include "nand_config.h"
#include "nand_dispatch_internal.h"

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
 *   （nand_dispatch_poll 的 ce/lun 双层循环），而 request.data 一旦指向本缓冲
 *   就跨多次 poll 存活（如 program ISSUE 先被 70h 忙轮询挡下，数据已 memcpy
 *   进缓冲却要等下一轮才 DATA_IN）。若全通道共用一块，后驱动的 LUN 会把先
 *   驱动的 LUN 尚未搬走的数据覆盖掉——故必须带 ce/lun 维度。
 *   多 plane 操作时保存双页数据（PAGE_BUFFER_SIZE = 2112×2），
 *   由 req_* 拼接写入 / poll 拆回各 CW buffer。
 *   归属模块：nand_dispatch.c（定义）与本文件（引用）。
 * -------------------------------------------------------------------- */
extern uint8_t g_nand_channel_page_buffer[CH_NUM][CE_NUM][LUN_NUM][PAGE_BUFFER_SIZE];

/* --------------------------------------------------------------------
 * completion_all_cws
 *   功能：将统一状态写入任务的**全部选中 CW**（PROGRAM/ERASE 类，含 multi-plane）。
 *   参数：task —— 正在收尾的任务；result —— NAND_RESULT_* 执行结果；
 *         nand_completion —— 底层 NAND 完成信息（本 handler 不使用）。
 *   返回：无。
 *   实现要点：result 到 FC_CW_STATUS_* 映射统一为
 *         OK→SUCCESS / TIMEOUT→TIMEOUT / 其余→NAND_FAIL；
 *         失败时 failed_descriptor_id 固定为 0（无逐 CW 区分）。
 *         遍历掩码置位、按 flat 绝对编号写 cw_status[n]——单 plane 与 multi-plane
 *         在此完全同构（旧实现里 multi-plane 走单独的 plane-major 走位版本，
 *         统一索引空间后两者逐字节等价，已合并）。
 *         **掩码为空时写 cw_status[0]**（与 completion_single 同口径）——v28 起
 *         ERASE 不再由 FC 伪造一个 CW0 位，擦除请求的位图是**合法空集**；FC 的非 page
 *         read 聚合分支是全表 OR，靠这一格读到擦除成败（否则失败被静默吞掉）。
 * -------------------------------------------------------------------- */
static void completion_all_cws(nand_dispatch_task_t *task, uint8_t result, const nand_fc_completion_t *nand_completion)
{
    uint8_t status = (result == NAND_RESULT_OK)        ? FC_CW_STATUS_SUCCESS
                     : (result == NAND_RESULT_TIMEOUT) ? FC_CW_STATUS_TIMEOUT
                                                       : FC_CW_STATUS_NAND_FAIL;
    uint8_t n;

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
    for (n = nand_dispatch_cw_next(task->cw_bitmap, 0U); n < CW_TOTAL_MAX;
         n = nand_dispatch_cw_next(task->cw_bitmap, (uint8_t)(n + 1U)))
    {
        task->completion.cw_status[n] = status;
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
 *         上报 task->cw_index（= 当前 CW 的 flat 绝对编号 n）而非固定 0，
 *         供上层定位具体 CW。
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
 *   功能：单次原子操作的默认策略——标记掩码内的 CW；掩码为空时标记槽位 0。
 *   参数：task —— 正在收尾的任务；result —— NAND_RESULT_* 执行结果；
 *         nand_completion —— 底层 NAND 完成信息（本 handler 不使用）。
 *   返回：无。
 *   实现要点：适用于 RESET/READ_ID/STATUS/FEATURES/内部搬移/块锁等。
 *         **掩码为空是常态**（RESET/STATUS/块锁等无 CW 概念），此时按旧口径写
 *         cw_status[0]——FC 的非 page read 聚合分支是全表 OR，靠这一格读到状态；
 *         掩码非空（如 FEATURES）时逐位置位，避免"状态写在下标 0、FC 却按 n 读"
 *         而丢失。
 * -------------------------------------------------------------------- */
static void completion_single(nand_dispatch_task_t *task, uint8_t result, const nand_fc_completion_t *nand_completion)
{
    uint8_t status = (result == NAND_RESULT_OK)        ? FC_CW_STATUS_SUCCESS
                     : (result == NAND_RESULT_TIMEOUT) ? FC_CW_STATUS_TIMEOUT
                                                       : FC_CW_STATUS_NAND_FAIL;
    uint8_t n;

    (void)nand_completion;
    if (task->cw_bitmap == 0U)
    {
        task->completion.cw_status[0] = status;
        return;
    }
    for (n = nand_dispatch_cw_next(task->cw_bitmap, 0U); n < CW_TOTAL_MAX;
         n = nand_dispatch_cw_next(task->cw_bitmap, (uint8_t)(n + 1U)))
    {
        task->completion.cw_status[n] = status;
    }
}

/* --------------------------------------------------------------------
 * completion_load
 *   功能：加载型 op（整页搬进页缓冲、自身不携带 CW 数据）的完成策略。
 *   参数：task —— 正在收尾的任务；result —— NAND_RESULT_* 执行结果；
 *         nand_completion —— 底层 NAND 完成信息（本 handler 不使用）。
 *   返回：无。
 *   实现要点：**成功时不写任何 cw_status**——各槽位由后续逐 CW 取数
 *         （05h/06h）逐个填入；失败时把失败状态刷满整张表，因为加载失败后
 *         取数链不会再启动，没有其它来源能反映这次失败。
 * -------------------------------------------------------------------- */
static void completion_load(nand_dispatch_task_t *task, uint8_t result, const nand_fc_completion_t *nand_completion)
{
    uint8_t status;
    uint8_t i;

    (void)nand_completion;
    if (result == NAND_RESULT_OK)
    {
        return;
    }
    status = (result == NAND_RESULT_TIMEOUT) ? FC_CW_STATUS_TIMEOUT : FC_CW_STATUS_NAND_FAIL;
    for (i = 0U; i < (CW_NUM * PLANE_NUM); i++)
    {
        task->completion.cw_status[i] = status;
    }
    task->completion.failed_descriptor_id = 0U;
}

/* ======================================================================
 * Request 构建 handler
 * ====================================================================== */

/* --------------------------------------------------------------------
 * req_program_page
 *   功能：将 task 的各 CW buffer 拼接到 page_buffer，一次性写入。
 *   参数：task —— 预解析完成的 nand_dispatch_task_t，cw_buffers[n]
 *         已由 nand_dispatch_task_load() 填好；cw_bitmap 指示要写哪些 CW。
 *   返回：无。
 *   实现要点：拼接偏移 = cw * CW_SIZE；先 memset 0xFF 使未填充
 *         CW 位保持擦除态；request 行地址取 plane_paa[task->plane]（单 plane 的目标 plane）。
 * -------------------------------------------------------------------- */
static void req_program_page(nand_dispatch_task_t *task)
{
    uint8_t  n;
    uint8_t *page_buffer = g_nand_channel_page_buffer[task->ch][task->ce][task->lun];

    memset(page_buffer, 0xFF, PAGE_SIZE);
    for (n = nand_dispatch_cw_next(task->cw_bitmap, 0U); n < CW_TOTAL_MAX;
         n = nand_dispatch_cw_next(task->cw_bitmap, (uint8_t)(n + 1U)))
    {
        /* CW 缓冲由 FC 数据路径填满，掩码内的指针恒非空 */
        ASSERT(task->cw_buffers[n] != NULL);
        /* 页缓冲内的偏移 = CW 号 × CW_SIZE；CW 号就是 n % CW_NUM
         * （未写到的 CW 保持上面的 0xFF 擦除态）。 */
        memcpy(&page_buffer[(n % CW_NUM) * CW_SIZE], task->cw_buffers[n], CW_SIZE);
    }
    task->request = (nand_fc_request_t){.column = 0U, .row = {paa_to_nand_row(task->plane_paa[task->plane])}, .data = page_buffer, .data_length = PAGE_SIZE};
}

/* --------------------------------------------------------------------
 * req_read_page
 *   功能：构建整页加载请求（把目标页搬进 NAND 页缓冲）。
 *   参数：task —— 预解析完成的任务，plane_paa[task->plane] 给行地址。
 *   返回：无。
 *   实现要点：同时服务两个 op：
 *     - OP_NORMAL_READ_PAGE_WITHOUT_DATA_OUT（读路径统一后的加载 op）：
 *       uop 序列无 DATA_OUT，data/data_length 被忽略，数据留在页缓冲，随后由
 *       poll() 的 chaining 循环经 OP_RANDOM_DATA_READ 逐 CW 取回（含掩码最低位那个）。
 *       v45：命令**不带 desc** 直接下发（测试用）时掩码为空 ⇒ data 置 NULL，且 chaining
 *       不启动（空掩码无 CW 可取），纯加载、不取数。
 *     - OP_NORMAL_READ_PAGE_WITH_DATA_OUT / OP_OTP_DATA_READ（旧语义，直接下发）：
 *       掩码最低位那个 CW 由本次 DATA_OUT 一次取回，后续 CW 再由 chaining 逐个随机读。
 * -------------------------------------------------------------------- */
static void req_read_page(nand_dispatch_task_t *task)
{
    uint8_t *cw_buffer;

    /* 空掩码 = 无 desc 的「纯加载」op（v45，op 33 直接下发）：uop 无 DATA_OUT，data 不被读，
     * 且 cw_buffers[] 是上一次占用该 slot 的残留值（见 nand_dispatch_internal.h 的契约注释）
     * ⇒ 显式置 NULL/0，不把残留指针挂到请求上。 */
    if (task->cw_bitmap == 0U)
    {
        task->request = (nand_fc_request_t){.column = 0U, .row = {paa_to_nand_row(task->plane_paa[task->plane])}, .data = NULL, .data_length = 0U};
        return;
    }

    /* 掩码最低位 = 本 plane 第一个要访问的 CW（其缓冲由 FC 数据路径填满，恒非空） */
    cw_buffer = task->cw_buffers[nand_dispatch_cw_first(task->cw_bitmap)];

    ASSERT(cw_buffer != NULL);
    task->request = (nand_fc_request_t){.column = 0U, .row = {paa_to_nand_row(task->plane_paa[task->plane])}, .data = cw_buffer, .data_length = (uint16_t)CW_SIZE};
}

/* --------------------------------------------------------------------
 * req_cache_read
 *   功能：将整页读入 page_buffer，构建 CACHE 读请求。
 *   参数：task —— 预解析完成的任务；cw_buffers[n] 为后续拆回目标。
 *   返回：无。
 *   实现要点：覆盖 READ_PAGE_CACHE_SEQ/RANDOM/LAST 三种 OP，均整页
 *         读入 page_buffer（data_length = PAGE_SIZE）后由 poll() 拆回。
 * -------------------------------------------------------------------- */
static void req_cache_read(nand_dispatch_task_t *task)
{
    uint8_t  n;
    uint8_t *page_buffer = g_nand_channel_page_buffer[task->ch][task->ce][task->lun];

    for (n = nand_dispatch_cw_next(task->cw_bitmap, 0U); n < CW_TOTAL_MAX;
         n = nand_dispatch_cw_next(task->cw_bitmap, (uint8_t)(n + 1U)))
    {
        /* CW 缓冲由 FC 数据路径填满，掩码内的指针恒非空 */
        ASSERT(task->cw_buffers[n] != NULL);
    }
    memset(page_buffer, 0x00, PAGE_SIZE);
    task->request = (nand_fc_request_t){.column = 0U, .row = {paa_to_nand_row(task->plane_paa[task->plane])}, .data = page_buffer, .data_length = PAGE_SIZE};
}

/* --------------------------------------------------------------------
 * req_erase
 *   功能：构建块擦除请求（无数据传输）。
 *   参数：task —— 预解析完成的任务，plane_paa[task->plane] 提供块行地址。
 *   返回：无。
 *   实现要点：data 为 NULL、data_length 为 0，仅携带行地址。
 * -------------------------------------------------------------------- */
static void req_erase(nand_dispatch_task_t *task)
{
    task->request = (nand_fc_request_t){.column = 0U, .row = {paa_to_nand_row(task->plane_paa[task->plane])}, .data = NULL, .data_length = 0U};
}

/* --------------------------------------------------------------------
 * req_feature
 *   功能：构建 SET/GET_FEATURES 请求。
 *   参数：task —— 预解析完成的任务；feature_addr 为 Feature 寄存器地址；
 *         cw_buffers[task->plane * CW_NUM] 已由 FC 重定向到 g_fc_return_staging[command_id]
 *         （控制器级 4 字节暂存，返回值最终经 CQ 的 param 联合体交回）。
 *   返回：无。
 *   实现要点：column = feature_addr，data_length 固定 4 字节
 *         （ONFI Feature 寄存器宽度）。
 *         Feature 是 die 级、无 CW/plane 语义：FC 固定占本 plane 的 CW0 那一格
 *         （掩码位 = plane*CW_NUM），故这里直接按该格下标取，不遍历掩码。
 * -------------------------------------------------------------------- */
static void req_feature(nand_dispatch_task_t *task)
{
    task->request = (nand_fc_request_t){.column = (uint16_t)task->feature_addr, .row = {0U}, .data = task->cw_buffers[(uint8_t)(task->plane * CW_NUM)], .data_length = 4U};
}

/* --------------------------------------------------------------------
 * req_meta_read
 *   功能：构建 META 只读回读请求（OP_READ_META，自带 00h-30h 加载）。
 *   参数：task —— 预解析完成的任务；plane_paa[task->plane] 携带目标页 + CW；
 *         cw_buffers[task->plane * CW_NUM] 已由 FC 重定向到 g_fc_return_staging[command_id]
 *         （控制器级 4 字节暂存，返回值最终经 CQ 的 param 联合体交回）。
 *   返回：无。
 *   实现要点：**列地址 = CW 号 × CW_SIZE = paa_to_cw(plane_paa[plane]) × CW_SIZE**
 *         ——CW 由 command->paa 的 cw 字段选（PAA 原生含 cw），故从该 CW 起输出 4 字节。
 *         行地址取同一 PAA（paa_to_nand_row 丢掉 cw，cw 只进列地址）。
 *         4 字节落本 plane 的 CW0 那一格（掩码位 = plane*CW_NUM），与 req_feature 同口径。
 * -------------------------------------------------------------------- */
static void req_meta_read(nand_dispatch_task_t *task)
{
    const uint8_t plane = task->plane;

    task->request = (nand_fc_request_t){
        .column      = (uint16_t)(paa_to_cw(task->plane_paa[plane]) * CW_SIZE),
        .row         = {paa_to_nand_row(task->plane_paa[plane])},
        .data        = task->cw_buffers[(uint8_t)(plane * CW_NUM)],
        .data_length = 4U};
}

/* --------------------------------------------------------------------
 * req_read_column
 *   功能：构建按列读请求（OP_READ_COLUMN，自带 00h-30h 加载）。
 *   参数：task —— 预解析完成的任务；**column 由命令级 param 字节 0/1 解出后经
 *         FC 落进 task->column**；plane_paa[task->plane] 只提供行地址；
 *         cw_buffers[task->plane * CW_NUM] 已由 FC 重定向到 g_fc_return_staging[command_id]
 *         （控制器级 4 字节暂存，返回值最终经 CQ 的 param 联合体交回）。
 *   返回：无。
 *   实现要点：与 req_meta_read 的唯一区别是**列地址不来自 PAA 的 cw 字段，而来自
 *         task->column**——cw 只有 2 位（列上限 1584），够不到 spare 区（出厂坏块标记
 *         在列 2048）。行地址仍取同一 PAA（paa_to_nand_row 丢掉 cw）。
 *         4 字节落本 plane 的 CW0 那一格（掩码位 = plane*CW_NUM），与 req_feature 同口径。
 * -------------------------------------------------------------------- */
static void req_read_column(nand_dispatch_task_t *task)
{
    const uint8_t plane = task->plane;

    task->request = (nand_fc_request_t){
        .column      = task->column,
        .row         = {paa_to_nand_row(task->plane_paa[plane])},
        .data        = task->cw_buffers[(uint8_t)(plane * CW_NUM)],
        .data_length = 4U};
}

/* --------------------------------------------------------------------
 * req_onfi_signature
 *   功能：构建 ONFI 签名读请求（OP_READ_ONFI_SIGNATURE，90h + 地址 20h）。
 *   参数：task —— 预解析完成的任务；cw_buffers[task->plane * CW_NUM] 已由 FC 重定向到
 *         g_fc_return_staging[command_id]（控制器级 4 字节暂存，返回值最终经 CQ 的 param
 *         联合体交回）。
 *   返回：无。
 *   实现要点：**列地址恒 0x20**（手册 Figure 27）——与 OP_READ_ID 共用 uop 序列
 *         g_nand_read_id_uops，两者唯一的差别就是这里填的地址字节（00h vs 20h），
 *         因为 NAND_UOP_COLUMN_0 的 handler 写的是 request->column 而非 uop->operand。
 *         行地址恒 0：该 uop 序列无 ROW 段，填了也不下发（同 req_feature）。
 *         4 字节落本 plane 的 CW0 那一格（掩码位 = plane*CW_NUM），与 req_feature 同口径。
 * -------------------------------------------------------------------- */
static void req_onfi_signature(nand_dispatch_task_t *task)
{
    task->request = (nand_fc_request_t){
        .column      = NAND_READ_ID_ADDR_ONFI_SIGNATURE,
        .row         = {0U},
        .data        = task->cw_buffers[(uint8_t)(task->plane * CW_NUM)],
        .data_length = 4U};
}

/* --------------------------------------------------------------------
 * req_single
 *   功能：构建固定长度数据传输请求（READ_ID/PARAMETER_PAGE/RANDOM 等）。
 *   参数：task —— 预解析完成的任务；operation 决定数据长度。
 *   返回：无。
 *   实现要点：数据长度取自 g_nand_operation_map[operation].data_length；
 *         行地址取 plane_paa[task->plane]（无 desc 的命令掩码为空、该格为零值 → 行地址 0，
 *         与旧行为一致）；数据缓冲取掩码最低位那一格。
 * -------------------------------------------------------------------- */
static void req_single(nand_dispatch_task_t *task)
{
    uint16_t data_length = g_nand_operation_map[task->operation].data_length;

    task->request = (nand_fc_request_t){
        .column      = 0U,
        .row         = {paa_to_nand_row(task->plane_paa[task->plane])},
        .data        = task->cw_buffers[nand_dispatch_cw_first(task->cw_bitmap)],
        .data_length = data_length};
}

/* --------------------------------------------------------------------
 * req_random_read
 *   功能：构建页缓冲随机读请求（05h-E0h / 06h-E0h），按列地址取单个 CW。
 *   参数：task —— 预解析完成的任务；掩码最低位指明目标 CW。
 *   返回：无。
 *   实现要点：**列地址 = CW 号 × CW_SIZE = (n % CW_NUM) × CW_SIZE**（n = 掩码最低位），
 *         行地址取 plane_paa[n / CW_NUM]（多 plane 时 plane 位随行地址下发，
 *         06h 据此选 plane 页缓冲）。
 *         链式取数路径（nand_dispatch_task_cw_chain）自行构建同样的请求，不走本 handler；
 *         本 handler 服务于直接下发 OP_RANDOM_DATA_READ 的场景。
 * -------------------------------------------------------------------- */
static void req_random_read(nand_dispatch_task_t *task)
{
    uint8_t n = nand_dispatch_cw_first(task->cw_bitmap);

    task->request = (nand_fc_request_t){
        .column      = (uint16_t)((n % CW_NUM) * CW_SIZE),
        .row         = {paa_to_nand_row(task->plane_paa[n / CW_NUM])},
        .data        = task->cw_buffers[n],
        .data_length = (uint16_t)CW_SIZE};
}

/* --------------------------------------------------------------------
 * req_no_data
 *   功能：构建无数据传输请求（RESET/块锁/块解锁等）。
 *   参数：task —— 预解析完成的任务。
 *   返回：无。
 *   实现要点：data 为 NULL、data_length 为 0；行地址与 req_single
 *         相同规则（plane_paa 未填则为 0）。
 * -------------------------------------------------------------------- */
static void req_no_data(nand_dispatch_task_t *task)
{
    task->request = (nand_fc_request_t){
        .column      = 0U,
        .row         = {paa_to_nand_row(task->plane_paa[task->plane])},
        .data        = NULL,
        .data_length = 0U};
}

/* --------------------------------------------------------------------
 * req_multi_plane_read
 *   功能：构建多 plane 整页加载请求（两个 plane 一起搬进各自页缓冲）。
 *   参数：task —— 预解析完成的任务，plane_paa[plane] 为各 plane 行地址。
 *   返回：无。
 *   实现要点：request.row 按 plane 遍历填 PLANE_NUM 个行地址。
 *     - OP_READ_PAGE_MULTI_PLANE_WITHOUT_DATA_OUT（读路径统一后的加载 op）：
 *       uop 序列无 DATA_OUT，数据留在各 plane 页缓冲，随后由 poll() 的 chaining
 *       循环经 OP_RANDOM_DATA_READ_MULTI_PLANE（06h-列-行-E0h，行地址带 plane 位）
 *       逐 CW 取回。
 *     - OP_READ_PAGE_MULTI_PLANE_WITH_DATA_OUT（旧语义，直接下发）：单条原子 uop
 *       内含 06h-E0h + DATA_OUT operand=PAGE_SIZE 分两次读，data_length = PAGE_SIZE。
 * -------------------------------------------------------------------- */
static void req_multi_plane_read(nand_dispatch_task_t *task)
{
    uint8_t  plane;
    uint8_t *page_buffer = g_nand_channel_page_buffer[task->ch][task->ce][task->lun];

    memset(page_buffer, 0x00, PAGE_BUFFER_SIZE);

    /* 每次 DATA_OUT 只读 1 个 Plane（PAGE_SIZE）；第二个 Plane 由 06h-E0h +
     * DATA_OUT operand=PAGE_SIZE 分两次读取。 */
    task->request = (nand_fc_request_t){.column = 0U, .data = page_buffer, .data_length = PAGE_SIZE};

    /* ⚠ 上界写死 PLANE_NUM，不看 task->plane_count：本 handler 只由 multi-plane OP
     *   选中，那条路径上 plane_count 恒 = PLANE_NUM。改 plane 语义时两处要一起动。
     * row[PLANE_NUM..NAND_PLANE_MAX-1] 由上面的复合字面量零填充。 */
    for (plane = 0U; plane < PLANE_NUM; plane++)
    {
        task->request.row[plane] = paa_to_nand_row(task->plane_paa[plane]);
    }
}

/* --------------------------------------------------------------------
 * req_multi_plane_program
 *   功能：将各 plane 各 CW buffer 拼接到 page_buffer，构建多 plane 写请求。
 *   参数：task —— 预解析完成的任务；cw_buffers[n] 已填好。
 *   返回：无。
 *   实现要点：拼接偏移 = plane * PAGE_SIZE + cw * CW_SIZE
 *         = (n / CW_NUM) * PAGE_SIZE + (n % CW_NUM) * CW_SIZE；
 *         memset 0xFF 保证未填充 CW 位保持擦除态；行地址按 plane 遍历填 PLANE_NUM 个。
 * -------------------------------------------------------------------- */
static void req_multi_plane_program(nand_dispatch_task_t *task)
{
    uint8_t  n, plane;
    uint8_t *page_buffer = g_nand_channel_page_buffer[task->ch][task->ce][task->lun];

    memset(page_buffer, 0xFF, PAGE_BUFFER_SIZE);
    for (n = nand_dispatch_cw_next(task->cw_bitmap, 0U); n < CW_TOTAL_MAX;
         n = nand_dispatch_cw_next(task->cw_bitmap, (uint8_t)(n + 1U)))
    {
        /* CW 缓冲由 FC 数据路径填满，掩码内的指针恒非空 */
        ASSERT(task->cw_buffers[n] != NULL);
        memcpy(&page_buffer[(n / CW_NUM) * PAGE_SIZE + (n % CW_NUM) * CW_SIZE], task->cw_buffers[n], CW_SIZE);
    }

    /* 每次 DATA_IN 只发 1 个 Plane（PAGE_SIZE），第二个 Plane 由 DATA_IN
     * operand=PAGE_SIZE 偏移读取。 */
    task->request = (nand_fc_request_t){.column = 0U, .data = page_buffer, .data_length = PAGE_SIZE};

    /* ⚠ 上界写死 PLANE_NUM，理由同上。row[PLANE_NUM..] 由复合字面量零填充。 */
    for (plane = 0U; plane < PLANE_NUM; plane++)
    {
        task->request.row[plane] = paa_to_nand_row(task->plane_paa[plane]);
    }
}

/* --------------------------------------------------------------------
 * req_multi_plane_erase
 *   功能：构建多 plane 块擦除请求（无数据传输，双行地址）。
 *   参数：task —— 预解析完成的任务，plane_paa[plane] 为各 plane 的块行地址。
 *   返回：无。
 *   实现要点：data 为 NULL、data_length 为 0；行地址按 plane 遍历填 PLANE_NUM 个。
 * -------------------------------------------------------------------- */
static void req_multi_plane_erase(nand_dispatch_task_t *task)
{
    uint8_t plane;

    task->request = (nand_fc_request_t){.column = 0U, .data = NULL, .data_length = 0U};

    /* ⚠ 上界写死 PLANE_NUM，不看 task->plane_count：本 handler 只由 multi-plane OP
     *   选中，那条路径上 plane_count 恒 = PLANE_NUM。改 plane 语义时两处要一起动。
     * row[PLANE_NUM..NAND_PLANE_MAX-1] 由上面的复合字面量零填充。 */
    for (plane = 0U; plane < PLANE_NUM; plane++)
    {
        task->request.row[plane] = paa_to_nand_row(task->plane_paa[plane]);
    }
}

/* ======================================================================
 * 函数指针表定义
 * ====================================================================== */

/* --------------------------------------------------------------------
 * completion 表 g_nand_dispatch_completion_table
 *   每个 OP 的完成策略选择依据（与 onfi_1_0.h 的 OP 语义对应）：
 *     - completion_single    ：无 CW 概念（RESET/ID/STATUS/FEATURES/内部搬移/块锁/META 回读等）
 *     - completion_per_cw    ：逐 CW 独立状态（普通读/OTP 读/CACHE_SEQ/RANDOM/LAST）
 *     - completion_load      ：加载型 op 不写状态，失败时刷满
 *                              （WITHOUT_DATA_OUT 两种 + Phase V 的 LOAD/WAIT）
 *     - completion_all_cws   ：所有选中 CW 同状态（编程/擦除/ISSUE/WAIT/CACHE_START，
 *                              单 plane 与 multi-plane 共用——索引空间统一后两者同构）
 *   下标 OP_* 不连续（含 Phase V 扩展 35..43），用设计初始化器定位。
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
    [OP_READ_PAGE_CACHE_START]                  = completion_all_cws,
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
 * req 表 g_nand_dispatch_req_table
 *   每个 OP 的 request 构建策略选择依据：
 *     - req_no_data        ：纯地址操作，无数据传输
 *     - req_single         ：固定长度数据（长度查 g_nand_operation_map）
 *     - req_random_read    ：页缓冲随机读单 CW（列地址 = paa_to_cw(PAA)×CW_SIZE）
 *     - req_read_page      ：读 CW0 到 cw_buffer（后续 CW 走 RANDOM_DATA_READ chaining）
 *     - req_cache_read     ：CACHE_SEQ/RANDOM/LAST 全页读入 page_buffer
 *     - req_program_page   ：单 plane 多 CW 拼接后一次写
 *     - req_multi_plane_read/program/erase：双 plane 行地址 / 双 plane 拼接写
 *     - req_feature        ：SET/GET_FEATURES（column=feature_addr, 4B）
 *     - req_meta_read      ：META 只读回读（column = CW 号×CW_SIZE, 4B, 无 desc）
 *     - req_onfi_signature ：ONFI 签名读（90h + 地址 20h, 4B, 无 desc）
 *     - req_read_column    ：按显式列地址读（列来自 task->column, 4B, 无 desc）
 *     - req_erase          ：擦除（无数据）
 *   下标 OP_* 不连续（含 Phase V 扩展 35..43），用设计初始化器定位。
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
    [OP_READ_PAGE_CACHE_START]                  = req_no_data,
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
