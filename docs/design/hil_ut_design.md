# HIL 单元测试设计（板端）

> 状态：活跃设计文档。本文是 **test design**，按被测模块（test item）组织：
> 每个模块有哪些设计要点、用什么判据覆盖、缺什么。
> 用例名只作代表，完整清单以 `tests/unit/test_hil.c` 的注册表为准；
> 运行证据见 `build/test-results/`，风险状态见 `docs/design/known_risks.md`。
> 范围：`src/hil/`（HIL 层）。架构设计见 `docs/design/hil_design.md`（本文只写测试，不复制设计细节）；
> FTL UT 见 `docs/design/ftl_ut_design.md`；FC-NAND UT 见 `docs/design/fc_nand_ut.md`。
> 关联：`docs/design/known_iss.md` ISS-HIL-01/02/03；
> `docs/design/known_risks.md` RISK-UT-34 / RISK-CTRL-01 / RISK-CTRL-03 / RISK-CTRL-04 /
> RISK-HOST-20 / RISK-HOST-21；
> `docs/design/usb_msc.md` Part B（PC 主机矩阵）。
> 分层坐标：`docs/design/testing_guide.md` §3 的 **L3 主机接口**（HIL）。

---

## 0. 文档定位与单一事实源

本文回答三个问题：

1. HIL 各模块的设计要点（状态机、路由、拆分、单飞行、回执、账本）是什么；
2. 这些要点靠哪些测试层、什么判据覆盖；
3. 哪些点当前没有覆盖，原因是什么。

下面这些信息**不在本文复制**：

| 信息 | 唯一事实源 | 本文怎么引用 |
|---|---|---|
| 用例是否存在、参数、断言 | `tests/unit/test_hil.c` | §2 只写代表用例与判据，不列全量清单 |
| 风险状态、历史、是否闭合 | `docs/design/known_risks.md` | §7 只写 `RISK-ID → 测试项 / 用例` 映射，**不写状态词** |
| 负例覆盖判定（M/N/K） | 本文 §7.2 台账 | §6 / §7 只给 HIL 层判定 |
| 未实现 / 未验证的设计项 | `docs/design/known_iss.md` | §6 只给链接 |
| 每次运行结果与证据 | `build/test-results/` + `docs/design/testing_guide.md` §8.7 | §5.4 只给目录与四件套 |
| 分层与跨层测试流程 | `docs/design/testing_guide.md` | 本文沿用其 L0~L6 坐标 |
| USB 主机侧矩阵 | `docs/design/usb_msc.md` Part B | §2.5 / §6 只给入口 |

> HIL 是**主机接口层**，不是 FTL 的一部分：本文不重复 FTL 的 L2P/GC/CP 契约；
> FTL 行为由 `ftl_ut_design.md` 覆盖，HIL 只钉「请求解析、路由、拆分、回执」这一层。

---

## 1. 测试项与测试分层

### 1.1 测试项清单

顺序按模块依赖：controller → request 拆分 → host queue → 真实 I/O；前端模块不在固件 UT。

| 测试项 | 源文件 | 主要接口 / 状态 | 测试层 |
|---|---|---|---|
| `ssd_controller` | `src/hil/controller.c`、`include/hil/controller.h` | `ssd_controller_state_transition`、`init`、`lba_range_ok`、`submit`、`is_complete`、`service`、`cancel_flush`、`host_lba_max` | contract / io |
| `ssd_request` | `src/hil/request.c`、`include/hil/ssd_request.h` | `ssd_controller_run_end`、`ssd_request_split`、`on_sub_complete` | contract / io（间接） |
| host_queue | `src/hil/host_queue.c`、`include/hil/host_queue.h` | `ssd_host_queue_init/alloc/free`、`tag/owner/used` | contract |
| `hil_io`（真实 I/O 路径） | `tests/unit/test_hil.c` 经 `src/hil/controller.c` → FTL → FC/NAND | READ / WRITE / FLUSH / TRIM、尾窗读写 | io |
| `cli` | `src/hil/cli.c`、`include/hil/cli.h` | 行编辑、命令表、`cli_host_busy` | 不覆盖（手工 CLI） |
| `usb_msc` | `src/hil/usb_msc.c`、`include/hil/usb_msc.h` | BOT class 回调、SCSI → controller、host busy gate | 不覆盖（PC 主机矩阵） |
| `runtime` | `src/main.c`（`ssd_runtime_pump`）、`include/hil/runtime.h` | `ssd_runtime_pump` 的服务顺序 | 不覆盖（主循环间接） |
| 跨层平台守卫 | `src/platform/stm32h743/dma_port.c` | `dma_port_uart_tx_init` 返回值语义 | contract（跨层） |

> 测试项按「模块 + 公共接口」切，不按函数逐个列；内部 `static` helper 由所属模块的
> 公共接口间接覆盖。`hil_io` 是测试路径，不是生产模块，保留一行是为了把真实 I/O
> 与纯契约用例分开。

### 1.2 测试层与套件

| 层 | 根套件 | 环境 | 能证明什么 | 不能证明什么 |
|---|---|---|---|---|
| **HIL contract** | `/hil/contract/` | 主 boot 的 `g_ctrl` + 真 FTL/NAND；不写介质 | 状态机、init、LBA 范围、submit 分类、host queue、完成轮询 | 持久化、真实 I/O、跨实例 |
| **HIL io** | `/hil/io/` | 真 FTL / NAND，容量尾部 8-LBA 窗口 | 经 controller 的真实 READ/WRITE/FLUSH/TRIM 闭环 | 真 POR、多 run、错误注入 |
| **平台守卫** | `/hil/contract/` 内 | 真板 UART DMA | `dma_port_uart_tx_init` 成功路径 | 失败路径、DMA 中断时序 |
| **PC 主机矩阵** | 不在 `/hil/` | PC + USB host | BOT / SCSI / fsync / UNMAP / 热插拔 | 不是固件 UT 判据 |

```text
/hil/
  contract/   10 条注册 = 9 条 HIL 契约 + 1 条跨层平台守卫
  io/          7 条：写读 / 写→FLUSH→读 / FLUSH / TRIM 及单飞行/越界
```

- 当前注册 **17 条**（contract 10 / io 7）；完整清单以 `test_hil.c` 注册表与
  `test --list` 为准。平台守卫 `uart_dma_init_returns` 不计入 HIL 契约覆盖点。
- 执行入口：`test /hil/`（展开到 `contract/` 与 `io/`）；也可定向
  `test /hil/contract/` 或 `test /hil/io/`。两个子套件共用同一个主 boot 实例；
  共享状态由 teardown 的 `hil_quiesce()` 排空，不靠用例顺序。
- `contract` 不写 NAND；`io` 是破坏性（尾窗写）。跑 `io` 前必须拔 USB。

### 1.3 测试原则

