#ifndef FTL_UT_COMMON_H
#define FTL_UT_COMMON_H

#include "allocator.h"
#include "ftl.h"
#include "metadata.h"
#include "munit.h"

#include <stdarg.h>

/**
 * @brief 开始一条用例的证据输出状态。
 *
 * @note 只重置行首状态；第一条可见证据由 emit 负责换行。
 */
void ftl_ut_trace_begin(void);

/**
 * @brief 临时压低所有 `[EV]` 输出（速度测试准备阶段使用）。
 *
 * @param suppressed true = 压低；false = 恢复。
 *
 * @note 调用者必须成对恢复 false，否则后续用例证据丢失。
 */
void ftl_ut_trace_suppress(BOOL suppressed);

/**
 * @brief 输出 info 档证据行（`[EV] ` 前缀）。
 *
 * @param format printf 格式。
 */
MUNIT_PRINTF(1, 2)
void ftl_ut_trace_summary(const char *format, ...);

/**
 * @brief 输出 debug 档证据行（`[EV]   ` 前缀）。
 *
 * @param format printf 格式。
 */
MUNIT_PRINTF(1, 2)
void ftl_ut_trace_detail(const char *format, ...);

/**
 * @brief 输出关键结果行，不受 `--log-visible` 档位限制（前缀仍是 `[EV] `）。
 *
 * @param format printf 格式。
 *
 * @note 只用于 `/speed` 结果与失败现场；普通证据用 summary/detail。
 */
MUNIT_PRINTF(1, 2)
void ftl_ut_result(const char *format, ...);

/**
 * @brief 取活实例（`g_ctrl.ftl[0]`）。
 *
 * @return 非空实例；取不到直接 munit 失败。
 */
ftl_ctx_s *ftl_ut_live(void);

/**
 * @brief 按上电扫描结果重建 UT 拓扑（每个套件 setup 调一次）。
 */
void ftl_ut_topology_init(void);

/**
 * @brief 安装伪时钟并保存原平台 ops。
 */
void ftl_ut_clock_install(void);

/**
 * @brief 恢复安装伪时钟前的平台 ops。
 */
void ftl_ut_clock_restore(void);

/**
 * @brief 设置伪时钟（毫秒）。
 *
 * @param ms 绝对毫秒值。
 */
void ftl_ut_clock_set_ms(DWORD ms);

/**
 * @brief 推进伪时钟。
 *
 * @param ms 增量毫秒。
 */
void ftl_ut_clock_advance_ms(DWORD ms);

/* 测试自有 RAM 表：生产同型结构 + 小状态（只用前 FTL_UT_BLOCKS 块/前 FTL_UT_PAGES 页）。 */
#define FTL_UT_BLOCKS 8U
#define FTL_UT_PAGES  4U

/**
 * @brief 重建测试自有 RAM 表与 allocator。
 */
void ftl_ut_ram_init(void);

/**
 * @brief 取测试自有 allocator。
 *
 * @return 指向测试 RAM 表的 allocator。
 */
alc_s *ftl_ut_ram_alloc(void);

/**
 * @brief 取测试自有 Directory cache。
 *
 * @return 指向测试 RAM 表的 Directory。
 */
md_dir_cache_s *ftl_ut_ram_md(void);

/* 真 NAND 媒体夹具（破坏性）：format + 模拟重启 + 主机读写 + flush。
 * LBA 窗口固定在**运行期**容量尾部（host_lba_max 已扣实际坏块；不能用编译期
 * FTL_LBA_MAX），避免碰文件系统常用区。 */
#define FTL_UT_LBA_WINDOW    8192U
#define FTL_UT_IO_TIMEOUT_MS 5000U

/**
 * @brief 运行期容量尾窗起点（`host_lba_max` 尾部 8192 LBA，按 pair 对齐）。
 *
 * @return 尾窗起始 LBA。
 */
DWORD ftl_ut_lba_base(void);

/**
 * @brief 排空 FTL/FC 的在飞资源（置 `bg_suspend` 并泵到全部池 / CQ / op ctx 归零）。
 *
 * @param ftl 实例。
 *
 * @note 用在夹具 teardown：用例可能把后台命令（journal / CP / BBT）的提交留在
 *       最后一次泵之后，完成会滞留在 FC 的 CQ 里，污染下一个套件。
 *       ⚠ 返回时 `bg_suspend` 仍为 1，调用者负责恢复。
 */
void ftl_ut_quiesce(ftl_ctx_s *ftl);

/**
 * @brief 模拟一次掉电重启（quiesce → fc_init → ftl_init → boot recovery）。
 */
void ftl_ut_reboot_from_media(void);

/**
 * @brief 完整格式化当前介质并重启 FTL。
 */
void ftl_ut_media_format(void);

/**
 * @brief 媒体夹具 host 写（同步等待组完成）。
 *
 * @param lba   起始 LBA。
 * @param count 连续 LBA 数。
 * @param buffer 数据源。
 * @return true = 组成功完成；false = submit 拒绝/超时/组报错。
 */
BOOL ftl_ut_host_write(DWORD lba, DWORD count, const void *buffer);

/**
 * @brief 媒体夹具 host 读（同步等待组完成）。
 *
 * @param lba   起始 LBA。
 * @param count 连续 LBA 数。
 * @param buffer 数据目的地。
 * @return true = 组成功完成；false = submit 拒绝/超时/组报错。
 */
BOOL ftl_ut_host_read(DWORD lba, DWORD count, void *buffer);

/**
 * @brief flush 到 NAND（推进 wbuf / L2P / Journal）。
 *
 * @return true = 完成；false = 超时。
 */
BOOL ftl_ut_flush(void);

/**
 * @brief trim 一段 LBA。
 *
 * @param lba   起始 LBA。
 * @param count 连续 LBA 数。
 * @return true = 完成；false = 失败/超时。
 */
BOOL ftl_ut_trim(DWORD lba, DWORD count);

/**
 * @brief pure 套件 setup：重建测试自有 RAM 表。
 *
 * @return NULL（无夹具状态）。
 */
void *ftl_ut_pure_setup(const MunitParameter params[], void *data);

/**
 * @brief pure 套件 teardown：检查活实例资源归零。
 *
 * @param fixture setup 返回值（未使用）。
 */
void ftl_ut_pure_teardown(void *fixture);

/**
 * @brief 用例结束不变量：pending/group/op-ctx/FC 资源、CP/MIG/metadata/journal/
 *        wbuf/L2P 在飞与 cache 序列页表必须归零。
 *
 * @param ftl 实例。
 */
void ftl_ut_assert_clean(ftl_ctx_s *ftl);

#endif /* FTL_UT_COMMON_H */
