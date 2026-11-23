# FC-NAND 单元测试设计（板端）

> 状态：活跃设计文档。本文是 **test design**，按被测模块（test item）组织：
> 每个模块有哪些设计要点、用什么判据覆盖、缺什么。
> 用例名只作代表，完整清单以 `tests/unit/test_fc_*.c` 的注册表为准；
> 运行结果见 `build/test-results/`，风险状态见 `docs/design/known_open_items.md`。
> 范围：`src/fc/` + `src/nand/` 的 FC-NAND 层；FTL UT 见 `docs/design/ftl_ut_design.md`。
> 关联：`docs/guides/testing_guide.md` §12（负例与故障注入框架，跨层）、
> §3（L1 FC）。

---

## 0. 文档定位与单一事实源

本文回答三个问题：

1. 被测模块各自的设计要点（不变量、契约、错误语义）是什么；
2. 这些要点靠哪些测试层、什么判据覆盖；
3. 哪些点当前没有覆盖，原因是什么。

下面这些信息**不在本文复制**：

| 信息 | 唯一事实源 | 本文怎么引用 |
|---|---|---|
| 用例是否存在、参数、断言 | `tests/unit/test_fc_pure.c` / `test_fc_data_path.c` / `test_fc_lifecycle.c` / `test_fc_negative.c` | §2 只写代表用例与判据，不列全量清单 |
| OP 操作码与编号 | `include/nand/onfi_1_0.h` | §2.9 只写覆盖策略，不抄 50 行表 |
| 风险状态、历史、是否闭合 | `docs/design/known_open_items.md`（未闭合 / 信息）+ `docs/design/known_closed_items.md`（已闭合） | §7 只写风险 → 模块追溯，不写状态词 |
| 负例覆盖判定（M/N/K） | 本文 §7.2 台账 | §7 只给 FC 层判定，其余层见各层文档 |
| 故障注入框架与 seam 语义 | `docs/guides/testing_guide.md` §12 | §4.3 只写本套件怎么用 |
| 未实现 / 未验证的设计项 | `docs/design/known_open_items.md` | §6 只给链接 |
| 每次运行结果与证据 | `build/test-results/` + `docs/guides/testing_guide.md` §8.7 | §5.5 只给目录与四件套 |
| 重构前版本 / 版本叙事 | `docs/archive/fc_nand_ut_pre_refactor_20261113.md` | 历史只查该快照 |

> 判据：读者要知道「某风险现在什么状态」，去 `known_open_items.md`；要知道「某条用例断言什么」，
> 看测试源。本文两处都不重复。

---

## 1. 被测对象与测试分层

### 1.1 测试项清单

顺序按模块依赖：命令/队列/池 → 计算引擎 → 数据路径 → NAND 执行 → 对外接口。

| 测试项 | 源文件 | 主要接口 | 测试层 |
|---|---|---|---|
| `fc_command` | `src/fc/fc_command.c` | `fc_command_validate`、`fc_cw_flat`、`fc_command_plane_span*`、`fc_cw_bitmap_is_plane_symmetric` | pure / data_path / negative |
| `fc_queue` | `src/fc/fc_queue.c` | `fc_submission_queue_*`、`fc_completion_queue_*` | pure |
| `fc_pool` | `src/fc/fc_pool.c` | `fc_command_alloc/release`、`fc_descriptor_*`、`fc_buffer_*`、`fc_get_resource_stats` | data_path / lifecycle / negative |
| `fc_crc_engine` | `src/fc/fc_crc_engine.c` | `fc_crc_compute`、`fc_crc_verify` | data_path / negative |
| `fc_ecc_engine` + `fc_bch_engine` | `src/fc/fc_ecc_engine.c`、`src/fc/fc_bch_engine.c` | `fc_ecc_*`、`fc_bch_*` | data_path |
| `fc_random_engine` | `src/fc/fc_random_engine.c` | `fc_random_seed_from_meta`、`fc_random_transform` | data_path |
| `fc_data_path` | `src/fc/fc_data_path.c` | `fc_data_path_encode`、`fc_data_path_process_read`、`fc_data_path_raid_recover` | data_path / negative |
| `fc_nand_dispatch` | `src/fc/fc_nand_dispatch.c` | `fc_nand_dispatch_submit*`、`fc_nand_dispatch_poll`、`fc_nand_dispatch_test_completion` | lifecycle / negative |
| `fc_nand_engine` | `src/fc/fc_nand_engine.c` | `fc_nand_init`、`fc_nand_enable_high_speed`、`fc_nand_test_read_parameter_page` | lifecycle / negative |
| `onfi_1_0` | `src/nand/onfi_1_0.c` | 参数页解码、uop 解释器、命令参数构造 | pure / lifecycle / negative |
| `fc.c` / `fc_interface` | `src/fc/fc.c`、`include/fc/fc_interface.h` | `fc_init`、`fc_submit*`、`fc_poll_completion*`、`fc_command_release`、`fc_get_cmd_data_ptr` | lifecycle / negative |
| `fc_raid_engine` | `src/fc/fc_raid_engine.c` | `fc_raid_*`（`CONFIG_FC_RAID=0`） | 不覆盖 |

> 测试项按「模块 + 公共接口」切，不按函数逐个列；内部 `static` helper 由所属模块的
> 公共接口间接覆盖。

### 1.2 四个测试层

| 层 | 根套件 | 环境 | 能证明什么 | 不能证明什么 |
|---|---|---|---|---|
| **pure** | `/fc-nand/pure/` | 栈上夹具，零硬件 | 纯函数契约、队列 FIFO、解码字段 | 资源池、真实命令生命周期 |
| **data_path** | `/fc-nand/data_path/` | `fc_init()` 池，无 NAND | 计算引擎与 data_path 编排的 round-trip | NAND 时序、命令下发 |
| **lifecycle** | `/fc-nand/lifecycle/` | 真 NAND | alloc → configure → submit → service → poll → release 全链路 | 多 target / 掉电 / 长时 soak |
| **negative** | `/fc-nand/negative/` | 池 + 部分真 NAND | 非法输入、release 守卫、故障注入 seam | debug 下的拒绝语义（用 release 档） |
| **PERF** | `/bench/speed/fc_nand_rw` | 真 NAND | 能不能跑完 + 错误计数 | 功能正确率（不是通过判据） |

