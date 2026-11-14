#ifndef FAULT_INJECT_H
#define FAULT_INJECT_H

#include "ssd_types.h"

/* ── 测试镜像专属故障注入（ISS-FTL-16）──
 *
 * 目的：让「注入类」验证不需要真 NAND 失败也能重复跑 —— `ftl_ut_design.md §5`
 * 的 `FI-NA` 档。当前收口 2 个注入点（cache program 完成、Journal 页写完成），
 * 覆盖 `` / `` / `` / `RISK-L2P-09` 的失败侧。
 *
 * 契约：
 *   · **只在测试镜像里存在**：开关是 `FC_NAND_TEST_ENABLE`（只有 `make tests` 定义），
 *     **不是** `NDEBUG`。理由：release 档同样要能跑 UT（`make tests BUILD_TYPE=release`，
 *     用 -O2 看最终速度）；注入若随 `NDEBUG` 消失，注入类用例在 release UT 里就失去判据。
 *     产品固件不定义该宏 ⇒ 全部退化为空实现、零运行期开销（H14）。
 *   · 按**数量**注入（`*_arm(n)`），不是布尔开关：一次命中后自减，测试能精确
 *     指定「失败几页」并在之后观察恢复路径。0 = 不注入。
 *   · 状态是文件级静态量，**测试必须在 setup/teardown 复位**（`fi_reset`），
 *     否则会静默污染后续用例 —— 本仓库已发生过「测试碰巧通过」的同型缺陷
 *     （CLAUDE.md §3.2）。
 *
 * ⚠ 注入是在 **FTL 层**伪造结果，不是让器件真的失败：它验证的是 FTL 的策略
 *   反应（不确认映射 / 关块 / WAL 闸门），不验证 FC↔器件的映射（FAILC 归属、
 *   7Ah 状态字节）—— 后者必须上板。
 */

#if defined(FC_NAND_TEST_ENABLE)

/**
 * @brief 复位全部注入点（测试 setup/teardown 调用）。
 */
void fi_reset(void);

/**
 * @brief 在指定 cache program 页的命令完成上伪造 `FAIL_CACHE`（一次性）。
 *
 * @param page_lba 目标 cache program 成员的页基址 LBA（8 LBA 对齐）。
 *
 * @note 器件语义：第 N 条命令的 70h 状态报的是**上一页**的结论（op44/op48
 *       映射，op49 首页不映射）。故「让 page0 失败」= 在 **page1** 的完成上注入。
 *       本函数不替调用者换算，调用者按器件语义挑 LBA。
 */
void fi_cache_fail_arm(DWORD page_lba);

/**
 * @brief 命中判定：本页命令完成是否应伪造 `FAIL_CACHE`。
 *
 * @param page_lba 本成员的页基址 LBA。
 * @return true = 本次注入，且注入点已被消费（之后同一页不再命中）
 */
BOOL fi_cache_fail_take(DWORD page_lba);

/**
 * @brief 让接下来 @p pages 次 Journal 页写按失败结算。
 *
 * @param pages 失败次数（0 = 不注入）。
 *
 * @note 命中点 = `jrn_write_complete`：失败页不推进 `persisted_seq`（WAL 水位），
 *       并置 `write_fail_pending`。多次失败会把 Journal 环形块写满并停写 ——
 *       测试应只注入少量页，随后放开注入观察恢复。
 */
void fi_journal_fail_arm(DWORD pages);

/**
 * @brief 命中判定：本次 Journal 页写是否按失败结算。
 *
 * @return true = 本次失败（剩余次数 -1）
 */
BOOL fi_journal_fail_take(void);

#else /* 产品固件（无 FC_NAND_TEST_ENABLE）：全部空实现，零运行期开销 */

static inline void fi_reset(void)
{
}

static inline void fi_cache_fail_arm(DWORD page_lba)
{
    (void)page_lba;
}

static inline BOOL fi_cache_fail_take(DWORD page_lba)
{
    (void)page_lba;
    return false;
}

static inline void fi_journal_fail_arm(DWORD pages)
{
    (void)pages;
}

static inline BOOL fi_journal_fail_take(void)
{
    return false;
}

#endif /* FC_NAND_TEST_ENABLE */

#endif /* FAULT_INJECT_H */
