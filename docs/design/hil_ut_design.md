# HIL 单元测试设计（板端）

> 状态：正常路径已上板（2026-11-13 13:21:29，15/15，0 skip）；剩余缺口见 §6。
> 2026-11-14 拆 `/hil/contract/`（9）+ `/hil/io/`（7），**拆分后未上板**。
> 范围：`src/hil/`（HIL 层）。FTL UT 见 `docs/design/ftl_ut_design.md`；
> FC-NAND UT 见 `docs/design/fc_nand_ut.md`。
> 关联：`docs/design/known_iss.md` ISS-HIL-01；`docs/design/known_risks.md`
> RISK-UT-34 / RISK-CTRL-01/02/04；`docs/design/usb_msc.md` Part B。
> 分层坐标：`docs/design/testing_guide.md` §3 的 **L3 HIL**。

---

## 0. 单一事实源

本文只维护「HIL UT 怎么设计、用例契约是什么」。下面这些信息**不在本文复制**：

| 信息 | 唯一事实源 | 本文怎么引用 |
|---|---|---|
| 用例是否存在、参数是什么 | 本文 §4 + `tests/unit/test_hil.c` | §4 只写契约与覆盖点 |
| 风险状态、历史、是否闭合 | `docs/design/known_risks.md` | §5 只写 `RISK-ID → 用例` 映射，**不写状态词** |
| 未实现 / 未验证的设计项 | `docs/design/known_iss.md` | §8 只给链接 |
| 每次运行结果与证据 | `build/test-results/` + `docs/design/testing_guide.md` §8.7 | §10 只留最近快照 |
| 分层与跨层测试流程 | `docs/design/testing_guide.md` | 本文沿用其 L0~L6 坐标 |

> HIL 是**主机接口层**，不是 FTL 的一部分：本文不重复 FTL 的 L2P/GC/CP 契约；
> FTL 行为由 `ftl_ut_design.md` 覆盖，HIL 只钉「请求解析、路由、拆分、回执」这一层。

---

## 1. 目的与原则

1. **只在目标板跑**（`CLAUDE.md` R4）：`make tests UT=HIL` 生成镜像，串口 CLI `test`
   执行；不做 host 模拟。
2. **真 FTL / 真 NAND，无 mock**：直接复用主 boot 建好的 `g_ctrl` 与
   `g_ctrl.ftl[0]`，不建 FTL 桩、不绕过 controller。
3. **不 format**：读写/FLUSH/TRIM 固定用容量尾部 8-LBA 窗口，避免破坏文件系统常用区。
4. **不碰 USB host**：USB 槽状态机由 PC 主机侧矩阵覆盖（`usb_msc.md` Part B），
   不放进 `/hil/`。
5. **一个用例只钉一个契约**：判据必须能回答「断言比较的是哪两个值」；没有区分力的
   断言不算覆盖（C7 / C8）。
6. **负例必须能红**：非法 opcode、越界 LBA、单飞行冲突等必须有明确拒绝判据。
7. **状态如实**：15/15 是正常路径回归，不等于 HIL 契约闭环；剩余缺口见 §6（R1）。

---

## 2. 测试架构

### 2.1 层与套件

| 层 | 根套件 | 环境 | 内容 | 破坏性 |
|---|---|---|---|---|
| **HIL** | `/hil/`（`contract/` + `io/`） | 真 FTL / NAND | controller 状态机 / init / LBA 范围 / submit 边界 / host queue / 真实 I/O | 是（尾窗写） |

```text
/hil/
  contract/   9 条：状态机 / init / host_queue / lba_range / submit / is_complete
  io/         7 条：write_read_roundtrip / write_flush_readback / flush_* / trim_*
```

执行入口：`test /hil/`（展开到 `contract/` 与 `io/`）；也可定向
`test /hil/contract/` 或 `test /hil/io/`。两个子套件共用同一个主 boot 实例；
共享状态由 teardown 的 `hil_quiesce()` 排空，不靠用例顺序。

### 2.2 测试技术矩阵

