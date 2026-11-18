#ifndef FC_NAND_ENGINE_H
#define FC_NAND_ENGINE_H

#include "nand_interface.h"
#include "ssd_types.h"

#include <stdint.h>

/*
 * fc_nand_engine.h — FC 的 NAND 命令引擎（异步事务 + 初始化/拓扑）
 *
 * 对外只暴露：
 *   - UOP 引擎游标/等待状态与 start/step；
 *   - 一条 ONFI 命令的异步事务（fc_nand_txn_s）；
 *   - 异步放行闩锁与初始化/高速切换入口。
 * 同步阻塞执行、typed ONFI helpers、target 探测只在本文件内使用（static）。
 *
 * 依赖方向：FC 本头 → nand_protocol.h / onfi_1_0.h；NAND 不 include 本头。
 */

/* 忙等超时状态：started 一次性锁存 SysTick 微秒起点 start_us，polls 轮询
 * 计数（饱和递增）；两者任一超限即判超时。 */
typedef struct FC_UOP_ENGINE_WAIT_S
{
    DWORD start_us; /* SysTick µs 起点 */
    WORD  polls;    /* 轮询计数 */
    BYTE  started;  /* 起点已锁存 */
} fc_uop_engine_wait_s;

/* Persistent UOP cursor. One instance belongs to one active CH/CE/LUN transaction.
 *
 * wait / jump 是两组独立的忙等超时状态：wait 归 WAIT_READY，jump 归
 * JUMP_IF_NOT_READY（回跳体会重跑返回 OK 的 UOP，若与 wait 共用会被清零）。 */
typedef struct FC_UOP_ENGINE_STATE_S
{
    const nand_phy_backend_s *backend;
    const nand_uop_s         *uops;
    const nand_op_timing_s   *timing;
    const nand_cmd_req_s     *request;
    nand_cmd_comp_s          *completion;
    WORD                      uop_index;
    WORD                      debug_polls;
    BYTE                      active;
    WORD                      pre_jump_index;
    fc_uop_engine_wait_s      wait;
    fc_uop_engine_wait_s      jump;
} fc_uop_engine_state_s;

/**
 * @brief 置/清异步路径放行闩锁（0 = 同步初始化；1 = 异步 dispatch）。
 *
 * @param armed 非 0 放行。
 */
void fc_nand_engine_set_armed(BYTE armed);

/**
 * @brief 查询异步路径放行闩锁。
 *
 * @return 1 = 已放行；0 = 同步初始化阶段。
 */
BYTE fc_nand_engine_is_armed(void);

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
nand_result_e fc_uop_engine_start(fc_uop_engine_state_s *state, const nand_uop_s *uops, const nand_op_timing_s *timing);

/**
 * @brief 推进 UOP 游标一次（可消费多个 UOP），驱动 NAND PHY。
 *
 * @param state 引擎状态
 * @return OK / NOT_READY（让出）/ 错误码
 */
nand_result_e fc_uop_engine_step(fc_uop_engine_state_s *state);

/* 一条 NAND 命令的异步事务：UOP 引擎状态 + 命令/完成存储体。 */
typedef struct FC_NAND_TRANSACTION_S
{
    fc_uop_engine_state_s engine;
    nand_cmd_req_s        request;
    nand_cmd_comp_s       completion;
} fc_nand_txn_s;

/**
 * @brief 发起一条异步 ONFI 事务
 *
 * @param transaction 事务对象；engine.backend 必须由调用者按 target 预先绑定
 * @param operation   OP_*
 * @param request     命令参数（字符串/缓冲生命周期须覆盖到事务完成）
 * @return OK（start 内完成）/ NOT_READY（需 fc_nand_step）/ 错误码
 *
 * @note transaction->engine.request/completion 由本函数统一绑定；
 *       调用者不再需要手工绑定这两个指针。
 */
nand_result_e fc_nand_start(fc_nand_txn_s *transaction, BYTE operation, const nand_cmd_req_s *request);

/**
 * @brief 推进一条异步事务
 *
 * @param transaction 已 start 且仍 active 的事务
 * @param completion  输出：本次完成时的执行状态 + NAND 状态
 * @return NOT_READY（继续 poll）/ 完成结果
 */
nand_result_e fc_nand_step(fc_nand_txn_s *transaction, nand_cmd_comp_s *completion);

#if defined(FC_NAND_TEST_ENABLE)
/**
 * @brief 测试专用：调用参数页读取路径（用于 ONFI 读失败注入）。
 */
nand_result_e fc_nand_test_read_parameter_page(BYTE ch, BYTE ce, BYTE page[NAND_ONFI_PARAM_PAGE_SIZE], nand_cmd_comp_s *completion);
#endif

#endif /* FC_NAND_ENGINE_H */
