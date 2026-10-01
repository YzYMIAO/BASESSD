#ifndef PLATFORM_STM32H743_NAND_ENGINE_H
#define PLATFORM_STM32H743_NAND_ENGINE_H

#include <stdint.h>

/* --------------------------------------------------------------------
 * nand_engine.h — UOP 微指令引擎抽象层
 *
 * 引擎是 NAND 命令执行的通用解释器：一条命令 = 一个 nand_uop_t 数组
 * （命令/地址/数据/等待/跳转微指令序列），引擎按顺序推进并驱动
 * "后端"（backend，本工程为 FMC）执行实际硬件访问。
 *
 * 多后端可替换：nand_engine_backend_t 是唯一依赖点，未来可挂接
 * 其他 NAND 控制器而不改引擎。
 *
 * 协作式调度：nand_engine_step() 返回 NOT_READY 表示"外设忙，让出"，
 * 调用方应稍后再次 step；引擎自身不阻塞。
 *
 * 错误码约定：NAND_RESULT_* 是自定义体系（非 HAL/POSIX），
 * 0x00 成功，0xEx 系列为错误。
 * -------------------------------------------------------------------- */

/* 引擎执行结果码（自定义错误体系） */
typedef enum
{
    NAND_RESULT_OK               = 0x00U, /* 成功 */
    NAND_RESULT_INVALID_ARGUMENT = 0xE1U, /* 参数/序列非法 */
    NAND_RESULT_TIMEOUT          = 0xE2U, /* busy 超时 */
    NAND_RESULT_NOT_READY        = 0xE3U, /* 外设忙，让出（非错误） */
    NAND_RESULT_OPERATION_FAILED = 0xE4U, /* NAND 操作失败（状态字节 FAIL 位） */
} nand_result_t;

/*
 * UOP 微指令类型。
 *
 * operand 字段在各 UOP 下的含义（关键约定，读代码前必读）：
 *   COMMAND           → 命令字节（如 00h/30h/80h/10h）
 *   COLUMN_0/1        → 不使用（列地址取自 request->column）
 *   ROW_0/1/2         → plane 下标（request->row[operand] 的 0/1）
 *   DATA_IN/OUT       → buffer 偏移（Multi-Plane 第二 plane 偏移量）
 *   DATA_OUT_STATUS   → 不使用（直读 DQ）
 *   JUMP_IF_NOT_READY → 回跳步数（NAND busy 时回跳重新执行 70h 轮询）
 *   CHECK_FAIL        → 不使用（检查上一步读到的状态字节）
 *   DELAY_US          → 延时微秒数
 *   WAIT_READY        → 不使用（等待 RB#）
 */
typedef enum
{
    NAND_UOP_WAIT_READY,        /* 等待 NAND ready（RB# 轮询，超时检测） */
    NAND_UOP_COMMAND,           /* 写命令字节，operand=命令值 */
    NAND_UOP_COLUMN_0,          /* 写列地址低字节 */
    NAND_UOP_COLUMN_1,          /* 写列地址高字节 */
    NAND_UOP_ROW_0,             /* 写行地址低字节（plane=operand） */
    NAND_UOP_ROW_1,             /* 写行地址中间字节 */
    NAND_UOP_ROW_2,             /* 写行地址高字节 */
    NAND_UOP_DATA_IN,           /* 写数据到 NAND（Program） */
    NAND_UOP_DATA_OUT,          /* 从 NAND 读数据 */
    NAND_UOP_DATA_OUT_STATUS,   /* 直读 70h 状态字节 */
    NAND_UOP_JUMP_IF_NOT_READY, /* 条件跳转：busy 则回跳 operand 步 */
    NAND_UOP_CHECK_FAIL,        /* 检查状态字节 FAIL 位 */
    NAND_UOP_DELAY_US,          /* 固定微秒延时，operand=延时微秒数 */
    NAND_UOP_END,               /* 序列结束 */
    NAND_UOP_COUNT,             /* 枚举数量（数组长度用） */
} nand_uop_type_t;

/* 单条微指令：类型 + 参数 */
typedef struct
{
    nand_uop_type_t type;
    uint32_t        operand;
} nand_uop_t;

