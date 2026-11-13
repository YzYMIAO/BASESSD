# 已知未实现 / 未验证项（known_iss）

> **边界**（与 `docs/design/known_risks.md` 的分工，R11）：
> - 本文件 = **设计 / 计划 / 测试中尚未实现或尚未上板验证**的条目（feature & verification backlog）。
> - `docs/design/known_risks.md` = **缺陷、静默失效、残余风险**（含已修未上板的实现风险）。
> - 同一件事只在一处登记；跨类引用写 ID，不复制内容。
>
> 来源文档多为一次性计划/设计稿，已按 2026-10-22 文档重组移入
> `docs/archive/`；条目保留来源文件名，便于追溯全文。
>
> 本文件采用与 `known_risks.md` 相同的骨架：**索引 + 详情块 + 历史/验证 + 已闭合/不实现表**。
> 字段按 ISS 语义适配：ISS 是 backlog，不是风险登记，不强制写「缓解」。

## 0. 文档规则

### 0.1 状态词（受控）

| 状态 | 含义 | 需要动作 |
|---|---|---|
| `未实现` | 功能/验证项未开始或未落地 | 排期/实现 |
| `部分实现` | 已有一部分；详情写清已完成/未完成 | 补缺口 |
| `待上板` | 实现/改动已完成，缺板端证据；按「最新验证」分两档 | 上板 |
| `待决策` | 需要用户裁决 | 用户 |
| `待硬件` | 依赖真实多 LUN / 空白片 / 仪器 | 等硬件 |
| `待测试源` | 需要测试源 | 等测试源 |
| `待定位` | 现象已知，根因未闭合 | 定位 |
| `待后续` | 已登记，独立任务 | 排期 |
| `信息` | 结论/维护规则，无动作 | 无 |
| `已闭合` | 有证据且无残余动作 | 归档 |

禁止同义写法：`未上板` / `未验证` / `已关闭` / `已修` / `待修` / `待接线` 一律归一到上表。
`不实现` 是用户明确裁决的结果，单独进各模块的「不实现」表，保留原因与重估触发。

### 0.2 索引表

每个模块一张索引表，四列固定：

| ID | 状态 | 一句话 | 最新验证 |
|---|---|---|---|
| ISS-XXX-NN | 待上板 | ≤40 字，只写缺口本身 | 见下 |

**最新验证**取值：`未上板` · `YYYY-MM-DD 编译通过` · `YYYY-MM-DD 代码核对` ·
`YYYY-MM-DD 板端测量` · `YYYY-MM-DD 常规回归通过（残余：…）` · `YYYY-MM-DD 上板通过` ·
`YYYY-MM-DD 上板失败` · `不适用`。

索引只放未闭合与 `信息` 条目；`已闭合` / `不实现` 进模块末尾的表，详情移
`docs/archive/known_iss_batches.md`。

### 0.3 详情块

每条一个小节，字段固定：

```markdown
#### ISS-XXX-NN 一句话标题

- **当前事实**：已实现/缺什么（只写当前，不写「原先/本轮」）
- **影响**：不做的后果 / 能力缺口
- **下一步 / 判据**：动作 + 验收判据
- **来源**：`docs/...`
- **关联**：`RISK-*` / `ISS-*` / 测试名
- **历史/验证**：（倒序，最新在上；只追加，不改写）
  - YYYY-MM-DD 动作 → 结果
```

- 当前事实、影响、下一步只写**当前**；变更史全部进历史行。
- 需要写当前替代方案时，加可选字段 `**当前替代**：…`，不强制。
- 历史行日期未知写 `（日期未记录）`，不得编造。

### 0.4 归档规则

- `已闭合` / `不实现`：从详情区移出，只保留模块末尾的表；详情原文移
  `docs/archive/known_iss_batches.md`，不改写。
- 状态变更时同步索引的「状态」与「最新验证」两列。

---

## 1. UT / 系统集成

### 索引

| ID | 状态 | 一句话 | 最新验证 |
|---|---|---|---|
| ISS-UT-01 | 部分实现 | FTL UT 已上板 95/95；剩余注入/空白片/白盒缺口 | 2026-11-11 上板通过 |
| ISS-HIL-01 | 部分实现 | HIL UT 15 例已上板；剩余 buffer NULL/TRIM 越界/跨重启/多 run | 2026-11-13 上板通过 |
| ISS-HIL-02 | 部分实现 | HIL L2 验收 24 例无逐条上板记录 | 未上板 |
| ISS-HIL-03 | 待硬件 | CH/CE/LUN>1 的 HIL 扩维用例 | 未上板 |

### 详情

#### ISS-UT-01 FTL UT 覆盖与剩余缺口

- **当前事实**：FTL UT 按 `docs/design/ftl_ut_design.md` 重建；当前注册 **81 个用例函数 / 参数化 95 次运行**（pure 57/65 + media 24/30）。2026-11-11 21:44:15 板端 `test /ftl/` **95/95，0 skip，235.63 s**。设计文档 §4 已按当前测试源同步，不再维护静态计数。
- **影响**：未落地项集中在故障注入、空白片、白盒构造；这些路径没有 UT 判据，不能声称覆盖。
- **下一步 / 判据**：只剩 M-11 真介质注入、CP 版本不符、真空白片、NAND 写擦失败注入；见 `docs/design/ftl_ut_design.md` §4.5/§7。每项必须有可失败的负例（C7）。
- **来源**：`docs/design/ftl_ut_design.md`；`tests/unit/ftl_ut_common.*`、`tests/unit/test_ftl_pure.c`、`tests/unit/test_ftl_media.c`
- **关联**：`ISS-UT-04`、`ISS-UT-05`、`RISK-UT-*`
- **历史/验证**：
  - 2026-11-11 21:44:15 `test /ftl/ --log-visible debug` → 95/95，0 skip，235.63 s
  - 2026-10-24 FTL UT 重建并上板 → 85 次运行全过

#### ISS-HIL-01 HIL 板端 UT

- **当前事实**：`tests/unit/test_hil.c` / `tests/unit/test_hil.h` + `g_hil_tests[]`（`make tests UT=HIL`）共 **15 例**。覆盖 controller 状态机 / init（缺槽、topo 不符）/ `lba_range_ok` / submit 边界（buffer 不足、0 长度、越界、未知 opcode、NULL controller/request）/ `is_complete` / host queue tag/owner/used / 经 controller 的真实 READ/WRITE/FLUSH/TRIM / `write→flush→read` / FLUSH/TRIM 单飞行背压。**不 format**，读写固定用容量尾部 8-LBA 窗口；不 mock，复用主 boot 的 FTL/NAND。USB 侧另跑主机矩阵（write→fsync 0x35/0x91、两写 lookahead→读、相同 LBA 连写、总线复位/拔出），由 PC 主机侧执行，不在 `/hil/` 套件内。
- **影响**：未覆盖 buffer NULL、TRIM 越界、跨重启、多 run 拆分/跨实例部分失败（单 die；需多 die 或 submit 注入）、USB 槽状态机（需 PC 主机侧矩阵）。这些是 HIL 契约的剩余缺口；测试设计见 `docs/design/hil_ut_design.md`。
- **下一步 / 判据**：按 `RISK-UT-34` 补：buffer NULL（见 `RISK-CTRL-04`）、TRIM 越界、跨重启、多 run / USB 槽矩阵。跨重启需把 `ftl_ut_common.c` 链入 `UT=HIL` 或走 `UT=` 全量构建。
- **来源**：`docs/design/hil_ut_design.md`；`tests/unit/test_hil.c` / `tests/unit/test_hil.h`；`tests/unit/ut_main.c`（`/hil/`）；`src/hil/makefile.mk` / `src/fc/makefile.mk` / `makefile`（`UT=HIL`）
- **关联**：`RISK-UT-34`、`ISS-HIL-02`、`ISS-HIL-03`
- **历史/验证**：
  - 2026-11-13 13:21:29 镜像 `test /hil/` → 15/15，0 skip，10.56 ms（seed `0x5a4f21e4`）
  - 2026-11-11 22:32:57 `test /hil/ --log-visible debug` → 15/15，0 skip，8.99 ms
  - 2026-11-11 22:14:53 `test /hil/` → 12/12，0 skip
  - 2026-11-11 22:25:23 15 例跑到 `write_flush_readback` 时用例体通过、teardown `timed_out`（只泵 `ftl_service`，未推进 CP/Journal）

