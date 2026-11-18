#ifndef NAND_BOARD_H
#define NAND_BOARD_H

#include "nand_protocol.h"

/*
 * nand_board.h — FC 需要的板级 bank ownership 接口
 *
 * target 枚举、PHY 绑定与初始化在 src/nand/nand_board_internal.h；
 * FC 只通过本头获取/释放 bank 所有权，避免看到 FMC/HAL 细节。
 */

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

#endif /* NAND_BOARD_H */
