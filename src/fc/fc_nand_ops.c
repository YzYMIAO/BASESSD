/*
 * fc_nand_ops.c — FC 的 NAND 命令编排 / 初始化 / 拓扑扫描
 *
 * 本文件是 FC 对 NAND 器件执行层的入口：
 *   - ONFI 操作表 + UOP 引擎（fc_uop_engine）+ NAND PHY 后端；
 *   - 初始化流程（hw_init → 逐 target RESET/READ_ID/参数页/78h 探测）；
 *   - timing mode 切换（SET_FEATURES + 器件层重配 FMC 时序）。
 *
 * 器件事实与 PHY 由 NAND 层提供（nand_device.h / nand_phy_backend_get），
 * 本文件不 include 任何 HAL 头。
 */

#include "assertion.h"
#include "delay.h"
#include "fc_nand_interface.h"
#include "fc_uop_engine.h"
#include "log.h"
#include "nand_config.h"
#include "nand_debug.h"
#include "nand_device.h"
#include "nand_protocol.h"
#include "onfi_1_0.h"

#include <stddef.h>
#include <string.h>

/* 无器件/无响应时 DQ 总线读到全 1（浮空）；78h 状态字节用它判 LUN 不存在。 */
#define NAND_STATUS_NO_RESPONSE (0xFFU)

/**
 * @brief 同步执行单个 ONFI 操作（阻塞至完成）
 *
 * 仅初始化阶段（armed == 0）可调。
 *
 * @param operation  OP_*（< OP_COUNT）
 * @param request    列/行/数据
 * @param completion 输出：执行状态 + NAND 状态字节
 * @return NAND_RESULT_OK / 错误码
 */
nand_result_e fc_nand_execute(BYTE operation, const fc_nand_request_t *request, fc_nand_completion_t *completion)
{
    const nand_operation_map_t *map;
    nand_command_request_t      command_request;
    nand_command_completion_t   command_completion;
    BYTE                        status;

    ASSERT(operation < OP_COUNT);
    ASSERT(request != NULL);
    ASSERT(completion != NULL);
    /* 同步入口契约：仅初始化阶段（armed==0）可调。放行后必须走
     * fc_nand_start/step 异步路径——此处阻塞式执行会卡住整个调度器。 */
    ASSERT(nand_is_armed() == 0U);

    map = &g_nand_operation_map[operation];
    if (map->uops == NULL)
    {
        return NAND_RESULT_INVALID_ARGUMENT;
    }

    memset(&command_request, 0, sizeof(command_request));
    command_request.operation   = operation;
    command_request.column      = request->column;
    command_request.buffer      = request->data;
    command_request.data_length = request->data_length;
    /* 行地址按 NAND_PLANE_MAX 整体搬运，不写死 plane 下标（H39）。 */
    for (BYTE plane = 0U; plane < NAND_PLANE_MAX; plane++)
    {
        command_request.row[plane] = request->row[plane];
    }
    status                       = fc_uop_engine_execute(nand_phy_backend_get(), map->uops, &map->timing, &command_request, &command_completion);
    completion->execution_status = status;
    completion->nand_status      = command_completion.nand_status;
    completion->pre_status       = command_completion.pre_status;
    return status;
}

/**
 * @brief 发起异步 ONFI 事务（非阻塞，只推进一步）
 *
 * @param transaction 事务状态（backend 已绑定）
 * @param operation   OP_*
 * @param request     命令数据
 * @return OK / NOT_READY（需 fc_nand_step）/ 错误码
 */
nand_result_e fc_nand_start(fc_nand_transaction_t *transaction, BYTE operation, const fc_nand_request_t *request)
{
    const nand_operation_map_t *map;
    BYTE                        result;

    ASSERT(transaction != NULL);
    ASSERT(operation < OP_COUNT);
    ASSERT(request != NULL);

    map = &g_nand_operation_map[operation];
    if (map->uops == NULL)
    {
        return NAND_RESULT_INVALID_ARGUMENT;
    }

    ENG_LOGD("fc_nand_start: op=%hhu col=%hu row=0x%06lX data=%p len=%hu\r\n", operation, request->column, request->row[0], (const void *)request->data, request->data_length);

    memset(&transaction->request, 0, sizeof(transaction->request));
    transaction->request.operation   = operation;
    transaction->request.column      = request->column;
    transaction->request.buffer      = request->data;
    transaction->request.data_length = request->data_length;
    for (BYTE plane = 0U; plane < NAND_PLANE_MAX; plane++)
    {
        transaction->request.row[plane] = request->row[plane];
    }
    memset(&transaction->completion, 0, sizeof(transaction->completion));
    /* backend 由 dispatch 的任务装载处绑定；未绑定属调用者违约。 */
    ASSERT(transaction->engine.backend != NULL);
    result = fc_uop_engine_start(&transaction->engine, map->uops, &map->timing);
    if (result != NAND_RESULT_NOT_READY)
    {
        transaction->engine.active = 0U;
        LOGE_IF(result != NAND_RESULT_OK, LOG_TAG_NAND "start failed, op=%u result=0x%02X status=0x%02X\r\n", operation, result, transaction->completion.nand_status);
    }
    return result;
}

