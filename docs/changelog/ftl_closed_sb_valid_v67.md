# v0.67：修 `allocator_close_sb` 的 valid 顺序缺陷 —— 运行期关闭的 SB 对 GC/WL **永久不可见**

**日期**：2026-10-14
**分支**：main
**涉及模块**：`src/ftl/allocator.c`（生产修复）、`tests/unit/ut_main.c`（测试基建）
**起因**：上板跑 FTL L0 UT，`/ftl/allocator/close_open_sb` 失败

---

## 〇、起因

板端 `test`（全套）跑到第 4 例即失败：

```
Error:test_ftl.c:345 assertion failed: a->closed_sb_list[0].valid == 0U (1 == 0)
```

用例 `test_alloc_close_open_sb` 的预期是「**关闭后的 Closed 条目 `valid == 0`**」。
核过源码：**用例是对的，生产代码是错的**。

## 一、根因：清的是源，不是副本

`include/ftl/allocator.h:79` 定义字段语义（**字段级**，非按槽位）：

```c
uint8_t valid;   /* 1=正在使用（Open），0=已关闭 */
```

`allocator_add_closed_sb`（开机重建路径）的文档同义：「`valid=0`（Closed，**`gc_select_victim` 依赖**）」。

GC / WL 的遍历确实以该字段为闸门：

| 位置 | 代码 | 注释 |
|---|---|---|
| `src/ftl/gc.c:387` | `if (sb->valid != 0U) { continue; }` | — |
| `src/ftl/wear_leveling.c:129` | `if (sb->valid != 0U) { continue; }` | 「跳过仍为 Open 的 SB」 |
| `src/ftl/gc.c:220` | `… \|\| (victim->valid != 0U)` → `return` | 待擦 SB 须 `valid == 0` |

而 `allocator_close_sb` **先拷贝、后清 valid**——清的是**源**（`hot_sb`/`cold_sb`）而不是**副本**：

```c
alloc->closed_sb_list[alloc->closed_sb_count++] = *sb;   /* ← 拷进去的仍是 Open 态 valid=1 */
sb->valid = 0U;                                          /* ← 只失效了 Open 槽 */
```

⇒ **运行期关闭的每个 SB 都带 `valid == 1` 进列表 ⇒ GC/WL 一律跳过它**。
只有开机重建路径（`allocator_add_closed_sb`，显式 `valid=0`）的条目对 GC 可见。

`allocator_close_open_sb` 的文档注释本来就写着「进 closed_sb_list + **valid=0**」——
**作者意图正确，实现写错了对象**。

## 二、影响：潜伏、非即时，但会导致写永久失败

`free_count` 只出不进：

```
水位跌破 FREE_POOL_TARGET(32) → gc_check_triggers 触发 T1
  → gc_select_victim 遍历 closed_sb_list，全部 valid==1 → 选不出 victim
  → 紧急 GC 无块可回收 → allocator_alloc_pair 返回 NAND_PBA_INVALID
  → 写永久失败（LOGE「GC 后无空闲块」）
```

WL 同理：`wl_select_victim` 一条候选都看不到，磨损均衡完全停摆。

**为什么此前没暴露**：SB 要写满 64 页才关闭，且 GC 要到 `free < 32` 才触发。板子开机后
`free=3710`（`F2 rebuild blocks=2`），短测 `wr`/`rd` 触不到。属**潜伏缺陷**。

## 三、修法：一行换序

`src/ftl/allocator.c` `allocator_close_sb`：把 `sb->valid = 0U;` 提到拷贝**之前**，
使条目落表时即为 Closed 态，与重建路径 `allocator_add_closed_sb` 收敛到同一约定。

```c
/* ⚠ 必须先清 valid 再拷贝（顺序不可换）。… */
sb->valid = 0U;

if (alloc->closed_sb_count < SUPER_BLOCK_LIST_MAX)
{
    alloc->closed_sb_list[alloc->closed_sb_count++] = *sb;
}
```

两处调用者（`allocator_alloc_pair` 写满关闭、`allocator_close_open_sb` 显式关闭）都传
`hot_sb`/`cold_sb`，语义不变。

## 四、顺带：`test /ftl/` 恒「NO test run」

板端 `test /ftl/` 输出 `NO test run,0(100%) skipped.`。根因在 munit 的过滤语义
（`tests/framework/munit.c:1297-1303`）：

```c
if ((pre_len == 0 || strncmp(pre, *test_name, pre_len) == 0) &&
    strncmp(test->name, *test_name + pre_len, strlen(*test_name + pre_len)) == 0)
```

过滤串必须 = **套件累积前缀 `pre` + 用例名前缀**。用例挂在**叶子**套件下，其 `pre`
（如 `/ftl/allocator/`）比用户输入的 `/ftl/` 长 ⇒ 第一条 `strncmp` 在路径末尾即失配。
⇒ 只有 `/ftl/allocator/`（叶子全路径）或 `/ftl/allocator/<用例名>` 能命中。

**修法**（`tests/unit/ut_main.c`，只动测试基建、不碰任何用例）：`ut_run_all` 在转交 munit 前
深度优先收集**持有用例的套件**的全路径，把「以 `/` 结尾」的实参展开为其下全部叶子套件路径。
展开幂等（叶子路径展开成自身），命中不到叶子的串原样透传（行为与展开前一致）。

⇒ `test /ftl/`、`test /fc-nand/` 现在按直觉工作；`test /` 等价全套。

## 五、验证

| 项 | 结果 |
|---|---|
| `make tests UT=FTL` | 0 warning / 0 error（`Binary size: 256436 bytes`） |
| `make tests`（全套） | 0 warning / 0 error |
| `test /ftl/` | 展开为 8 个叶子套件（待上板复核） |
| `test /ftl/allocator/close_open_sb` | 待上板复核（本地无板） |

**上板判据**：`test /ftl/` → 53/53 PASS。`close_open_sb` 此前是必 FAIL 的一例。

## 六、附：为什么 UT 值得先跑

本缺陷由 `test_alloc_close_open_sb` 一例直接顶出来，而同一套件里的
`test_gc_victim_skips_open`（`test_ftl.c:817/831` 手工置 `valid = 1U` 模拟 Open 条目）
**早已把「`valid != 0` = 跳过」的正确约定写进了断言**。两个用例一个描述约定、一个验证生产者，
合起来正好把这条潜伏缺陷夹住——纯静态审查（v0.66 那轮 53 例逐条过）没能发现它，
因为缺陷在**生产代码**里，用例本身看不出矛盾。
