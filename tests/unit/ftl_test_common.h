/*
 * ftl_test_common.h — FTL 层测试共享夹具 + L0 异步链伪完成注入 seam
 *
 * 供 test_ftl.c（现有 8 套件）与 test_ftl_async.c（L0 异步套件）共用。
 *
 * 设计文档：docs/design/ftl_ut_munit.md（§4 用例清单、§5.4 异步、§8 不做清单）。
 */
#ifndef FTL_TEST_COMMON_H
#define FTL_TEST_COMMON_H

#include "fc_command.h" /* fc_command_t / fc_command_status_t */
#include "ftl.h"        /* ftl_context_t */
#include "munit.h"      /* MunitParameter / MunitTest */

/* ── §1 活实例夹具 ──
 *
 * 复用活实例 `g_ctrl.ftl[0][0][0]`（boot 已建），不另开大数组——ftl_context_t ≈ 287KB，
 * AXI_RAM 已放 g_ftl/g_blocks，再开一份装不下。CLI `test` 是同步命令，执行期无并发泵，
 * 独占该实例。 */
ftl_context_t *ftl_fixture(void);

/* 基础复位：破坏性 memset 各子结构 + 重 init（test_ftl.c §1 原样搬移）。
 * 只清会被用例写的子结构；F->topo 保持不动（reset 不该改路由）。 */
void ftl_fixture_reset(void);

/* 异步套件专用：调 ftl_fixture_reset() 后**额外**零化异步 job 结构
 * （alloc.bbt / alloc.wear / mig / cp_job——fixture 不 memset alloc/cache）。 */
void ftl_async_fixture_reset(void);

/* munit setup 包装：调用 ftl_async_fixture_reset()。返回 NULL 是合法契约
 * （测试体用 ftl_fixture() 自己取指针）。 */
void *ftl_async_setup(const MunitParameter params[], void *user_data);

/* ── §2 L0 异步链伪完成注入 seam ──
 *
 * 目的：纯 RAM 驱动「ftl_async_* → ftl_op_submit → 完成 → ftl_op_dispatch」完整异步链，
 * 不碰 NAND。依据：g_fc_controller 是 extern（fc_internal.h:85）、SQ/HSQ/CQ 环形队列字段
 * 公开（fc_queue.h）、fc_completion_queue_push 公开（fc_queue.h:55）。
 *
 * ⚠ 关键纪律：伪完成**必须**同时从 SQ/HSQ 摘除该命令——否则 `test` 返回后 superloop 的
 * fc_service 会把 stale QUEUED 命令派给真 NAND，fc_poll_dispatch_single（fc.c:1516）的
 * ASSERT(state==QUEUED) 在 debug 下必爆。
 *
 * ⚠ 只接受「ftl_op_submit 已返 FTL_OK」的命令（在 SQ/HSQ、state==QUEUED）；对 FTL_AGAIN
 * （SQ 满回滚）路径绝不能调它。 */
void ut_fake_complete(fc_command_t *cmd, uint8_t status);

/* 伪完成 + 消费 CQ 并派发（服务函数自行 release）。 */
void ut_complete_and_drain(fc_command_t *cmd, uint8_t status);

/* 取 SQ 队头命令（单命令在飞纪律下即「刚提交的那条」）；空返回 NULL。 */
fc_command_t *ut_sq_head(void);

#endif /* FTL_TEST_COMMON_H */