1. **只在目标板跑**（CLAUDE.md R4）：`make tests UT=HIL` 生成镜像，串口 CLI `test`
   执行；不做 host 模拟。
2. **真 FTL / 真 NAND，无 mock**：直接复用主 boot 建好的 `g_ctrl` 与
   `g_ctrl.ftl[0]`，不建 FTL 桩、不绕过 controller。
3. **不 format**：读写/FLUSH/TRIM 固定用容量尾部 8-LBA 窗口，避免破坏文件系统常用区。
4. **不碰 USB host**：USB 槽状态机由 PC 主机侧矩阵覆盖（`usb_msc.md` Part B），
   不放进 `/hil/`。
5. **一个用例只钉一个契约**：判据必须能回答「断言比较的是哪两个值」；没有区分力的
   断言不算覆盖（C7 / C8）。
6. **负例必须能红**：非法 opcode、越界 LBA、单飞行冲突等必须有明确拒绝判据。
7. **状态如实**：正常路径全绿不等于 HIL 契约闭环；未覆盖项见 §6（R1）。

---

## 2. 按模块的测试设计

> 每个小节固定三段：**设计要点**（不变量 / 契约 / 错误语义）、
> `设计点 | 判据 | 代表用例 | 缺口` 表、**负例 / 守卫**。
> 代表用例只用于定位测试意图，不是完整清单。

### 2.1 `ssd_controller` — 状态机、init、LBA、submit、FLUSH/TRIM 服务

**设计要点**

- 生命周期是 `RESET → INITIALIZING → READY / FAILED`；`READY → FAILED` 是运行时
  致命错误预留；`FAILED` 是终态；非法转换被忽略；`state_transition(NULL, …)` 安全返回。
- `init` 对每个 `die < DIE_MAX` 都要求槽非空，且 `slot->topo.ch/ce/lun` 必须等于
  `die_2_ch/ce/lun(die)`；缺失或拓扑不符 → `INVALID_ARGUMENT` + `FAILED`。
  「稀疏拓扑的空洞不访问」注释与当前实现相反，登记 `RISK-CTRL-03`。
- `host_lba_max` 默认 `FTL_LBA_MAX`；boot 可按实际坏块扣减。它是 host 容量口径的
  唯一 reader（`ssd_controller_host_lba_max`），未初始化时回退编译期上限。
- LBA 范围是半开区间 `[lba, lba + count)`：`count == 0`、`lba >= max`、
  `lba + count > max` 都拒绝。`count` 与 `lba` 的上界之和小于 2^33，`QWORD` 加法不回绕。
- `submit` 要求 `controller != NULL`、`request != NULL`、`state == READY`；进入后先清
  `complete_flag` / `completion_status` / `agg_status`，再按 opcode 分派。
- READ/WRITE 在拆分前做 LBA 范围与 `buffer_size` 校验（`block_count * 512B`）；
  FLUSH/TRIM 是异步命令，`flush_req` / `trim_req` 单飞行，重复提交返回 `BUSY`。
- 未知 opcode → `NOT_SUPPORTED`。进入 opcode 分支后的参数拒绝都要置
  `complete_flag = 1` 并写 `completion_status`，否则调用者会永久等待；guard 级拒绝
  （controller/request NULL、非 READY）只返回 `INVALID_ARGUMENT`，此时调用者没有
  可用的请求对象。
- FLUSH 提交只为各 present die 调 `ftl_flush_begin`；TRIM 只登记
  `trim_req/trim_lba/trim_started`。完成由 `ssd_controller_service` 每轮推进后回执。
- `is_complete(NULL)` 返回 1（视为已完成）；非 NULL 直接读 `complete_flag`。
- `cancel_flush` 摘掉 `flush_req` 并清各实例 flush job，避免旧 job 完成时写到已复用的
  请求对象上；当前无 UT。

| 设计点 | 判据 | 代表用例 | 缺口 |
|---|---|---|---|
| 状态机合法 / 非法转换 | RESET→READY 被忽略；INITIALIZING→READY；READY→FAILED；FAILED→READY 被忽略 | `state_machine` | — |
| 状态机 NULL 守卫 | NULL 不解引用、不改状态 | `state_machine` | — |
| init 绑定全部 present die | 合法槽表 → READY；`host_lba_max = FTL_LBA_MAX` | `init_ready` | boot 扣减坏块路径未测 |
| init 缺槽 | `route[0] = NULL` → `INVALID_ARGUMENT` + `FAILED` | `init_missing_slot` | 稀疏拓扑实现与注释冲突（RISK-CTRL-03） |
| init 拓扑不符 | `slot.topo.ch = 0xFF` → `INVALID_ARGUMENT` + `FAILED` | `init_topo_mismatch` | 只覆盖 ch；ce/lun 未逐项构造 |
| LBA 半开区间 | `count=0`、`lba=max`、`[max-1,2)` 拒绝；`[0,1)`、`[max-1,1)` 接受 | `lba_range_ok` | 容量 <2 LBA 时边界分支跳过 |
| submit 非 READY / NULL | RESET/FAILED、controller=NULL、request=NULL → `INVALID_ARGUMENT` | `submit_not_ready` | — |
| submit 参数分类 | buffer 不足、count=0、越界 LBA → `INVALID_ARGUMENT`；未知 opcode → `NOT_SUPPORTED`；均置 `complete_flag=1` | `submit_edges` | buffer NULL 未覆盖（RISK-CTRL-04） |
| 完成轮询 | NULL → 1；flag 0 → 0；flag 1 → 1 | `is_complete` | — |
| FLUSH 异步完成 | submit → 泵到 `complete_flag=1` → `status=OK`、`flush_req == NULL` | `flush_job` | 无失败注入 |
| FLUSH 单飞行 | 第一条在飞时第二条 → `BUSY`、`complete_flag=1`；第一条最终 OK | `flush_busy` | — |
| TRIM 非法范围 | OOB / count=0 → `INVALID_ARGUMENT`、`complete_flag=1`、`trim_req` 保持 NULL | `trim_edges` | — |
| TRIM 单飞行 | 第一条在飞时第二条 → `BUSY`、`complete_flag=1`；第一条最终 OK | `trim_busy` | — |
| `cancel_flush` | 摘掉回执指针并取消各实例 flush job，旧请求不再被完成 | 无 | 未覆盖 |
| 多 die 路由 | `route` 指向对应实例；`run_end` 在实例边界截断 | 无 | 1 die，`route` 恒 `ftl[0]`（ISS-HIL-03） |

**负例 / 守卫**

- `state_machine`：非法转换、FAILED 终态、NULL 守卫必须不崩且状态不变。
- `init_missing_slot` / `init_topo_mismatch`：缺槽和拓扑不符必须拒绝并进 `FAILED`，
  不能继续 READY 把错误路由到 NULL/错误 chip。