- debug 构建 `ASSERT` 停机；release（`-DNDEBUG`）下 `ASSERT_REJECT` 返回错误码。
- negative 的 13 条 release-only 用例在 debug 下 `MUNIT_SKIP`，要验证拒绝语义必须跑 release UT。
- lifecycle、negative 中碰介质的用例与 `/bench/speed/fc_nand_rw` 会擦写原始块，破坏 live FTL 映射；
  FC-only 跑完必须 `format` + POR。

### 1.3 测试原则

1. **只在目标板跑**（R4）：`make tests UT=FC` 出镜像，串口 `test` 执行；不做 host 模拟。
2. **一个用例只钉一个契约**：断言必须能回答「比较的是哪两个值」；没有区分力的断言
   不算覆盖（C7 / C8）。
3. **负例必须能红**：非法输入、越界、UNC/CRC/META、池满、链锁、注入失败都要有明确判据；
   注入类还必须给出 `fi_hits` 增长等「故障确实发生」的证据。
4. **正常路径全绿 ≠ 全覆盖**：不等于全部 OP / 错误路径 / 多 target 已验证（R1）。

---

## 2. 按模块的测试设计

> 每个小节固定三段：**设计要点**（不变量 / 契约 / 错误语义）、**判据与代表用例**、**缺口**。
> 代表用例只用于定位测试意图，不是完整清单。

### 2.1 `fc_command` — 命令对象、校验、链

**设计要点**

- opcode 合法域是 `0..OP_COUNT-1`；SET/GET_FEATURES 是「无 desc 链」的内联例外。
- `cw_bitmap` 是 **flat 绝对编号**：plane 1 的 CW0 是 bit 4，不是 bit 0。
- desc 链的 MORE/END、命令状态、句柄与槽位映射必须在建链/释放两侧一致。
- 非法链（重复 id、混优先级、成环）必须在 release 档拒绝或有界截断。

| 设计点 | 判据 | 代表用例 | 缺口 |
|---|---|---|---|
| opcode 合法域 | 0..`OP_COUNT-1` 全部不误拒；非法值不静默通过 | `cmd_validate_op_range` | — |
| SET/GET 无 desc 例外 | 无 desc 链仍能通过校验 | `cmd_validate_set_get_no_desc` | — |
| flat CW 位图 | 位图展开后的 plane / cw / 链序与 desc 一致 | `dp_cw_bitmap`、`cw_write_plane1_only` | — |
| 链参数守卫 | 重复 id / 越界 id / 混优先级 → `FC_STATUS_INVALID_ARGUMENT`，队列占用不变 | `batch_arg_guards` | 多 target 组合 |
| 成环释放 | 成环 desc 链释放有界返回，不死循环 | `desc_cycle_release_bounded` | — |

### 2.2 `fc_queue` — SQ / CQ

**设计要点**

- SQ 是 FIFO：push/pop 顺序、回绕、peek 不消耗、满时 `QUEUE_FULL`。
- CQ 整项拷贝状态；只有 4 个 status slot（`CW_NUM=4`），超出填 `UNUSED`。
- 队列是 FC 内部结构，非法下标/越界由模块入口 `ASSERT` 守；host 输入由上层拒绝。

| 设计点 | 判据 | 代表用例 | 缺口 |
|---|---|---|---|
| SQ FIFO 与回绕 | head/tail 回绕后顺序仍正确 | `sq_push_pop`、`sq_wrap_around` | — |
| SQ 满 / peek | 满返回 `QUEUE_FULL`；peek 连查不消耗；空队列 offset 越界返回 INVALID | `sq_overflow`、`sq_peek`、`sq_peek_empty` | — |
| CQ FIFO / 满 | 整项拷贝；满返回 `QUEUE_FULL` | `cq_init`、`cq_push_pop`、`cq_overflow` | — |

### 2.3 `fc_pool` — 命令 / 描述符 / buffer 三池

**设计要点**

- 三池账本（command / descriptor / buffer）是 allocator 的「四本账」在 FC 侧的对应物；
  任何分配都必须有唯一释放点。
- `fc_command_release` 沿 desc 链归还 desc + buffer；已挂链 buffer 不能单独释放。
- 在飞命令释放必须拒绝；`fc_init()` 负责丢弃该命令并复位队列。
- desc 链成环时释放必须有界截断，不能死循环。

| 设计点 | 判据 | 代表用例 | 缺口 |
|---|---|---|---|
| 账本一致 | alloc 1 条 → `command_used +1`；release 后回基线，desc/buffer 不变 | `stats_consistency` | — |
| 释放归属 | 已提交命令立刻 release 被拒；`command_used` 不变 | `in_flight_release_guard` | — |
| desc 链释放 | 成环链有界返回，不重复归还 | `desc_cycle_release_bounded` | — |
| buffer 合法性 | 非法 `buffer_id` 提交 → 命令状态带错误位，恢复后释放 | `invalid_buffer_rejected` | — |
| buffer 归属四档 | `buf_own` 00/01/10/11 的 `buffer_used` 增量正确 | `buf_own_release` | — |

### 2.4 `fc_crc_engine` — CRC

**设计要点**

- CRC 覆盖 user 区；写侧按 `cmd->config` 决定是否写 CRC 字段，读侧按配置决定是否校验。
- `fc_crc_compute` 失败必须可被 seam 观测；`fc_crc_verify` 必须返回明确错误码。
- release 下 CRC 初始化失败会静默降级，属于当前未覆盖路径（`RISK-DP-28`）。

