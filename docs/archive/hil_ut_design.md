# HIL（`src/controller/`）层 UT 与系统集成验收设计

> **状态：设计文档（供审查）。实施（测试文件 / makefile 接线）在审查通过后进行。**
>
> **分层定位**（判据 `fc_nand_design.md §26.1`）：本文覆盖 **HIL（Host Interface Layer）**
> 与 **端到端系统集成** 两层，均**不属于 FTL 层**。
>
> - **HIL = `src/controller/`**（`cli.c` / `controller.c` / `request.c` / `completion.c` /
>   `state_machine.c` / `scheduler.c`）
> - **FTL 层**（`src/ftl/`）纯 RAM UT → `ftl_ut_munit.md`
> - **FC-NAND 三级 UT** → `fc_nand_ut.md`
>
> 本文由 `ftl_ut_munit.md` 拆分而来（2026-10-13）：原文把 HIL 行为验收（CLI `wr/rd/…`）
> 与 FTL 层纯逻辑 UT 混作一份「FTL 层测试」，标题名不副实，故拆出。

---

## 1. 为什么 HIL 要单独测（它不是 FTL 的一部分）

`fc_nand_design.md §26.1` 给出企业级主控固件的四层分层（术语各家不同，职责一致）：

```
Host
 │
 ▼
HIL    ← src/controller/   （协议/请求解析、LBA→实例路由、请求拆分/聚合、完成回执）
FTL    ← src/ftl/          （L2P / GC / WL / 坏块 / 掉电恢复 / 缓冲调度）
FCC    ← src/fc/           （通道/CE/LUN/plane 时序、描述符链、ECC、加扰、DMA）
NAND   ← src/nand/         （器件命令/时序）
```

**划界判据**：「**决定访问哪些物理单元 / 用什么策略**」→ **FTL**；
「**解析请求、路由、拆聚合、回执完成**」→ **HIL**。

因此 `src/controller/` 与 `src/ftl/` 是**平级的两层**，各自需要自己的 UT：

| 层 | 已有 UT | 本文档补的 |
|---|---|---|
| FC（FCC） | ✅ `fc_nand_ut.md`（`test_fc_level1/2/3.c`，4782 行） | — |
| **HIL** | ❌ **无** | **Part A**（L0）+ **Part B~D**（L2 验收） |
| FTL | 设计在 `ftl_ut_munit.md` | — |

**历史缺陷就在 HIL**，正是 HIL UT 该钉住的：
- `ftl_host_interface.md §A.2`：旧版 `request.c` 里 **HIL 替 FTL 决定 plane 配对**
  （「连续两 LBA → `pages=2` Multi-Plane」）——越权，v0.58 已收敛（`pages`→`count`）。
- `request.c` 子请求聚合曾有的**提前完成缺陷**（`sub_remaining` 计数在提交前置位，见
  `ssd_split_count` 注释）。

## 2. 路线：不做 mock——纯逻辑走 L0，跨层走 L2

| 层 | 运行环境 | 测什么 | 本项目 |
|---|---|---|---|
| **L0 单元** | 板上 RAM（纯函数） | HIL 纯逻辑：状态机 / 完成判定 / 路由 / run 边界 | **Part A** |
| **L2 系统** | 真 NAND 上板 | 端到端：自拆分 / 组完成 / 背压 / **跨重启** | **Part B~D** |

**不做 mock**（用户定调，见 `ftl_ut_munit.md §7`）：凡需真 NAND 的路径（`ssd_controller_submit`
实际提交、拆分后的 FTL 执行、GC/掉电）**一律走真板**，不建 `ftl_submit_*` 桩。

---

## Part A — HIL 层 UT（L0 纯 RAM）

### A.1 可测单元

HIL 层可 L0 测的，只有**不触 NAND 的纯函数**。逐一核过：

