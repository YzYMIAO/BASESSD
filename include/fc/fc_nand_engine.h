#ifndef FC_NAND_ENGINE_H
#define FC_NAND_ENGINE_H

#include "nand_protocol.h"
#include "onfi_1_0.h"
#include "ssd_types.h"

#include <stdint.h>

/*
 * fc_nand_engine.h — FC 的 NAND 命令引擎与命令接口
 *
 * 合并自原 fc_nand_engine.h + fc_nand_engine.h：
 *   - UOP 微指令解释器（引擎游标/等待状态）；
 *   - 一条 ONFI 命令的异步事务；
 *   - 同步/异步执行入口、typed helpers、初始化与拓扑扫描。
 *
 * 依赖方向：FC 本头 → nand_protocol.h / onfi_1_0.h；NAND 不 include 本头。
 */

/* 忙等超时状态：started 一次性锁存 SysTick 微秒起点 start_us，polls 轮询
 * 计数（饱和递增）；两者任一超限即判超时。 */
typedef struct
{
    DWORD start_us; /* SysTick µs 起点 */
    WORD  polls;    /* 轮询计数 */
    BYTE  started;  /* 起点已锁存 */
} fc_uop_engine_wait_t;

/* Persistent UOP cursor. One instance belongs to one active CH/CE/LUN transaction.
 *
 * wait / jump 是两组独立的忙等超时状态：wait 归 WAIT_READY，jump 归
 * JUMP_IF_NOT_READY（回跳体会重跑返回 OK 的 UOP，若与 wait 共用会被清零）。 */
typedef struct
{
    const nand_phy_backend_t      *backend;
    const nand_uop_t              *uops;
    const nand_operation_timing_t *timing;
    const nand_command_request_t  *request;
    nand_command_completion_t     *completion;
    WORD                           uop_index;
    WORD                           debug_polls;
    BYTE                           active;
    WORD                           pre_jump_index;
    fc_uop_engine_wait_t           wait;
    fc_uop_engine_wait_t           jump;
} fc_uop_engine_state_t;

/**
 * @brief 阻塞式执行整条 UOP 序列，直到完成或出错。
 *
 * @deprecated 仅用于初始化阶段的同步调用（fc_nand_init）。
 * 运行时用 fc_uop_engine_start + fc_uop_engine_step 非阻塞推进。
 *
 * @param backend     NAND PHY 后端
 * @param uops        UOP 序列
 * @param timing      时序参数
 * @param request     命令请求
 * @param completion  完成信息出参
 * @return NAND_RESULT_OK 或错误码
 */
nand_result_e fc_uop_engine_execute(const nand_phy_backend_t *backend, const nand_uop_t *uops, const nand_operation_timing_t *timing, const nand_command_request_t *request, nand_command_completion_t *completion);

/**
 * @brief 复位引擎游标并立即推进一步（异步事务发起）。
 *
 * 调用前必须已绑定 state->backend / state->request / state->completion。
 *
 * @param state  引擎状态
 * @param uops   UOP 序列
 * @param timing 时序参数（busy_timeout_us 须 > 0）
 * @return OK（start 内完成）/ NOT_READY（需 step）/ 错误码
 */
nand_result_e fc_uop_engine_start(fc_uop_engine_state_t *state, const nand_uop_t *uops, const nand_operation_timing_t *timing);

/**
 * @brief 推进 UOP 游标一次（可消费多个 UOP），驱动 NAND PHY。
 *
 * @param state 引擎状态
 * @return OK / NOT_READY（让出）/ 错误码
 */
nand_result_e fc_uop_engine_step(fc_uop_engine_state_t *state);

/* 一条 NAND 命令的异步事务：UOP 引擎状态 + 命令/完成存储体。 */
typedef struct
{
    fc_uop_engine_state_t     engine;
    nand_command_request_t    request;
    nand_command_completion_t completion;
} fc_nand_transaction_t;

/* 同步执行一条 OP（仅初始化阶段，armed == 0）。 */
nand_result_e fc_nand_execute(BYTE ch, BYTE ce, BYTE operation, const nand_command_request_t *request, nand_command_completion_t *completion);

/* 异步执行：start 发起 + step 推进。 */
nand_result_e fc_nand_start(fc_nand_transaction_t *transaction, BYTE operation, const nand_command_request_t *request);
nand_result_e fc_nand_step(fc_nand_transaction_t *transaction, nand_command_completion_t *completion);

/* Typed ONFI 操作 helpers。 */
nand_result_e fc_nand_reset(BYTE ch, BYTE ce, nand_command_completion_t *completion);
nand_result_e fc_nand_read_id(BYTE ch, BYTE ce, BYTE id[5], nand_command_completion_t *completion);
nand_result_e fc_nand_read_parameter_page(BYTE ch, BYTE ce, BYTE page[NAND_ONFI_PARAM_PAGE_SIZE], nand_command_completion_t *completion);
nand_result_e fc_nand_set_features(BYTE ch, BYTE ce, BYTE feature_address, const BYTE feature_data[4], nand_command_completion_t *completion);
nand_result_e fc_nand_read_status_enhanced(BYTE ch, BYTE ce, DWORD row, nand_command_completion_t *completion);

/* 启动流程（FC 编排，NAND 只提供 hw_init/desc/PHY）。 */
nand_result_e fc_nand_init(void);
nand_result_e fc_nand_enable_high_speed(DWORD fmc_hz);

/* 拓扑扫描。 */
nand_result_e fc_nand_target_probe(BYTE ch, BYTE ce, nand_target_info_t *info);
nand_result_e fc_nand_scan_targets(void);

#endif /* FC_NAND_ENGINE_H */