| 设计点 | 判据 | 代表用例 | 缺口 |
|---|---|---|---|
| 覆盖边界 | user / CRC 字段改动必检出；META / parity 改动必不检出 | `dp_crc_step_detect` | — |
| compute 失败 seam | `fc_crc_compute` 返 0；`fc_crc_verify` 报 `FC_CRC_RESULT_ERROR`；`fi_hits` 增长 | `crc_compute_fail_seam` | release 静默路径（`RISK-DP-28`） |
| CRC 关闭 | encode 不写 CRC 字段；读侧跳过步骤 3 | `dp_encode_crc_disabled` | — |

### 2.5 `fc_ecc_engine` / `fc_bch_engine` — BCH

**设计要点**

- BCH(m=13, t=4)：≤4 bit 错必须纠正；>t 必须失败原子，不误纠成「成功」。
- 解码结果必须逐 CW 上报 `ecc_bits`；命令级 `fc_get_cmd_ecc_max` 是聚合，不是替代。
- LDPC 分支当前只验证「RAW 跳过 ECC」的编排，不证明 LDPC 算法。

| 设计点 | 判据 | 代表用例 | 缺口 |
|---|---|---|---|
| t=4 精确纠正 | 1..4 bit 错的数据必须恢复 | `dp_ecc_step` | 抽样非穷举；见 §6.2 |
| >t 失败原子 | 只钉「失败原子 / 纠正数 ≤ t」，不钉具体结果 | `dp_ecc_step`（`beyond-5`） | 数据误纠不在此例钉死 |
| 逐 CW 上报表 | `ecc_bits` 逐 CW 正确；命令级聚合与逐 CW 一致 | `dp_process_read_ecc_max` | — |
| 不可纠读 | CW1 破坏 → 命令 `IO_ERROR`，CW1=UNC，其余 READY | `dp_process_read_unc` | — |

### 2.6 `fc_random_engine` — 加扰

**设计要点**

- 加扰按 `fc_random_seed_from_meta(cw_meta)` 派生种子；同一数据 + 同一 meta 必须自反。
- 白化判据是统计的：段数、平均段长、长游程；统计判据**测不出周期性**（见 §6.3）。
- 加扰关闭时 encode 必须原样输出，不能「半开」。

| 设计点 | 判据 | 代表用例 | 缺口 |
|---|---|---|---|
| 自反性 | transform 后逆变换恢复原文 | `dp_scramble_whiten` | — |
| 白化度 | 全 0x00 / 0xFF / PAA 重复三种输入过统计阈值 | `dp_scramble_whiten` | 周期性盲区；见 §6.3 |
| 关闭路径 | random=OFF 时 encode 不加扰 | `dp_encode` | — |

### 2.7 `fc_data_path` — encode / process_read 编排

**设计要点**

- encode 按 `cmd->config` 决定是否加扰/CRC/ECC；步骤关闭时必须保持数据原样。
- `process_read` 逐 CW 返回状态；UNC / CRC / META 不能混淆。
- 掩码链的步骤 4 必须用 `paa_to_cw(desc->paa)`，不能按链走位。
- **步骤级 vs 管线级**：步骤级直接调 CRC / ECC / 加扰单 CW 原语，只借
  `test_cw_scratch()` 的 528B buffer，不建链、不需要 `cmd->config`；管线级建链后调
  `fc_data_path_encode` / `process_read`，用 `cmd->config` 表达开哪些步骤。
  判据：**验算法 → 步骤级；验编排 → 管线级**。已提交命令绝不能再手工调原语，
  否则 FC 会重复编码 ⇒ 数据损坏。

| 设计点 | 判据 | 代表用例 | 缺口 |
|---|---|---|---|
| encode 步骤开关 | CRC off 不写 CRC；random off 不加扰；cw 数 / META / parity 正确 | `dp_encode`、`dp_encode_crc_disabled` | — |
| 读正常路径 | encode → process_read → 全 READY + 原文恢复 | `dp_process_read_ok` | — |
| 错误归因 | UNC / CRC / META 分别置位，命令状态正确 | `dp_process_read_unc`、`dp_process_read_crc_error`、`dp_process_read_meta_error` | — |
| 掩码链 | mask=0x05 时按 desc PAA 取 CW，不按链走位 | `dp_process_read_masked_chain` | — |
| LDPC RAW | 两端跳过 ECC，数据原样往返 | `dp_process_read_ldpc_raw` | 不证明 LDPC 算法 |

### 2.8 `fc_nand_dispatch` / `fc_nand_engine` — 下发、完成、timing

**设计要点**

- 每 `(ch, ce, lun)` 有独立填充窗口与链锁：链未结束，HSQ 插不进来。
- completion 必须按 CID 配对；late / drop 不能导致命令永久悬挂。
- 引擎失败必须把选中 CW 全部标 FAIL；超时必须写 `FC_CMD_STATUS_TIMEOUT`。
- timing mode 切换失败必须回滚 Mode 0；用例结束必须恢复 Mode 5（否则污染后续套件）。
- **cache 语义**：ONFI 1.0 没有 multi-plane（Rev 1.0 全文 `plane` 0 次）；本器件
  MT29F4G08 另有 two-plane cache 读写。单 plane 写侧前 63 页 15h、末页 10h；读侧
  `START_SEQ → 31h ×62 → LAST`。双 plane 写侧首页 `..._FIRST`、中间 `..._CACHE`、
  末页 `..._END`；读侧 `START_SEQ_MP → SEQ_MP ×62 → LAST_MP`，每条后用 `06h P0/P1 E0h`
  分别输出两个 plane。
- **load/fetch 两段式**：生产读路径先加载（op 33/29，无 desc、无数据输出）再逐 CW 取数
  （op 8/41，带 desc）；两段必须紧邻。
