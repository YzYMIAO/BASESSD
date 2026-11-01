#ifndef FTL_UT_COMMON_H
#define FTL_UT_COMMON_H

#include "allocator.h"
#include "ftl.h"
#include "metadata.h"
#include "munit.h"

#include <stdarg.h>

/* 证据输出（与 FC UT 的 [EV] 行同格式）：默认档不输出，info/debug 档可见。 */
void ftl_ut_trace_begin(void);

/* 临时压低所有 [EV] 输出（速度测试在 format 等准备阶段使用；跑完必须恢复 false）。 */
void ftl_ut_trace_suppress(bool suppressed);

MUNIT_PRINTF(1, 2)
void ftl_ut_trace_summary(const char *format, ...);

MUNIT_PRINTF(1, 2)
void ftl_ut_trace_detail(const char *format, ...);

/* 关键指标行：无论 --log-visible 取值都输出（前缀仍是 "[EV] "）。基准类用例
 * （/speed）的读/写速度属于"结果"而不是诊断，默认档也必须看得到。 */
MUNIT_PRINTF(1, 2)
void ftl_ut_result(const char *format, ...);

/* 活实例（g_ctrl.ftl[0][0][0]）；结构性恒非空，取不到直接断言失败。 */
ftl_ctx_t *ftl_ut_live(void);

/* 伪时钟：保存当前平台 ops → 换成测试时钟；必须成对调用并恢复。 */
void ftl_ut_clock_install(void);
void ftl_ut_clock_restore(void);
void ftl_ut_clock_set_ms(DWORD ms);
void ftl_ut_clock_advance_ms(DWORD ms);

/* 测试自有 RAM 表：生产同型结构 + 小状态（只用前 FTL_UT_BLOCKS 块/前 FTL_UT_PAGES 页）。 */
#define FTL_UT_BLOCKS 8U
#define FTL_UT_PAGES  4U

void            ftl_ut_ram_init(void);
alc_t          *ftl_ut_ram_alloc(void);
md_dir_cache_t *ftl_ut_ram_md(void);

/* 真 NAND 媒体夹具（破坏性）：format + 模拟重启 + 主机读写 + flush。
 * LBA 窗口固定在容量尾部，避免碰文件系统常用区。 */
#define FTL_UT_LBA_BASE      (((DWORD)FTL_LBA_MAX - 8192U) & ~7U)
#define FTL_UT_IO_TIMEOUT_MS 5000U

/**
 * @brief 排空 FTL/FC 的在飞资源（置 `bg_suspend` 并泵到全部池 / CQ / op ctx 归零）。
 *
 * @param ftl 实例。
 *
 * @note 用在夹具 teardown：用例可能把后台命令（journal / CP / BBT）的提交留在
 *       最后一次泵之后，完成会滞留在 FC 的 CQ 里，污染下一个套件。
 *       ⚠ 返回时 `bg_suspend` 仍为 1，调用者负责恢复。
 */
void ftl_ut_quiesce(ftl_ctx_t *ftl);

void ftl_ut_reboot_from_media(void);
void ftl_ut_media_format(void);
bool ftl_ut_host_write(DWORD lba, DWORD count, const void *buffer);
bool ftl_ut_host_read(DWORD lba, DWORD count, void *buffer);
bool ftl_ut_flush(void);
bool ftl_ut_trim(DWORD lba, DWORD count);

/* pure 套件的资源钩子：setup 重建测试自有 RAM 表；teardown 检查活实例归零。 */
void *ftl_ut_pure_setup(const MunitParameter params[], void *data);
void  ftl_ut_pure_teardown(void *fixture);

/* 用例结束不变量：pending/group/op-ctx/FC 资源必须归零。 */
void ftl_ut_assert_clean(ftl_ctx_t *ftl);

#endif /* FTL_UT_COMMON_H */
