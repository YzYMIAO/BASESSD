#ifndef FC_NAND_ENGINE_H
#define FC_NAND_ENGINE_H

#include "nand_interface.h"
#include "ssd_types.h"

#include <stdint.h>

/*
 * fc_nand_engine.h — FC 的 NAND 命令引擎（异步事务 + 初始化/拓扑）
 *
 * 对外只暴露：
 *   - UOP 引擎游标/等待状态（start/step 只在本文件使用；step 仅单测构建导出）；
 *   - 一条 ONFI 命令的异步事务（fc_nand_txn_s）；
 *   - 异步放行闩锁与初始化/高速切换入口。
 * 同步阻塞执行、typed ONFI helpers、target 探测只在本文件内使用（static）。
 *
 * 依赖方向：FC 本头 → nand_protocol.h / onfi_1_0.h；NAND 不 include 本头。
 */

/* 单测直接调用 uop 步进（覆盖 JUMP 回跳守卫等）；产品档该入口保持 static。 */
#if defined(FC_NAND_TEST_ENABLE)
#    define FC_UOP_STEP_LINKAGE
#else
#    define FC_UOP_STEP_LINKAGE static
#endif

/* 忙等超时状态：started 一次性锁存 SysTick 微秒起点 start_us，polls 轮询
 * 计数（饱和递增）；两者任一超限即判超时。 */
typedef struct FC_UOP_ENGINE_WAIT_S
{
    DWORD start_us; /* SysTick µs 起点 */
    WORD  polls;    /* 轮询计数 */
    BYTE  started;  /* 起点已锁存 */
} fc_uop_engine_wait_s;

/* UOP 游标：一个实例属于一条活跃的 CH/CE/LUN 事务。
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

/* 一条 NAND 命令的异步事务：UOP 引擎状态 + 命令/完成存储体。 */
typedef struct FC_NAND_TRANSACTION_S
{
    fc_uop_engine_state_s engine;
    nand_cmd_req_s        request;
    nand_cmd_comp_s       completion;
} fc_nand_txn_s;

/**
 * @brief 查询异步路径放行闩锁。
 */
BYTE fc_nand_engine_is_armed(void);

/**
 * @brief 置/清异步路径放行闩锁（0 = 同步初始化；1 = 异步 dispatch）。
 */
void fc_nand_engine_set_armed(BYTE armed);

/**
 * @brief 发起一条异步 ONFI 事务。
 * @note transaction->engine.backend 必须由调用者按 target 预先绑定；
 *       request 的字符串 / 缓冲生命周期须覆盖到事务完成；
 *       transaction->engine.request / completion 由本函数统一绑定。
 */
nand_result_e fc_nand_start(fc_nand_txn_s *transaction, BYTE operation, const nand_cmd_req_s *request);

#if defined(FC_NAND_TEST_ENABLE)
/**
 * @brief 测试专用：调用参数页读取路径（用于 ONFI 读失败注入）。
 */
nand_result_e fc_nand_test_read_parameter_page(BYTE ch, BYTE ce, BYTE page[NAND_ONFI_PARAM_PAGE_SIZE], nand_cmd_comp_s *completion);
#endif

#if defined(FC_NAND_TEST_ENABLE)
/**
 * @brief 推进 UOP 游标一次（可消费多个 UOP），驱动 NAND PHY。
 * @note 产品档为本文件 static；单测需要直接构造引擎状态覆盖回跳守卫，故仅 UT 构建导出。
 */
FC_UOP_STEP_LINKAGE nand_result_e fc_uop_engine_step(fc_uop_engine_state_s *state);
#endif

/**
 * @brief 推进一条异步事务。
 */
nand_result_e fc_nand_step(fc_nand_txn_s *transaction, nand_cmd_comp_s *completion);

#endif /* FC_NAND_ENGINE_H */