- **META / 列读**：META 只读（op 11）回读 CW 前 4 字节，无 desc、不占 buffer、不进 ECC；
  显式列读（op 42）的列地址走命令 param 小端，够得到列 2048 的 spare 区。
- **流式背压**：`mixed_pipeline` 连续提交不等完成；每成功 4 条或本条失败才 poll + 取 CQ。
  在途命令顶到池上限 ⇒ 重试同一条；硬判据是「全块写读完成 + 逐 CW 校验 + 三池归零」。

| 设计点 | 判据 | 代表用例 | 缺口 |
|---|---|---|---|
| 链锁 | SQ 8 命令链未结束 → HSQ 写不进去；终值是 HSQ 写的值 | `chain_priority_lock` | — |
| completion 时序 | late 下一轮投递；drop 有限轮询内不投递且命令可 release | `completion_late_delivered`、`completion_drop_bounded` | — |
| 引擎失败归因 | 全选中 CW FAIL；TIMEOUT 状态位；8 条 desc 逐条 FAIL | `engine_fail_all_cws`、`engine_timeout_sets_status`、`fail_cw_attribution` | — |
| timing 回滚 | 注入 `set_timing(Mode5)` 失败 → 回滚 Mode 0；结束恢复 Mode 5 | `timing_rollback_on_set_timing_fail` | Mode 0 构建见 §5.4 / `RISK-UT-43` |
| 下发入口守卫 | NULL / 零 txn / 越界 opcode → `NAND_RESULT_INVALID_ARGUMENT` | `dispatch_enqueue_guards`、`nand_start_unbound_txn` | — |
| 状态查询 | READ_STATUS / READ_STATUS_ENHANCED 生命周期 + CW status | `status_query` | — |
| 单 plane cache | 31h 序列 + RANDOM；写侧 15h/10h；读回逐页一致 | `block_cache_ops` | — |
| 双 plane cache | 06h P0/P1 分别输出；两 plane 独立 pattern；跨 block 续读 | `block_cache_multi_plane`、`block_cache_multi_plane_cross_block` | — |
| load/fetch 两段式 | LOAD(33/29) → FETCH(8/41)；两段紧邻、逐 CW PAA 一致 | `block_load_fetch` | — |
| META / 列读 | op 11 只回读 4B META；op 42 按显式列地址读 4B | `block_read_meta`、`read_column` | — |
| 块生命周期 | 擦/写/读、multi-plane、内部搬移、块锁；逐页 / 逐 CW 校验 | `block_write_read`、`block_erase_verify`、`block_multi_plane`、`internal_data_move`、`block_lock_ops` | OTP / tight lock 见 §6.1 |
| 流式背压 | 全块写读 + 逐 CW 校验 + 三池归零；`retry` 只作调试证据 | `mixed_pipeline` | — |
| buffer 归属 | `buf_own` 四档 + 建链未提交不泄漏 | `buf_own_release` | — |

### 2.9 `onfi_1_0` — 参数页、uop、命令参数

**设计要点**

- 参数页解码：坏签名必须返回失败，但字段仍应填满（不能解引用空指针）。
- uop 解释器：JUMP 回跳、DATA_OUT 范围、零 timing 都是不可信输入，必须运行期拒绝。
- OP 数值表 `0..50`（`OP_COUNT=51`），槽位 20 是墓碑，不复用；事实源是
  `include/nand/onfi_1_0.h`。

| 设计点 | 判据 | 代表用例 | 缺口 |
|---|---|---|---|
| 参数页解码 | 合成 256B 页逐字段正确；坏签名返回 0 但字段填满 | `onfi_param_page_decode` | — |
| uop JUMP 守卫 | operand 回跳越界 → INVALID，不发生 WORD 下溢 | `uop_jump_operand_guard` | — |
| uop DATA_OUT 范围 | operand > 页缓冲 → INVALID，不越界读写 | `uop_data_range_guard` | — |
| 零 timing 守卫 | `DELAY_TIMING(tadl)` 且 `tadl_us=0` → INVALID | `uop_zero_timing_guard` | — |
| 参数页读失败 seam | 返回 `NAND_RESULT_OPERATION_FAILED` | `onfi_read_fail_seam` | 启动拒绝路径见 §6.1 |
| OP 覆盖 | 50 个数值 op 有定义；实际执行覆盖由 lifecycle/negative 用例决定 | `cmd_validate_op_range` | 部分 op 无独立用例 |

### 2.10 `fc.c` / `fc_interface` — 对外接口与生命周期

**设计要点**

- `fc.c` 是 FC 对 FTL/host 的唯一入口：alloc → configure → submit → poll → release。
- FTL 边界上，单 plane 位图 + two-plane op 必须拒绝；multi-plane 命令的 data ptr 不是单 CW 语义。
- 短链读、跨 target 批提交都是 host 可构造的边界，必须在 release 下返回错误码。

| 设计点 | 判据 | 代表用例 | 缺口 |
|---|---|---|---|
| FTL pair 守卫 | 单 plane 位图 + two-plane op → `FTL_ERROR`，`out_cmd` 为空 | `iface_release_guards` | 跨 die pair 在当前板不可构造 |
| data ptr 语义 | multi-plane 命令 `fc_get_cmd_data_ptr(cmd_id,cw)` 返回 NULL | `data_ptr_single_plane_only` | — |
| 短链读 | 1-CW 链读 4 KiB → `FTL_ERROR` | `short_chain_read_short` | — |
| 批提交跨 target | 批内 ch 不一致 → 整批拒绝 | `batch_cross_target_reject` | 多 target 硬件 |
| 命令生命周期 | alloc 后配置 desc；release 沿链归还；池归零 | `stats_consistency`、`buf_own_release` | — |

### 2.11 `fc_raid_engine` — 当前不覆盖

- `CONFIG_FC_RAID=0`（`include/fc/fc_interface.h`），功能代码存在但未验证。
- `dp_raid_recover_no_random` / `dp_generate_stripe_parity` 只在 RAID=1 编译；当前构建的 UT
  不覆盖 RAID 恢复与 stripe parity。
