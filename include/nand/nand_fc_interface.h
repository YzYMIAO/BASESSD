#ifndef NAND_FC_INTERFACE_H
#define NAND_FC_INTERFACE_H

#include "onfi_1_0.h"

#include <stdint.h>

/*
 * NAND FC 通用接口类型。
 * 定义 FC 模块和 NAND 底层之间的请求/完成/事务数据结构，
 * 不依赖具体芯片型号。
 *
 * 依赖：onfi_1_0.h（nand_engine_state_t / nand_command_* 类型）、
 *        nand_config.h（NAND_PLANE_MAX 拓扑宏）。
 */

#include "nand_config.h"

/* NAND 命令请求：列/行/数据。row 按 plane 索引（NAND_PLANE_MAX 深）。 */
typedef struct
{
    WORD  column;
    DWORD row[NAND_PLANE_MAX]; /* 按 plane 索引，单 plane 用 row[0]，多 plane 填 row[0..plane_count-1] */
    BYTE *data;
    WORD  data_length;
} nand_fc_request_t;

/* NAND 完成信息：execution_status = NAND_RESULT_*；nand_status = 70h 原始状态。 */
typedef struct
{
    BYTE execution_status; /* NAND_RESULT_* */
    BYTE nand_status;      /* 70h 原始状态：OP 后（FAIL/FAILC） */
    BYTE pre_status;       /* 70h 原始状态：OP 前（RDY/ARDY/WP） */
} nand_fc_completion_t;

/* 异步事务：引擎状态 + 命令数据存储体。
 * per-LUN backend 与 active 不在这里另存——统一走 engine.backend / engine.active，
 * 避免同一状态存两份、靠手工同步。engine.request / engine.completion 指向下面两个
 * 存储体，由 nand_dispatch_task_load 在命令装载时一次性绑定，地址随事务固定。 */
typedef struct
{
    nand_engine_state_t       engine;
    nand_command_request_t    request;
    nand_command_completion_t completion;
} nand_fc_transaction_t;

/*
 * ONFI 通用执行入口与 typed 壳。
 *
 * 这些函数由**被选中的器件驱动**实现（src/nand/devices/<family>.c），
 * 但对上层是器件无关契约：OP_* → g_nand_operation_map → UOP → FMC backend。
 * nand_fc_execute = 同步；nand_fc_start/step = 异步（start 发起 + step 推进）。
 */
nand_result_e nand_fc_execute(BYTE operation, const nand_fc_request_t *request, nand_fc_completion_t *completion);
nand_result_e nand_fc_start(nand_fc_transaction_t *transaction, BYTE operation, const nand_fc_request_t *request);
nand_result_e nand_fc_step(nand_fc_transaction_t *transaction, nand_fc_completion_t *completion);

/* Typed ONFI operation helpers. No helper writes FMC command/address/data windows directly. */
nand_result_e nand_fc_reset(nand_fc_completion_t *completion);
nand_result_e nand_fc_read_id(BYTE id[5], nand_fc_completion_t *completion);
/* 读 ONFI 参数页（ECh，256 字节，同步）。缓冲长度 = NAND_ONFI_PARAM_PAGE_SIZE（nand/onfi_1_0.h）。 */
nand_result_e nand_fc_read_parameter_page(BYTE page[NAND_ONFI_PARAM_PAGE_SIZE], nand_fc_completion_t *completion);
nand_result_e nand_fc_set_features(BYTE feature_address, const BYTE feature_data[4], nand_fc_completion_t *completion);
/* READ STATUS ENHANCED (78h)：按行地址（含 LUN/plane 位）读指定 LUN 的状态字节。 */
nand_result_e nand_fc_read_status_enhanced(DWORD row, nand_fc_completion_t *completion);

/*
 * 板级 target 选择：把后续 NAND 命令发往 (ch,ce)。
 *
 * 当前板只有 FMC Bank3 + 单 NCE，(0,0) 有物理通路；其他坐标返回
 * NAND_RESULT_NOT_READY。多 target 板卡在此切换 FMC bank / CE# GPIO。
 * 扫描与逐 target 探测前必须先调用；它只选择通路，不判断器件是否存在。
 */
nand_result_e nand_select_target(BYTE ch, BYTE ce);

/*
 * 上电拓扑扫描（同步，仅在 nand_dispatch_set_armed(1) 之前可调）：
 *   nand_target_probe — 探测单个 (ch,ce)：RESET / READ ID / ONFI 参数页 +
 *                       逐 LUN 78h 探测；返回 present、lun_count、lun_present[]。
 *   nand_scan_targets — 遍历 CH_MAX × CE_MAX，填 g_nand_targets。
 *                       无物理通路 / 器件不存在 ⇒ 跳过；存在但 ID/几何不符 ⇒ 返回错误。
 */
nand_result_e nand_target_probe(BYTE ch, BYTE ce, nand_target_info_t *info);
nand_result_e nand_scan_targets(void);

#endif /* NAND_FC_INTERFACE_H */