#include "nand_config.h"

/* 命令请求：列/行/数据。row 按 plane 索引（NAND_PLANE_MAX 深），
 * 单 plane 只用 row[0]，多 plane 填 row[0..plane_count-1]。 */
typedef struct
{
    uint8_t  operation;
    uint16_t column;
    uint32_t row[NAND_PLANE_MAX]; /* 按 plane 索引，uop ROW  handler 通过 operand 选取 */
    uint8_t *buffer;
    uint16_t data_length;
} nand_command_request_t;

/* 命令完成信息：nand_status = 70h 原始状态字节；
 * transfer_status = NAND_RESULT_* 执行结果。两字段语义不同。 */
typedef struct
{
    uint8_t nand_status;
    uint8_t transfer_status;
} nand_command_completion_t;

/* 操作时序参数：busy_timeout_us 供引擎和 FMC handler 共用超时 */
typedef struct
{
    uint16_t twb_us;
    uint16_t trst_us;
    uint16_t twh_us;
    uint16_t busy_timeout_us;
} nand_operation_timing_t;

/* UOP handler 签名：context 透传给后端（各 handler 自行转换） */
typedef nand_result_t (*nand_uop_handler_t)(void *context, const nand_uop_t *uop, const nand_command_request_t *request, const nand_operation_timing_t *timing, nand_command_completion_t *completion);

/* 后端抽象：handler 表 + 透传上下文 */
typedef struct
{
    const nand_uop_handler_t *handlers;
    void                     *context;
} nand_engine_backend_t;

/*
 * Persistent UOP cursor. One instance belongs to one active CH/CE/LUN transaction.
 *
 * wait_start_cycles / wait_polls / wait_started：
 *   JUMP_IF_NOT_READY 与 WAIT_READY 共用的超时状态——
 *   wait_started 一次性锁存 DWT 起点，wait_polls 轮询计数，
 *   两者任一超限即判超时。
 * debug_polls 仅调试用（program pending 日志）。
 */
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

/* --------------------------------------------------------------------
 * nand_engine_execute
 *   功能：阻塞式执行整条 UOP 序列，直到完成或出错。
 *   参数：backend —— 后端 handler 表 + context；
 *         uops —— UOP 序列；timing —— 时序参数；
 *         request/completion —— 命令数据/完成信息。
 *   返回：NAND_RESULT_OK 或错误码。
 *
 * @deprecated 仅用于初始化阶段的同步调用（nand_init）。
 * 运行时请用 nand_engine_start + nand_engine_step 非阻塞推进。
 * -------------------------------------------------------------------- */
nand_result_t nand_engine_execute(const nand_engine_backend_t *backend, const nand_uop_t *uops, const nand_operation_timing_t *timing, const nand_command_request_t *request, nand_command_completion_t *completion);

/* --------------------------------------------------------------------
 * nand_engine_start
 *   功能：初始化引擎状态并立即推进一步（异步事务发起）。
 *   参数：state —— 引擎游标（调用方提供）；其余同 execute。
 *   返回：NAND_RESULT_OK（无 busy 阶段，start 内即完成，active 已清）/
 *         NOT_READY（有 busy 阶段，需 step 推进）/ 错误码。
 * -------------------------------------------------------------------- */
nand_result_t nand_engine_start(nand_engine_state_t *state, const nand_engine_backend_t *backend, const nand_uop_t *uops, const nand_operation_timing_t *timing, const nand_command_request_t *request, nand_command_completion_t *completion);

/* --------------------------------------------------------------------
 * nand_engine_step
 *   功能：推进 UOP 游标一次（可消费多个 UOP），驱动后端。
 *   参数：state —— 引擎状态（游标 + 超时字段）。
 *   返回：NAND_RESULT_OK / NOT_READY（让出）/ 错误码。
 *   调用方约定：NOT_READY 时稍后重调；active==0 后不得再调。
 * -------------------------------------------------------------------- */
nand_result_t nand_engine_step(nand_engine_state_t *state);

#endif /* PLATFORM_STM32H743_NAND_ENGINE_H */
