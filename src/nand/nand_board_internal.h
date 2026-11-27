#ifndef NAND_BOARD_INTERNAL_H
#define NAND_BOARD_INTERNAL_H

#include "nand_board.h"

#include <stddef.h>

/*
 * nand_board_internal.h — NAND 内部 target 表与 PHY 绑定
 *
 * header-only: contract（无 .c；src/nand 内部共享）。
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

/**
 * @brief 初始化 / 重配一个 target 的 PHY 后端。
 * @note target 不在板上时返回 NOT_READY；其余后端错误码透传。
 */
nand_result_e nand_board_init_target(BYTE ch, BYTE ce);

/**
 * @brief 初始化板上全部 target 的 PHY 后端。
 * @note 无 target 时返回 NOT_READY；其余后端错误码透传。
 */
nand_result_e nand_board_init_all_targets(void);

#endif /* NAND_BOARD_INTERNAL_H */