#### ISS-HIL-02 HIL L2 验收 24 例

- **当前事实**：HIL L2 验收 24 例：基础读写 9、拆分/聚合/背压 4、跨重启 8、后台/水位观测 3。v0.58~v0.66 期间可能用 CLI 人工跑过，但**无逐条上板记录**。
- **影响**：没有逐条记录就不能声称 L2 验收覆盖。
- **下一步 / 判据**：按 `docs/design/usb_msc.md` Part B 的矩阵补记录；跨重启 8 例优先（`reboot_persist_*` 已修，需确认）。
- **来源**：`docs/archive/hil_ut_design.md` Part B；`docs/design/usb_msc.md` Part B
- **关联**：`ISS-HIL-01`
- **历史/验证**：
  - （日期未记录）v0.58~v0.66 期间可能用 CLI 人工跑过 → 无逐条记录

#### ISS-HIL-03 HIL 扩维用例

- **当前事实**：CH/CE/LUN>1 后的 HIL 扩维用例（route / run_end 在 1 维下过于平凡）尚未编写。
- **影响**：多实例路由/run 边界没有 HIL 覆盖；1 维下测不出真实分片问题。
- **下一步 / 判据**：等真实多实例硬件或 N=2 影子构建；勿在 1 维伪造覆盖。
- **来源**：`docs/archive/hil_ut_design.md` §3
- **关联**：`ISS-ML-01`
- **历史/验证**：
  - （无板端记录）

### 已闭合

| ID | 日期 | 一句话 | 归档 |
|---|---|---|---|
| ISS-UT-02 | 2026-11-13 | FC/NAND 测试源同步；`test /fc-nand/` 61 OK + 3 SKIP（预期 SKIP） | `docs/archive/known_iss_batches.md` |
| ISS-UT-04 | 2026-11-11 | FTL 测试源按当前 API 同步，`test /ftl/` 100/100 | `docs/archive/known_iss_batches.md` |
| ISS-UT-05 | 2026-11-11 | FTL UT 覆盖缺口补齐，`test /ftl/` 95/95 | `docs/archive/known_iss_batches.md` |

---

## 2. NAND OP 启用（方案已评审，未改代码）

### 索引

| ID | 状态 | 一句话 | 最新验证 |
|---|---|---|---|
| ISS-OP-01 | 未实现 | GC/WL 未用 multi-plane 写（OP 31） | 未上板 |
| ISS-OP-02 | 未实现 | Copy-Back op45 未实现 | 未上板 |
| ISS-OP-03 | 部分实现 | two-plane cache program op44 正常路径已上板；FAILC/掉电/GC batch 未验 | 2026-10-31 常规回归通过（残余：FAILC/掉电/GC batch） |
| ISS-OP-04 | 未实现 | Phase 0 测量未做 | 未上板 |
| ISS-OP-05 | 待决策 | D1~D5 路线决策 | 未上板 |

### 详情

#### ISS-OP-01 P1：GC/WL 用现有 multi-plane 写（OP 31）

- **当前事实**：`mig_write_submit` 仍逐 plane 提交，与 `RISK-PERF-02` 同一问题。
- **影响**：GC/WL 写放大与写速未优化；source 侧未用 OP 31。
- **下一步 / 判据**：先完成 `ISS-OP-04` 的 Phase 0；P1 判据 = 写放大下降、host 写速不降。
- **来源**：`docs/archive/nand_op_enablement_plan.md` §3.1
- **关联**：`ISS-OP-04`、`RISK-PERF-02`
- **历史/验证**：
  - （无板端记录）

#### ISS-OP-02 P2：Copy-Back 复合 op 45

- **当前事实**：未实现；仅限整页全有效、同 plane、非 metadata、源 ECC 干净四条件。
- **影响**：GC 搬移无法省去读回；写放大/带宽收益未拿到。
- **下一步 / 判据**：依赖 P1 与 Phase 0 结论；方案 A/B 选型待定。
- **来源**：`docs/archive/nand_op_enablement_plan.md` §3.2
- **关联**：`ISS-OP-01`、`ISS-OP-04`
- **历史/验证**：
  - （无板端记录）

#### ISS-OP-03 P3：Two-plane Cache Program（op 44）

- **当前事实**：host 16 KiB 正常路径已上板（H2testw 415/423 MB，843/889 KB/s，`cw` 24906/99624/0 abort）；GC/WL batch 已接 FTL 但本次 `mb=0` 未触发；FAILC 负例、失败注入、掉电注入、静态 WL high-wear 批次未做。
- **影响**：cache program 的失败/掉电路径没有完整判据；GC/WL batch 未真正压过。
- **下一步 / 判据**：补 FAILC 负例/掉电注入；GC batch 需要构造可批的 victim 后再验。
- **来源**：`docs/archive/nand_op_enablement_plan.md` §3.3；`docs/design/ftl_design.md` ADR-15；`docs/design/ftl_revision_history.md` v0.94/v0.96
- **关联**：`RISK-CACHE-19/24/27/28`、`ISS-FTL-16`
- **历史/验证**：
  - 2026-10-31 H2testw 415/423 MB → host 16 KiB 正常路径通过
  - 2026-10-31 GC/WL batch → `mb=0`，未触发

#### ISS-OP-04 Phase 0 测量

- **当前事实**：5 项假设埋点（host 写分块 / NAND program 次数 / GC 占比等）+ Stop/Go 判据未做。
- **影响**：没有基线就无法判断 P1/P2/P3 的收益与顺序。
- **下一步 / 判据**：只加计数器不改行为；Stop 条件见 `docs/archive/nand_op_enablement_plan.md` §5.4。
- **来源**：`docs/archive/nand_op_enablement_plan.md` §5
- **关联**：`ISS-OP-01`、`ISS-OP-02`、`ISS-OP-03`
- **历史/验证**：
  - （无板端记录）

#### ISS-OP-05 D1~D5 路线决策

- **当前事实**：是否先做 P1、copy-back 选项 A/B、cache program 是否进入路线、op 表扩展方式等未裁决。
- **影响**：P1/P2/P3 的排期无法确定。
- **下一步 / 判据**：需用户裁决后才能排期。
- **来源**：`docs/archive/nand_op_enablement_plan.md` §8
- **关联**：`ISS-OP-01`~`ISS-OP-04`
- **历史/验证**：
  - （无板端记录）

### 不实现

