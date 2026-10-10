# v42：上板验证 v40/v41 + 订正 `--iterations` 语义

- **日期**：2026-10-11
- **范围**：**纯文档**——订正 v40 记录里写错的 `--iterations` 条数，补上板验证结论。
  `tests/**`、`src/**` 一行不动。
- **介质格式**：无变更。
- **测试**：无代码改动，无需重编。

## 一、上板结论：v40 + v41 全部通过

2026-10-11 上板（stm32h743 + MT29F4G08），串口日志 `comtool3.0.log`（1899 行，用户提供）。
开场 `[INF] soft_ssd debug revision=24`。

| # | 命令 | 结果 |
|---|---|---|
| 1 | `test --list` | **32 条**（Level 1 = 11 / Level 2 = **12 条目** / Level 3 = 9）——路径**全部带分隔斜杠**，粘连形式 **0 处** |
| 2 | `test --log-visible info /fc-nand/lifecycle/` | **9 of 9**，522.62 ms |
| 3 | `test --log-visible info --iterations 5 /fc-nand/lifecycle/` | **9 of 9**，2.61 s（见 §二） |
| 4 | `test --log-visible debug /fc-nand/lifecycle/block_write_read` | **1 of 1**，399.21 ms |
| 5 | `test --log-visible debug`（全套） | **51 of 51 (100%)**，7.08 s |

`51 = 11 + 31 + 9`（Level 2 的 12 条目参数化展开成 31，故 `--list` 只有 32 行而实跑 51 条——与 §2.2 一致）。

**v41 验收**：`--list` 逐条路径带斜杠（`/fc-nand/lifecycle/block_multi_plane` 形态）；
`test /fc-nand/lifecycle/` 跑出 **9 条**（改前这个串跑 0 条）。✅

**v40 验收**（seed `0x5a4f21e4`）：

| 判据 | 实测 |
|---|---|
| 六条打印的 `block` ∈ **614..1433** | ✅ 全部落在窗口内（run 2：1314 / 684 / 798 / 647 / 663 / 942） |
| `internal_data_move` 的 **src ≠ dest** | ✅ 1314→639；`--iterations 5` 的另外四组也全部 src ≠ dest |
| `block_random_data` 掩码 **popcount == 打印个数** | ✅ 见下 |

`block_random_data` 掩码逐条核对（`--iterations 5` 抽的 5 组）：

| 打印 | page_mask | 位 → 页 | cw_mask | 位 → (plane,cw) |
|---|---|---|---|---|
| 2 pages × 2 cw = 4 | `0x0002400000000000` | {46, 49} ✅ 与日志 `read p46` / `read p49` 对上 | `0x00000009` | (0,0) (0,3) |
| 3 pages × 2 cw = 6 | `0x0008000002004000` | {14, 25, 51} | `0x00000060` | (1,1) (1,2) |
| 5 pages × 3 cw = 15 | `0x0080000008010802` | {1, 11, 16, 27, 55} | `0x000000B0` | (1,0) (1,1) (1,3) |
| 2 pages × 1 cw = 2 | `0x0100100000000000` | {44, 56} | `0x00000020` | (1,1) |
| 3 pages × 1 cw = 3 | `0x0000010000080001` | {0, 19, 40} | `0x00000040` | (1,2) |

**每个掩码的 popcount 都等于打印的个数** ⇒ 页集 / CW 集互不重复，自证成立。✅

**复现性**：seed 在四轮里恒为 `0x5a4f21e4`，run 2 / run 3 第 1 次迭代 / run 5 的六条地址**逐字相同**
（1314/684/798/647/663/942）⇒ 复现契约 `(seed, 用例名, 第几次抽样)` 成立。✅

**唯一一条 `[ERR]`**：`[ERR] [FAULT] previous abort (assertion/test terminal)`（日志第 4 行，配
`reset reason: PIN SFT`）——这是**上一次会话**留在 `.noinit` 里的故障记录（v36 那次 `0x89` 上板红的
遗留），**不是本轮失败**。本轮无 FAIL / ERROR / abort。

## 二、订正：`--iterations N` **不改变报告的条数**（v40 记录写错了）

v40 的记录里写「`test --iterations 20 /fc-nand/lifecycle/` ⇒ Level 3 跑 `3 + 6×20 = 123` 条」——**错的**。

**实际语义**（`tests/framework/munit.c`）：

| 位置 | 行为 |
|---|---|
| `:760-772` | `iterations = runner->iterations`；`SINGLE_ITERATION` ⇒ 硬压成 1；为 0 ⇒ 回落套件级 |
| `:780` / `:827` | `do { … } while (++i < iterations);` ⇒ **用例体跑 N 次** |
| `:803` | 每次迭代累加**局部** `report->successful` |
| `:989` | `runner->report.successful++` 在**调用点**，**每条只加一次**（不看迭代数） |
| `:981` | 打印的 `[ N ms ]` 是 `report.wall_clock / report.successful` ⇒ **每次迭代的平均值** |

⇒ **条数恒为 9（全套 51），加量只体现在耗时上。**

**上板实测佐证**（同一 seed，run 2 vs run 3）：

- 总耗时 **522.62 ms → 2.61 s**（≈ ×5）；
- 九条的单条时间**几乎逐字不变**（`internal_data_move` 144.93 → 144.83 ms），因为是**平均值**；
- `internal_data_move` 一条打了 **5 组** `[EV] move …`（5 个不同地址）；
- 汇总行两次都是 `9 of 9`。

**怎么判断加量生效**：看**总耗时**（或数 `[EV]` 组数），**不要看条数**。

已同步订正：`docs/design/fc_nand_ut.md` §2.3（并加 §7 v1.24 行）、
`docs/changelog/fc_ut_level3_seed_random_v40.md` §五·1 与验证段。

## 三、顺带观察（未处理）

- **Level 2 仍有写死的块**：`scramble_whiten` 打印 `block=1328` / `block=2042`（`all-0x00` / `all-0xFF`
  两条探针）。这正是 v40 给 Level 3 消掉的那类「固定选取」，但**Level 2 不在本轮范围**，只记录。
- `--list` 只列 **32** 行而实跑 **51** 条——Level 2 的 5 条参数化用例（`crc_step_detect` 4 /
  `ecc_step` 11 / `encode` 3 / `process_read_ok` 3 / `cw_bitmap` 3 = 24 展开 + 7 非参数化 = 31）。
  不是缺陷，是 munit 的参数化模型，但容易让人以为「少跑了 19 条」。

## 验证

本轮**无代码改动**，无需重编。上板日志见用户提供的 `comtool3.0.log`。

## 不做

- 不动 `tests/**` / `src/**`（含 `src/ftl/**`）；
- 不动 `tests/framework/munit.*`（`--iterations` 的语义是框架既有行为，不改）；
- 不重写 v40 changelog 的**结论性**内容，只订正那一处**事实错误**的条数（并留痕指向本文）。

## 出处

- 上板日志：用户提供的 `comtool3.0.log`（2026-10-11，1899 行）。
- 迭代语义：`tests/framework/munit.c:760-772` / `:780` / `:803` / `:827` / `:981` / `:989`。
- 被订正的记录：`docs/changelog/fc_ut_level3_seed_random_v40.md` §五·1。
- 同批的 v41（套件前缀尾斜杠）：`docs/changelog/fc_ut_suite_prefix_slash_v41.md`。