| 技术 | 判据 | 本仓落点 |
|---|---|---|
| **Directed / contract** | 一个状态机/接口契约一条用例 | `state_machine`、`write_read_roundtrip` |
| **Negative** | 非法状态/参数必须被拒绝且置 `complete_flag` | `init_missing_slot`、`init_topo_mismatch`、`submit_edges`、`flush_busy`、`trim_busy` |
| **Boundary** | LBA 半开区间、count=0/max、越界 | `lba_range_ok`、`submit_edges` |
| **Seeded random** | 记录 seed；失败可复现 | HIL UT 当前不随机 |
| **Fault injection** | 注入点 → 预期降级/恢复 | HIL UT 不注入（无 HIL 专属注入 seam） |
| **Reboot / crash point** | 模拟重启或真 POR 后映射/账本一致 | **未覆盖**，见 §6 |
| **Stress / perf** | 只作基准，不作功能通过率 | PC 主机矩阵 / `/bench/speed/`，不在 `/hil/` |

> HIL UT 的强项是**同步契约**：状态机、init、范围、submit、单飞行、完成回执；
> 弱项是**持久化/并发/主机侧时序**——这些必须由 §6 的专项或 PC 矩阵补，不能靠 15/15 推定。

### 2.3 覆盖模型（CP-*）

| 覆盖点 | 要覆盖什么 | 典型用例 |
|---|---|---|
| `CP-CTRL` | controller 状态机、init 槽位/拓扑、host tag/owner/used | `state_machine`、`init_*`、`host_queue` |
| `CP-SUBMIT` | LBA 范围、buffer 容量、非法 opcode、NULL 入参、`complete_flag` | `lba_range_ok`、`submit_not_ready`、`submit_edges` |
| `CP-IO` | 经 controller 的真实 READ/WRITE/TRIM 闭环 | `write_read_roundtrip`、`trim_job` |
| `CP-DUR` | FLUSH 完成与写后读一致 | `write_flush_readback`、`flush_job` |
| `CP-CONC` | FLUSH/TRIM 单飞行背压 | `flush_busy`、`trim_busy` |
| `CP-LEDGER` | teardown 后 FC 池/FTL job/请求表归零 | `hil_quiesce` 的 INV-* |
| `CP-ERR` | 错误码与 `complete_flag` 一致，不得悬挂 | `submit_edges`、`flush_busy`、`trim_busy` |

> `CP-*` 是**本仓自拟**的功能覆盖点，不是代码覆盖率；裸机目标上不以行覆盖作为主指标。

---

## 3. 夹具与隔离

### 3.1 活实例

- `g_ctrl` 是主 boot 建好的 controller；`g_ctrl.ftl[0]` 是唯一 FTL 实例。
- `hil_setup` 断言 `g_ctrl.state == READY` 且 `g_ctrl.ftl[0] != NULL`；不复制
  `ftl_ctx_s`（大型静态对象）。
- 所有 I/O 走 `ssd_controller_submit` → `ssd_request_split` → FTL → FC/NAND 的真实路径。

### 3.2 数据缓冲与尾窗

| 对象 | 尺寸 | 用法 |
|---|---|---|
| `s_hil_wr` / `s_hil_rd` | 4 LBA × 512 B | 写读闭环 |
| `s_hil_zero` | 4 LBA × 512 B | TRIM 后读零比对 |
| `s_hil_edge_buf` | 4 LBA × 512 B | 非法参数用例（buffer 不足等） |

- `hil_lba_base() = (host_lba_max - 8) & ~(8-1)`；`HIL_IO_LBAS=4`、`HIL_LBA_WINDOW=8`。
- 只用容量尾部 8 LBA，重复运行只覆盖同一窗口；**不 format**。

### 3.3 时钟

- HIL UT 使用真实平台时钟；不安装伪时钟。
- `hil_wait` / `hil_quiesce` 用 `platform_now_ms()` 做 5 s 超时。

### 3.4 结束不变量（INV-*）

