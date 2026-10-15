# FTL 层补齐 FC 式详细注释（全 16 个 `.c` + 3 个 `.h`，**纯注释、零行为变更**）

- **日期**：2026-10-13
- **范围**：`src/ftl/**`（16 个 `.c`）+ `include/ftl/**`（3 个 `.h`）+ 本文档。
  **一行代码未动**（不重排、不改宏、不动 `#include` 顺序）。
- **介质格式**：无变更。
- **构建**：`make clean && make all` **0 warning 0 error**，`debug.bin` **185388 B**。
- **验证**：预处理剥注释逐文件 `diff` —— **19/19 逐 token 一致**（见 §五）。
- 未提交、未上板。

---

## 〇、起因

用户提出：**「先对 FTL 层的代码像 FC 层那样做详细注释方便我查看分析」**。

勘察现状：FTL 层 16 个 `.c` **无一**有 FC 式文件头注释块（`src/fc/fc.c` / `fc_interface.c`
都有），注释占比 **3%–23%**（`wear_leveling.c` 7.9%、`pending.c` 3.0% 最低），远低于 FC
（`fc.c` 38%）。`.h` 已基本合规（50%–76%）。用户要读 FTL 代码做分析，缺一层
「文件头 + 分区 + 逐函数 doc + 行内 why」的结构化注释。

**目标**：把 FTL `.c` 提到与 FC 同等的注释水准。经确认，范围 = **全部 16 个 `.c`**、
深度 = **FC 全平齐**。

## 一、FC 注释约定（逐字范本，已核对 `src/fc/*.c`）

FC 的文件头**不是刚性模板，而是一份共享标题词表**，逐文件按需取用。首行固定
`/* 文件名.c — 一句话职责 */`，随后从中选段落（全角冒号「：」结尾）：

- `职责：`、`与 X 的分工：`、`资源模型：`、`生命周期（对 FTL）：`、
  `文件结构：`、`服务主循环（fn）：`、`状态机：`、`设计说明（为什么…）：`。

**分区标题**三种样式：
- 大区（`fc.c`）：上下各一条 58 列 `═` 线 —— `/* ═══…\n * N. 名称\n * ═══… */`
- 小区：`/* ── 名称 ── */`
- 函数 doc 围栏：上下各一条 68 列 `-` 线 —— `/* ----\n * fn\n *   …\n * ---- */`

**函数 doc（`.c` 用字段式围栏）**：`功能： / 参数： / 返回： / 实现要点：`。

**行内标记**：`⚠`（坑/代价）、`──`（分隔）、`vNN`（版本沿革）、圈码 `①②③`（枚举步骤）、
`§N`（回指 `assertion_guidelines.md`）、句式「为什么…：」。（`★` 是 FTL 专有，FC 不用。）

**重点覆盖**（零注释且逻辑复杂处）：位运算 / 环形缓冲 / 地址换算 / 状态机 / 失败回滚 /
水位阈值公式 / 加权评分 / 掉电恢复分支。算法语义依据 `docs/design/ftl_design.md`
（§3.7 水位、§4.3 GC 评分、§4.4 触发、§6.5 thrashing、§7 Journal、§8 Checkpoint）。

## 二、每个文件做的四件事

① 加文件头块（职责 / 分工 / 资源模型 / 状态机 / 文件结构）；
② 加/补分区标题（**不编号已存在的分区**）；
③ 逐函数 doc（围栏式「功能/参数/返回/实现要点」，复杂函数重点）；
④ 行内 why 注释（`⚠` / `vNN` / 圈码 /「为什么…：」）。

## 三、逐文件清单（16 `.c` + 3 `.h`）

