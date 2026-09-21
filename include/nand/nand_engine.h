#ifndef PLATFORM_STM32H743_NAND_ENGINE_H
#define PLATFORM_STM32H743_NAND_ENGINE_H

#include <stdint.h>

#define NAND_RESULT_OK               0x00U
#define NAND_RESULT_INVALID_ARGUMENT 0xE1U
#define NAND_RESULT_TIMEOUT          0xE2U
#define NAND_RESULT_NOT_READY        0xE3U
#define NAND_RESULT_OPERATION_FAILED 0xE4U

typedef enum
{
    NAND_UOP_WAIT_READY,
    NAND_UOP_COMMAND,
    NAND_UOP_COMMAND_PRIMARY,
    NAND_UOP_COMMAND_SECONDARY,
    NAND_UOP_COLUMN_0,
    NAND_UOP_COLUMN_1,
    NAND_UOP_ROW_0,
    NAND_UOP_ROW_1,
    NAND_UOP_ROW_2,
    NAND_UOP_DATA_IN,
    NAND_UOP_DATA_OUT,
    NAND_UOP_DATA_OUT_STATUS,
    NAND_UOP_READ_STATUS,
    NAND_UOP_CHECK_READY,
    NAND_UOP_CHECK_FAIL,
    NAND_UOP_END,
    NAND_UOP_COUNT,
} nand_uop_type_t;

typedef struct
{
    nand_uop_type_t type;
    uint32_t        operand;
} nand_uop_t;

#include "nand_config.h"

typedef struct
{
    uint8_t  operation;
    uint16_t column;
    uint32_t row[NAND_PLANE_MAX]; /* 按 plane 索引，uop ROW  handler 通过 operand 选取 */
    uint8_t *buffer;
    uint16_t data_length;
} nand_command_request_t;

typedef struct
{
    uint8_t nand_status;
    uint8_t transfer_status;
} nand_command_completion_t;

typedef struct
{
    uint16_t twb_us;
    uint16_t trst_us;
    uint16_t twh_us;
    uint16_t busy_timeout_us;
} nand_operation_timing_t;

typedef uint8_t (*nand_uop_handler_t)(void *context, const nand_uop_t *uop, const nand_command_request_t *request, const nand_operation_timing_t *timing, nand_command_completion_t *completion);

typedef struct
{
    const nand_uop_handler_t *handlers;
    void                     *context;
} nand_engine_backend_t;

/* Persistent UOP cursor. One instance belongs to one active CH/CE/LUN transaction. */
typedef struct
{
    const nand_engine_backend_t   *backend;
    const nand_uop_t              *uops;
    const nand_operation_timing_t *timing;
    const nand_command_request_t  *request;
    nand_command_completion_t     *completion;
    uint16_t                       uop_index;
    uint32_t                       wait_start_cycles;
    uint16_t                       wait_polls;
    uint16_t                       debug_polls;
    uint8_t                        wait_started;
    uint8_t                        active;
} nand_engine_state_t;

uint8_t nand_engine_execute(const nand_engine_backend_t *backend, const nand_uop_t *uops, const nand_operation_timing_t *timing, const nand_command_request_t *request, nand_command_completion_t *completion);
uint8_t nand_engine_start(nand_engine_state_t *state, const nand_engine_backend_t *backend, const nand_uop_t *uops, const nand_operation_timing_t *timing, const nand_command_request_t *request, nand_command_completion_t *completion);
uint8_t nand_engine_step(nand_engine_state_t *state);

#endif /* PLATFORM_STM32H743_NAND_ENGINE_H */