/**
 * @brief 推进异步事务一个引擎步
 *
 * @param transaction 发起中的事务
 * @param completion  输出：完成状态
 * @return NOT_READY（继续 poll）/ 完成结果
 */
nand_result_e fc_nand_step(fc_nand_transaction_t *transaction, fc_nand_completion_t *completion)
{
    BYTE result;

    ASSERT(transaction != NULL);
    ASSERT(completion != NULL);
    ASSERT(transaction->engine.active != 0U);

    result = fc_uop_engine_step(&transaction->engine);
    if (result != NAND_RESULT_NOT_READY)
    {
        completion->execution_status = result;
        completion->nand_status      = transaction->completion.nand_status;
        completion->pre_status       = transaction->completion.pre_status;
        ENG_LOGD("complete, op=%u result=0x%02X status=0x%02X\r\n", transaction->request.operation, result, completion->nand_status);
        transaction->engine.active = 0U;
    }
    return result;
}

/**
 * @brief NAND 复位（同步）
 *
 * @param completion 输出：完成状态
 * @return NAND_RESULT_OK / 错误码
 */
nand_result_e fc_nand_reset(fc_nand_completion_t *completion)
{
    fc_nand_request_t request;

    memset(&request, 0, sizeof(request));
    request.column      = 0U;
    request.data        = NULL;
    request.data_length = 0U;
    return fc_nand_execute(OP_RESET, &request, completion);
}

/**
 * @brief 读取芯片 ID（5 字节，同步）
 *
 * @param id         输出：5 字节
 * @param completion 输出：完成状态
 * @return NAND_RESULT_OK / 错误码
 */
nand_result_e fc_nand_read_id(BYTE id[5], fc_nand_completion_t *completion)
{
    fc_nand_request_t request;

    ASSERT(id != NULL);
    memset(&request, 0, sizeof(request));
    request.column      = 0U;
    request.data        = id;
    request.data_length = 5U;
    return fc_nand_execute(OP_READ_ID, &request, completion);
}

/**
 * @brief 读 ONFI 参数页（ECh，256 字节，同步）
 *
 * @param page       输出：参数页
 * @param completion 输出：完成状态
 * @return NAND_RESULT_OK / 错误码
 */
nand_result_e fc_nand_read_parameter_page(BYTE page[NAND_ONFI_PARAM_PAGE_SIZE], fc_nand_completion_t *completion)
{
    fc_nand_request_t request;

    ASSERT(page != NULL);
    memset(&request, 0, sizeof(request));
    request.column      = 0U;
    request.data        = page;
    request.data_length = NAND_ONFI_PARAM_PAGE_SIZE;
    return fc_nand_execute(OP_READ_PARAMETER_PAGE, &request, completion);
}

/**
 * @brief 写 Feature 寄存器（同步）
 *
 * @param feature_address 寄存器地址（复用 column）
 * @param feature_data    4 字节值
 * @param completion      输出：完成状态
 * @return NAND_RESULT_OK / 错误码
 */
nand_result_e fc_nand_set_features(BYTE feature_address, const BYTE feature_data[4], fc_nand_completion_t *completion)
{
    fc_nand_request_t request;

    ASSERT(feature_data != NULL);
    memset(&request, 0, sizeof(request));
    request.column      = feature_address;
    request.data        = (BYTE *)feature_data;
    request.data_length = 4U;
    return fc_nand_execute(OP_SET_FEATURES, &request, completion);
}

/**
 * @brief 78h READ STATUS ENHANCED（同步）
 *
 * @param row        行地址（含 LUN/plane 位）
 * @param completion 输出：完成状态
 * @return NAND_RESULT_OK / 错误码
 */