每个用例 teardown 调用 `hil_quiesce()`，至少检查：

- `INV-FC`：FC 池 `sq/hsq/cq/command/descriptor/buffer` 全 0；
- `INV-REQ`：`ftl_op_req_count() == 0`、`flush_req == NULL`、`trim_req == NULL`；
- `INV-FTL`：CP / migrate / metadata / journal / wbuf / cache 序列 / cache inflight 全 idle；
- `INV-BG`：恢复 `bg_suspend` 原值（断言失败也不能污染后续用例）。

> `INV-*` 是**本仓自拟**的不变量编号；用于把「结束条件」写成可引用的契约。

### 3.5 真 NAND 安全模型

- 破坏性：会写容量尾部 8-LBA 窗口；不 format、不恢复 host 数据。
- 运行前必须拔掉 USB，并确认没有 host 在飞请求；`test` 命令**不**替用例检查
  `cli_host_busy()` / `usb_msc_host_busy()`，由操作者保证。
- 跑完不要求 `format`（尾窗不是文件系统区），但正式交付前仍按 `testing_guide.md` 收尾。

### 3.6 可测性 seam 清单

| seam | 用途 | 产品固件是否编入 |
|---|---|---|
| `ssd_controller_*` 公共 API | 状态机 / init / submit / complete | 是 |
| `fc_get_resource_stats` | teardown FC 池账本 | 是 |
| `ftl_service_pump` / `ftl_service` / `ssd_controller_service` | 主循环泵 | 是 |
| `timer_scheduler_poll` | 后台 credit / CP / Journal | 是 |
| `g_ctrl` | 主 boot 实例 | 是 |

> HIL UT **不新增 seam、不 mock、不注入**；能测的必须是生产路径可直接驱动的部分。

### 3.7 跨重启夹具的边界

- `ftl_ut_common.c`（含 `ftl_ut_reboot_from_media`）只在 `UT_FTL_ENABLE=1`
  （`UT=` 为空或 `UT=FTL`）编入；`UT=HIL` 下**没有**该夹具。
- 因此跨重启用例不能在当前 `UT=HIL` 构建里直接写；可选：
  1. 把 `ftl_ut_common.c` 链入 `UT=HIL`（需确认它不拉入 FTL 套件依赖）；
  2. 走 `UT=` 全量构建，并在 `UT_FTL_ENABLE` 条件下编译该用例；
  3. 写 HIL 本地的最小 reboot helper（quiesce → fc_init → ftl_init → pfr_boot_recovery）。
- 当前选择：**留以后**，见 §6 与 `RISK-UT-34`。

---

## 4. 用例清单

> 首列是 `test --list` 里的用例名；对应测试函数名为 `test_hil_<name>`。
> 本表只写**契约 / 覆盖点 / RISK**；具体输入、断言值、负例写在测试源注释里。

