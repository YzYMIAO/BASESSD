/*
 * nand_runtime.c — NAND 运行时放行闩锁
 *
 * 闩锁表示「FMC 已由同步初始化路径移交给异步执行路径」。它属于 NAND 器件层：
 * 器件驱动的同步入口 fc_nand_execute() 用它断言「仅初始化阶段可调」，而 FC 的
 * 命令分发只负责置位，不拥有这份状态。这样 NAND 不需要 include 任何 FC 头，
 * 依赖方向保持 FC → NAND。
 */

#include "nand_device.h"

static BYTE g_nand_runtime_armed;

/**
 * @brief 置/清异步执行放行闩锁（归一化到 0/1）
 *
 * @param armed 非 0 放行
 */
void nand_set_armed(BYTE armed)
{
    g_nand_runtime_armed = (armed != 0U) ? 1U : 0U;
}

/**
 * @brief 查询异步执行放行闩锁
 *
 * @return 1 已放行；0 未放行
 */
BYTE nand_is_armed(void)
{
    return g_nand_runtime_armed;
}