- `submit_not_ready` / `submit_edges` / `lba_range_ok` / `trim_edges`：host 入口是
  不可信输入（H29），参数拒绝必须发生在分配/拆分之前；通过 guard 后的拒绝要置
  `complete_flag=1`。controller/request NULL、非 READY 属 guard 级拒绝，只返回码。
- 已知缺口：`buffer == NULL` 且 `buffer_size` 足够时，入口只查容量不查指针；
  会进 `ssd_request_split` 的指针算术，靠 FTL 兜底但状态分类错误（RISK-CTRL-04）。

### 2.2 `ssd_request` — run 拆分、`sub_remaining`、聚合回执

**设计要点**

- `ssd_controller_run_end` 从 `lba` 起找「路由到同一 FTL 实例」的最长连续 run；
  路由按 `L2P_REGION_SHIFT` 粒度分片，所以 run 只在 region 边界或实例切换处结束。
  返回 `> lba` 且 `<= lba_limit`；路由失败只可能是宏配置错误，用 `ASSERT` 守。
- `ssd_split_count` 在提交前精确统计 run 段数：子请求可能同步完成（如整段未映射读），
  先置 `sub_remaining`，否则同步回调会把父请求提前置完成。
- `ssd_request_split` 逐 run 填 `io.lba/buffer/count/cmd_remaining`，再调
  `ftl_submit_write/read`；`buffer` 步进按 `FTL_IU_BYTES`（512B，1 LBA = 1 CW）。
- 部分 run 提交失败时，从 `sub_remaining` 扣掉「未提交数」，错误锁存到 `agg_status`；
  若已有 run 在飞，`submit` 返回 `OK`（请求已受理），最终状态由完成后回执；
  若第一个 run 就失败，直接返回错误并置完成。
- `on_sub_complete` 一次 run 恰好一次：首个非 OK 状态胜出，`sub_remaining` 递减，
  归零时把 `agg_status` 写回 `completion_status` 并置 `complete_flag`。
- `io` 是跨命令复用模板：`cmd_remaining` 在 `split` 前后保存/还原，不能把上一条命令的
  剩余长度留给下一条。

| 设计点 | 判据 | 代表用例 | 缺口 |
|---|---|---|---|
| run 边界 | 只在 region 边界或实例切换处结束；不产生零长度 run | `write_read_roundtrip`（单 run） | 1 die，跨实例边界未测 |
| 预统计 run 数 | 提交前置 `sub_remaining`；同步完成不提前回执 | `write_read_roundtrip` 断言完成后归零 | 只测终值，未测同步完成的中间态 |
| 每 run 字段 | `lba/buffer/count/cmd_remaining` 正确步进；模板还原 | `write_read_roundtrip`、`write_flush_readback` | `cmd_remaining` 未断言 |
| 部分提交失败 | 未提交数从 `sub_remaining` 扣除；首个非 OK 胜出 | 无 | 无 HIL 注入 seam（RISK-CTRL-01） |
| 完成聚合 | 全部 run 完成后 `completion_status = agg_status`，`complete_flag=1` | `write_read_roundtrip` | 多 run 失败路径未测 |
| 路由 | `route` 非 NULL；LBA 到实例映射一致 | 间接覆盖 | 多 die 未测 |

**负例 / 守卫**

- `submit_edges` 在拆分前拒绝无效参数；`ssd_request_split` 内部用
  `ASSERT(ftl != NULL)` 守路由，release 不产生额外分支。
- `sub_remaining` 扣减有 `not_submitted <= sub_remaining` 断言；release 下异常收敛到 0，
  宁可立即回执错误也不挂死。
- 已知缺口：HIL 没有 `fi_*` 注入 seam，无法构造「部分 run 已提交、后续 run 失败」；
  多 run 只在单 die 下退化为单 run（RISK-CTRL-01）。

### 2.3 `host_queue` — tag / owner / used

**设计要点**

- `SSD_HOST_QUEUE_DEPTH = 8`；entry 的 `tag == 数组下标`，`owner` 是 USB / CLI；
  `used` 是当前占用数。
- `ssd_host_queue_alloc` 扫描第一个 `in_use == false` 的 entry，置 `in_use/tag/owner`，
  `used++` 后返回；表满返回 NULL。`request` 字段不由队列清零，调用者负责填充。
- `ssd_host_queue_free` 要求 entry 已分配、`used > 0`，`memset` 整个 entry 后 `used--`；
  释放后 `in_use == 0`，旧 `request` 不再可见。
- 这张表只做**请求归属与 tag 分配**，不是 host 前端的互斥闸门；USB/CLI 的单闸门
  仍在各自模块（RISK-HOST-20）。

| 设计点 | 判据 | 代表用例 | 缺口 |
|---|---|---|---|
| 表满 | 填满 8 entry 后 alloc 返回 NULL，`used == 8` | `host_queue` | — |
| tag 唯一 | `entry->tag == i`，分配顺序与下标一致 | `host_queue` | — |
| owner 归属 | USB / CLI owner 正确写入 | `host_queue` | 非法 owner 未构造 |
| 释放 / 复用 | 释放 tag0 后可再分配 tag0；释放后 entry 归零 | `host_queue` | — |
| 全量归还 | 全部 free 后 `used == 0`、`in_use == 0` | `host_queue` | — |
| 并发 / 互斥 | 同一时刻只有一个 host 前端提交 | 无 | RISK-HOST-20 |
| 重复释放 | 第二次 free 被 ASSERT 拒绝 | 无 | 未构造 |

**负例 / 守卫**

- 表满返回 NULL，不得覆盖已分配 entry；`free` 清整项，避免旧 `request` 被复用。
- `alloc` 的 owner 合法域与 `free` 的 `in_use/used` 前置条件由 `ASSERT` 守；
  debug 下非法调用停机，release 下没有等价运行期拒绝（H14 / P3 判据 9）。
- 已知缺口：单线程 UT 不证明并发安全；host 前端互斥不是单一闸门（RISK-HOST-20）。

### 2.4 `hil_io` — 真实 READ/WRITE/FLUSH/TRIM

**设计要点**

- 只使用容量尾部 8-LBA 窗口：`hil_lba_base() = (host_lba_max - 8) & ~(8 - 1)`；
  `HIL_IO_LBAS = 4`，每个用例读写 4 个 512B LBA。窗口避开文件系统常用区，**不 format**。
- READ/WRITE 走 `ssd_controller_submit` → `ssd_request_split` → FTL → FC/NAND 的真实路径，
  不绕过 controller、不 mock FTL。
- `write_read_roundtrip` 不 flush 就立即读回，证明早回执/读覆盖路径返回新数据；
  `write_flush_readback` 在写后提交 FLUSH，证明软件 barrier 后读回一致。