nand_result_e fc_nand_read_status_enhanced(DWORD row, fc_nand_completion_t *completion)
{
    fc_nand_request_t request;

    memset(&request, 0, sizeof(request));
    request.column      = 0U;
    request.row[0]      = row;
    request.data        = NULL;
    request.data_length = 1U;
    return fc_nand_execute(OP_READ_STATUS_ENHANCED, &request, completion);
}

/**
 * @brief 把解码后的 ONFI 参数页打三行开机日志
 *
 * @param info 已解码的参数页
 */
static void fc_nand_onfi_param_print(const nand_onfi_param_info_t *info)
{
    /* 器件总容量（MiB）= 页主区 × 页/块 × 块/单元 × 逻辑单元。 */
    unsigned long capacity_mib = info->data_bytes_per_page * info->pages_per_block * info->blocks_per_unit * info->logical_units / (1024UL * 1024UL);

    LOGI(LOG_TAG_NAND "ONFI sig=%c%c%c%c rev=0x%04X model=%s\r\n", (int)info->signature[0], (int)info->signature[1], (int)info->signature[2], (int)info->signature[3], (unsigned int)info->revision, info->model);
    LOGI(LOG_TAG_NAND "ONFI %luMiB page=%lu+%u partial=%lu+%u block=%lup blocks=%lu lun=%u addrcyc=0x%02X(C%u/R%u) bitcell=%u\r\n", capacity_mib, info->data_bytes_per_page, (unsigned int)info->spare_bytes_per_page, info->data_bytes_per_partial_page, (unsigned int)info->spare_bytes_per_partial_page, info->pages_per_block, info->blocks_per_unit, (unsigned int)info->logical_units, (unsigned int)info->address_cycles, (unsigned int)info->column_cycles, (unsigned int)info->row_cycles, (unsigned int)info->bits_per_cell);
    LOGI(LOG_TAG_NAND "ONFI ecc=%ubit prog/pg=%u tprog=%u terase=%u tr=%u tccs=%u timingmodes=0x%04X\r\n", (unsigned int)info->ecc_bits, (unsigned int)info->programs_per_page, (unsigned int)info->tprog_max, (unsigned int)info->tbers_max, (unsigned int)info->tr_max, (unsigned int)info->tccs_min, (unsigned int)info->timing_mode_support);
}

/**
 * @brief READ ID 是否为无器件（全 00 / 全 FF）
 *
 * @param id 5 字节 ID
 * @return 1 无器件；0 有器件
 */
static BYTE fc_nand_id_is_blank(const BYTE id[5])
{
    BYTE all_00 = 1U;
    BYTE all_ff = 1U;

    for (BYTE i = 0U; i < 5U; i++)
    {
        if (id[i] != 0x00U)
        {
            all_00 = 0U;
        }
        if (id[i] != 0xFFU)
        {
            all_ff = 0U;
        }
    }
    return ((all_00 != 0U) || (all_ff != 0U)) ? 1U : 0U;
}

/**
 * @brief 单 target 探测：选择通路 → RESET → READ_ID → ONFI 参数页 → 逐 LUN 78h
 *
 * @param ch   目标 ch
 * @param ce   目标 ce
 * @param info 输出：target 信息
 * @return OK / NOT_READY 无器件 / INVALID_ARGUMENT / OPERATION_FAILED
 */
