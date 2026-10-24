#ifndef FTL_UT_COMMON_H
#define FTL_UT_COMMON_H

#include "allocator.h"
#include "ftl.h"
#include "metadata.h"
#include "munit.h"

#include <stdarg.h>

/* 证据输出（与 FC UT 的 [EV] 行同格式）：默认档不输出，info/debug 档可见。 */
void ftl_ut_trace_begin(void);

MUNIT_PRINTF(1, 2)
void ftl_ut_trace_summary(const char *format, ...);

MUNIT_PRINTF(1, 2)
void ftl_ut_trace_detail(const char *format, ...);

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

void ftl_ut_reboot_from_media(void);
void ftl_ut_media_format(void);
bool ftl_ut_host_write(DWORD lba, DWORD count, const void *buffer);
bool ftl_ut_host_read(DWORD lba, DWORD count, void *buffer);
bool ftl_ut_flush(void);
bool ftl_ut_trim(DWORD lba, DWORD count);

/* 用例结束不变量：pending/group/op-ctx/FC 资源必须归零。 */
void ftl_ut_assert_clean(ftl_ctx_t *ftl);

#endif /* FTL_UT_COMMON_H */