- `trim_job` 先写再 TRIM，回读必须全零：TRIM 让旧映射不可见，已删数据不复活。
- FLUSH/TRIM 都是异步命令：`hil_wait` 泵 `ftl_service_pump` / `ftl_service` /
  `ssd_controller_service` / `timer_scheduler_poll`，直到 `complete_flag` 或 5 s 超时。
- 写数据模式由每个用例自己生成；用例之间不依赖对方留下的数据，teardown 必须排空。

| 设计点 | 判据 | 代表用例 | 缺口 |
|---|---|---|---|
| 写→读（不 flush） | 4 LBA 逐字节一致；`sub_remaining == 0` | `write_read_roundtrip` | 只证明当前映射，不证明持久化 |
| 写→FLUSH→读 | FLUSH `status=OK`、`flush_req == NULL`；读回一致 | `write_flush_readback` | 无真 POR / 掉电窗口 |
| FLUSH 异步完成 | 泵到 `complete_flag=1`，`status=OK` | `flush_job` | 无失败注入 |
| FLUSH 单飞行 | 第二条 `BUSY` + `complete_flag=1`；第一条最终 OK | `flush_busy` | — |
| TRIM 不复活 | 写 4 LBA → TRIM → 回读全零 | `trim_job` | 无掉电后复验 |
| TRIM 非法范围 | OOB / count=0 拒绝，`trim_req` 不登记 | `trim_edges` | — |
| TRIM 单飞行 | 第二条 `BUSY`；第一条最终 OK | `trim_busy` | — |
| 尾窗对齐 | `hil_lba_base` 8 对齐且 `> 8`；不碰文件系统区 | 所有 io 用例 | 容量 <8 LBA 时前置断言失败 |

**负例 / 守卫**

- `trim_edges`、`flush_busy`、`trim_busy` 覆盖非法范围与单飞行拒绝；拒绝必须置
  `complete_flag=1`，避免调用者等一个不会完成的请求。
- teardown `hil_quiesce()` 检查 FC/FTL 账本归零；超时先恢复 `bg_suspend` 再让 munit 失败。
- 真 NAND 安全模型见 §4.5：`io` 是破坏性套件，跑前拔 USB；`test` 命令不替用例检查
  `cli_host_busy()` / `usb_msc_host_busy()`，由操作者保证。

### 2.5 `cli` / `usb_msc` / `runtime` — 前端与主循环（不覆盖）

**设计要点**

- `usb_msc` 把 BOT 的 SCSI READ(10)/WRITE(10)/SYNCHRONIZE CACHE(10/16)/UNMAP(0x42)
  映射到 controller 的 512B LBA；回调运行在 `usb_device_task()`，提交后返回
  `TUD_MSC_RET_ASYNC`，由 `usb_msc_task()` 后续检查完成，不在回调里自旋。
- `cli` 是 UART 调试前端：行编辑、命令表、`wr/rd/tr/fl/wp` 等；`cli_host_busy()`
  还计入 `wp` 的诊断组（不占 host queue）。
- `runtime` 的唯一入口 `ssd_runtime_pump()` 固定顺序：FTL/FC → controller → USB → timer；
  主循环和 CLI 的同步等待共用它，避免命令 handler 手抄服务顺序。
- 这三个模块都**不在 `/hil/` UT**：USB 需要 PC 主机 + TinyUSB 协议栈；CLI 本身是测试
  执行入口；runtime 的顺序由主循环手工观察。`test` 命令不检查 host busy，操作者必须在
  跑破坏性用例前拔 USB / 确认 CLI 无在飞请求。

| 设计点 | 判据 | 代表用例 | 缺口 |
|---|---|---|---|
| USB BOT / SCSI 映射 | PC 枚举、READ/WRITE、fsync、UNMAP、热插拔 | PC 主机矩阵（`usb_msc.md` Part B） | 整个模块不在固件 UT |
| host busy gate | 同一时刻只有一个前端提交 controller | 手工观察 | RISK-HOST-20 |
| host 超时 / 中止 | 命令不悬挂；复位/拔出可恢复 | 无 | RISK-HOST-21 |
| CLI 同步命令 | `wr/rd/tr/fl` 在命令返回前完成 | 手工 CLI | 无逐条 UT |
| runtime 服务顺序 | controller 在 USB 之前推进；timer 补齐 credit | 间接 | 无顺序断言 |

**负例 / 守卫**

- 不在 `/hil/` 内造 mock；不能把 PC 主机矩阵的通过算成固件 UT 覆盖。
- 破坏性 `io` 用例的前置条件由操作者保证：拔 USB、确认没有 host 在飞请求。
- 已知缺口：`cli_host_busy` / `usb_msc_host_busy` 的互斥不是单一闸门，
  多处手工维护（RISK-HOST-20）；host 命令没有超时/中止（RISK-HOST-21）。

### 2.6 跨层平台守卫 — `uart_dma_init_returns`

**设计要点**

- `uart_dma_init_returns` 在 `g_hil_contract_tests[]` 注册，但测试对象是平台层的
  `dma_port_uart_tx_init()`：成功必须返回 `FUNC_STATUS_SUCCESS`（0），失败返回
  `FUNC_STATUS_ERROR`（1）；不能混用 CMSIS `SUCCESS/ERROR` 的相反值。
- 它只覆盖成功路径；失败路径没有注入（`RISK-PLAT-13` 明确“失败路径未注入”）。
- 该用例同时是 `RISK-PLAT-14` 的板端证据：UART TX DMA 初始化必须幂等，运行期重入
  不能中止在飞传输，否则日志/CLI 会静默阻塞。
- **模块归属不是 HIL**：按模块边界应放平台套件；当前测试源把它挂在
  `/hil/contract/`，本文按现状记录，并把它从 HIL 契约覆盖点中单列。

| 设计点 | 判据 | 代表用例 | 缺口 |
|---|---|---|---|
| DMA 初始化返回值 | `dma_port_uart_tx_init() == FUNC_STATUS_SUCCESS` | `uart_dma_init_returns` | 失败路径未注入 |
| 运行期重入幂等 | 重复 init 不中止在飞 TX DMA | 同用例（板端日志/CLI 仍可用） | 无独立重入计数判据 |
| 模块归属 | 平台层守卫，不占 HIL 契约 | — | 测试源未迁出 `/hil/contract/` |

**负例 / 守卫**

- 只有成功路径断言；失败路径需要注入或可控 fake，当前不可做（HIL 层不新增 seam，
  见 `docs/design/testing_guide.md` §12）。
- `dma_port.c` 的幂等标志 `s_uart_tx_dma_initialized` 是生产守卫；不能靠 HIL 用例
  反复初始化来验证，除非确认不会打断日志输出。

---

## 3. 跨模块 / 系统级场景

模块内用例只证明单点契约；下面这些场景跨多个模块，必须单独设计：