nand_result_e fc_nand_target_probe(BYTE ch, BYTE ce, nand_target_info_t *info)
{
    const nand_device_desc_t *dev = nand_device_get();
    fc_nand_completion_t      completion;
    BYTE                      id[5];
    nand_result_e             result;
    WORD                      lun_count;

    ASSERT(dev != NULL);
    ASSERT(info != NULL);
    memset(info, 0, sizeof(*info));

    result = nand_select_target(ch, ce);
    if (result == NAND_RESULT_INVALID_ARGUMENT)
    {
        return result;
    }
    if (result != NAND_RESULT_OK)
    {
        return NAND_RESULT_NOT_READY;
    }

    delay_ms(1U);
    result = fc_nand_reset(&completion);
    if (result != NAND_RESULT_OK)
    {
        return NAND_RESULT_NOT_READY;
    }

    result = fc_nand_read_id(id, &completion);
    if (result != NAND_RESULT_OK)
    {
        return NAND_RESULT_NOT_READY;
    }
    if (fc_nand_id_is_blank(id) != 0U)
    {
        return NAND_RESULT_NOT_READY;
    }
    LOGI(LOG_TAG_NAND "ID=%02X %02X %02X %02X %02X\r\n", (unsigned int)id[0], (unsigned int)id[1], (unsigned int)id[2], (unsigned int)id[3], (unsigned int)id[4]);

    const DWORD id_value = ((DWORD)id[1] << 24U) | ((DWORD)id[2] << 16U) | ((DWORD)id[3] << 8U) | id[4];

    if (id_value != dev->expected_id)
    {
        LOGE(LOG_TAG_NAND "unexpected ID\r\n");
        return NAND_RESULT_OPERATION_FAILED;
    }

    {
        BYTE                   page[NAND_ONFI_PARAM_PAGE_SIZE];
        nand_onfi_param_info_t onfi;

        result = fc_nand_read_parameter_page(page, &completion);
        if (result != NAND_RESULT_OK)
        {
            LOGE(LOG_TAG_NAND "read parameter page failed, result=0x%02X status=0x%02X\r\n", (unsigned int)result, (unsigned int)completion.nand_status);
            return NAND_RESULT_OPERATION_FAILED;
        }
        if (nand_onfi_parameter_page_decode(page, &onfi) == 0U)
        {
            LOGE(LOG_TAG_NAND "parameter page signature mismatch (got %02X %02X %02X %02X)\r\n", (unsigned int)page[0], (unsigned int)page[1], (unsigned int)page[2], (unsigned int)page[3]);
            return NAND_RESULT_OPERATION_FAILED;
        }

        fc_nand_onfi_param_print(&onfi);
        if ((onfi.column_cycles != NAND_COLUMN_ADDR_CYCLES) || (onfi.row_cycles != NAND_ROW_ADDR_CYCLES))
        {
            LOGE(LOG_TAG_NAND "addr cycle mismatch: device C%u/R%u, driver C%u/R%u\r\n", (unsigned int)onfi.column_cycles, (unsigned int)onfi.row_cycles, (unsigned int)NAND_COLUMN_ADDR_CYCLES, (unsigned int)NAND_ROW_ADDR_CYCLES);
            return NAND_RESULT_INVALID_ARGUMENT;
        }
        if ((onfi.logical_units == 0U) || (onfi.logical_units > LUN_MAX))
        {
            LOGE(LOG_TAG_NAND "ONFI logical_units=%u out of range (LUN_MAX=%u)\r\n", (unsigned int)onfi.logical_units, (unsigned int)LUN_MAX);
            return NAND_RESULT_INVALID_ARGUMENT;
        }
        lun_count = (WORD)onfi.logical_units;
    }

    for (BYTE lun = 0U; lun < lun_count; lun++)
    {
        nand_addr_t addr;
        DWORD       row;
        BYTE        status;

        memset(&addr, 0, sizeof(addr));
        addr.lun = lun;
        row      = paa_to_nand_row(paa_from_addr(&addr));

        result = fc_nand_read_status_enhanced(row, &completion);
        if (result != NAND_RESULT_OK)
        {
            LOGE(LOG_TAG_NAND "lun %hhu status probe failed, result=0x%02X\r\n", lun, (unsigned int)result);
            return NAND_RESULT_OPERATION_FAILED;
        }

        status                 = completion.nand_status;
        info->lun_present[lun] = (status == NAND_STATUS_NO_RESPONSE) ? 0U : 1U;
        LOGI(LOG_TAG_NAND "lun=%hhu status=0x%02X present=%hhu\r\n", lun, (unsigned int)status, (unsigned int)info->lun_present[lun]);
    }

    if (info->lun_present[0] == 0U)
    {
        LOGE(LOG_TAG_NAND "target ch=%hhu ce=%hhu: LUN0 no response\r\n", ch, ce);
        return NAND_RESULT_NOT_READY;
    }

    info->present   = 1U;
    info->lun_count = (BYTE)lun_count;
    return NAND_RESULT_OK;
}

/**
 * @brief 遍历 CH_MAX × CE_MAX 探测 target
 *
 * @return OK / NOT_READY 无 target / 错误码
 */