| ID | 日期 | 一句话 | 归档 |
|---|---|---|---|
| ISS-OP-06 | 2026-11-13 | Cache Read / ISSUE/WAIT/FETCH / Block Lock/OTP/on-die ECC 当前明确不做 | `docs/archive/known_iss_batches.md` |

---

## 3. FTL 优化计划遗留

### 索引

| ID | 状态 | 一句话 | 最新验证 |
|---|---|---|---|
| ISS-FTL-01 | 部分实现 | OOB/spare 出厂坏块通路未启用 | 未上板 |
| ISS-FTL-02 | 待决策 | band 级 sequence/validity 聚合是否删除 | 未上板 |
| ISS-FTL-03 | 未实现 | GC Bitmap 断点续搬 | 未上板 |
| ISS-FTL-04 | 未实现 | Prefetch 顺序读 | 未上板 |
| ISS-FTL-05 | 未实现 | 多流 host stream hint | 未上板 |
| ISS-FTL-06 | 未实现 | 学习型 L2P 压缩 | 未上板 |
| ISS-FTL-07 | 待定位 | 擦除 op 18→17 原因未闭合 | 未上板 |
| ISS-FTL-08 | 待上板 | F8/F9 跨层项缺板端证据 | 未上板 |
| ISS-FTL-09 | 待后续 | HAL RCC 频率查询对 FMC/QSPI/USART 恒返 0 | 未上板 |
| ISS-FTL-10 | 待上板 | FMC HiZ 写方向压力测试未做 | 未上板 |
| ISS-FTL-11 | 未实现 | HIL 队列 + 门铃 + CQ 重构 | 未上板 |
| ISS-FTL-12 | 未实现 | 跨 LUN superblock 条带/GC/WL | 未上板 |
| ISS-FTL-13 | 待决策 | 顺序流物理相邻选块 vs WL 权衡 | 2026-10-31 板端测量 |
| ISS-FTL-14 | 待决策 | CP 让步策略 A/B | 2026-10-31 板端测量 |
| ISS-FTL-15 | 未实现 | 消除 cache 写路径 wbuf 回退 | 未上板 |
| ISS-FTL-18 | 部分实现 | GC 搬移有效页缺少自然触发场景 | 2026-10-31 上板通过（残余：内容正确性未验） |
| ISS-FTL-17 | 未实现 | 持久化 block→SB 归属（需介质格式升级） | 未上板 |
| ISS-FTL-16 | 部分实现 | 注入类验证工程化；CLI/deferred-error 端到端未做 | 2026-10-31 上板通过（部分） |

### 详情

#### ISS-FTL-01 OOB/spare 出厂坏块通路

- **当前事实**：出厂坏块检测已改为 Page0 META 抽样；**OOB 标记通路未启用**。
- **影响**：二手片出厂标记已丢（AN1819），当前不追；换新片/需要出厂坏块图时没有 OOB 通路。
- **下一步 / 判据**：换新片或需要出厂坏块图时重估；当前不追。
- **来源**：`docs/archive/ftl_optimization_plan.md` §8.1
- **关联**：`RISK-BBT-*`
- **历史/验证**：
  - （无板端记录）

#### ISS-FTL-02 band 级 sequence/validity 聚合

- **当前事实**：P2-4 band 级聚合字段的消费者已被 journal 恢复取代，判定为死字段。
- **影响**：残留代码/文档会形成第二套坐标（C9）。
- **下一步 / 判据**：确认关闭并删除残留代码/文档；不要重新引入第二套坐标。
- **来源**：`docs/archive/ftl_optimization_plan.md` §8.1
- **关联**：`RISK-ARCH-*`
- **历史/验证**：
  - （无板端记录）

#### ISS-FTL-03 GC Bitmap 断点续搬

- **当前事实**：GC Bitmap（Layer 2）断点续搬未实现；正确性窗口已由「擦除前 flush」闭合，仅剩性能收益。
- **影响**：GC 掉电后需要重扫/重搬，性能下降；正确性不受影响。
- **下一步 / 判据**：需要 GC 掉电续搬性能数据后再排期。
- **来源**：`docs/archive/ftl_optimization_plan.md` §8.2
- **关联**：`RISK-GC-05`
- **历史/验证**：
  - （无板端记录）

#### ISS-FTL-04 Prefetch 顺序读

- **当前事实**：P3-1 Prefetch 顺序读未实现。
- **影响**：顺序读预取收益未拿到。
- **下一步 / 判据**：前沿项，未排期。
- **来源**：`docs/archive/ftl_optimization_plan.md` §8.2
- **关联**：`RISK-PERF-*`
- **历史/验证**：
  - （无板端记录）

#### ISS-FTL-05 多流 host stream hint

- **当前事实**：P3-2 多流 host stream hint 未实现；当前仅双温区。
- **影响**：企业级多流隔离能力缺失。
- **下一步 / 判据**：企业级方向，未排期。
- **来源**：`docs/archive/ftl_optimization_plan.md` §8.2
- **关联**：`RISK-WL-*`
- **历史/验证**：
  - （无板端记录）

#### ISS-FTL-06 学习型 L2P 压缩

- **当前事实**：P3-3 学习型 L2P 压缩（LeaFTL 方向）未实现。
- **影响**：L2P 内存/带宽压缩收益未拿到。
- **下一步 / 判据**：工程量大，未排期。
- **来源**：`docs/archive/ftl_optimization_plan.md` §8.2；`docs/reference/papers/LeaFTL.pdf`
- **关联**：`RISK-L2P-*`
- **历史/验证**：
  - （无板端记录）

#### ISS-FTL-07 擦除 op 18→17

- **当前事实**：固件侧实测零改写，原因未闭合。
- **影响**：op 映射与日志口径不一致，排障时会误导。
- **下一步 / 判据**：插桩停在 `tests/unit/test_fc_level3.c`；未出结论前不改 op 映射。
- **来源**：`docs/archive/ftl_optimization_plan.md` §8.5
- **关联**：`RISK-NAND-*`
- **历史/验证**：
  - （日期未记录）固件侧实测零改写 → 原因未闭合

#### ISS-FTL-08 F8 / F9 跨层项

- **当前事实**：F8 / F9（依赖上板的跨层项）缺板端证据。
- **影响**：跨层行为无法判定。
- **下一步 / 判据**：按原条目补板端证据后再定。
- **来源**：`docs/archive/ftl_optimization_plan.md` §8.5
- **关联**：`RISK-*`
- **历史/验证**：
  - （无板端记录）

#### ISS-FTL-09 HAL_RCCEx_GetPeriphCLKFreq 恒返 0

- **当前事实**：`HAL_RCCEx_GetPeriphCLKFreq` 对 FMC/QSPI/USART1 恒返 0（HAL 无这三个 case）。
- **影响**：依赖该查询的时钟配置会静默拿 0；属于平台层陷阱（`CLAUDE.md` §3.3）。
- **下一步 / 判据**：平台层自建频率查询或固定表；不要在 FTL 侧兜。
- **来源**：`docs/archive/ftl_optimization_plan.md` §8.5；平台层
- **关联**：`RISK-PLAT-*`
- **历史/验证**：
  - （日期未记录）代码核对 → HAL 只实现 12 个外设

#### ISS-FTL-10 FMC HiZ 写方向

- **当前事实**：FMC HiZ 是写方向；写压力测试未做。
- **影响**：长时写/拔电回归未覆盖。
- **下一步 / 判据**：长时写 + 拔电回归。
- **来源**：`docs/archive/ftl_optimization_plan.md` §8.5
- **关联**：`RISK-NAND-*`
- **历史/验证**：
  - （无板端记录）