| 场景 | 涉及模块 | 判据 | 代表用例 |
|---|---|---|---|
| 写→读（不 flush） | `ssd_controller` + `ssd_request` + FTL cache | 4 LBA 逐字节一致；`sub_remaining` 归零 | `write_read_roundtrip` |
| 写→FLUSH→读 | controller + request + FTL flush/CP | FLUSH 完成、`flush_req` 清空、读回一致 | `write_flush_readback` |
| TRIM 不复活 | controller trim service + FTL 映射 | 写后 TRIM，回读全零 | `trim_job` |
| FLUSH / TRIM 单飞行 | `ssd_controller` | 第二条 `BUSY` + `complete_flag`，第一条最终 OK | `flush_busy`、`trim_busy` |
| 非法范围统一拒绝 | controller + request | `INVALID_ARGUMENT` + `complete_flag` + 不登记在飞请求 | `submit_edges`、`trim_edges` |
| teardown 账本归零 | controller + FTL + FC | `INV-FC` / `INV-REQ` / `INV-JOB` / `INV-BG` 全过 | 所有用例的 `hil_quiesce` |
| 多 run / 跨实例部分失败 | `ssd_request` + route | run 在实例边界拆开；首个非 OK 聚合 | 无（1 die，RISK-CTRL-01） |
| 跨重启 / 真 POR | 全链路 | 重启后映射/账本一致 | 无（§4.6 / §6.1） |

### 3.1 写读与持久化的分界

- `write_read_roundtrip` 不 flush，只证明「写完成后立即读能看到新数据」——这是
  早回执/读覆盖路径，不是掉电持久化判据。
- `write_flush_readback` 加 FLUSH，证明软件 barrier 完成后读回一致；仍**不证明**真 POR
  后数据在介质上。真掉电窗口由人工流程/FTL media 用例覆盖。
- `trim_job` 的回读全零只证明当前映射不可见；TRIM 的掉电持久化不在 HIL UT。

### 3.2 FLUSH / TRIM 单飞行

- FLUSH 和 TRIM 各只有一条在飞请求指针（`flush_req` / `trim_req`）；第二个请求必须
  返回 `BUSY`，并立即置自己的 `complete_flag`，不能覆盖第一条的完成回执对象。
- 单飞行判据依赖「提交与提交之间没有主循环 service 推进」：用例在两次 submit 之间不
  泵 controller，才能稳定构造在飞状态。若后台 timer/ISR 推进了 service，可能不成立
  （§6.2）。

### 3.3 多 run / 跨实例（未覆盖）

- 当前 `DIE_MAX = 1`，`ssd_controller_route` 恒返回 `ftl[0]`；`run_end` 的多实例分支
  与 `ssd_split_count > 1` 都没有真实输入。
- 因此「部分 run 已提交、后续 run 失败」「父请求提前完成 / 错误被覆盖」无法在 HIL UT
  构造。需要多 die / N=2 影子构建，或在 `ftl_submit` 加注入 seam；见 `ISS-HIL-03`、
  `RISK-CTRL-01`。
- 1 维下不要用「单 run 通过」声称多 run 拆分已覆盖（R1）。

### 3.4 覆盖模型（`HIL-CP-*`）

| 覆盖点 | 要覆盖什么 | 典型用例 |
|---|---|---|
| `HIL-CP-CTRL` | controller 状态机、init 槽位/拓扑、host tag/owner/used | `state_machine`、`init_*`、`host_queue` |
| `HIL-CP-SUBMIT` | LBA 范围、buffer 容量、非法 opcode、NULL 入参、`complete_flag` | `lba_range_ok`、`submit_not_ready`、`submit_edges` |
| `HIL-CP-IO` | 经 controller 的真实 READ/WRITE/TRIM 闭环 | `write_read_roundtrip`、`trim_job` |
| `HIL-CP-DUR` | FLUSH 完成与写后读一致 | `write_flush_readback`、`flush_job` |
| `HIL-CP-CONC` | FLUSH/TRIM 单飞行背压 | `flush_busy`、`trim_busy` |
| `HIL-CP-LEDGER` | teardown 后 FC 池/FTL job/请求表归零 | `hil_quiesce` 的 `INV-*` |
| `HIL-CP-ERR` | 错误码与 `complete_flag` 一致，不得悬挂 | `submit_edges`、`flush_busy`、`trim_busy`、`trim_edges` |

> `HIL-CP-*` 是**本仓自拟**的 HIL 层功能覆盖点，不是代码覆盖率；裸机目标上不以行覆盖
> 作为主指标。它与 `ftl_ut_design.md` §3.6 的 `CP-*` **不是同一套坐标**：后者描述 FTL
> 层（allocator/L2P/CP/Journal），前者只描述 HIL 层。加 `HIL-` 前缀避免同名异义（C9/C11）。

---

## 4. 测试技术、夹具与 seam

### 4.1 技术矩阵

| 技术 | 判据 | 本仓落点 |
|---|---|---|
| **Directed / contract** | 一个状态机/接口契约一条用例 | `state_machine`、`write_read_roundtrip` |
| **Negative** | 非法状态/参数必须被拒绝且置 `complete_flag` | `init_missing_slot`、`init_topo_mismatch`、`submit_edges`、`flush_busy`、`trim_busy`、`trim_edges` |
| **Boundary** | LBA 半开区间、count=0/max、越界 | `lba_range_ok`、`submit_edges` |
| **Seeded random** | 记录 seed；失败可复现 | HIL UT 当前不随机 |
| **Fault injection** | 注入点 → 预期降级/恢复 | HIL UT 不注入（无 HIL 专属注入 seam） |
| **Reboot / crash point** | 模拟重启或真 POR 后映射/账本一致 | **未覆盖**，见 §4.6 / §6.1 |
| **Stress / perf** | 只作基准，不作功能通过率 | PC 主机矩阵 / `/bench/speed/`，不在 `/hil/` |

> HIL UT 的强项是**同步契约**：状态机、init、范围、submit、单飞行、完成回执；
> 弱项是**持久化/并发/主机侧时序**——这些必须由 §6 的专项或 PC 矩阵补，不能靠正常路径
> 全绿推定。

### 4.2 活实例与测试缓冲

- `g_ctrl` 是主 boot 建好的 controller；`g_ctrl.ftl[0]` 是唯一 FTL 实例。
- `hil_setup` 断言 `g_ctrl.state == READY` 且 `g_ctrl.ftl[0] != NULL`；不复制
  `ftl_ctx_s`（大型静态对象）。
- 所有 I/O 走 `ssd_controller_submit` → `ssd_request_split` → FTL → FC/NAND 的真实路径。
- 测试缓冲：

| 对象 | 尺寸 | 用法 |
|---|---|---|
| `s_hil_wr` / `s_hil_rd` | 4 LBA × 512 B | 写读闭环 |
| `s_hil_zero` | 4 LBA × 512 B | TRIM 后读零比对 |
| `s_hil_edge_buf` | 4 LBA × 512 B | 非法参数用例（buffer 不足等） |