- 风险登记：`RISK-DP-05`。

---

## 3. 跨模块 / 系统级场景

模块内用例只证明单点契约；下面这些场景跨多个模块，必须单独设计：

| 场景 | 涉及模块 | 判据 | 代表用例 |
|---|---|---|---|
| data_path 管线 | `fc_data_path` + CRC / ECC / random | encode → process_read → 原文恢复；错误位置位 | `dp_process_read_ok`、`dp_process_read_*` |
| 真实 NAND 生命周期 | `fc.c` + dispatch + engine + onfi | alloc → configure → submit → service → poll → release 全链路；数据逐 CW / 逐页一致 | `block_write_read`、`block_erase_verify` |
| cache / multi-plane | dispatch + engine + data_path | 31h 序列、15h/10h、双 plane 06h P0/P1；两 plane 独立校验 | `block_cache_ops`、`block_cache_multi_plane` |
| 两段式读 / META / 列读 | dispatch + engine + onfi | LOAD→FETCH 紧邻；op 11 只回 4B META；op 42 列地址精确到 4B | `block_load_fetch`、`block_read_meta`、`read_column` |
| 块生命周期 / 锁 | `fc.c` + engine + onfi | 擦写读 / multi-plane / 内部搬移逐页一致；LOCK→UNLOCK 可逆 | `block_write_read`、`block_erase_verify`、`block_multi_plane`、`internal_data_move`、`block_lock_ops` |
| 并发 / 池压 | dispatch + pool + queue | 链锁生效；池满可重试；全块完成 + 三池归零 | `chain_priority_lock`、`mixed_pipeline` |
| release 守卫 | `fc.c` + pool + dispatch | 非法输入返回具名错误码；队列/池不变 | `/fc-nand/negative/` |
| 速度基准 | FC 原始块 + engine | 只判「跑得完 + 错误计数」；不作功能通过率 | `/bench/speed/fc_nand_rw` |

---

## 4. 测试技术、夹具与 seam

### 4.1 技术矩阵

| 技术 | 判据 | 落点 |
|---|---|---|
| **Directed / contract** | 一个状态机/接口契约一条用例 | `init_sequence`、`status_query`、`block_write_read` |
| **Negative / guard** | 非法输入/错误路径必须被拒绝或置错误位 | `dp_process_read_*`、`/fc-nand/negative/` |
| **Boundary / parameterized** | 边界值成组覆盖，不写死单点；不同分支/期望参数化，同一断言不同输入迭代 | `dp_crc_step_detect`、`dp_ecc_step`、`dp_cw_bitmap`、`dp_encode`、`dp_scramble_whiten` |
| **Seeded random** | 地址由 `(seed, 用例名, 抽样序号)` 决定，可复现 | data_path `l2_*`、lifecycle `l3_*` |
| **Fault injection** | 注入点 → 预期错误位/返回值，且 `fi_hits` 可证 | data_path bit 注入；negative `fi_arm` / `fi_wrap` |
| **Release-only guard** | debug 停机、release 返回错误码 | 13 条 negative 用例 |
| **Reboot / crash point** | 真 POR 后映射/账本一致 | 不在 FC UT；见 `ftl_ut_design.md` / `known_open_items.md` |
| **Stress / perf** | 只作基准，不作功能通过率 | `mixed_pipeline`、`/bench/speed/fc_nand_rw` |

### 4.2 夹具与结束不变量

- `test_setup` / `test_tear_down` 初始化/回收 FC 资源池；不碰 NAND 的层不建 NAND 事务。
- 每个使用夹具的用例 teardown 至少检查：
  - `INV-POOL`：三池 `used == 0`；
  - `INV-CQ`：CQ 空、无未取完成项；
  - `INV-CMD`：命令已 `fc_command_release`；
  - `INV-BCH`：BCH 全局状态未被重复初始化/污染。
- `g_bch` 跨用例持续存在，`fc_ecc_init_bch` 只在首次分配时初始化。
- 释放统一走 `fc_command_release()`；不要对已挂链 buffer 单独 `fc_buffer_release`。
- **lifecycle 夹具有固定约定**：`test_setup` 内重申 `fc_nand_engine_set_armed(1U)`；建链走
  `ftl_cmd_configure_descriptors()`；地址固定 `ch=0 ce=0 lun=0`，page=0..63，block / plane
  用例体内抽样，内部搬移目标块另抽且 `src ≠ dest`。读回校验分两档：整段读按地址重算
  （`test_verify_page_paa`），掩码读只看链上实际 desc（`verify_chain_paa`）。擦除判据只能是
  逐 CW `EMPTY`，不能用聚合 status。
- **negative 夹具**：每条用例的 setup 与 teardown 都必须 `fi_reset()`，否则注入状态跨用例
  静默污染。13 条 release-only，11 条两档都跑。真 NAND 边界用例（`data_path_encode_fail` /
  `data_path_read_fail` / `batch_encode_fail` / `invalid_buffer_rejected`）走
  `test_submit_wait` 或 `fc_service`；其余多数只碰软件状态。
- **pure 夹具**是零 `fc_init()` 的栈上对象；`cmd_validate_*` 是冒烟用例，用来拦住
  「以后加了过严内层 ASSERT」。

### 4.3 可测性 seam

