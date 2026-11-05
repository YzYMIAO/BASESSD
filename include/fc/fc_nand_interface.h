#ifndef FC_NAND_INTERFACE_H
#define FC_NAND_INTERFACE_H

#include "fc_uop_engine.h"
#include "nand_device.h"
#include "nand_protocol.h"
#include "onfi_1_0.h"

#include <stdint.h>

/*
 * fc_nand_interface.h — FC 对 NAND 器件执行层的命令接口
 *
 * 职责：FC 的 ONFI 命令序列 + UOP 引擎的编排入口。器件事实由
 * nand_device_desc_t 提供，PHY 由 NAND 的 FMC 后端实现；
 * 本头不暴露 HAL 类型。
 *
 * 依赖方向：FC 本头 → nand_device.h / nand_protocol.h；NAND 不 include 本头。
 */

/* FC 侧命令请求（对 nand_command_request_t 的语义化视图）。 */
typedef struct
{
    WORD  column;
    DWORD row[NAND_PLANE_MAX]; /* 按 plane 索引 */
    BYTE *data;
    WORD  data_length;
} fc_nand_request_t;

/* FC 侧完成信息。 */
typedef struct
{
    BYTE execution_status; /* NAND_RESULT_* */
    BYTE nand_status;      /* 70h 原始状态：OP 后（FAIL/FAILC） */
    BYTE pre_status;       /* 70h 原始状态：OP 前（RDY/ARDY/WP） */
} fc_nand_completion_t;

/* 一条 NAND 命令的异步事务：UOP 引擎状态 + 命令/完成存储体。 */
typedef struct
{
    fc_uop_engine_state_t     engine;
    nand_command_request_t    request;
    nand_command_completion_t completion;
} fc_nand_transaction_t;

/* 同步执行一条 OP（仅初始化阶段，armed == 0）。 */
nand_result_e fc_nand_execute(BYTE operation, const fc_nand_request_t *request, fc_nand_completion_t *completion);

/* 异步执行：start 发起 + step 推进。 */
nand_result_e fc_nand_start(fc_nand_transaction_t *transaction, BYTE operation, const fc_nand_request_t *request);
nand_result_e fc_nand_step(fc_nand_transaction_t *transaction, fc_nand_completion_t *completion);

/* Typed ONFI 操作 helpers。 */
nand_result_e fc_nand_reset(fc_nand_completion_t *completion);
nand_result_e fc_nand_read_id(BYTE id[5], fc_nand_completion_t *completion);
nand_result_e fc_nand_read_parameter_page(BYTE page[NAND_ONFI_PARAM_PAGE_SIZE], fc_nand_completion_t *completion);
nand_result_e fc_nand_set_features(BYTE feature_address, const BYTE feature_data[4], fc_nand_completion_t *completion);
nand_result_e fc_nand_read_status_enhanced(DWORD row, fc_nand_completion_t *completion);

/*
 * 启动流程（FC 编排，NAND 只提供 hw_init/desc/PHY）：
 *   fc_nand_init —— 器件 hw_init → 逐 target RESET/READ_ID/参数页/78h 探测，
 *                   填 g_nand_targets。仅在 armed == 0 时调用。
 *   fc_nand_enable_high_speed —— SET_FEATURES 切 mode，再让器件层重配 FMC 时序。
 */
nand_result_e fc_nand_init(void);
nand_result_e fc_nand_enable_high_speed(DWORD fmc_hz);

/* 拓扑扫描。 */
nand_result_e fc_nand_target_probe(BYTE ch, BYTE ce, nand_target_info_t *info);
nand_result_e fc_nand_scan_targets(void);

#endif /* FC_NAND_INTERFACE_H */