- `s_hil_bad_ftl` 是拓扑不符用例的假实例，放 AXI_NOCACHE（`NONCACHEABLE_BUFFER`）；
  全 UT 镜像下 AXI_RAM 会被测试夹具占满，该结构 `sizeof = 9648 B`（约 9.4 KB）且
  CPU-only，不需要 D-Cache。
- `hil_lba_base() = (host_lba_max - 8) & ~(8 - 1)`；`HIL_IO_LBAS=4`、`HIL_LBA_WINDOW=8`。
  只用容量尾部 8 LBA，重复运行只覆盖同一窗口；**不 format**。

### 4.3 时钟与超时

- HIL UT 使用真实平台时钟；不安装伪时钟。
- `hil_wait` / `hil_quiesce` 用 `platform_now_ms()` 做 5 s 超时；`HIL_IO_TIMEOUT_MS=5000`。
- `hil_wait` 泵 `ftl_service_pump` → `ftl_service`（逐 die）→ `ssd_controller_service`
  → `timer_scheduler_poll`；`timer_scheduler_poll` 不能漏，否则 CP/Journal/WL 的 credit
  耗尽会让长写/FLUSH 等待超时。
- 超时是失败判据，不是 SKIP；`hil_quiesce` 超时先恢复 `bg_suspend` 再让 munit 失败。

### 4.4 结束不变量（`INV-*`）

每个用例 teardown 调用 `hil_quiesce()`，至少检查：

- `INV-FC`：FC 池 `sq/hsq/cq/command/descriptor/buffer` 全 0；
  与 `ftl_ut_design.md` §4.4 的 `INV-FC` 同义。
- `INV-REQ`：`ftl_op_req_count() == 0`、`flush_req == NULL`、`trim_req == NULL`。
  **HIL 专用**：它不等同 FTL 的 `INV-PEND`（后者还查 `pend_slot` / `pend_group`）。
- `INV-JOB`：CP / migrate / metadata / journal / wbuf / cache 序列 / cache inflight 全 idle。
  **HIL 专用聚合**：它对应 FTL `INV-MD` + `INV-CACHE` 的可观测子集，不覆盖 FTL 的
  L2P/CMT 完整账本，也不覆盖 `INV-ALLOC`。
- `INV-BG`：恢复 `bg_suspend` 原值（断言失败也不能污染后续用例）。

> `INV-*` 是**本仓自拟**的不变量编号；用于把「结束条件」写成可引用的契约。
> 新增/修改 teardown 时同步本节；不能用 HIL 的 `INV-JOB` 声称 FTL 层已全部归零。

### 4.5 真 NAND 安全模型

- 破坏性：会写容量尾部 8-LBA 窗口；不 format、不恢复 host 数据。
- 运行前必须拔掉 USB，并确认没有 host 在飞请求；`test` 命令**不**替用例检查
  `cli_host_busy()` / `usb_msc_host_busy()`，由操作者保证。
- 跑完不要求 `format`（尾窗不是文件系统区），但正式交付前仍按 `testing_guide.md` 收尾。
- `contract` 不写 NAND；`io` 才破坏性。不要把 `contract` 的通过当成介质已验证。

### 4.6 跨重启夹具的边界

- `ftl_ut_common.c`（含 `ftl_ut_reboot_from_media`）只在 `UT_FTL_ENABLE=1`
  （`UT=` 为空或 `UT=FTL`）编入；`UT=HIL` 下**没有**该夹具。
- 因此跨重启用例不能在当前 `UT=HIL` 构建里直接写；可选：
  1. 把 `ftl_ut_common.c` 链入 `UT=HIL`（需确认它不拉入 FTL 套件依赖）；
  2. 走 `UT=` 全量构建，并在 `UT_FTL_ENABLE` 条件下编译该用例；
  3. 写 HIL 本地的最小 reboot helper（quiesce → fc_init → ftl_init → pfr_boot_recovery）。
- 当前选择：**留以后**，见 §6.1 与 `RISK-UT-34`。

### 4.7 可测性 seam

| seam | 用途 | 产品固件是否编入 |
|---|---|---|
| `ssd_controller_*` 公共 API | 状态机 / init / submit / complete | 是 |
| `fc_get_resource_stats` | teardown FC 池账本 | 是 |
| `ftl_service_pump` / `ftl_service` / `ssd_controller_service` | 主循环泵 | 是 |
| `timer_scheduler_poll` | 后台 credit / CP / Journal | 是 |
| `g_ctrl` | 主 boot 实例 | 是 |

> HIL UT **不新增 seam、不 mock、不注入**；能测的必须是生产路径可直接驱动的部分。
> 故障注入类缺口见 §6.1；新增 seam 前先说明为什么生产路径不可直接驱动（R4）。

---

## 5. 构建、运行与证据

### 5.1 构建矩阵

| 档 | 命令 | 产物 | 说明 |
|---|---|---|---|
| debug HIL | `make tests UT=HIL` | `build/firmware/debug/bin/debug_ut.elf` | 默认；`ASSERT` 停机；HIL 无 SKIP |
| release HIL | `make tests BUILD_TYPE=release UT=HIL` | `build/firmware/release/bin/release_ut.elf` | `-O2 -DNDEBUG`；同一批 HIL 用例执行 |
| 全量 UT | `make tests UT=` | `build/firmware/debug/bin/debug_ut.elf` | FC + FTL + HIL 一起链接 |
| 产品 | `make all` / `make release` | `debug.elf` / `release.elf` | 无 `test` 命令 |

- `UT=HIL` / `UT=FC` / `UT=FTL` 共用 `debug_ut.elf`（release 共用 `release_ut.elf`）；
  烧 HIL UT 前必须让 `make tests UT=HIL` 是最后一次 UT 链接。
- 症状对照：`test` 命令不存在 ⇒ 烧了产品镜像；`test /hil/` 打 `NO test run`
  ⇒ 镜像里不是 HIL 套件。两者都先查产物路径与横幅时间戳（`RISK-PLAT-07`）。
- `make tests` 只证链接，不证正确（R2）；HIL 改动涉及 controller/request 行为时仍须上板。

### 5.2 运行

```text
make tests UT=HIL
test --list
test /hil/contract/
test /hil/io/
test /hil/
```

1. `test --list` 确认 `/hil/contract/` 与 `/hil/io/` 都在，用例名与注册表一致。
2. `test /hil/contract/`：不写 NAND；`uart_dma_init_returns` 会重入 UART TX DMA 初始化，
   确认串口日志没有中断。
3. `test /hil/io/`：破坏性，先拔 USB；观察 4 LBA 写读、FLUSH、TRIM、单飞行。
4. `test /hil/`：完整套件；记录套件/用例数、耗时、失败项、SKIP 原因。
5. 收尾：按 `testing_guide.md` 恢复可用盘；`io` 只写尾窗，不要求 format。