| 函数 | 位置 | 纯度 | 头文件可见 |
|---|---|---|---|
| `ssd_controller_state_transition` | `state_machine.c` | ✅ 纯（转换表） | ✅ `controller.h` |
| `ssd_controller_is_complete` | `controller.c:90` | ✅ 纯 | ✅ `controller.h` |
| `ssd_controller_schedule` | `scheduler.c` | ✅ 纯（读 `host_io_active`） | ✅ `controller.h` |
| `ssd_controller_route` | `request.c:18` | ✅ 纯（LBA→实例指针） | ⚠ **内部**（需导出） |
| `ssd_controller_run_end` | `request.c:52` | ✅ 纯（同实例最长 run） | ⚠ **内部**（需导出） |
| `ssd_controller_init` | `controller.c:44` | ❌ 校验真实 `ftl[]` 槽位 | — → L2 |
| `ssd_controller_submit` | `controller.c:99` | ❌ 实际提交 FTL → NAND | — → L2 |
| `ssd_request_split` / `ssd_split_count` | `request.c:101/86` | ❌ 内部调 `ftl_submit_*` | — → L2 |

> **⚠ 接线决策**：`ssd_controller_route` / `ssd_controller_run_end` 目前只在 `request.c`/
> `controller.c` 里前置声明（不在 `controller.h`）。要在 L0 测它们，须先**导出为公开原语**
> ——与 `ftl_ut_munit.md` 里 `l2p_cache_note_access` 的同类接线决策。
>
> **⚠ 当前 `CH_NUM=CE_NUM=LUN_NUM=1`**：路由恒返回同一实例 ⇒ `route`/`run_end` 是**平凡**的
> （run 恒等于整段）。这两条的真正价值在维度 > 1 时——那时按 region 粒度分片。第一版仍纳入
> （钉住「1 维下不分片」这个基线），扩维时自动变有意义。

### A.2 用例清单（`g_hil_*_tests[]`）

| 用例 | 前置 | 动作 | 断言 |
|---|---|---|---|
| `sm_legal_transitions` | `state=RESET` | RESET→INITIALIZING→READY | 每步 `state` 正确 |
| `sm_illegal_ignored` | `state=RESET` | RESET→READY（跳过 INITIALIZING） | 现态保持 RESET（负例） |
| `sm_failed_terminal` | `state=READY` | READY→FAILED→（试 INITIALIZING/READY） | 停在 FAILED，后续转换全忽略（终态，负例） |
| `sm_null_noop` | — | `state_transition(NULL, …)` | 不崩（`NULL` 守卫） |
| `is_complete_flag` | `complete_flag=0` | `is_complete` → 置 1 → `is_complete` | 0 → 1 |
| `schedule_host_io` | — | `schedule(ctrl, 1)` / `schedule(ctrl, 0)` | 1→让路(0) / 0→后台(1) |
| `route_single_dim` | 1 维路由表 | `route(ctrl, 任意 lba)` | 恒返回唯一实例指针 |
| `run_end_whole_segment` | 1 维 | `run_end(ctrl, lba, limit)` | == `limit`（1 维不分片） |
| `route_null_guard` | 空槽位表 | `route(ctrl, lba)` | 返回 NULL 不崩 |

> A.2 是**第一版**（1 维基线）。维度 > 1 时补：跨 region 边界的 run 切分、`route` 按
> `region % DIM` 分片、`run_end` 在实例边界处收尾。

### A.3 fixture 与接线

- **fixture 极小**：`ssd_controller_t` 只有 `state` + `ftl[CH][CE][LUN]` 指针表
  （1 维 = 4B 状态 + 1 指针）。纯 RAM，静态全局 + `setup` memset 复位。
  `route`/`run_end` 只需指针表里有**非 NULL** 的哑指针即可（不 deref，除非扩维）。
- **单文件** `tests/unit/test_hil.c`（仿 `test_ftl.c` / `test_fc_level1.c` 一文件多套件）。
- 导出 `g_hil_tests[]`，在 `ut_main.c` 加 `/hil/` 根套件（前缀**必须以 `/` 结尾**，
  `ut_main.c:24-27` 注释：munit 只做首尾相接、不补分隔符）。

---

## Part B — 对外接口行为验收（L2 真 NAND + UART CLI）

