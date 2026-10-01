#ifndef NAND_FC_INTERFACE_H
#define NAND_FC_INTERFACE_H

#include "nand/onfi_1_0.h"

#include <stdint.h>

/*
 * NAND FC 通用接口类型。
 * 定义 FC 模块和 NAND 底层之间的请求/完成/事务数据结构，
 * 不依赖具体芯片型号。
 *
 * 依赖：onfi_1_0.h（nand_engine_state_t / nand_command_* 类型）、
 *        nand_config.h（NAND_PLANE_MAX 拓扑宏）。
 */

#include "nand/nand_config.h"

/* NAND 命令请求：列/行/数据。row 按 plane 索引（NAND_PLANE_MAX 深）。 */
typedef struct
{
    uint16_t column;
    uint32_t row[NAND_PLANE_MAX]; /* 按 plane 索引，单 plane 用 row[0]，多 plane 填 row[0..plane_count-1] */
    uint8_t *data;
    uint16_t data_length;
} nand_fc_request_t;

/* NAND 完成信息：execution_status = NAND_RESULT_*；nand_status = 70h 原始状态。 */
typedef struct
{
    uint8_t execution_status; /* NAND_RESULT_* */
    uint8_t nand_status;      /* Raw 70h status value after command completion. */
} nand_fc_completion_t;

/* 异步事务：封装引擎状态 + 命令数据 + per-LUN backend（Phase V）。 */
typedef struct
{
    nand_engine_state_t          engine;
    nand_command_request_t       request;
    nand_command_completion_t    completion;
    const nand_engine_backend_t *backend; /* per-LUN backend (Phase V) */
    uint8_t                      active;
} nand_fc_transaction_t;

#endif /* NAND_FC_INTERFACE_H */