#### ISS-FTL-11 HIL 队列 + 门铃 + CQ

- **当前事实**：D4/D5（HIL 队列 + 门铃 + CQ，更大重构，主控学习方向）未实现。
- **影响**：当前 USB BOT 单命令模型无法表达主流多队列/门铃。
- **下一步 / 判据**：单独设计；不要与 USB BOT 单命令模型混用。
- **来源**：`docs/archive/ftl_optimization_plan.md` §8.5
- **关联**：`ISS-FC-01`
- **历史/验证**：
  - （无板端记录）

#### ISS-FTL-12 跨 LUN superblock 条带 / GC/WL

- **当前事实**：跨 LUN superblock 条带 / 跨 LUN GC/WL 未实现；当前严格 per-LUN。
- **影响**：设备级并行与容量利用受限。
- **下一步 / 判据**：需要设备级 relocation 引擎；先在真实多 LUN 上做基线。
- **来源**：`docs/archive/multi_lun_generalization.md`；`RISK-ARCH-02`
- **关联**：`ISS-ML-01`、`ISS-ML-04`
- **历史/验证**：
  - （无板端记录）

#### ISS-FTL-13 顺序流物理相邻选块

- **当前事实**：写序列跨 block 已允许（`RISK-CACHE-20/21`），但读侧实测段长只到 **41 pair/start**（`cr=27435/105840/2583`）——分配器 `alc_free_find_block()` 按动态 WL（最小擦除次数）选块，顺序写下去的下一个 block 大多不是物理 `block+1`，于是读 cache 序列在 block 边界就断（预取 100% 命中，128 pair 上限不是限制）。
- **影响**：顺序读段长/吞吐受限；若改为物理相邻，会与 WL（最小 EC）直接冲突。
- **下一步 / 判据**：先裁决是否把物理相邻列为顺序流选块偏好；实现前先测「现有块序里 `block+1` 的命中率」（可在 `alc_super_open` 记录），命中率低才值得改。
- **来源**：`src/ftl/allocator.c` `alc_free_find_block`；`docs/design/ftl_revision_history.md` v1.06；`RISK-CACHE-20`
- **关联**：`RISK-CACHE-20/21`、`ISS-FTL-14`
- **历史/验证**：
  - 2026-10-31 板端 `cr`/`pf` → 41 pair/start

#### ISS-FTL-14 CP 让步策略

- **当前事实**：持续 host 写期间 CP 只能在序列让出 die 的窗口提交（单 die，序列占用 cache 寄存器时 `ARDY=0`，程序命令发不进去）。可选：A. must-run 后台被拒超过 N ms 时请求一次 `break_requested`（下一窗口 END）；B. 序列结束后空出让位一轮。
- **影响**：CP 推进慢（`phase=8` 长时间心跳），但非死锁；选 A 会牺牲写吞吐，选 B 会延长 CP 延迟。
- **下一步 / 判据**：需用户裁决阈值（如每 1 s 最多让一次）、是否接受 `jf` 上升与写吞吐略降；上板判据 = 写期间 `cp` 序列持续前进且写速下降可接受。
- **来源**：`RISK-CACHE-23`；`src/ftl/ftl_op.c` `ftl_op_seq_gate`；2026-10-31 板端 CP 心跳
- **关联**：`RISK-CACHE-23`
- **历史/验证**：
  - 2026-10-31 板端 → `phase=8` 长时间心跳，非死锁

#### ISS-FTL-15 消除 cache 写路径的 wbuf 回退

- **当前事实**：`jrn_seq_begin()` 在「顺序页页刷/擦在飞」时返回 false，写路径直接 `DECLINED` 回退 wbuf；该等待是瞬时的，等页刷完成后本可以 1 页窗口开序列并跨 block 续写。现在只有 decline 总数（2,020），没有原因分布。
- **影响**：`wr−cw` 残差 ≈1.2 页/块；写路径多一次回退。
- **下一步 / 判据**：① 加 decline 归因计数（零行为变化）→ 上板一次取分布；② `ring_full` ⇒ 仍回退 wbuf，页刷/擦在飞 ⇒ 返回 `AGAIN` 等待（`bg_progress` 已计 Journal 在飞）；③ 上板判据：`wr−cw` 降到 ~0.1 页/块、`jf` 不上升。
- **来源**：`RISK-CACHE-21` 的 `wr−cw` 残差；`src/ftl/cache_batch.c` `ftl_cache_write_try`；`src/ftl/journal.c` `jrn_seq_begin`
- **关联**：`RISK-CACHE-21`
- **历史/验证**：
  - 2026-10-31 板端 → `wr−cw` ≈2,019（≈1.2 页/块）

#### ISS-FTL-18 GC「搬移有效页」路径

- **当前事实**：所有上板运行 `mv=0`、`nop` 持续增长——victim 全是空块（删文件/覆盖写使旧页失效）。`mig_job_*` 的 READ/WRITE/Journal 搬移分支只在 victim 带有效 CW 时才走。
- **影响**：搬移分支长期未被上板压过；搬移内容正确性未单独验证。
- **下一步 / 判据**：构造带有效 CW 的 victim；判据 = `mv>0`、搬移后 verify 0 错误、`dng=0`。
- **来源**：2026-10-31 板端 `info`/`[STAT]`（`mv=0 nop=1290`）；`RISK-GC-09`/`RISK-GC-14`；`src/ftl/migrate.c`
- **关联**：`RISK-GC-09`、`RISK-GC-14`、`RISK-ALLOC-10`
- **历史/验证**：
  - 2026-10-31 首次自然触发：`[GC ] start reason=periodic trig=0x01 mode=off free=30` → `end jobs=11 moved=66 rec=11 noop=6 abort=0 elapsed=10804ms`，`free` 22→32；搬移路径完整执行、无 abort；同次运行随后出现数据丢失（`RISK-ALLOC-10`/`RISK-L2P-09`），内容正确性仍未单独验证

#### ISS-FTL-17 持久化 block→SB 归属

- **当前事实**：CP 块快照 word 只有 16 bit（`cp_block_word_pack(valid, st, temp, pool)`，`src/ftl/checkpoint.c:1350`），不含 `sb_id` ⇒ 恢复后 closed 条目只能是单 plane（`pfr_closed_sb_add`），GC 逐条回收会让 free 池 plane 失衡（`RISK-ALLOC-09`）。彻底修法 = 把 sb_id 持久化（扩快照 word 或在块 OOB/META 记录组号），恢复时成对重建；代价：介质布局版本 +1、首启格式化、整条掉电恢复路径重验（`RISK-CACHE-17` 等）。
- **影响**：恢复后 closed 条目单 plane，GC 回收造成 free 池 plane 失衡；低水位时成对分配失败。
- **下一步 / 判据**：格式升级后恢复的 `closed` 条目全部成对（除非原始数据本身单 plane），GC 回收后 `fpmin` 与 `free` 同步增长；升级同时重跑两轮掉电回放。用户 2026-10-31 决定：本轮不做打折版（恢复期合成配对），等做完整版。
- **来源**：`src/ftl/checkpoint.c` `cp_block_word_pack`；`src/ftl/power_failure_recovery.c` `pfr_closed_sb_add`；`RISK-ALLOC-09`；`docs/design/ftl_design.md` §3.13.4
- **关联**：`RISK-ALLOC-09`、`RISK-CACHE-17`
- **历史/验证**：
  - 2026-10-31 用户裁决 → 不做打折版，等完整版

