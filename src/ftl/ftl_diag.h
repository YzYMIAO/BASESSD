#ifndef FTL_DIAG_H
#define FTL_DIAG_H

#include "ftl_type.h"

/* 诊断 / 统计 / 初始化契约自检。实现见 ftl_diag.c。 */

/**
 * @brief 打印 FTL 各模块的关键现场（module = NULL / "all" 或指定模块）。
 *
 * @note 只读、标量快照，供 CLI `ftl_ctx` 与断言终端路径使用；不调用会断言的池扫描。
 *       断言路径由 `ftl_assert_context_dump()` 弱钩子调用（见 common/assertion.c）。
 */
BOOL ftl_debug_dump(const char *module);

/**
 * @brief debug-only 初始化契约自检（release 下空实现）。
 *
 * @note 只证明初始化后**不可变**的绑定（句柄 / storage / 端口回调 / topology /
 *       PBA 布局 / bg 任务表 / owner-route 身份），不检查运行期可变状态。
 */
void ftl_contract_verify(const ftl_ctx_s *ftl);

/** @brief 注册模块级 stats（mig / alloc / stream / journal / gc / rr）。 */
void ftl_stats_register_modules(ftl_ctx_s *ftl);

/** @brief 注册 scheduler stats（必须在 ftl_sched_init 之后调用）。 */
void ftl_stats_register_sched(ftl_ctx_s *ftl);

/**
 * @brief 断言终端的 FTL 现场钩子（强符号覆盖 assertion.c 的弱默认）。
 */
void ftl_assert_context_dump(void);

#endif /* FTL_DIAG_H */