| seam | 用途 |
|---|---|
| `test_cmd_build` / `test_cmd_try_build` | 建链；后者资源申请失败返回 NULL 而非 abort |
| `test_submit_wait` | 提交 + 轮询完成（命令仍归调用者） |
| `fc_poll_completion` / `fc_poll_completions` | 单条/批量取 CQ |
| `fc_get_resource_stats` | 三池账本 |
| `test_cw_scratch` | 步骤级 528B buffer |
| `fc_nand_test_read_parameter_page` | 参数页读测试 seam |
| `fc_nand_dispatch_test_completion` | completion 策略表 seam，只算状态位、不提交 NAND |
| `fc_test_complete_nonactive` | 非 ACTIVE 命令完成拒绝 seam |
| `fi_arm` / `fi_hits` / `fi_take` / `fi_reset` | 故障注入控制与命中计数 |
| `fi_wrap.c` | 链接期包装测试符号（`-Wl,--wrap`），生产源码不动 |
| `l2_*` / `l3_*` | 地址抽样与复现 |
| `--iterations` / `--seed` | 覆盖旋钮与复现 |

> FC UT 不新建 mock 对象；能测的必须是生产 API 可直接驱动的部分，或
> `FC_NAND_TEST_ENABLE` 下显式存在的测试 seam。故障注入属于后者，不是生产路径。

### 4.4 地址抽样与复现

- **默认种子每次运行变化**：`ut_main.c` 的弱符号 `munit_platform_seed()` 用
  `delay_time_now_us64()` 的 SysTick 微秒数取高低 32 位异或；munit 开场打印 seed。
- **复现契约 = (seed, 用例名, 第几次抽样)**；抽到的值必须经 `test_trace_*` 打出。
- **抽样窗口**：lifecycle 写介质，block ∈ `[BLOCKS_PER_PLANE*30/100, BLOCKS_PER_PLANE*70/100]`
  避开固定元数据区，page / CW 用例抽，plane 可由 `l3_plane_draw` 抽或由 multi-plane op
  派生；data_path 不写介质，使用整段 `[0, BLOCKS_PER_PLANE)` / `[0, PAGES_PER_BLOCK)`，
  plane 由调用者钉死。
- **盐**：`munit_rand_*` 每次 exec 前重置回 `runner->seed`，且不带 salt ⇒ 必须
  `rand ^ hash(用例名) ^ hash(用途标签)`；data_path 的 `l2_rand` 末尾加 murmur3 `fmix32`
  终混（DJB2 低位退化）；lifecycle 的 `l3_rand` 当前不加终混，但复现契约相同。
- **覆盖旋钮是 `--iterations N`**：只乘 `MUNIT_TEST_OPTION_NONE`；`SINGLE_ITERATION`
  被硬压成 1；报告条数不变。

### 4.5 真 NAND 安全模型

- lifecycle / negative 中碰介质的用例与 `/bench/speed/fc_nand_rw` 会擦写原始块，
  破坏 live FTL 映射。
- FC-only 跑完必须 `format` + POR，不能与裸 `test` / FTL 状态混用。
- `/bench/speed/fc_nand_rw` 从 per-plane 块 50% 处向上选 100 个 good pair（不足则回扫 98..），
  约 25 MiB；只判完成与错误计数。
- **池在可缓存内存**：命令/描述符池落默认 `.bss`（AXI_RAM，Normal Write-Back）。SWD 读内存
  绕过 D-Cache，调试器看到的池内容可能陈旧；用 CPU 侧打印或先 `SCB_CleanDCache()`。
- **断言失败现场保留**：裸机 ASSERT 后卡在 `while(1)`，不会继续后续用例；一个 test 函数
  只验一个行为。
- **生命周期插桩**：`test_fc_lifecycle.c` 的 `run_lifecycle` 上方 `#if 0` 快照/差异打印块
  默认不编译；排查池泄漏时再打开。

---

## 5. 构建、运行与 SKIP

### 5.1 构建矩阵

| 档 | 命令 | 产物 | 断言/负例语义 |
|---|---|---|---|
| debug | `make tests UT=FC` | `build/debug/bin/debug_ut.elf` | `ASSERT` 停机；13 条 release-only 负例 `MUNIT_SKIP` |
| release | `make tests BUILD_TYPE=release UT=FC` | `build/release/bin/release_ut.elf` | `-O2 -DNDEBUG`；`ASSERT` 裁空；`ASSERT_REJECT` 返回错误码；24 条负例执行 |

- `FC_NAND_TEST_ENABLE=1` 时链接 `linker/stm32h743xx_flash_ut.ld`；生产镜像仍用生产链接脚本。
- `UT=FC` / `UT=FTL` / `UT=HIL` 共用产物名；烧写前确认最后一次构建是 `UT=FC`。
- 串口横幅时间戳必须与构建时间一致；「烧了旧镜像」的判据是横幅。
- ⚠ `make tests` 只证链接，不证正确（R2）；碰建链/位图/地址三类量必须上板。

### 5.2 运行

```text
test --list
test /fc-nand/pure/                 # 纯逻辑，安全
test /fc-nand/data_path/             # 只碰 buffer，安全
test /fc-nand/lifecycle/             # 真 NAND，破坏性
test /fc-nand/negative/              # debug 13 SKIP；release 执行拒绝语义
test --log-visible info --iterations 5 /fc-nand/lifecycle/
test --log-visible info /bench/speed/fc_nand_rw   # 破坏性，单独跑
test                                  # 全套；不加过滤串才是全套
```

- 套件前缀必须带尾斜杠；`/fc-nand/` 由 `ut_run_all()` 展开为全部叶子套件。
- `--seed N` 复现；`--iterations N` 只乘 `MUNIT_TEST_OPTION_NONE`。

### 5.3 SKIP 语义

| 条件 | SKIP 用例 | 说明 |
|---|---|---|
| `CONFIG_NAND_TIMING_MODE != 0`（默认 Mode 5） | `init_sequence`、`feature_write_read`、`chain_priority_lock` | 依赖「启动低速、运行高速」的自洽性；Mode 0 构建会执行 |
| debug UT（`!NDEBUG`） | 13 条 release-only 负例 | `ASSERT_REJECT` 在 debug 停机，无法用 munit 断言 |
| release UT | 无额外 SKIP | 仅保留 Mode 5 的 3 条 lifecycle SKIP |

### 5.4 Mode 0 专项

