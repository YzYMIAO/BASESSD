#ifndef FI_WRAP_H
#define FI_WRAP_H

/*
 * fi_wrap.h — 链接期故障注入 wrapper 的测试侧控制口
 *
 * 仅测试构建使用（FC_NAND_TEST_ENABLE）。生产构建不编译 fi_wrap.c。
 */

#include "ftl_type.h"

/* 前向声明：super_block_s 在 allocator.h。 */
typedef struct SUPER_BLOCK_S super_block_s;

/**
 * @brief 强制下一次 gc_select_victim 返回指定 Closed 条目（一次性）。
 *
 * @param victim closed_sb_list 中的条目；NULL = 不强制。
 *
 * @note 只在测试构建里有意义；用于消除「成本排序选中谁」的不确定性。
 */
void fi_wrap_gc_force_victim(const super_block_s *victim);

/**
 * @brief 清除强制 victim 状态（setup/teardown 调用）。
 */
void fi_wrap_reset(void);

/**
 * @brief 放行一条被 FI_COMPLETION_HOLD 扣住的完成（下一轮 poll 投递）。
 */
void fi_wrap_release_held(void);

/**
 * @brief 当前被扣住的完成条数。
 */
DWORD fi_wrap_held_count(void);

#endif /* FI_WRAP_H */
