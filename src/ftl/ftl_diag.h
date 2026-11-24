#ifndef FTL_DIAG_H
#define FTL_DIAG_H

#include "ftl_internal.h"

/* 诊断 / 统计 / 初始化契约自检。实现见 ftl_diag.c。 */

/**
 * @brief 打印 FTL 各模块的关键现场（allocator / pending / cache seq / CP / MIG /
 *        metadata / Journal / wbuf）。
 *
 * @param module NULL / "all" = 全部；否则只打印指定模块：
 *               alloc / pend / seq / cache / jrn / cp / mig / md / wb。
 * @return true = 参数合法并已输出；false = 未知模块。
 *
 * @note 只读、标量快照，供 CLI `ftl_ctx` 与断言终端路径使用；不调用会断言的池扫描。
 *       断言路径由 `ftl_assert_context()` 弱钩子调用（见 common/assertion.c）。
 */
BOOL ftl_debug_dump(const char *module);

/**
 * @brief debug-only 初始化契约自检（release 下空实现）
 *
 * 在 ftl_init 与设备级 owner 登记完成后调用一次。只证明初始化后**不可变**的绑定：
 * 句柄、storage 数组、端口回调、topology 副本、PBA 布局、bg 任务表、owner/route 身份。
 * 不检查运行期可变状态（phase / 计数 / 池成员），也不替代每次调用的入参断言。
 *
 * @param ftl FTL 上下文（非 NULL；由 ssd_ftl_boot 在 ftl_init 成功后调用）
 */
void ftl_contract_audit(const ftl_ctx_s *ftl);

void ftl_stats_register_modules(ftl_ctx_s *ftl);
void ftl_stats_register_sched(ftl_ctx_s *ftl);

/**
 * @brief 断言终端的 FTL 现场钩子（强符号覆盖 assertion.c 的弱默认）。
 */
void ftl_assert_context(void);

#endif /* FTL_DIAG_H */