```text
make clean && make tests UT=FC CFLAGS_EXTRA="-DCONFIG_NAND_TIMING_MODE=0"
```

- 启动日志出现 `high-speed mode disabled (CONFIG_NAND_TIMING_MODE=0)`。
- **当前套件在 Mode 0 下未验证**：`timing_rollback_on_set_timing_fail` 的首断言要求
  `fc_nand_enable_high_speed(0) != NAND_RESULT_OK`，而 Mode 0 分支恒返回 `NAND_RESULT_OK`
  ⇒ 静态必红。该差异登记为 `RISK-UT-43`（`待测试源`），测试源由用户维护（R4）。
- 旧 Mode 0 快照不能作为当前套件判据；Mode 0 是专项回归，不是默认档。
- `CFLAGS_EXTRA` 不参与对象目录命名，切换 Mode 0 / Mode 5 必须 `make clean`。
- 跑完 live 映射已被原始块擦写破坏；下次启动出现
  `[L2P] region N primary unreadable — backup OK` 属预期回退，继续 H2testw 前必须 `format` + POR。

### 5.5 日志与证据

- 默认：用例名 + `[ OK ]/[ SKIP ]` + 耗时。
- `--log-visible info`：`[EV]` 前后对比；`--log-visible debug`：完整 inputs/outputs。
- `--log-project info/debug`：FC/NAND 项目日志（默认关闭）。
- 每轮一个目录：`build/test-results/<YYYYMMDD-HHMMSS>_<build>_<target>/`，含 `run.log`
  与 `meta.txt`；用 `sh/test_run_dir.sh [debug|release] [debug_ut|release_ut]` 创建。
- `meta.txt` 四件套：`commit`、`image` 横幅时间戳、`command`、`result`；原始日志不进版本库。

---

## 6. 不覆盖与判据盲区

### 6.1 不覆盖项

- `CONFIG_FC_RAID=0`：RAID 恢复与 stripe parity 不编译（`RISK-DP-05`）。
- LDPC 完整解码：只测 `dp_process_read_ldpc_raw` 的跳过路径（`RISK-DP-15` / `RISK-DP-27`）。
- CRC 初始化失败的 release 静默路径（`RISK-DP-28`）。
- 多 target / 多 LUN / per-bank 初始化（`RISK-NANDP-01` / `RISK-NANDP-04` / `RISK-NANDP-05`）；当前板单 target。
- 多 target 相关的负例分支：`iface_release_guards` 的跨 die pair 在本板不可构造；
  `sync_submit_wait` 超时上界未覆盖。
- Mode 0 专项：当前套件未验证（`RISK-UT-43`）。
- OTP 族、`block_lock_tight`、真 POR、长时 soak、USB/HIL/FTL 行为：不在 FC UT。
- 性能：`/bench/speed/fc_nand_rw` 是基准，不作功能通过率。

### 6.2 `dp_ecc_step` 的判据边界

`dp_ecc_step` 验的是 **BCH(m=13, t=4) 的实现**，不是「算法对不对」：≤t 个错的唯一译码由代数
保证；测试守的是 BM/Chien/Forney、位翻转落点、`errloc` 映射、parity 布局。六处缺口：

| # | 缺口 | 具体 |
|---|---|---|
| ① | 抽样非穷举 | 每 case 每轮只抽 1 组位置；空间 `C(4160,4) ≈ 10¹³` 量级 |
| ② | 抽样有偏 | bit-within-byte 只覆盖少数偏移 ⇒ 只错在 bit 7 的实现 bug 看不见 |
| ③ | `beyond-5` 不钉结果 | OK 分支只验 `ecc_bits ≤ 4`、不验数据 ⇒ 误纠被容忍 |
| ④ | parity 区是「软」的 | `p_k > 0` 时只比数据区 520B；解码器刻意不回写 parity 字节 |
| ⑤ | 单一信道模型 | 只有干净码字 + 均匀随机位错，不含突发/相关错 |
| ⑥ | 同源编解码 | 用 `fc_data_path_ecc_encode` 造码字，与解码同族 ⇒ 共享约定 bug 仍能通过 |

提高顺序：定向补 bit-within-byte / 跨字节 / 突发错 / `errloc` 边界；再用 `--iterations` 加量；
最后用独立实现的 KAT 比 parity 字节。

### 6.3 `dp_scramble_whiten` 的统计盲区

- 统计：一趟流式扫描维护 `cur` / `run`，段切换时结算 `hist[cur][桶]`，循环结束必须补收尾；
  `≥16` 钳位进第 15 桶，不丢弃（全 0xFF / PAA 重复正是要治的长游程）。
- 判据：段数 ≈ 2048、平均段长 ≈ 2、长游程 ≤ 3；`mean_x100 ∈ [170,240]` 双侧。低了 =
  被强制交替（周期 2）；高了 = 长游程/偏置（加扰没起作用）。
- 盲区：统计量测不出周期性。反例 `00110011…`（每段恒长 2）三条判据全过，但不是随机；
  实际加扰器输出由 LFSR 驱动，`all-0x00` 的输出就是裸 LFSR 最大长度序列。
- 判据的理论背景见 `docs/design/fc_nand_design.md` §5.4。

---

## 7. 风险追溯（副轴）

> 风险是**追溯维度**，不是本文的组织轴。状态、历史、是否闭合见
> `docs/design/known_open_items.md`；负例判定台账（M/N/K）见 §7.2。
> §7.1 只保留「判据非显然」的 FC-NAND 风险。