#### ISS-FTL-16 注入类验证的工程化

- **当前事实**：① FTL 层注入 `include/common/fault_inject.h`（**测试镜像专属**：开关 `FC_NAND_TEST_ENABLE`，产品固件零代码；2026-11-01 起不再是 `NDEBUG` 门控）：`fi_cache_fail_arm`（cache program 完成上伪造 FAIL_CACHE，一次性、按页 LBA）、`fi_journal_fail_arm`（Journal 页写按失败结算）；UT 侧已落地 `cache_failc_abort` / `journal_write_fail_wal_stall`。② **未做**：`ftl_sync_*`（`RISK-BBT-02` / `RISK-MD-16`；⚠ 注入同步页读失败前要先裁决 `bbt_medium_has_ftl_data` 的判据——它用普通同步读探「介质是否写过」，读不通会被当成空片 ⇒ 出厂扫描可能把写过的块全判坏）、命令池精确饥饿（`RISK-L2P-05` / `RISK-CP-04`）、CLI 注入钩子、deferred error 的控制器/USB 端到端（`RISK-USBC-01`；2026-11-01 已接线：`usb_msc_take_deferred_error()` 先问 FTL，UT 用例规格见 `docs/design/ftl_ut_design.md` §4.5，**上板未做**）；③ 掉电回放 JSEG（`RISK-CACHE-17`）需物理断电/复位。
- **影响**：失败路径没有完整注入判据；CLI/USB 端到端未验。
- **下一步 / 判据**：注入只在 debug 档编入；判据 = page0 失败时其后页均不确认、Journal 失败时区域写回被 WAL 闸门挡住、清注入后能恢复；测试 setup 必须 `fi_reset()`（状态是进程级）。
- **来源**：`RISK-CACHE-19` / `RISK-JN-03` / `RISK-L2P-09` / `RISK-USBC-01`；`docs/design/ftl_ut_design.md` §4.5/§5
- **关联**：`RISK-CACHE-19`、`RISK-JN-03`、`RISK-L2P-09`、`RISK-USBC-01`、`RISK-BBT-02`、`RISK-MD-16`
- **历史/验证**：
  - 2026-10-31 `cache_failc_abort` arm=1/2/last、`journal_write_fail_wal_stall` → 上板全过
  - 2026-11-01 注入不再由 `NDEBUG` 门控 → 接线完成

---

## 4. 多 LUN 泛化

### 索引

| ID | 状态 | 一句话 | 最新验证 |
|---|---|---|---|
| ISS-ML-01 | 待硬件 | 多 LUN/共享 L2P cache 仅影子链接 | 未上板 |
| ISS-ML-02 | 未实现 | 多 LUN Phase D 未开始 | 未上板 |
| ISS-ML-03 | 待决策 | 目标 LUN 数与物理并行通道 | 未上板 |
| ISS-ML-04 | 未实现 | 跨 LUN GC/WL/条带 | 未上板 |

### 详情

#### ISS-ML-01 多 LUN / 共享 L2P cache Phase A/B/C

- **当前事实**：仅影子 N=2 链接，无实机。
- **影响**：多 LUN 并行/共享 L2P cache 未在真实硬件上验证。
- **下一步 / 判据**：需要真实多 die/多 LUN 硬件。
- **来源**：`docs/archive/multi_lun_generalization.md`
- **关联**：`ISS-ML-02`、`ISS-ML-03`
- **历史/验证**：
  - （无板端记录）

#### ISS-ML-02 多 LUN Phase D

- **当前事实**：Phase D（审计、文档、验证清单）未开始。
- **影响**：多 LUN 改动缺少收口审计。
- **下一步 / 判据**：依赖 `ISS-ML-01`。
- **来源**：`docs/archive/multi_lun_generalization.md` Phase D
- **关联**：`ISS-ML-01`
- **历史/验证**：
  - （无板端记录）

#### ISS-ML-03 目标 LUN 数与物理并行通道

- **当前事实**：目标 LUN 数（2 / 4）与是否需要物理并行通道未裁决。
- **影响**：多 LUN 设计方向未定。
- **下一步 / 判据**：用户裁决；与 `ISS-CW-01` 的 `LUN_ADDR_WIDTH` 结论对齐。
- **来源**：`docs/archive/multi_lun_generalization.md` §9
- **关联**：`ISS-ML-01`、`ISS-CW-01`
- **历史/验证**：
  - （无板端记录）

#### ISS-ML-04 跨 LUN GC/WL / 条带

- **当前事实**：未实现；同 `ISS-FTL-12`。
- **影响**：跨 LUN 容量/并行利用受限。
- **下一步 / 判据**：单独设计，不在当前 per-LUN 引擎上打补丁。
- **来源**：`docs/archive/multi_lun_generalization.md`
- **关联**：`ISS-FTL-12`
- **历史/验证**：
  - （无板端记录）

---

## 5. USB / 验证手册待测项

### 索引

| ID | 状态 | 一句话 | 最新验证 |
|---|---|---|---|
| ISS-USB-01 | 待上板 | 写过程中掉电恢复未验 | 未上板 |
| ISS-USB-02 | 待上板 | USB 吞吐 / `dd` 基线未验 | 未上板 |
| ISS-USB-04 | 待上板 | 多分区/第二分区写满校验未验 | 未上板 |

### 详情

#### ISS-USB-01 写过程中掉电恢复（USB-V08）

- **当前事实**：未验证。
- **影响**：写中断电后的卷可挂载性/哈希一致性没有板端证据。
- **下一步 / 判据**：写 32–64 MB 中拔电，重启后卷可挂载、哈希一致。
- **来源**：`docs/design/usb_msc.md` Part B §B.8
- **关联**：`RISK-CACHE-18`、`RISK-BOOT-*`
- **历史/验证**：
  - （无板端记录）

#### ISS-USB-02 吞吐 / `dd` 基线（USB-V09）

- **当前事实**：未验证。
- **影响**：没有可复现的 USB 吞吐基线。
- **下一步 / 判据**：记录顺序写/读、4K 随机；与 `RISK-NAND-04` 的瓶颈定位合并。
- **来源**：`docs/design/usb_msc.md` Part B §B.8
- **关联**：`RISK-NAND-04`
- **历史/验证**：
  - （无板端记录）

#### ISS-USB-04 多分区 / 第二分区

- **当前事实**：多分区/第二分区（>128 MB）写满 + 校验未验证；历史 rev33 第二分区失败见 `RISK-MIG-07`。
- **影响**：第二分区容量/映射可能仍有问题。
- **下一步 / 判据**：v12 介质上重跑；与全盘 H2testw 分开记录。
- **来源**：`docs/design/usb_msc.md` Part B §B.8；`RISK-MIG-07`
- **关联**：`RISK-MIG-07`
- **历史/验证**：
  - （无板端记录）

### 不实现

| ID | 日期 | 一句话 | 归档 |
|---|---|---|---|
| ISS-USB-03 | 2026-11-01 | WRITE(10) FUA 位语义不实现，主机 fsync 走 SYNCHRONIZE CACHE | `docs/archive/known_iss_batches.md` |

---

## 6. 平台 / 日志 / 故障捕获

### 索引