> 前置纪律：**擦净芯片**（`bbt --erase` → **立刻断电**，见 P0-2：运行中的 FTL 空闲会重写元数据）；
> 每条用例前记录开机 `[FTL] boot type`。断言以 `rd … c` 的 PASS/FAIL 与开机日志为准。
>
> **⚠ CW 级映射（v0.57）+ 页打包写缓冲（v0.66）之后，`wr` 的编程次数语义已变**——
> 旧文档「`wr 0 2` = 1 条 multi-plane」「`wr 0 5` = 3 条 2+2+1」**均已作废**，见下表。

### B.1 基础读写（对外接口行为）

| 用例 | 命令 | 断言 |
|---|---|---|
| `cli_rw_1` | `wr 0 1` → `rd 0 1 c` | PASS |
| `cli_rw_2`（单 plane 子页） | `wr 0 2` → `rd 0 2 c` | PASS；LBA0/1 同在 **plane0** ⇒ 1 次 `OP_NORMAL_PROGRAM_PAGE`(**op=16**) |
| `cli_rw_5`（跨 plane 一页） | `wr 0 5` → `rd 0 5 c` | PASS；LBA0..4 = plane0 cw0-3 + plane1 cw0 ⇒ **1 次** `OP_PROGRAM_PAGE_MULTI_PLANE`(**op=31**) |
| `cli_rw_8`（整对窗口） | `wr 0 8` → `rd 0 8 c` | PASS；整对 ⇒ 1 次 multi-plane(**op=31**) |
| `cli_rw_32`（CLI 上限） | `wr 0 32` → `rd 0 32 c` | PASS；4 个 pair 窗口 ⇒ **4 次**编程（`g_wr_buf`=16KB ⇒ `n≤32`） |
| `cli_large_rejected` | `wr 0 33` | 被容量校验**拒**（`INVALID_ARGUMENT`），不写爆 16KB 缓冲 |
| `cli_trim` | `wr 0 4` → `tr 0` → `rd 0 1 c` | 全 0 / 未映射语义 |
| `cli_flush` | `wr 0 2` → `fl` | 无错误；L2P/journal 落盘 |
| `cli_info` | `info` | `free/op/spare` 与几何相符 |

> **观测口径**：`info` 的 `total_writes` 现 = **编程次数**（每 flush 一次，v0.66）⇒ 是天然的
> 「编程计数」判据；每次 flush 另打一行 `[FTL] wbuf flush base=… cw=… nplanes=… owners=…`。
> `wp <lba> <n> <k>`（v0.66 新增诊断命令）可一次提交 k 组同窗口小写，验「多组合并成 1 次编程」。

### B.2 拆分与聚合（HIL 行为，借 CLI 观测）

| 用例 | 命令 | 断言 |
|---|---|---|
| `split_count_observe` | `wr 0 32` → `log level debug` | 4 条编程（4 个 pair 窗口）；`sub_remaining` 归零后**恰一次**完成 |
| `group_complete_once` | 任一 `wr` | 完成日志**恰一次**（不重复）——钉住聚合的提前完成缺陷 |
| `backpressure` | 连续提交（`wp`） | FTL 返 `FTL_AGAIN` 时不丢请求、不重复计数 |
| `async_pipeline`（**L2 异步压力**） | `wp <lba> <n> <k>` 一次提交 k 组同窗口小写 → `fl` → `rd … c` | 多组同时在飞：**恰一次**完成（每组）、`total_writes` = 编程次数、回读一致；对应 `ftl_ut_munit.md §5.4` 的 L2 侧 |

> **异步两层**（`ftl_ut_munit.md §5.4`）：**L0** 直接驱动纯簿记 `pending_group_*`（乱序完成 / `owners` 掩码 /
> 回调重入 / 粘性错误）；**L2** 即本表的 `async_pipeline`（真 NAND 提交→泵→完成整链）。

### B.3 跨重启一致性（**物理断电**）