| # | 文件 | 组 | 本次新增注释要点 |
|---|---|---|---|
| 1 | `ftl_config.c` | 接口/顶层 | 文件头（配置表模型）+ 逐项 doc |
| 2 | `pending.c` | 数据结构 | 文件头 + `pending_add`(11 参) / `pending_group_maybe_complete` 组完成编排 |
| 3 | `completion_table.c` | 数据结构 | 文件头 + 5 个路由函数 doc |
| 4 | `l2p_cache.c` | 数据结构 | 文件头 + CLOCK 替换 / 脏行写回；⚠ `init` **刻意不清** `md_dir`/`alloc` |
| 5 | `journal.c` | 元数据/恢复 | 文件头 + 环形页追加 / `journal_replay` 递增序回放 |
| 6 | `checkpoint.c` | 元数据/恢复 | 文件头 + 触发门 T1/T2/T3 + 记录探测 |
| 7 | `metadata.c` | 元数据/恢复 | 文件头 + 环形定位 / 目录回填 / anchor |
| 8 | `allocator.c` | 分配/磨损 | 文件头 + 三级水位 / `alloc_pair` 逐 plane 选块 |
| 9 | `bbt.c` | 分配/磨损 | 文件头 + 出厂扫描 / 坏块图打印 / spare 池补充 |
| 10 | `wear.c` | 分配/磨损 | 文件头 + 页式追加日志 / compact / 回放（绝对值+最新胜） |
| 11 | `wear_leveling.c` | 分配/磨损 | 文件头 + 静态均衡评分 / 预测搬移 |
| 12 | `gc.c` | 分配/磨损 | 文件头 + victim 评分 / SB 迁移 / 触发位图 |
| 13 | `read_recovery.c` | 元数据/恢复 | 文件头 + 三级恢复 / 慢死预测 / 退役 |
| 14 | `power_failure_recovery.c` | 元数据/恢复 | 文件头 + 三条启动路径 / F2 全盘重建 / F4 bitmap 局部加载 |
| 15 | `ftl_fc_interface.c` | 接口/顶层 | 文件头 + 建链 / 数据搬运 / 6 个页级同步原语 |
| 16 | `ftl.c` | 接口/顶层 | 文件头 + 四阶段服务主循环 / 三级后台 / 子操作三态下发 |
| — | `include/ftl/journal.h` | `.h` 补漏 | 文件头 + `journal_init`/`journal_flush_page` doc |
| — | `include/ftl/completion_table.h` | `.h` 补漏 | 5 个路由函数 doc |
| — | `include/ftl/ftl_fc_interface.h` | `.h` 补漏 | `ftl_cmd_setup_ppa` / `ftl_cmd_write_data` / `ftl_cmd_read_data` doc |

## 四、量化结果

注释行增删（对照改动前快照）：**+1452 / −79** 行（−79 为散文式 doc 改写成围栏式的重排）。

改后各 `.c` 注释占比：

| 文件 | 占比 | 文件 | 占比 |
|---|---|---|---|
| `ftl_config.c` | 70% | `power_failure_recovery.c` | 32% |
| `checkpoint.c` | 33% | `ftl_fc_interface.c` | 32% |
| `bbt.c` | 31% | `wear.c` | 21% |
| `allocator.c` / `ftl.c` / `journal.c` / `l2p_cache.c` / `pending.c` / `read_recovery.c` | 30% | `metadata.c` | 22% |
| `completion_table.c` | 29% | `wear_leveling.c` | 18% |
| `gc.c` | 26% | | |

（改动前 3%–23% ⇒ 改动后 18%–70%。）

## 五、验证方法（**「零代码改动」的铁证**）

`.bin` 逐字节比对**不可用**：`src/main.c` 的横幅内嵌 `__DATE__`/`__TIME__`，每次重编必变。
改用**预处理剥注释后逐 token 比对**：

```sh
strip() { arm-none-eabi-gcc -fpreprocessed -dD -E -P "$1" 2>/dev/null; }
diff <(strip /tmp/ftl_orig/<file>) <(strip src/ftl/<file>)   # 无输出 = 仅注释变化
```

- 改动前把 19 个文件快照到 `/tmp/ftl_orig/`；每次编辑后立即跑 `strip`-`diff`。
- 该法**精确**：函数的重排、宏名拼写、任何 token 变化都会被抓出（过程中确实拦下过
  3 次误重排：`checkpoint.c` 分区编号、`allocator.c` 函数次序 + 宏名笔误、
  `read_recovery.c` 函数次序）。
- 终检：19/19 全部 `OK: comment-only`。
- `make all` 前置的 clang-format 仅对 `include/ftl/completion_table.h` 一处换行原型的
  续行缩进做了重排，`diff -w` 确认**纯空白**、无 token 变化。

## 六、边界与未做

- **不重排代码、不编号已存在的分区**（计划硬约束；过程中三次违规均被 `strip`-`diff` 拦下并回退）。
- `journal.c` / `crash.c` 的 ASSERT 策略不变（注释可加，`journal.c` 整个文件不转断言）。
- 不动 `include/nand/nand_config.h`。
- 未把 FTL 头现有自由标题词（`背景`/`流程`/`设计原则`…）改写成 FC 词表——属风格取舍，非缺漏。
- 未提交、未上板。