| ID | 状态 | 一句话 | 最新验证 |
|---|---|---|---|
| ISS-PLAT-01 | 未实现 | 平台抽象 Stage 3 未做 | 未上板 |
| ISS-PLAT-03 | 待后续 | linker `.noncacheable_buffer` 输入段漏前导点 | 未上板 |
| ISS-PLAT-04 | 待上板 | 统一定时调度器已实现未上板 | 2026-11-03 编译通过 |
| ISS-BG-01 | 待上板 | 后台节拍 flag + token bucket 已实现未上板 | 2026-11-03 编译通过 |
| ISS-TOP-01 | 待上板 | NAND 拓扑扫描 + FTL die 表已实现未上板 | 2026-11-03 编译通过 |
| ISS-LOG-01 | 待上板 | `fault_capture.md` §10.5 的 9 项未逐条确认 | 未上板 |
| ISS-FAULT-01 | 待上板 | `fault_capture.md` 三项手工验证未做 | 未上板 |
| ISS-FAULT-02 | 待上板 | PC 采样器已实现未上板 | 2026-11-05 编译通过 |

### 详情

#### ISS-PLAT-01 平台抽象 Stage 3

- **当前事实**：NAND 层裸 MMIO / `HAL_NAND_*` 收敛未做。
- **影响**：平台边界仍不纯，换平台成本高。
- **下一步 / 判据**：单独一轮；改后地址/时序类量必须上板（R2）。
- **来源**：`docs/design/platform_abstraction.md` §五
- **关联**：`RISK-PLAT-*`
- **历史/验证**：
  - （无板端记录）

#### ISS-PLAT-03 linker `.noncacheable_buffer` 输入段

- **当前事实**：linker `.noncacheable_buffer` 输入段漏前导点，落点随启发式漂移。
- **影响**：DMA/页缓冲落点不稳定，可能静默破坏数据（`RISK-PLAT-01`）。
- **下一步 / 判据**：单独一轮：`KEEP(*(.noncacheable_buffer))` + 上板回归 DMA。
- **来源**：`RISK-PLAT-01`；`linker/stm32h743xx_flash.ld`
- **关联**：`RISK-PLAT-01`
- **历史/验证**：
  - （无板端记录）

#### ISS-PLAT-04 统一定时调度器 + 时间源纠正

- **当前事实**：`btim`→`timer` 改名；一张表 `{fn, ctx, first_delay, period}`；watchdog / `[STAT]` / FTL 周期评估迁入；拆 `mig_handle` 的 advance/eval；`bg_scheduler` 降为预算子引擎。系统时基 = ARM 核心 SysTick（ms + µs）；timer 调度器心跳 = TIM6；DWT 从产品路径删除。Phase 0~3 已落地（改名 + 容量 64 + `add_at/set_period/task_count` + `[STAT]` 迁移 + `mig_eval` 注册）；delay/usart/watchdog/TinyUSB/FTL `time_ms` 统一到 SysTick；NAND/USB 的 DWT µs 计时改为 SysTick `LOAD/VAL`；TIM6 只驱动 timer 任务表。
- **影响**：未上板验证，时间源/调度行为没有板端证据。
- **下一步 / 判据**：上板验证 timebase/调度；见 `docs/design/platform_abstraction.md` §8。
- **来源**：`docs/design/platform_abstraction.md` §8
- **关联**：`ISS-BG-01`、`RISK-UT-05`
- **历史/验证**：
  - 2026-11-03 Phase 0~3 落地 → 编译通过，未上板

#### ISS-BG-01 后台节拍 flag + token bucket

- **当前事实**：CP T3 / L2P flush / BBT 起新 job 由 10 ms timer 任务置 flag；`bg_scheduler` 预算从「每轮配额」改为「每 10 ms 补 `grant_units` credit（IDLE/NORMAL 4、BUSY 1，上限 4 个 interval）」；must-run 与在飞推进不变。
- **影响**：GC/WL 吞吐与 host 写延迟未上板测量。
- **下一步 / 判据**：上板测 GC/WL 吞吐与 host 写延迟；按结果调 `grant_units` / `BG_TOKEN_INTERVAL_MS`；验证 flag 消费、T3 不延迟、BBT 仍能落盘。
- **来源**：`RISK-BG-10`；`docs/design/platform_abstraction.md` §8.5
- **关联**：`RISK-BG-10`、`ISS-PLAT-04`
- **历史/验证**：
  - 2026-11-03 落地 → 编译通过，未上板

#### ISS-TOP-01 NAND 拓扑扫描 + FTL die 表

- **当前事实**：板级 `nand_scan_targets` 在 armed 前经 `nand_select_target` 选通路、探测 (ch,ce) target，并用 78h 逐 LUN 探测，结果写 `g_nand_targets`；FTL `ftl_topology_build` 给实际存在的 LUN 分配紧凑 die 号，维护 `g_chcelun_table` / `g_die_table`。Phase 2a：实例遍历改走 `for die < DIE_MAX` + `g_die_table`。Phase 2b：L2P 路由（`lba_2_*` / `l2p_global_*` / `ftl_op_route_get_region`）、容量（`FTL_LBA_MAX` / `FTL_IU_TOTAL`）、介质布局版本（`ftl_media_layout_version()`）、die 槽位（controller 只校验 present die）全部按运行时 `DIE_MAX`；数组按静态上限 `FTL_DIE_MAX`。
- **影响**：多 target/多 LUN 未在真实硬件验证。
- **下一步 / 判据**：上板：单 die 介质不触发重格式化（版本仍 BASE）；H2testw 读写/坏块/吞吐；多 target 需扩展板级 `nand_select_target` 通路表；多 LUN 的逐 LUN 78h probe 需换多 LUN 器件再验。
- **来源**：`RISK-TOP-01`；`include/nand/nand_config.h` / `include/ftl/ftl_config.h` / `include/ftl/l2p_cache.h` / `include/ftl/metadata.h` / `src/ftl/ftl.c` / `src/main.c` / `src/hil/controller.c`
- **关联**：`RISK-TOP-01`、`ISS-ML-01`
- **历史/验证**：
  - 2026-11-03 Phase 1/2a/2b 落地 → 编译通过，未上板

#### ISS-LOG-01 `fault_capture.md` §10.5 落地状态

- **当前事实**：`[STAT]` / `[MIG] done` 已在 2026-10-22 日志出现，其余（飞行记录器、打印预算逐项）未逐条确认。
- **影响**：不能声称 logging 设计已全部落地。
- **下一步 / 判据**：按 `docs/design/fault_capture.md` §10.6 判据逐项打勾；未验的不要写成已验。
- **来源**：`docs/design/fault_capture.md` §10.5/§10.6
- **关联**：`RISK-LOG-*`
- **历史/验证**：
  - 2026-10-22 `[STAT]` / `[MIG] done` 出现 → 其余未确认

#### ISS-FAULT-01 `fault_capture.md` 三项手工验证

- **当前事实**：人工 BusFault、断言失败 dump、跨复位读取 `.noinit` 记录三项未验证。
- **影响**：故障取证链路没有板端证据。
- **下一步 / 判据**：烧录→拔电→上电确认无记录；再逐个注入。
- **来源**：`docs/design/fault_capture.md` §6/§9
- **关联**：`RISK-PLAT-*`
- **历史/验证**：
  - （无板端记录）

#### ISS-FAULT-02 PC 采样器