nand_result_e fc_nand_scan_targets(void)
{
    BYTE present = 0U;

    memset(g_nand_targets, 0, sizeof(g_nand_targets));

    for (BYTE ch = 0U; ch < CH_MAX; ch++)
    {
        for (BYTE ce = 0U; ce < CE_MAX; ce++)
        {
            nand_target_info_t  info;
            const nand_result_e result = fc_nand_target_probe(ch, ce, &info);

            if (result == NAND_RESULT_OK)
            {
                g_nand_targets[ch][ce] = info;
                present                = 1U;
                LOGI(LOG_TAG_NAND "scan ch=%hhu ce=%hhu present luns=%hhu\r\n", ch, ce, info.lun_count);
            }
            else if (result != NAND_RESULT_NOT_READY)
            {
                LOGE(LOG_TAG_NAND "scan ch=%hhu ce=%hhu failed 0x%02X\r\n", ch, ce, (unsigned int)result);
                return result;
            }
        }
    }

    if (present == 0U)
    {
        LOGE(LOG_TAG_NAND "scan: no target present\r\n");
        return NAND_RESULT_NOT_READY;
    }
    return NAND_RESULT_OK;
}

/**
 * @brief FC 侧 NAND 初始化：器件硬件初始化 + 拓扑扫描
 *
 * @return OK / 错误码
 */
nand_result_e fc_nand_init(void)
{
    const nand_device_desc_t *dev = nand_device_get();

    ASSERT(dev != NULL);
    ASSERT(dev->hw_init != NULL);

    nand_result_e result = dev->hw_init();

    if (result != NAND_RESULT_OK)
    {
        LOGE(LOG_TAG_NAND "device hw init failed: 0x%02X\r\n", (unsigned int)result);
        return result;
    }

    result = fc_nand_scan_targets();
    if (result != NAND_RESULT_OK)
    {
        LOGE(LOG_TAG_NAND "target scan failed: 0x%02X\r\n", (unsigned int)result);
        return result;
    }

    LOGI(LOG_TAG_NAND "init complete (low speed)\r\n");
    return NAND_RESULT_OK;
}

/**
 * @brief boot 完成后切到 CONFIG_NAND_TIMING_MODE 并更新 FMC 时序
 *
 * armed 已置位，用 fc_nand_start/step 推进 SET_FEATURES；失败保持低速。
 *
 * @param fmc_hz FMC 内核时钟
 * @return OK / 错误码
 */
nand_result_e fc_nand_enable_high_speed(DWORD fmc_hz)
{
#if (CONFIG_NAND_TIMING_MODE != 0U)
    const BYTE                mode[4] = {(BYTE)CONFIG_NAND_TIMING_MODE, 0U, 0U, 0U};
    fc_nand_request_t         request;
    fc_nand_completion_t      completion;
    fc_nand_transaction_t     transaction;
    nand_result_e             result;
    const nand_device_desc_t *dev = nand_device_get();

    ASSERT(dev != NULL);
    ASSERT(dev->set_timing != NULL);

    if (fmc_hz == 0U)
    {
        LOGE(LOG_TAG_NAND "FMC clock unknown, fallback to 200MHz\r\n");
        fmc_hz = 200000000U;
    }

    memset(&request, 0, sizeof(request));
    request.column      = 0x01U; /* ONFI timing mode feature address */
    request.data        = (BYTE *)mode;
    request.data_length = 4U;

    memset(&completion, 0, sizeof(completion));
    memset(&transaction, 0, sizeof(transaction));
    transaction.engine.backend    = nand_phy_backend_get();
    transaction.engine.request    = &transaction.request;
    transaction.engine.completion = &transaction.completion;

    result = fc_nand_start(&transaction, OP_SET_FEATURES, &request);
    while (result == NAND_RESULT_NOT_READY)
    {
        result = fc_nand_step(&transaction, &completion);
    }
    if (result != NAND_RESULT_OK)
    {
        LOGE(LOG_TAG_NAND "set feature 01h (Mode %u) failed, result=0x%02hhX status=0x%02hhX\r\n", CONFIG_NAND_TIMING_MODE, result, transaction.completion.nand_status);
        return result;
    }

    result = dev->set_timing((BYTE)CONFIG_NAND_TIMING_MODE, fmc_hz);
    if (result != NAND_RESULT_OK)
    {
        LOGE(LOG_TAG_NAND "set timing Mode %u failed: 0x%02X\r\n", CONFIG_NAND_TIMING_MODE, (unsigned int)result);
        return result;
    }

    LOGI(LOG_TAG_NAND "switched NAND to ONFI Mode %u\r\n", CONFIG_NAND_TIMING_MODE);
    return NAND_RESULT_OK;
#else
    LOGI(LOG_TAG_NAND "high-speed mode disabled (CONFIG_NAND_TIMING_MODE=0)\r\n");
    return NAND_RESULT_OK;
#endif /* CONFIG_NAND_TIMING_MODE */
}