| RISK | 涉及测试项 | 判据 | 代表用例 |
|---|---|---|---|
| `RISK-UT-42` | `fc_nand_engine` | 注入 `set_timing(Mode5)` 失败→回滚 Mode 0；结束前恢复 Mode 5 | `timing_rollback_on_set_timing_fail` |
| `RISK-UT-43` | `fc_nand_engine` | Mode 0 构建下该用例断言与实现契约相反，当前未验证 | `timing_rollback_on_set_timing_fail` |
| `RISK-DP-13` | `fc_ecc_engine` / `fc_bch_engine` | t=4 边界注入 1..4 bit 错→纠正数与数据恢复同时正确 | `dp_ecc_step` |
| `RISK-DP-28` | `fc_crc_engine` | 注入 CRC compute 失败→`fc_crc_verify` 报错，`fi_hits` 增长 | `crc_compute_fail_seam` |
| `RISK-TOP-03` | `onfi_1_0` | 注入参数页读失败→返回 `NAND_RESULT_OPERATION_FAILED` | `onfi_read_fail_seam` |
| `RISK-IF-07` | `fc_nand_dispatch` | 链未结束时 HSQ 插不进来 | `chain_priority_lock` |
| `RISK-POOL-01` | `fc_pool` | 在飞命令 release 被拒绝，`command_used` 不变 | `in_flight_release_guard` |
| `RISK-SCH-08` | `fc.c` / `fc_interface` | multi-plane 命令的单 CW data ptr 返回 NULL | `data_ptr_single_plane_only` |
| `RISK-NANDP-03` | `fc_nand_engine` | Mode 5 运行期 UNC 只观测，不作 UT 通过判据 | `/bench/speed/fc_nand_rw` |
| `RISK-DP-05` | `fc_raid_engine` | RAID 路径当前不编译、不覆盖 | —（RAID=1 专用用例） |

### 7.2 负例判定台账（FC-NAND 层）

> 本表只列**当前未闭合**的 FC-NAND / NAND 器件风险（索引状态取自
> `docs/design/known_open_items.md`）。**M** = 有能变红的负例且上板通过；**N** = 注入点 /
> 触发步骤 / 断言已从源码确认、只差上板；**K** = 当前不可构造（写明原因）；非负例项保留
> 其状态词。**信息**条目只在 `known_open_items.md` 维护；**已闭合**条目只在 `known_closed_items.md` 维护。
> 跨层框架（注入 API、宏隔离、验收）见 `docs/guides/testing_guide.md` §12。

**FC 资源池（1）**

| ID | 判定 | 用例 / 判定说明 |
|---|---|---|
| `RISK-POOL-12` | 待议 | 描述符池按单 plane 口径估算；满额双 plane 并发的上限需裁决 |

**数据面（3）**

| ID | 判定 | 用例 / 判定说明 |
|---|---|---|
| `RISK-DP-15` | K | LDPC 未启用；切 LDPC 构建才可构造 |
| `RISK-DP-27` | K | LDPC 未启用；解码栈超 2 KiB 栈预算 |
| `RISK-DP-28` | N | `crc_compute_fail_seam`：注入 CRC 计算失败→`fc_crc_verify` 报错、`fi_hits` 增长（CRC init 失败的 release 守卫仍待做） |

**主循环 / 调度 / CQ（2）**

| ID | 判定 | 用例 / 判定说明 |
|---|---|---|
| `RISK-SCH-03` | 待决策 | CQ 满时丢完成项；阻塞 / 拒绝二选一需裁决 |
| `RISK-SCH-06` | K | 断言终端路径再次断言，需制造池账分叉，风险高 |

**NAND 器件 / PHY（3）**

| ID | 判定 | 用例 / 判定说明 |
|---|---|---|
| `RISK-NANDP-05` | K | 多 target timing 共享单套 HAL 句柄，需多 target 硬件 |
| `RISK-NANDP-08` | K | timing 表部分字段无消费者；改动需板端时序回归，非负例 |
| `RISK-NANDP-17` | K | 需启用 on-die ECC 才能构造 `tR_ECC` 超时 |

**NAND / FMC 时序与板端读完整性（3）**

| ID | 判定 | 用例 / 判定说明 |
|---|---|---|
| `RISK-NAND-05` | K | 需芯片物理坏页；当前介质无 |
| `RISK-NAND-06` | 待定位 | 运行期 BAD 标记来源未定（Mode 5 时序 / 同页二次编程 / 边缘块），需板端复现 |
| `RISK-RD-01` | 待定位 | 单次不可纠读的 `CRC|MATA` 事件，需复现与映射链路定位 |

**UT 测试源（1）**

| ID | 判定 | 用例 / 判定说明 |
|---|---|---|
| `RISK-UT-43` | 待测试源 | Mode 0 构建下 `timing_rollback_on_set_timing_fail` 的断言与实现契约相反，测试源待同步 |

> 台账合计 **13** 条：N 1 / K 7 / 状态词 5。HIL / USB / CLI 侧的负例见
> `docs/design/hil_ut_design.md` §7.2；FTL 侧见 `docs/design/ftl_ut_design.md` §7.2。

---

## 8. 维护规则

- **新增/删除测试项**：同批更新 §1.1 与对应模块小节；不要新增全局用例表。
- **新增/删除用例**：改测试源；只有当「设计要点或判据」变化时才改本文。
- **风险状态变化**：只改 `docs/design/known_open_items.md`；§7 只在映射变化时改。
- **覆盖判定变化**：只改 §7.2 台账。
- **地址抽样契约**：改抽样顺序 / 盐 / `fmix32` / `munit_platform_seed()` 会改变
  seed→地址映射；必须同批更新 §4.4 与测试源注释。
- **OP 表**：`include/nand/onfi_1_0.h` 是唯一事实源；`OP_COUNT` / 槽位变化时只改 §2.9 的说明。
- **禁止**：写运行快照、写风险状态、复制全量用例清单。
- **一致性检查**：`python3 tools/check_fc_nand_ut_doc.py`，输出 `fc_nand_ut_doc: OK` 才算过。
- **测试源按 R4 由用户维护**；新增 mock / 注入前先说明为什么生产 API 不可直接驱动。
- **历史版本**：查 `docs/archive/fc_nand_ut_pre_refactor_20261113.md`；本文不写版本叙事。