- **当前事实**：SysTick naked 蹦床取被中断上下文的 `frame[6]`（PC），写入 `.noinit` 256 项活跃环；`pc_sample_init()` 在启动最早把上一轮活跃环按时间顺序重排后快照到 prev。CLI：`pc` 默认打上一轮原始序列（oldest→newest，4 个/行，最近 64 个；一行行重复即循环）、`pc all` 打全部 256、`pc top` 打命中次数排行（含 share）、`pc live` 看本轮、`pc clear` 归零、`pc rate <n>` 设每 n 个 tick 采一次（窗口 256×n ms，持久化到 `.noinit`，复位后沿用）。边界：原始序列是采样顺序不是指令顺序；紧凑循环（< rate ms 且整除）仍混叠，调大 rate 只会更差；中断关闭 / 更高优先级 ISR / SysTick 自身死循环采不到；`.noinit` 只跨软复位/看门狗复位，不跨 POR。
- **影响**：死循环/跑飞定位能力未上板验证。
- **下一步 / 判据**：① 人为 `while(1)` → NRST → `pc` 原始序列命中该函数；`pc top`/`pc all` 一致；② `pc rate 4` 后窗口/span 与 run 行为符合预期；③ 回归 HAL tick/系统时间/串口（naked SysTick 零回归）；④ 掉电后 `pc` 无快照。反查必须用同一镜像的 `debug.map`/`debug.elf`（`CLAUDE.md` §3.3）。
- **来源**：`src/common/pc_sample.c` / `include/common/pc_sample.h`；`src/platform/stm32h743/sys_port.c` `SysTick_Handler` / `sys_tick_handler_c`；`src/main.c` `pc_sample_init` / boot hint；`src/hil/cli.c` `uart_cli_pc`；`src/platform/fault.c` / `fault_port.c`（异常现场 + 96 词栈扫描，与本模块互补）
- **关联**：`RISK-PLAT-*`
- **历史/验证**：
  - 2026-11-05 落地 → 编译通过，未上板

### 已闭合

| ID | 日期 | 一句话 | 归档 |
|---|---|---|---|
| ISS-PLAT-02 | 2026-11-07 | 删除 stm32h7r3 平台，只保留 stm32h743 | `docs/archive/known_iss_batches.md` |

---

## 7. 架构 / 契约残余

### 索引

| ID | 状态 | 一句话 | 最新验证 |
|---|---|---|---|
| ISS-ARCH-01 | 部分实现 | 模块 handle 契约迁移正常路径已上板；掉电/格式化中断/注入未单独验 | 2026-10-25 上板通过（残余） |
| ISS-ARCH-02 | 待决策 | 契约正文与已落地签名偏离 | 未上板 |
| ISS-CW-01 | 待决策 | `LUN_ADDR_WIDTH=0` 结论是否仍成立 | 未上板 |
| ISS-CACHE-01 | 部分实现 | cache read/program 依赖 SQ 连续入队；跨轮次/失败页未验 | 2026-10-30 常规回归通过（残余） |
| ISS-FC-01 | 部分实现 | `fc_desc.config` 无读者，错误恢复上下文未接线 | 未上板 |

### 详情

#### ISS-ARCH-01 模块 handle 契约迁移

- **当前事实**：Phase 0~8 已上板；正常读写/GC/CP/journal/BBT 已由 `/ftl/` 覆盖与 H2testw 415 MB 覆盖；掉电/格式化中断/故障注入仍未单独验（见 `RISK-BOOT-22`/`RISK-BBT-18` 等）。
- **影响**：契约迁移的异常路径没有独立回归。
- **下一步 / 判据**：剩余只跟随 `RISK-BOOT-22`/`RISK-BBT-18` 等专项条目推进，不再单列契约回归。
- **来源**：`docs/archive/ftl_module_contracts.md` §12；`RISK-ARCH-06`（已闭合，archive §2026-10-25）
- **关联**：`RISK-BOOT-22`、`RISK-BBT-18`、`RISK-ARCH-06`
- **历史/验证**：
  - 2026-10-25 上板 → 正常路径通过；掉电/格式化中断/注入未单独验

#### ISS-ARCH-02 契约正文与已落地签名偏离

- **当前事实**：契约正文（目标设计）与已落地签名存在偏离（`jrn_handle` trigger、`wbuf_handle` 返回码、`pending_confirm_*` 入参）。
- **影响**：读者按文档写会调错；签名与契约谁权威未定。
- **下一步 / 判据**：定「权威契约以哪份为准」；定稿前不要按文档强行改签名。
- **来源**：`docs/archive/ftl_module_contracts.md` §12；`RISK-ARCH-10`
- **关联**：`RISK-ARCH-10`
- **历史/验证**：
  - （无板端记录）

#### ISS-CW-01 `LUN_ADDR_WIDTH=0` 结论

- **当前事实**：`LUN_ADDR_WIDTH=0` 与多维地址构造的根因结论是否仍成立未裁决；cw 文档 §3.7 与 v0.73/v0.90 多维改动需对齐。
- **影响**：地址编码/多维布局可能仍有第二套坐标（C9）。
- **下一步 / 判据**：与 `ISS-ML-03` 一起裁决。
- **来源**：`docs/archive/cw_level_mapping_design.md` §3.7
- **关联**：`ISS-ML-03`、`RISK-ARCH-*`
- **历史/验证**：
  - （无板端记录）

#### ISS-CACHE-01 cache read/program 序列依赖 SQ 连续入队

- **当前事实**：cache read/program 序列依赖「全部 FTL 命令走 SQ 且连续入队」；若 RR 等改走 HSQ，`fc_poll_fill` 先扫 HSQ，命令会插入 31h/3Fh（或 15h/10h）序列，破坏器件 cache 管线。FC 已有 `config.chain` + `chain_owner` 链锁可解。方案 A 已落地：cache read/program、GC/WL batch 都标成一条 FC chain（末条 `FC_CHAIN_END`，其余 `FC_CHAIN_MORE`），经 `ftl_op_submit_batch` 原子入队；FC 零功能改动。
- **影响**：跨轮次/失败页未验；chain 写路径不处理 RAID stripe encode（`RISK-IF-06`）。
- **下一步 / 判据**：① 上板跑 FC UT `test_chain_priority_lock`（8 条跨轮次链 + HSQ 回推）；② host 顺序读（4 条链上 45h/46h/47h 不交错，H2testw 校验通过）；③ GC batch `mb>0`。
- **来源**：`docs/design/ftl_design.md` §3.9.5 / ADR-17；`src/fc/internal/fc_internal.h` 的 `fc_sched_s`；`src/fc/fc.c` 的 `fc_poll_fill_queue` / `fc_poll_chain_length`；`src/ftl/cache_batch.c`；`src/ftl/migrate.c`
- **关联**：`RISK-IF-06`、`ISS-ARCH-01`
- **历史/验证**：
  - 2026-10-30 host 4 页链 H2testw 415/423 MB → 无错；跨轮次/失败页未验

#### ISS-FC-01 `fc_desc_s.config` 错误恢复上下文

- **当前事实**：`fc_desc_s.config` 保留为命令泛化/错误恢复上下文；当前无读者。只保留 config 并不能单独恢复整条命令（route/paa/meta/param/cmd_id 仍在 `fc_cmd_s`）。
- **影响**：错误恢复时上下文可能不完整；删掉又会丢未来能力。
- **下一步 / 判据**：先定「错误恢复时 `fc_cmd_s` 是否仍存活」：存活则登记为校验副本；不存活则补齐完整恢复上下文。无消费者前不得删除或改名（H5/R11）。
- **来源**：`include/fc/fc_command.h`；`src/fc/fc_pool.c` 建链写入
- **关联**：`ISS-FTL-11`
- **历史/验证**：
  - （无板端记录）