| 用例 | 参数 | 契约 / 覆盖点 | RISK |
|---|---|---|---|
| `state_machine` | — | RESET→INITIALIZING→READY→FAILED；非法转换被忽略；`state_transition(NULL,…)` 不崩；`CP-CTRL` | `RISK-CTRL-03` |
| `init_ready` | — | 合法槽表 → READY；`host_lba_max` 初值 = 编译期 `FTL_LBA_MAX`；`CP-CTRL` | — |
| `init_missing_slot` | — | `route[0]=NULL` → `INVALID_ARGUMENT` 且 `state=FAILED`；`CP-CTRL` | `RISK-CTRL-03` |
| `init_topo_mismatch` | — | `slot.topo.ch=0xFF` → `INVALID_ARGUMENT` 且 `state=FAILED`；`CP-CTRL` | `RISK-CTRL-03` |
| `host_queue` | — | 填满 8 entry（tag=下标）→ alloc 返回 NULL；释放 tag0 后可再分配；释放后 entry 归零；`CP-CTRL` / `CP-LEDGER` | `RISK-HOST-20` |
| `lba_range_ok` | — | count=0、lba=max、`[max-1,2)` 拒绝；`[0,1)`、`[max-1,1)` 接受；`CP-SUBMIT` | `RISK-CTRL-02` |
| `submit_not_ready` | — | RESET/FAILED、controller=NULL、request=NULL → `INVALID_ARGUMENT`；`CP-SUBMIT` | — |
| `submit_edges` | — | buffer 不足、count=0、越界 LBA → `INVALID_ARGUMENT`；未知 opcode → `NOT_SUPPORTED`；均置 `complete_flag=1`；`CP-SUBMIT` / `CP-ERR` | `RISK-CTRL-02` / `RISK-CTRL-04`（buffer NULL 未覆盖） |
| `is_complete` | — | request=NULL → 1；flag 0/1 直读；`CP-CTRL` | `RISK-UT-34` |
| `write_read_roundtrip` | — | 尾窗 4 LBA 写 → 不 flush 立即读回逐字节一致；`sub_remaining` 归零；`CP-IO` | — |
| `write_flush_readback` | — | 尾窗 4 LBA 写 → FLUSH 完成并清 `flush_req` → 读回一致；`CP-DUR` | `RISK-UT-34` |
| `flush_job` | — | submit FLUSH → 泵到 `complete_flag=1` → `status=OK`、`flush_req` 清空；`CP-CTRL` / `CP-DUR` | — |
| `flush_busy` | — | 第一条在飞时第二条 → `BUSY`、`complete_flag=1`；第一条最终 OK；`CP-CONC` | — |
| `trim_job` | — | 写 4 LBA → TRIM → `trim_req` 清空，回读全零；`CP-IO` | — |
| `trim_busy` | — | 第一条 TRIM 在飞时第二条 → `BUSY`、`complete_flag=1`；第一条最终 OK；`CP-CONC` | — |

---

## 5. 风险追溯

> 只写「哪个 RISK 由哪些用例/人工流程覆盖」。**状态与历史见
> `docs/design/known_risks.md`**；未列出的 RISK 可能仍是 BOARD/PC 矩阵，本表不声称完整。

| RISK | 处置 | 用例 / 流程 |
|---|---|---|
| `RISK-UT-34` | HIL UT | 15 例正常路径；剩余缺口见 §6 |
| `RISK-CTRL-01` | 多 die / 注入 | 当前单 die 不可达；多 run 中途失败需多 die 或 `ftl_submit` 注入 |
| `RISK-CTRL-02` | `/hil/` | `lba_range_ok`、`submit_edges` |
| `RISK-CTRL-03` | `/hil/` | `state_machine`、`init_missing_slot`、`init_topo_mismatch` |
| `RISK-CTRL-04` | `/hil/` + controller 修复 | `submit_edges`（buffer NULL 尚未覆盖） |
| `RISK-HOST-20` | `/hil/` 部分 | `host_queue`；完整互斥闸门仍待收口 |
| `RISK-HOST-21` | 未覆盖 | host 命令超时 / 中止；见 §6 |
| `RISK-USB-09..13` | PC 主机矩阵 | `usb_msc.md` Part B；不在 `/hil/` |

---

## 6. 覆盖缺口与可测性

> 本节回答「剩余缺口现在能不能测」。结论是**分三类**，不要把它们混成「以后补就行」。

| 缺口 | 现在能测? | 前置 / 判据 | 关联 |
|---|---|---|---|
| **buffer NULL** | **能加用例，但现在会红** | `ssd_controller_submit` 只查 `buffer_size`，不查 `buffer`；NULL 会进 `ssd_request_split` 的指针算术，靠 FTL 兜底但状态分类错误 | `RISK-CTRL-04` |
| **TRIM 越界** | **能直接加，预期通过** | TRIM 分支已调 `ssd_controller_lba_range_ok`；判据 = `INVALID_ARGUMENT` + `complete_flag=1` + `trim_req==NULL` | `RISK-CTRL-02` / `RISK-UT-34` |
| **跨重启** | **`UT=HIL` 下不能直接测** | `ftl_ut_common.c` 只在 `UT_FTL_ENABLE=1` 编入；需链入 HIL 夹具或走 `UT=` 全量构建（§3.7） | `RISK-UT-34` |
| **多 run / 跨实例部分失败** | **当前不能** | 1 CH / 1 CE / 1 LUN，`DIE_MAX=1`，`ssd_controller_route` 恒返回 `ftl[0]`；需多 die / N=2 影子构建 / `ftl_submit` 注入 | `RISK-CTRL-01` |
| **USB 槽状态机** | **不能放在固件 UT** | 需要 PC 主机侧矩阵（flush 槽 / read BUSY / 0x91 / 相同写匹配 / 总线复位） | `RISK-USB-09..13` |