- 套件前缀必须带尾斜杠；`/hil/` 由 `ut_run_all()` 展开为叶子套件。
- HIL 用例是 `MUNIT_TEST_OPTION_SINGLE_ITERATION`，`--iterations` 不增加运行次数。
- HIL 当前不随机，`--seed` 只影响 munit 的全局 seed，不影响用例输入。

### 5.3 SKIP 语义

| 条件 | SKIP 用例 | 说明 |
|---|---|---|
| 无 | 无 | HIL 套件当前没有 `MUNIT_SKIP` 分支 |

- 任何 SKIP 都是异常：可能是测试源被改、夹具前置失败或套件过滤错；必须打印原因，
  不能当 PASS（R1）。
- release UT 没有额外的 release-only 负例；HIL 的拒绝判据靠返回码与 `complete_flag`，
  不靠 `ASSERT_REJECT`。

### 5.4 日志与证据

输出层级互相独立：

| 开关 | 额外可见内容 |
|---|---|
| 默认（无开关） | 用例名 + `[ OK ]/[ SKIP ]` + 耗时 |
| `--log-visible info` | `[EV]` 简单前后对比（每个用例一条 summary） |
| `--log-visible debug` | `[EV]` 完整对比（inputs / outputs / 中间计数逐项列出） |
| `--log-project info/debug` | 额外打开 FTL/FC 项目日志（默认 `LOG_OFF`） |

- `[EV]` 行由 `hil_trace_summary/detail` 输出；没有调用就只有 `[ OK ]`。
- 串口工具若开启「接收设置 → 自动换行」，会在静默期后补一个换行，把 munit 的名字行
  与 `[ OK ]` 拆成两行；这是采集端行为，不是固件输出（`RISK-PLAT-06`）。

原始串口日志与运行元数据放 `build/test-results/`（`build/` 已被 `.gitignore` 整目录忽略）：

```text
build/test-results/<YYYYMMDD-HHMMSS>_<build>_<target>/
  run.log      # 串口终端工具的 log（原始输出）
  meta.txt     # commit / 镜像横幅时间戳 / 命令 / 结果 / 备注
```

`meta.txt` 的四件套是证据判据（P6）：`commit` = 代码版本，`image` 的横幅时间戳 =
烧的是哪个镜像，`command` / `result` = 跑了什么、结果如何。需要长期留证的结论写
`docs/design/known_risks.md` 或 `docs/design/known_iss.md`，原始日志不进版本库。

---

## 6. 不覆盖与判据盲区

### 6.1 不覆盖项

| 不覆盖项 | 原因 | 关联 |
|---|---|---|
| 真 POR / 掉电 | 人工流程；HIL UT 只复用主 boot 实例，不做 reboot | `RISK-UT-34` |
| 跨重启 | `ftl_ut_common.c` 不在 `UT=HIL` 编入（§4.6） | `RISK-UT-34` |
| 多 die / 跨实例部分失败 | 1 CH / 1 CE / 1 LUN，`DIE_MAX=1`，`route` 恒 `ftl[0]` | `RISK-CTRL-01`、`ISS-HIL-03` |
| 多 run 拆分 | 单 die 下退化为单 run；无 `ftl_submit` 注入 seam | `RISK-CTRL-01` |
| buffer NULL | `ssd_controller_submit` 只查 `buffer_size`，不查 `buffer` | `RISK-CTRL-04` |
| `cancel_flush` | host 复位 / 拔出路径；当前无 UT | `RISK-UT-34` |
| USB 槽状态机 | 需要 PC 主机侧矩阵（flush 槽 / read BUSY / 0x91 / 相同写匹配 / 总线复位） | `RISK-USB-09` / `RISK-USB-10` / `RISK-USB-11` / `RISK-USB-12` / `RISK-USB-13` |
| host 超时 / 中止 | 无固件 UT；需前端或 controller 超时机制 | `RISK-HOST-21` |
| host 前端互斥 | 多处手工维护，不是单一闸门 | `RISK-HOST-20` |
| host 命令 / CLI 前端 | CLI 是测试执行入口；USB 需要 PC host | `ISS-HIL-02` |
| soak / 性能 | `/bench/speed/` 与主机工具，不是 `/hil/` | — |
| `uart_dma_init_returns` 失败路径 | 无注入 / fake UART 实例 | `RISK-PLAT-13` |
| FTL 内部 L2P / GC / CP / Journal 契约 | HIL 只钉接口层；由 FTL UT 覆盖 | `ftl_ut_design.md` |
| HIL L2 验收 24 例 | 需 PC 主机侧逐条记录 | `ISS-HIL-02`、`usb_msc.md` Part B |

**2026-11-13 用户裁决**：跨重启、多 run、USB 槽等剩余缺口**留以后检验**；本轮已补的
TRIM 越界由 `trim_edges` 覆盖。`buffer NULL` 的设计问题单独登记 `RISK-CTRL-04`。

### 6.2 判据盲区

- `write_read_roundtrip` 不 flush：只证明写后立即可读，不证明持久化；不能拿它替代
  `write_flush_readback`，也不能替代真 POR。
- `write_flush_readback` 的 FLUSH 是软件 barrier：没有断电，不能证明介质上所有缓存页
  已落盘。
- `trim_job` 的回读全零只证明当前映射不可见；TRIM 后断电/重启是否复活不在 HIL UT。
- `flush_job` 只覆盖成功路径；FTL flush 失败、部分 die 失败的聚合未注入。
- `state_machine` 用栈上的 `ssd_ctl_s`，只验证纯转移表；init 用例用 `c` 绑定
  `g_ctrl.ftl`，不验证 boot 的真实副作用。
- `init_ready` 只验证 `host_lba_max == FTL_LBA_MAX`；boot 按坏块扣减容量的路径未测。
- `lba_range_ok` 的 `[max-1,1)` / `[max-1,2)` 分支在 `max >= 2` 才执行；容量 <2 LBA
  时边界判据不完整。
- `lba_range_ok(NULL, …)` 没有 NULL 守卫：`host_lba_max(NULL)` 回退编译期上限，
  有效数字范围会返回 true；只约定调用者传 controller，未测 NULL。
- `host_queue` 是单线程测试：不能证明并发下 tag/used 一致；非法 owner、重复 free
  只由 `ASSERT` 守，未构造失败输入。
- `flush_busy` / `trim_busy` 依赖两次 submit 之间没有 service 推进；若后台 timer/ISR
  推进 controller，在飞状态可能变化。
- `submit_edges` 只测 `buffer_size` 不足，不测 `buffer == NULL`；后者是
  `RISK-CTRL-04` 的已知缺口。
- `is_complete(NULL) == 1` 把 NULL 与「已完成」混为一谈；调用者无法区分空指针和
  真正完成，契约只保证不悬挂。