---

## 8. 控制器观测 / 调试命令 backlog（2026-11-05 评估）

> E0 = `q/cp/jrn/stat` 四条只读命令已实现、未上板，其上板核对登记在
> `docs/design/known_risks.md` `RISK-CLI-06`，不在此重复。
> 本节只登记尚未实现的 P1/P2 观测项与评估结论。

### 索引

| ID | 状态 | 一句话 | 最新验证 |
|---|---|---|---|
| ISS-OBS-01 | 部分实现 | ECC 观测 E1 已落地；E2/E3 未实现 | 2026-11-05 编译通过 |
| ISS-OBS-02 | 待上板 | `wear` 磨损/read-disturb 视图已实现未上板 | 2026-11-05 编译通过 |
| ISS-OBS-03 | 待上板 | `id` 配置/身份视图已实现未上板 | 2026-11-05 编译通过 |
| ISS-OBS-04 | 待决策 | `errlog` RAM 错误事件环 | 未上板 |
| ISS-OBS-05 | 待决策 | 控制类命令 `gc now/stop`、`reset`、`scan` | 未上板 |

### 详情

#### ISS-OBS-01 ECC 观测分三级

- **当前事实**：E1 纯 CLI（已落地 `ecc`）：扫 `bst_block_s.rr`（`last_ecc/rising_count/health`）+ `read_count`，出 last_ecc 直方图 / health 计数 / top-N / `read_blocks` 覆盖率；零新增状态，数据已由 host 读（`ftl_host_io_read_ok` → `rr_record_ecc`）与 GC 读（`migrate.c` → `fc_get_cmd_ecc_max`）维护。E2 FTL 级累计（未做）：`ftl_stats_s` 加读次数、纠正 bit 总和/最大、UNC/CRC/MATA/RAID 事件计数；增量点是 `ftl_host_io_read_ok` 与读失败分支，不需要 FC 改动，POR 清零。E3 FC 级 per-CW（未做）：`fc_ecc_stats_t` + 完成路径增量 + `fc_get_ecc_stats()`，覆盖 CP/journal/BBT 同步读的全 NAND 流量口径。不统计 read retry：`read_recovery` 的 read-retry feature 地址 0x89 在本器件是保留值、自始空转（用户明确不处理，见 `CLAUDE.md` §四）。
- **影响**：E2/E3 未做，无法从 FTL/FC 累计口径看 ECC 趋势。
- **下一步 / 判据**：E1 上板核对并入 `RISK-CLI-06`；E2 等板端需要累计值再排；E3 不做。判据：E1 的 health 计数与 `rr_predictive_scan` 触发一致；E2 的 UNC/CRC 事件数与 `[FTL] ... cmd failed` 日志条数一致。
- **来源**：`src/hil/cli.c` `uart_cli_ecc`；`src/ftl/ftl.c` `ftl_host_io_read_ok` / `ftl_host_io_callback`；`src/ftl/migrate.c` `mig_read` 完成；`include/ftl/bst.h` `rr_ecc_track_s`；`src/ftl/read_recovery.c` `rr_record_ecc` / `rr_get_health`；`include/fc/fc_interface.h` `fc_get_cmd_ecc_max`
- **关联**：`RISK-CLI-06`、`RISK-RD-01`
- **历史/验证**：
  - 2026-11-05 E1 落地 → 编译通过，未上板；E2/E3 未实现

#### ISS-OBS-02 `wear` 磨损 / read-disturb 视图

- **当前事实**：扫 `erase_count` / `read_count` / `rr.{last_ecc,health}`，出 EC 直方图 + min/max/avg + top-N 冷热块 + `read_count` 分布 + WL 触发差值。纯 CLI，O(`FTL_BLOCK_TOTAL`)/die。WL 判据同一漏斗：新增 `wl_ec_spread_get()`（取 `max_free_ec` / `min_used_ec`），`wl_should_trigger` 改为调用它；CLI 不重算判据（C9）。`writes < WL_MIN_WRITES(1000)` 时启动门未过，`valid=0` 是预期。
- **影响**：未上板核对，视图与真实 WL 判据可能不一致。
- **下一步 / 判据**：上板核对并入 `RISK-CLI-06`；判据：静态 WL 触发前后 `max_free−min_used` 与 `[WL ]` 日志一致。
- **来源**：`src/hil/cli.c` `uart_cli_wear`；`include/ftl/wear_leveling.h` `wl_ec_spread_get`；`src/ftl/wear_leveling.c`；`include/ftl/bst.h`；`include/ftl/allocator.h` `alc_free_get_max_erase`
- **关联**：`RISK-CLI-06`、`RISK-WL-04`
- **历史/验证**：
  - 2026-11-05 落地 → 编译通过，未上板

#### ISS-OBS-03 `id` 配置 / 身份视图

- **当前事实**：编译期几何（CH/CE/LUN/PLANE/BLOCKS/PAGE/CW/IU）、容量（LBA / IU 总数 / 主机 MiB）、FTL 配置（pair / 块总数 / group / op / spare / CMT）、介质布局版本、timing mode、控制器状态。纯 CLI、无 NAND 访问。ONFI 参数页按计划不做（boot 已打印，`id --onfi` 需 idle 重读，未实现）。
- **影响**：未上板核对，视图可能与 boot 横幅/`info` 不一致。
- **下一步 / 判据**：上板核对与 boot 横幅 / `info` 一致（并入 `RISK-CLI-06`）。
- **来源**：`src/hil/cli.c` `uart_cli_id`；`include/ftl/ftl_config.h`；`ftl_media_layout_version()`；`nand_get_timing_mode()`
- **关联**：`RISK-CLI-06`
- **历史/验证**：
  - 2026-11-05 落地 → 编译通过，未上板

#### ISS-OBS-04 `errlog` RAM 错误事件环

- **当前事实**：`{t, layer, op, lba, pba, status}` × 32 ≈ 512 B；单漏斗 `errlog_record()`，挂 FTL 完成/失败路径；POR 丢失，**不是** NVMe Error Log 的实现（本仓自拟）。多层级直接写环会变成「不变量靠外部模块维护」（C3/H10），必须一个 API + 主循环/完成上下文（H21）。
- **影响**：间歇性失败没有低开销事件环可查。
- **下一步 / 判据**：等板端出现间歇性失败再排；先定记录点清单与是否持久化。
- **来源**：`RISK-USBC-01`；`src/ftl/ftl.c` `ftl_host_io_callback`；`src/ftl/power_failure_recovery.c`
- **关联**：`RISK-USBC-01`
- **历史/验证**：
  - （无板端记录）

#### ISS-OBS-05 控制类命令

- **当前事实**：`gc now/stop`、`reset`、`scan` 未实现。风险：与 `bg_scheduler` / `mig_handle` 单飞行冲突、host 在飞、长操作喂狗；`reset` 需「无在飞 IO」门。`scan` 可先用 `test` 的速度/读扫描替代。
- **影响**：板端排障需要控制类命令时没有安全入口。
- **下一步 / 判据**：等板端排障需要再排；任何控制命令都 debug-only + host-idle 门。
- **来源**：`src/ftl/gc.c` / `src/ftl/bg_scheduler.c`；`RISK-CLI-01`
- **关联**：`RISK-CLI-01`
- **历史/验证**：
  - （无板端记录）
