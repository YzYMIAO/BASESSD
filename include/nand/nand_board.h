#ifndef NAND_BOARD_H
#define NAND_BOARD_H

#include "nand_protocol.h"

#include <stddef.h>

/*
 * nand_board.h — 板级 NAND target 枚举与选择接口
 *
 * 本头只暴露 (ch,ce) 坐标和板级动作，不依赖具体 MCU/PHY：
 *   - 目标板替换 src/nand/nand_board.c 与平台 FMC/PHY 实现；
 *   - 器件层只通过 target 表和 backend 取得物理通路；
 *   - nand_fmc.h 等平台头不进入本头，避免把 H743/HAL 依赖带到通用层。
 *
 * 当前板只有 (0,0)；多 target 由 board 实现扩展 target 表、NCE/R-B#/FMC
 * bank 选择，并在 nand_board_init_target() 中完成该 target 的 PHY 绑定。
 */

/* 板级物理通路项：一项表示一个可寻址的 NAND target。
 * bank 是板级“共享总线/FMC 时序”归属；同 bank 的 target 不能并发。 */
typedef struct NAND_BOARD_TARGET_S
{
    BYTE ch;
    BYTE ce;
    BYTE bank;
} nand_board_target_s;

/* 板上声明的 target 数量（不是 CH_NUM×CE_NUM 的矩形上限）。 */
size_t nand_board_target_count(void);

/* 按索引取 target；越界返回 NULL。 */
const nand_board_target_s *nand_board_target_at(size_t index);

/**
 * @brief 选择板级物理通路
 *
 * @param ch 目标通道
 * @param ce 目标片选
 * @return OK / INVALID_ARGUMENT（坐标越界）/ NOT_READY（板上无此 target）
 *
 * @note 当前单 bank 单 NCE 只校验表格；多 target 板在此切 NCE/FMC bank。
 */
nand_result_e nand_board_select_target(BYTE ch, BYTE ce);

/**
 * @brief 获取 target 所属 bank 的运行期所有权
 *
 * 同一 bank 只允许一个异步事务持有；当前单 target 板直接成功。
 *
 * @param ch 目标通道
 * @param ce 目标片选
 * @return OK：已持有；NOT_READY：同 bank 被其他 target 占用；INVALID：target 不存在
 */
nand_result_e nand_board_acquire_target(BYTE ch, BYTE ce);

/**
 * @brief 释放 target 所属 bank 的所有权
 *
 * @param ch 目标通道
 * @param ce 目标片选
 */
void nand_board_release_target(BYTE ch, BYTE ce);

/* 清空所有 bank ownership；软件重初始化时使用。 */
void nand_board_ownership_reset(void);

/**
 * @brief 初始化/重配一个 target 的 PHY 后端
 *
 * 由器件 hw_init 与 timing 切换调用；board 负责构造平台配置并绑定 backend。
 *
 * @param ch 目标通道
 * @param ce 目标片选
 * @return OK / 错误码
 */
nand_result_e nand_board_init_target(BYTE ch, BYTE ce);

/**
 * @brief 初始化板上全部 target 的 PHY 后端
 *
 * @return OK / 错误码；无 target 时返回 NOT_READY。
 */
nand_result_e nand_board_init_all_targets(void);

#endif /* NAND_BOARD_H */