- `uart_dma_init_returns` 只测成功返回；失败返回 `FUNC_STATUS_ERROR` 与重入中止
  在飞 TX 的路径未注入。
- `hil_quiesce` 超时是失败判据，但超时日志只列计数，不证明哪个 job 卡住；定位仍需
  `ftl_ctx` / 项目日志。

---

## 7. 风险追溯（副轴）

> 风险是**追溯维度**，不是本文的组织轴。状态、历史、是否闭合见
> `docs/design/known_risks.md`；负例判定台账（M/N/K）见 §7.2。
> §7.1 只保留「判据非显然 / 跨模块」的 HIL 风险。

| RISK | 涉及测试项 | 判据 | 代表用例 |
|---|---|---|---|
| `RISK-UT-34` | `ssd_controller` / `ssd_request` / `host_queue` / `hil_io` | HIL 契约剩余缺口：buffer NULL、跨重启、多 run、USB 矩阵 | `is_complete`、`host_queue`、`write_flush_readback` |
| `RISK-CTRL-03` | `ssd_controller` | init 的稀疏拓扑注释与实现相反 | `init_missing_slot` |
| `RISK-CTRL-04` | `ssd_controller` | submit 未校验 `buffer == NULL`，状态分类错误 | `submit_edges`（未覆盖 NULL） |
| `RISK-HOST-20` | `host_queue` / `cli` / `usb_msc` | host 前端互斥不是单一闸门 | `host_queue`（部分） |
| `RISK-HOST-21` | `cli` / `usb_msc` | 没有 host 命令超时 / 中止 | 无 |
| `RISK-USB-12` / `RISK-USB-13` | `usb_msc` | SYNCHRONIZE CACHE(16) 未支持；两写槽占用时 READ 返回 ERROR | PC 主机矩阵 |

### 7.2 负例判定台账（HIL / USB / CLI 层）

> 本表只列**当前未闭合**的 HIL 前端风险（索引状态取自 `docs/design/known_risks.md`）。
> 判定口径同 `docs/design/testing_guide.md` §12.1；HIL 层没有注入 seam，失败路径只能靠
> 主机侧矩阵或人工操作构造，因此以 **K** 和状态词为主。
> **信息**与**已闭合**条目只在 `known_risks.md` 维护。

**CLI（7）**

| ID | 判定 | 用例 / 判定说明 |
|---|---|---|
| `RISK-CLI-01` | 待决策 | IWDG 未启用，长命令阻塞主循环的复位后果无法验证（RISK-CLI-01 同时是 IWDG 启用裁决） |
| `RISK-CLI-02` | K | 需 CLI `format` / `bbt --erase` 与 USB 在飞并发（主机侧矩阵） |
| `RISK-CLI-03` | K | 需连续 `wp` 重入，CLI 交互构造 |
| `RISK-CLI-06` | K | `q`/`cp`/`jrn`/`stat`/`id`/`ecc`/`wear` 字段需人工对拍 |
| `RISK-CLI-07` | K | 需制造读失败并抓串口输出，触发不可确定性构造 |
| `RISK-CLI-08` | K | 需 `wp` 与 USB 并发进入 FTL（主机侧矩阵） |
| `RISK-CLI-09` | 待上板 | 断言现场 CLI 自带泵，但 FTL 内断言仍可能重入；未上板 |

**Controller / host queue（4）**

| ID | 判定 | 用例 / 判定说明 |
|---|---|---|
| `RISK-CTRL-03` | 待决策 | 稀疏拓扑注释与实现相反，注释/实现二选一 |
| `RISK-CTRL-04` | 待后续 | `submit` 未校验 `buffer == NULL`；先改 controller 才可构造 |
| `RISK-HOST-20` | 待决策 | host 前端互斥需单点 claim 设计裁决 |
| `RISK-HOST-21` | 待后续 | 缺 host 命令超时 / 中止，需设计 |

**USB MSC / 主机侧（8）**

| ID | 判定 | 用例 / 判定说明 |
|---|---|---|
| `RISK-USB-02` | K | MSC 异步完成 / deferred / discard 需主机拔线或复位 |
| `RISK-USB-07` | K | `cmd_remaining` 在 `usb_msc.c` 内，HIL 只到 controller，无法从请求对象断言 |
| `RISK-USB-08` | K | 需主机早期回执 + 程序失败组合 |
| `RISK-USB-12` | K | 需主机发 0x91，PC 侧验证 |
| `RISK-USB-13` | K | 需两写槽在飞时 READ；槽状态构造未证明 |
| `RISK-USB-14` | K | 需主机 `blkdiscard` / `fstrim` |
| `RISK-USB-15` | 待决策 | UNMAP 未向主机广告，是否广告未定 |
| `RISK-USBC-03` | 待定位 | USB 盘间歇报无介质；主机侧采样 / 盘符问题，需 PC 侧 |

**平台 / 证据（2）**

| ID | 判定 | 用例 / 判定说明 |
|---|---|---|
| `RISK-PLAT-08` | 待定位 | `.noinit` 一次 HardFault，PC 落 newlib printf；故障镜像未定 |
| `RISK-UT-34` | N | 已上板 17/17；剩余 buffer NULL / 跨重启 / 多 run / USB 槽矩阵 / `cancel_flush` |

**cache 主机面（1）**

| ID | 判定 | 用例 / 判定说明 |
|---|---|---|
| `RISK-USBC-01` | K | deferred sense 语义需主机侧观察 |

> 台账合计 **22** 条：N 1 / K 12 / 状态词 9。已闭合的平台守卫
> `RISK-PLAT-13` / `RISK-PLAT-14` 与信息项 `RISK-USB-09` / `RISK-USB-10` /
> `RISK-USB-11` / `RISK-CTRL-01` / `RISK-PLAT-06` / `RISK-PLAT-07` 只在
> `known_risks.md` 维护。

---

## 8. 维护规则

- **新增/删除用例**：同批更新 §2 对应模块表 + 测试源注释；不得只改源或只改文档。
  全量用例清单不写进本文，归 `tests/unit/test_hil.c`。
- **风险状态变化**：只改 `docs/design/known_risks.md`；§7 仅在映射变化时改，
  **不写状态词**。
- **覆盖判定变化**：只改 §7.2 台账。
- **可测性变化**（夹具/硬件/主机矩阵）：更新 §6；不要把「能测」写成「已测」。
- **运行结果**：写 `build/test-results/` 与 `known_risks.md` 历史；设计文档不维护
  验收快照。
- **禁止**：§7 写状态词；§2 复制实现细节；文档头写「N 函数 / M 次运行」快照。
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
- **结构检查**：`python3 tools/check_hil_ut_doc.py` 与 `--self-test` 必须在改动后通过；
  checker 守本文的章节、模块小节、风险引用、负例套件路径与注册用例覆盖。
