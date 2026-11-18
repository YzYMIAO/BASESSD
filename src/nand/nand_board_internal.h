#ifndef NAND_BOARD_INTERNAL_H
#define NAND_BOARD_INTERNAL_H

#include "nand_board.h"

#include <stddef.h>

/*
 * nand_board_internal.h — NAND 内部 target 表与 PHY 绑定
 *
 * 只给 src/nand/ 内部使用；FC 不得 include。
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
 * @brief 初始化/重配一个 target 的 PHY 后端
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

#endif /* NAND_BOARD_INTERNAL_H */
