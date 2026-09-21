/*
 * nand_dispatch_internal.h
 *
 * 内部数据结构，不对外暴露。
 * 由 nand_dispatch.c / nand_dispatch_tables.c 共享。
 */

#ifndef NAND_DISPATCH_INTERNAL_H
#define NAND_DISPATCH_INTERNAL_H

#include "fc/fc_command.h"
#include "fc/fc_types.h"
#include "nand_config.h"
#include "nand_dispatch.h"
#include "nand_fc_interface.h"
#include "onfi_1_0.h"

#include <stdint.h>

typedef struct
{
    uint16_t command_id;
    uint8_t  operation;
    uint8_t  ch;            /* Channel — needed for page_buffer access. */
    uint8_t  page_type : 2; /* Copy from dispatch request for LDPC engine. */
    uint8_t  reserved  : 6;
    uint8_t  cw_index; /* Current CW being processed (0-based). */
    uint8_t  cw_count;
    uint8_t  started;
    uint8_t  parameter[4];
    uint8_t  feature_addr;

    /* Pre-resolved per-CW data; cw_count entries valid. */
    nand_paa_t cw_paa[CW_NUM];
    uint8_t   *cw_buffers[CW_NUM];

    nand_fc_request_t          request;
    nand_fc_transaction_t      transaction;
    nand_dispatch_completion_t completion;
} nand_dispatch_task_t;

/* ======================================================================
 * 函数指针表类型定义
 * ====================================================================== */

/* 异步 request 构建 handler：用 task 的预解析数据组装 nand_fc_request_t。
 * 返回 NAND_RESULT_OK 或 NAND_RESULT_INVALID_ARGUMENT。 */
typedef uint8_t (*nand_dispatch_req_handler_t)(nand_dispatch_task_t *task);

/* Completion 策略 handler：根据不同操作类型设置 cw_status 和 failed_descriptor_id。 */
typedef void (*nand_dispatch_completion_handler_t)(
    nand_dispatch_task_t       *task,
    uint8_t                     result,
    const nand_fc_completion_t *nand_completion);

/* OP_COUNT 项，OP_* 直接作为下标。 */
extern const nand_dispatch_req_handler_t        g_nand_dispatch_req_table[OP_COUNT];
extern const nand_dispatch_completion_handler_t g_nand_dispatch_completion_table[OP_COUNT];

#endif /* NAND_DISPATCH_INTERNAL_H */