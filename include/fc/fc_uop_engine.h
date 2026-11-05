#ifndef FC_UOP_ENGINE_H
#define FC_UOP_ENGINE_H

#include "nand_protocol.h"
#include "ssd_types.h" /* BYTE/WORD/DWORD/QWORD */

#include <stdint.h>

/* --------------------------------------------------------------------
 * fc_uop_engine.h — FC 的 UOP 微指令引擎
 *
 * 引擎是 FC 命令序列器的执行核心：一条命令 = 一个 nand_uop_t 数组
 * （命令/地址/数据/等待/跳转微指令序列），引擎按顺序推进并通过
 * nand_phy_backend_t 驱动 NAND PHY（FMC 后端）。
 *
 * 协作式调度：fc_uop_engine_step() 返回 NOT_READY 表示"外设忙，让出"，
 * 调用方稍后再次 step；引擎自身不阻塞。
 *
 * 类型归属：UOP/请求/完成/时序/后端在 include/nand/nand_protocol.h（裸类型，
 * NAND 与 FC 共用）；引擎游标与状态在本头（FC 私有）。
 * -------------------------------------------------------------------- */

/* 忙等超时状态：started 一次性锁存 SysTick 微秒起点 start_us，polls 轮询
 * 计数（饱和递增）；两者任一超限即判超时。 */
typedef struct
{
    DWORD start_us; /* SysTick µs 起点 */
    WORD  polls;    /* 轮询计数 */
    BYTE  started;  /* 起点已锁存 */
} fc_uop_engine_wait_t;

/*
 * Persistent UOP cursor. One instance belongs to one active CH/CE/LUN transaction.
 *
 * wait / jump：两组**独立**的忙等超时状态。
 *   - wait 归 WAIT_READY：该 UOP 未就绪时原地重入（uop_index 不变），
 *     无前置 OK-UOP 复位，计数可正常累加。
 *   - jump 归 JUMP_IF_NOT_READY：其回跳体会重跑若干**返回 OK** 的 UOP
 *     （COMMAND 70h / DELAY_US / DATA_OUT_STATUS），而引擎 OK 分支会清零
 *     wait——若与 wait 共用，计数每轮被清零，超时永不触发。故独立。
 * debug_polls 仅调试用（program pending 日志）。
 */
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
    WORD                           pre_jump_index; /* 第一个 JUMP_IF_NOT_READY 的 uop 下标；只在该下标更新 pre_status */
    fc_uop_engine_wait_t           wait;           /* WAIT_READY 忙等状态 */
    fc_uop_engine_wait_t           jump;           /* JUMP_IF_NOT_READY 忙等状态 */
} fc_uop_engine_state_t;

/**
 * @brief 阻塞式执行整条 UOP 序列，直到完成或出错。
 *
 * @deprecated 仅用于初始化阶段的同步调用（fc_nand_init）。
 * 运行时用 fc_uop_engine_start + fc_uop_engine_step 非阻塞推进。
 *
 * @param backend     NAND PHY 后端（handler 表 + context）
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
 * 只复位游标/超时/active，不整体清零已绑定指针。
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

#endif /* FC_UOP_ENGINE_H */