**2026-11-13 用户裁决**：以上剩余缺口**留以后检验**，本轮不补用例；重访触发 =
下次 HIL 收口或 controller/USB 改动。buffer NULL 的设计问题单独登记 `RISK-CTRL-04`。

---

## 7. 运行与证据

```text
make tests UT=HIL
test --list
test /hil/
```

- 产物：`build/firmware/debug/bin/debug_ut.elf`；`UT=HIL` 与 `UT=FC/FTL` 共用该文件，
  烧写前确认最后一次构建是 `UT=HIL`。
- 串口横幅时间戳必须与构建时间一致（`__DATE__ __TIME__`）。
- 默认输出：用例名 + `[ OK ]/[ SKIP ]` + 耗时；`--log-visible info/debug` 打开 `[EV]` 证据。
- 原始日志与 `meta.txt` 放 `build/test-results/<时间>_<build>_<target>/`。

---

## 8. 不覆盖项

- 真 POR / 掉电：人工流程；HIL UT 只复用主 boot 实例，不做 reboot。
- 多 die / 跨实例 / 多 run 中途失败：见 §6。
- USB 槽状态机、主机 fsync/TRIM 时序：PC 主机矩阵（`usb_msc.md` Part B）。
- 长时间 soak、性能：`/bench/speed/` 与主机工具，不是 `/hil/`。
- FTL 内部 L2P/GC/CP/Journal 契约：`ftl_ut_design.md`；HIL 只钉接口层。

> 未实现 / 未验证项统一登记 `docs/design/known_iss.md`；本文不复制状态。

---

## 9. 维护规则

- **新增/删除用例**：同批更新 §4 对应表 + 测试源注释；不得只改源或只改文档。
- **风险状态变化**：只改 `docs/design/known_risks.md`；§5 仅在映射变化时改。
- **可测性变化**（夹具/硬件/主机矩阵）：更新 §6；不要把「能测」写成「已测」。
- **运行结果**：写 `build/test-results/` 与 `known_risks.md` 历史；§10 只留最近快照。
- **禁止**：§5 写状态词；§4 复制实现细节；文档头写「N 函数 / M 次运行」快照。
- **用例注释模板**：

```c
/* <name> — <一句话契约>
 *
 * 覆盖：<状态/边界/错误/账本/持久化/并发>。
 * 判据：<断言比较哪两个值；负例是什么；失败后果>。
 * 备注：<真 NAND / 尾窗 / 依赖顺序>。
 */
```

- **HIL 用例**：不 format、不碰 USB host、只用容量尾部 8-LBA 窗口；teardown 必须
  `hil_quiesce()` 并恢复 `bg_suspend`。
- **测试源按 R4 由用户维护**；新增 seam / mock 前先说明为什么生产路径不可直接驱动。

---

## 10. 最近验收快照

> 只作时间点快照；状态以 `docs/design/known_risks.md` / `docs/design/known_iss.md` 为准。
> 构建门禁不在此表：`make all` / `make release` / `make tests UT=HIL` 按 R2 每次改动执行。

| 日期 | 镜像横幅 | 命令 | 结果 | 证据 |
|---|---|---|---|---|
| 2026-11-13 | 13:21:29 | `test /hil/` | 15/15，0 skip，10.56 ms | `docs/design/known_iss.md` ISS-HIL-01 |