> **落盘契约**（v0.61/v0.64 已闭合）：`ftl_submit_write` 返回 `FTL_OK` 后，映射经
> **journal 追加 + checkpoint/anchor** 持久化；journal 环的**写态由 `journal_scan_latest`
> 在启动时从 NAND 恢复**（v0.64），不再复位序号。下列用例即断言该契约。

| 用例 | 步骤 | 断言 |
|---|---|---|
| `reboot_persist_single` | 擦净 → `wr 0 1` → **断电** → 上电 → `rd 0 1 c` | PASS（v0.58 曾 FAIL，v0.61/v0.64 已修） |
| `reboot_persist_multi` | `wr 0 8` → 断电 → 上电 → `rd 0 8 c` | PASS |
| `reboot_after_flush` | `wr 0 2` → `fl` → 断电 → 上电 → `rd 0 2 c` | PASS |
| `reboot_no_flush` | `wr 0 1` → **不 `fl`** → 断电 → 上电 → `rd 0 1 c` | 按契约断言（journal 追加时机决定该丢还是该留） |
| `overwrite_then_reboot` | `wr 0 1`(A) → `wr 0 1`(B) → 断电 → 上电 → `rd 0 1 c` | 读回 **B**（最新胜） |
| `reboot_after_trim` | `wr 0 4` → `tr 0` → 断电 → 上电 → `rd 0 1 c` | 仍全 0 |
| `boot_type_classify` | 三种镜像（空 / 干净关机 / 写中掉电）分别启动 | `[FTL] boot type` = FRESH / CLEAN / POWER_FAILURE |
| `layout_version_mismatch` | 烧入**旧介质版本**镜像 → 启动 | 自动全片擦（v0.61）；`[FTL]` 日志报版本不符 |

### B.4 后台 / 寿命观测（上板压力）

| 用例 | 步骤 | 断言 |
|---|---|---|
| `gc_watermark_observe` | 连写直至 `free` 逼近 `FREE_POOL_TARGET(32)` | 观察 GC 触发（`info` free 回升） |
| `journal_watermark_observe` | 连写触发 journal 85%/95% | 观察水位告警 / emergency |
| `wear_persist_observe` | 重启后 `[WEAR] seq/pages` | 与上次写次数相符 |

---

## 覆盖矩阵与用例总数

| 层 | 套件/分组 | 被测 | 用例数 |
|---|---|---|---|
| L0 | `hil` | `state_machine.c` / `controller.c` / `scheduler.c` / `request.c` | 9 |
| L2 | B.1 基础读写 | HIL + FTL + FC 全链路 | 9 |
| L2 | B.2 拆分聚合 | HIL 拆分 / 聚合 / **异步流水** | 4 |
| L2 | B.3 跨重启 | boot recovery / journal / anchor | 8 |
| L2 | B.4 后台观测 | gc / journal / wear | 3 |
| **合计** | **1 L0 套件 + 4 L2 组** | — | **33** |

## 关键文件

| 文件 | 动作 |
|---|---|
| `docs/design/hil_ut_design.md`（本文档） | 设计（HIL UT + 系统集成验收） |
| `tests/unit/test_hil.c` | 实施：HIL L0 单文件——**用户自建** |
| `include/controller/controller.h` | 实施：导出 `ssd_controller_route` / `ssd_controller_run_end`（A.1 接线决策） |
| `src/controller/makefile.mk` | 实施：`FC_NAND_TEST_ENABLE` 块（一行 `TEST_C_SRCS`） |
| `tests/unit/ut_main.c` | 实施：`g_hil_tests` extern + `/hil/` 子套件注册 |
| `docs/design/ftl_host_interface.md` | Part C 的 CLI 行为引用改指本文档 Part B |

## 登记（本轮不做）

- **mock `ftl_submit_*` 桩 / Mock NAND / 掉电注入器**：**用户定调不做**——纯逻辑走 L0、
  跨层走真板。
- 扩维（`CH/CE/LUN > 1`）后的路由分片 / run 边界用例：等维度扩展时补（A.2 备注）。
- host（x86）编译链：本仓只支持 `arm-none-eabi` 上板。
