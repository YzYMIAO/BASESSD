# FC-NAND 单元测试设计（板端）

> 状态：2026-11-13 13:37:20 镜像；`test` 全套 **61 OK + 3 SKIP（64 次运行）**；其中 `/fc-nand/` **59 OK + 3 SKIP**、`/bench/speed/fc_nand_rw` **2 OK**，
> 总 27.95 s。3 条 SKIP 是 Mode 5 下的按设计跳过，不是失败（见 §6.4）。
> 范围：`src/fc/` + `src/nand/` 的 FC-NAND 层。FTL UT 见 `docs/design/ftl_ut_design.md`；
> HIL UT 见 `docs/design/hil_ut_design.md`。
> 关联：`docs/design/known_iss.md` ISS-UT-02（已闭合）；`docs/design/known_risks.md`
> RISK-DP-* / RISK-NANDP-* / RISK-IF-07。
> 分层坐标：`docs/design/testing_guide.md` §3 的 **L1 FC**。

---

## 0. 单一事实源

本文只维护「FC-NAND UT 怎么设计、用例契约是什么」。下面这些信息**不在本文复制**：

| 信息 | 唯一事实源 | 本文怎么引用 |
|---|---|---|
| 用例是否存在、参数是什么 | 本文 §4 + `tests/unit/test_fc_pure.c` / `test_fc_data_path.c` / `test_fc_lifecycle.c` | §4 只写契约与覆盖点 |
| 风险状态、历史、是否闭合 | `docs/design/known_risks.md` | §5 只写 `RISK-ID → 用例` 映射，**不写状态词** |
| 未实现 / 未验证的设计项 | `docs/design/known_iss.md` | §7 只给链接 |
| 每次运行结果与证据 | `build/test-results/` + `docs/design/testing_guide.md` §8.7 | §9 只留最近快照 |
| 分层与跨层测试流程 | `docs/design/testing_guide.md` | 本文沿用其 L0~L6 坐标 |
| 重构前版本 / 版本叙事 | `docs/archive/fc_nand_ut_pre_refactor_20261113.md` | 历史只查该快照，本文不写 |

> 判据：读者要知道「某风险现在什么状态」，去 `known_risks.md`；要知道「这条用例断言
> 什么」，看测试源注释。本文两处都不重复。

---

## 1. 目的与原则

1. **只在目标板跑**（`CLAUDE.md` R4）：`make tests UT=FC` 生成镜像，串口 CLI `test`
   执行；不做 host 模拟。
2. **三层按依赖面分**：
   - **pure**：零硬件依赖，纯函数测试，栈上夹具、零 `fc_init()`；
   - **data_path**：需 `fc_init()` 初始化资源池，但不碰 NAND；在 buffer 层做 round-trip；
   - **lifecycle**：真实 NAND 执行完整命令生命周期（alloc → configure → submit → service →
     poll → release）。
3. **测试源与生产同仓**：`tests/unit/test_fc_{pure,data_path,lifecycle}.c` 与 `fc_test_common.*`；不建 mock
   FTL、不绕过 FC 公共 API。
4. **一个用例只钉一个契约**：判据必须能回答「断言比较的是哪两个值」；没有区分力的
   断言不算覆盖（C7 / C8）。
5. **负例必须能红**：非法 opcode、越界、UNC/CRC/META 错误、池满、链锁等必须有明确判据。
6. **真 NAND 安全模型**：lifecycle 与 `/bench/speed/fc_nand_rw` 会擦写原始块，破坏 live FTL 映射；
   FC-only 跑完必须 `format` + POR，不能与裸 `test`/FTL 状态混用。
7. **状态如实**：61 OK + 3 SKIP（含 `/bench/speed`）是正常路径回归，不等于全部 OP/错误路径/多 target 已覆盖；
   剩余项见 §7（R1）。

---

## 2. 测试架构

### 2.1 层与套件

| 层 | 根套件 | 环境 | 内容 | 破坏性 |
|---|---|---|---|---|
| **pure** | `/fc-nand/pure/` | 栈上夹具，零硬件 | command / queue / completion queue / ONFI 参数页解码 | 否 |
| **data_path** | `/fc-nand/data_path/` | `fc_init()` 池，无 NAND | 13 条 / 32 runs（RAID=1 时 15 条 / 34 runs）：CRC / ECC / 加扰 / encode / process_read / cw_bitmap | 否（只碰 buffer） |
| **lifecycle** | `/fc-nand/lifecycle/` | 真 NAND | 27 个 block 级场景（命令生命周期 / cache / multi-plane / META / 列读 / 块锁 / 背压） | 是 |
| **PERF** | `/bench/speed/fc_nand_rw` | 真 NAND | FC 原始块 25 MiB 读写，normal/cache 两档 | 是 |

```text
/fc-nand/
  pure/        11 条：command / submission queue / completion queue / ONFI decode
  data_path/   13 条（RAID=1 时 15 条）：步骤级 + 管线级 + cw_bitmap
  lifecycle/   27 条：真实 NAND 命令生命周期
/bench/speed/
  fc_nand_rw   1 条，normal / cache 两档
```

- 套件前缀**必须带尾斜杠**：`/fc-nand/lifecycle/` ✅；`/fc-nand/lifecycle` ❌。
- `test /fc-nand/` 由 `ut_run_all()` 展开为全部叶子套件路径，可正常使用；跑全套也可用
  **不带过滤串**的 `test`。

### 2.2 测试技术矩阵

| 技术 | 判据 | 本仓落点 |
|---|---|---|
| **Directed / contract** | 一个状态机/接口契约一条用例 | `init_sequence`、`status_query`、`block_write_read` |
| **Negative** | 非法输入/错误路径必须被拒绝或置错误位 | `dp_process_read_unc`、`dp_process_read_crc_error`、`dp_process_read_meta_error`、`fail_cw_attribution` |
| **Boundary / parameterized** | 边界值成组覆盖，不写死单点 | `dp_crc_step_detect`、`dp_ecc_step`、`dp_encode`、`dp_process_read_ok`、`dp_cw_bitmap` |
| **Seeded random** | 地址由 `(seed, 用例名, 抽样序号)` 决定，可复现 | data_path 地址抽样、lifecycle 14 条随机化用例 |
| **Fault injection** | 注入点 → 预期错误位/返回值 | `dp_crc_step_detect` / `dp_ecc_step` 的 bit 注入；`fail_cw_attribution` 关放行闩锁 |
| **Reboot / crash point** | 真 POR 后映射/账本一致 | **不在 FC UT**；见 `ftl_ut_design.md` / `known_risks` |
| **Stress / perf** | 只作基准，不作功能通过率 | `mixed_pipeline`（背压）；`/bench/speed/fc_nand_rw` |

### 2.3 覆盖模型（CP-*）

| 覆盖点 | 要覆盖什么 | 典型用例 |
|---|---|---|
| `CP-LOGIC` | 命令校验、SQ/CQ FIFO/回绕/满、ONFI 参数页解码 | `cmd_validate_*`、`sq_push_pop`、`sq_wrap_around`、`cq_*`、`onfi_param_page_decode` |
| `CP-DP` | CRC / ECC / 加扰 / 编解码管线的数据变换 | `dp_crc_step_detect`、`dp_ecc_step`、`dp_scramble_whiten`、`dp_encode`、`dp_process_read_*` |
| `CP-CHAIN` | desc 链几何、flat CW 位图、plane 展开 | `cw_bitmap`、`dp_encode`、`dp_process_read_masked_chain` |
| `CP-OP` | 命令生命周期、完成项 CID 配对、状态字 | `init_sequence`、`status_query`、`mixed_pipeline` |
| `CP-MEDIA` | 真 NAND 擦/写/读/cache/multi-plane/META/列读/块锁 | `block_*`、`read_column`、`internal_data_move` |
| `CP-ERR` | UNC / CRC / META / FAIL_CACHE / 下发失败归因 | `dp_process_read_*`、`fail_cw_attribution` |
| `CP-CONC` | 链锁、批量提交/收取、池满背压 | `chain_priority_lock`、`mixed_pipeline` |
| `CP-LEDGER` | command / descriptor / buffer 三池归零、CQ 空 | 每个 data_path/lifecycle 用例的 teardown |

> `CP-*` 是**本仓自拟**的功能覆盖点，不是代码覆盖率；裸机目标上不以行覆盖作为主指标。

### 2.4 设计要点

#### 2.4.1 地址抽样与复现契约

data_path / lifecycle 的地址不挂 `MunitParameterEnum`（表达不了几百宽的窗口），而是在用例体内
按种子抽：

- **窗口**：lifecycle 写介质，block ∈ `[BLOCKS_PER_PLANE*30/100, BLOCKS_PER_PLANE*70/100]`
  避开固定元数据区，page/CW 按用例抽，plane 可由 `l3_plane_draw` 抽或由 multi-plane op
  派生；data_path 不写介质，使用整段 `[0, BLOCKS_PER_PLANE)` / `[0, PAGES_PER_BLOCK)`，
  plane 由调用者钉死。
- **盐**：`munit_rand_*` 每次 exec 前重置回 `runner->seed`，且不带 salt ⇒ 必须
  `rand ^ hash(用例名) ^ hash(用途标签)`；data_path 的 `l2_rand` 末尾加 murmur3 `fmix32`
  终混（DJB2 低位退化）；lifecycle 的 `l3_rand` 当前不加（实测无碰撞），但复现契约相同。
- **复现契约 = (seed, 用例名, 第几次抽样)**；抽到的值必须经 `test_trace_detail` /
  `test_trace_summary` 打出，否则有 seed 也对不上地址。
- **覆盖旋钮是 `--iterations N`**：`MUNIT_TEST_OPTION_NONE` 的用例才会被乘；`SINGLE_ITERATION`
  会被硬压成 1。报告条数不变，判断加量看总耗时。

#### 2.4.2 参数化 vs 迭代

- **不同分支 / 不同期望 → 参数化**（每个 case 独立 setup/teardown，运行器打印 `case=…`）；
- **同一断言、不同输入 → 迭代**（`--iterations N` 或函数内循环）。

实例：`dp_encode` / `dp_process_read_ok` 三条断言集相同、只有期望不同 ⇒ 参数化；
`dp_scramble_whiten` 三种输入走同一组断言 ⇒ 函数内迭代。

#### 2.4.3 步骤级 vs 管线级

- **步骤级**：直接调 `fc_data_path_*` 单 CW 原语（CRC / ECC / 加扰），只碰
  `test_cw_scratch()` 的 528B buffer，不建链、不需要 config 位。「调了哪些原语」=
  「开了哪些功能」；验算法本身。
- **管线级**：建链后调 `fc_data_path_encode` / `process_read` / `raid_recover`，用
  `cmd->config` 表达「开哪些步骤」，验整条编排。
- 判据：**验算法 → 步骤级；验编排 → 管线级**。步骤级原语绝不可对已提交命令手工调用。

#### 2.4.4 Cache 语义与两段式读写

- **ONFI 1.0 没有 multi-plane**：Rev 1.0 命令集里 cache 只有单 plane 的 31h/3Fh/00h-31h/
  80h-15h，全文 `plane` 0 次。本器件 MT29F4G08 有 **two-plane cache 读写**（datasheet
  Figure 74 / p.62-63），由 `block_cache_multi_plane*` 覆盖。
- **单 plane cache**：写侧前 63 页 15h、末页 10h；读侧 `START_SEQ → 31h ×62 → LAST`，
  64 条命令输出 64 页。
- **双 plane cache**：写侧 page0 `OP_TWO_PLANE_PROGRAM_CACHE_FIRST`、page1..62
  `OP_TWO_PLANE_PROGRAM_CACHE`、末页 `..._END`；读侧 `START_SEQ_MP → SEQ_MP ×62 → LAST_MP`，
  每条 31h/3Fh 后用 `06h P0/P1 E0h` 分别输出两个 plane。
- **load/fetch 两段式**：生产读路径先加载（op 33/29，无 desc、无数据输出）再逐 CW 取数
  （op 8/41，带 desc）；`block_load_fetch` 显式拆成两条命令，两段必须紧邻。
- **META 只读**（op 11）：只回读 CW 前 4 字节，无 desc、不占 buffer、不进 ECC；
  **显式列读**（op 42）：列地址走命令 param 小端，够得到列 2048 的 spare 区。

#### 2.4.5 流式背压

`mixed_pipeline` 连续 `fc_submit`、不等完成；每成功下发 4 条或本条失败才 polling + 取 CQ。
在途命令顶到池上限 ⇒ 建链失败 ⇒ 抽 CQ 腾资源 ⇒ 重试同一条。multi-plane 每条命令吃
`PLANE_NUM*CW_NUM` 份 desc/buffer，64 页内必然触顶。硬判据是「全块写读完成 + 逐 CW 校验
+ 三池归零」；`retry` 只作 debug 证据，不依赖它判通过。

#### 2.4.6 链锁与 buffer 归属

- **链锁**（`chain_priority_lock`）：SQ 8 命令链（前 7 MORE、末条 END）+ 同 LUN HSQ 命令；
  链未结束期间该 LUN 的填充权归链所在队列独占，HSQ 插不进来（终值 = HSQ 写的值）。
- **buffer 归属**（`buf_own_release`）：`buf_own:2` 四档 00/01/10/11；判据 =
  `fc_get_resource_stats().buffer_used` 增量；建链未提交也不泄漏；满池 id 两两不同。

#### 2.4.7 游程统计判据（`dp_scramble_whiten`）

`dp_scramble_whiten` 的判据建立在「随机数据的游程该长什么样」之上；统计方法与概率推导如下，
既解释阈值来源，也标出判据的**盲区**：

- **统计**：一趟流式扫描，维护 `cur`/`run`；段切换时结算 `hist[cur][桶]`；循环后必须补收尾
  （否则段数恒少 1）；`≥16` 钳位进第 15 桶，不丢弃（全 0xFF / PAA 重复正是要治的长游程）。
- **期望**：随机数据每步 1/2 概率翻转 ⇒ 段数 ≈ 位数/2 = **2048**，平均段长 = **2.0**；
  段长服从几何分布（一半的段只有 1 bit）。
- **判据 ② 双侧**：`mean_x100 ∈ [170, 240]`（中心 200）。低了 = 被强制交替（周期 2）；
  高了 = 长游程/偏置（加扰没起作用）。
- **盲区**：统计量测不出周期性。反例 `00110011…`（每段恒长 2）三条判据全过，但它不是
  随机；实际加扰器输出由 LFSR 驱动，`all-0x00` 的输出就是裸 LFSR 最大长度序列。

> 判据的理论背景（加扰「目标 = 统计独立」）见 `docs/design/fc_nand_design.md` §5.4。

---

## 3. 夹具与隔离

### 3.1 pure

- 全部栈上夹具：栈上队列 / 栈上命令 / 栈上 ONFI 参数页；零 `fc_init()`、零资源池。
- `cmd_validate_op_range` / `cmd_validate_set_get_no_desc` 是**冒烟**：把合法输入送进内层 ASSERT，
  拦住「以后加了过严断言」。

### 3.2 data_path

- `test_setup` / `test_tear_down` 初始化/回收 FC 资源池；不碰 NAND。
- `g_bch` 是全局状态，`fc_ecc_init_bch` 只在首次分配时初始化，跨用例持续存在；不要在
  每个 setup 重复初始化。
- 步骤级用例只借 `test_cw_scratch(idx)` 的 528B buffer；管线级用 `test_cmd_build` 建链，
  释放统一走 `fc_command_release()`（它沿 desc 链把 desc + buffer 一起还池；不要再手动
  `fc_buffer_release`）。

### 3.3 lifecycle

- `test_setup` / `test_tear_down` （`test_setup` 内重申 `fc_nand_engine_set_armed(1U)`）。
- 建链走 FTL 策略层 `ftl_cmd_configure_descriptors()`；释放统一 `fc_command_release()`。
- 地址约定：`ch=0 ce=0 lun=0`，page=0..63，plane 0/1；block/plane 在用例体内随机抽，
  窗口见 §2.4.1；内部搬移目标块另抽且 `src ≠ dest`。
- 读回校验分两档：整段读按地址重算期望（`test_verify_page_paa`）；部分掩码读只看链上
  实际 desc（`verify_chain_paa`）。
- 擦除页判据只能是逐 CW `FC_CW_STATUS_EMPTY` / 成功 = 0，不能用聚合 status。

### 3.4 结束不变量（INV-*）

每个 data_path/lifecycle 用例 teardown 至少检查：

- `INV-POOL`：command / descriptor / buffer 三池 `used == 0`；
- `INV-CQ`：CQ 空、无未取完成项；
- `INV-CMD`：命令已 `fc_command_release`，不在 CQ 中；
- `INV-BCH`：BCH 全局状态未被重复初始化/污染。

> `INV-*` 是**本仓自拟**的不变量编号。

### 3.5 可测性 seam 清单

| seam | 用途 |
|---|---|
| `test_cmd_build` / `test_cmd_try_build` | 建链；后者资源申请失败返回 NULL 而非 abort |
| `test_submit_wait` | 提交 + 轮询完成（命令仍归调用者） |
| `fc_poll_completion` / `fc_poll_completions` | 单条/批量取 CQ |
| `fc_get_resource_stats` | 三池账本 |
| `test_cw_scratch` | 步骤级 528B buffer |
| `l2_*` / `l3_*` | 地址抽样与复现 |
| `--iterations` / `--seed` | 覆盖旋钮与复现 |

> FC UT **不新增 mock、不注入 NAND 失败**；能测的必须是生产 API 可直接驱动的部分。

### 3.6 注意事项

- **池在可缓存内存**：命令/描述符池落默认 `.bss`（AXI_RAM，Normal Write-Back）。SWD 读内存
  绕过 D-Cache，调试器看到的池内容可能陈旧；用 CPU 侧打印或先 `SCB_CleanDCache()`。
- **CQ entry slot**：只有 4 个 status slot（`status0..3`），`CW_NUM=4`；超出填
  `FC_CW_STATUS_UNUSED`。
- **断言失败现场保留**：裸机断言后卡在 `while(1)`，不会继续后续测试；一个 test 函数只验
  一个行为。
- **生命周期插桩**：`test_fc_lifecycle.c` 的 `run_lifecycle` 上方 `#if 0` 块（cmd 快照/差异
  打印）默认不编译；排查池泄漏时再打开。

---

## 4. 用例清单

> 首列是 `test --list` 里的用例名（稳定 ID）；测试函数名以 `test_` 开头，
> 不要求与用例名逐字相同。
> 本表只写**契约 / 覆盖点 / RISK**；具体断言值与负例写在测试源注释里。

### 4.1 `/fc-nand/pure/`（11 条）

| 用例 | 参数 | 契约 / 覆盖点 | RISK |
|---|---|---|---|
| `cmd_validate_op_range` | — | 0..`OP_COUNT-1` 全部合法（冒烟，钉住内层 ASSERT）；`CP-LOGIC` | — |
| `cmd_validate_set_get_no_desc` | — | SET/GET_FEATURES 无 desc 链的内联例外；`CP-LOGIC` | — |
| `sq_push_pop` | — | SQ FIFO 顺序；`CP-LOGIC` | — |
| `sq_wrap_around` | — | SQ head/tail 回绕后 FIFO 正确；`CP-LOGIC` | — |
| `sq_overflow` | — | 满队列 `QUEUE_FULL`，pop 后恢复；`CP-LOGIC` | — |
| `sq_peek` | — | 按偏移 peek，连查不消耗；`CP-LOGIC` | — |
| `sq_peek_empty` | — | 空队列 offset 越界返回 INVALID；`CP-LOGIC` | — |
| `cq_init` | — | CQ depth 记录、count=0；`CP-LOGIC` | — |
| `cq_push_pop` | — | CQ FIFO + 整项拷贝；`CP-LOGIC` | — |
| `cq_overflow` | — | CQ 满 `QUEUE_FULL`；`CP-LOGIC` | — |
| `onfi_param_page_decode` | — | 合成 256B 参数页逐字段解码；坏签名返回 0 但字段仍填满；`CP-LOGIC` | — |

### 4.2 `/fc-nand/data_path/`（13 条；RAID=1 时 15 条）

| 用例 | 参数 | 契约 / 覆盖点 | RISK |
|---|---|---|---|
| `dp_crc_step_detect` | `case=user-1/crc-field-1/meta-1/parity-1` | CRC 覆盖边界：user/CRC 字段必检出，META/parity 必不检出（负例）；`CP-DP` | — |
| `dp_scramble_whiten` | — | 全 0x00/0xFF/PAA 重复三种输入的白化度 + 自反性；`CP-DP` | — |
| `dp_ecc_step` | `case=clean/meta-1/crc-1/user-1/data-2/data-3/data-4/parity-1/parity-4/mixed-4/beyond-5` | BCH t=4 精确纠正；`beyond-5` 只钉「失败原子 / 纠正数 ≤ t」；`CP-DP` | `RISK-DP-13` |
| `dp_encode` | `case=random-off-4cw/random-on-4cw/single-cw` | 编码产物：cw 数 / META / CRC / parity；random=ON 自证加扰发生；`CP-DP` | — |
| `dp_encode_crc_disabled` | — | CRC 关：encode 不写 CRC 字段，读侧步骤 3 跳过；`CP-DP` | — |
| `dp_process_read_ok` | `case=random-on-4cw/random-off-4cw/single-cw` | encode → process_read → 全 READY + 原文恢复；`CP-DP` | — |
| `dp_process_read_unc` | — | CW1 损坏：IO_ERROR，CW1=UNC，其余 READY；`CP-ERR` | — |
| `dp_process_read_ecc_max` | — | 三段（干净 / 1+2bit / 整段破坏+1bit）：逐 CW `ecc_bits` 与命令级 `fc_get_cmd_ecc_max` 聚合；`CP-ERR` | `RISK-DP-29` |
| `dp_process_read_masked_chain` | — | mask=0x05：步骤 4 用 `paa_to_cw(desc->paa)` 而非链走位；`CP-CHAIN` | — |
| `dp_process_read_meta_error` | — | 改 `cmd->meta`：返回 OK 但逐 CW `META_ERROR`；`CP-ERR` | — |
| `dp_process_read_crc_error` | — | CRC 写关读开 + 哨兵：逐 CW `CRC_ERROR`；`CP-ERR` | — |
| `dp_process_read_ldpc_raw` | — | `LDPC_RAW`：两端跳过 ECC，数据原样往返；`CP-DP` | `RISK-DP-15` |
| `dp_cw_bitmap` | `case=flat-mixed/plane1-only/span-multi-plane` | flat CW 位图 → desc 链几何（plane/cw 展开与链序）；`CP-CHAIN` | — |
| `dp_raid_recover_no_random` | — | 无加扰 RAID 恢复（仅 `CONFIG_FC_RAID=1` 编译）；`CP-DP` | `RISK-DP-05` |
| `dp_generate_stripe_parity` | — | 跨命令 stripe XOR parity（仅 RAID=1）；`CP-DP` | `RISK-DP-05` |

### 4.3 `/fc-nand/lifecycle/`（27 条）

| 用例 | 参数 | 契约 / 覆盖点 | RISK |
|---|---|---|---|
| `init_sequence` | — | RESET → READ_ID → 参数页 → GET_FEATURES → 唯一 ID → ONFI 签名；`CP-OP` | `RISK-DP-21` |
| `feature_write_read` | — | 四个 Feature 地址 GET；01h 读-改-写回环；`CP-OP` | `RISK-NANDP-08` |
| `chain_priority_lock` | — | SQ 8 命令链 + HSQ 插入：链未结束 HSQ 插不进来；`CP-CONC` | `RISK-IF-07` |
| `status_query` | — | READ_STATUS / READ_STATUS_ENHANCED 生命周期 + CW status；`CP-OP` | — |
| `completion_late_delivered` | — | D2 seam：`FI_COMPLETION_LATE` 压一条完成，下一轮 poll 投递；`CP-CONC` | — |
| `completion_drop_bounded` | — | D2 seam：`FI_COMPLETION_DROP` 丢完成，有限轮询内不投递且命令可 release；`CP-ERR` | — |
| `engine_fail_all_cws` | — | D2 seam：`FI_FC_ENGINE_FAIL` 在策略表前失败，链上全部选中 CW 标 FAIL；`CP-ERR` | — |
| `engine_timeout_sets_status` | — | D2 seam：`FI_FC_ENGINE_FAIL` arg=TIMEOUT，命令状态带 `FC_CMD_STATUS_TIMEOUT`；`CP-ERR` | `RISK-IF-08` |
| `engine_fail_param_page` | — | D2 seam：参数页读 FAIL 让命令失败；`CP-ERR` | `RISK-DP-21` |
| `engine_fail_random_read` | — | D2 seam：多 CW random read 中间 FAIL 必须上报；`CP-ERR` | `RISK-DP-18` |
| `fail_cw_attribution` | — | 关放行闩锁：命令级 NAND_FAIL，8 条 desc 逐条 FAIL；无 desc 命令走空位图分支；`CP-ERR` | `RISK-DP-29` |
| `internal_data_move` | — | 源块 → 目标块 copyback，64 页逐字节一致（期望取源坐标）；`CP-MEDIA` | — |
| `block_erase_verify` | — | 擦除后逐页全 0xFF（prog→erase 判据有区分力）；`CP-MEDIA` | — |
| `block_write_read` | — | 整块 PROGRAM → READ 数据一致；`CP-MEDIA` | — |
| `block_cache_ops` | — | 单 plane cache 读管线（31h 序列 + RANDOM）；写侧 15h/10h；`CP-MEDIA` | `RISK-DP-19` |
| `sparse_data_out_read` | — | 稀疏 CW1/CW2 直接 `WITH_DATA_OUT` 读：逐槽 PAA 验证，走 WITHOUT/LOAD 分流；`CP-CHAIN` | `RISK-DP-26` |
| `block_cache_multi_plane` | — | 双 plane cache 读写；06h P0/P1 分别输出；两 plane pattern 独立校验；`CP-MEDIA` | — |
| `block_cache_multi_plane_cross_block` | — | 双 plane cache 跨 block 续读；`CP-MEDIA` | `RISK-DP-19` |
| `block_multi_plane` | — | MP_ERASE → MP_PROGRAM → MP_READ；两 plane 独立 pattern；`CP-MEDIA` | `RISK-DP-06` |
| `block_random_data` | — | READ + RANDOM_DATA_READ 抽页/列；列地址解码；`CP-MEDIA` | `RISK-DP-18` |
| `block_load_fetch` | — | LOAD(33/29，无 desc) → FETCH(8/41，带 desc)；两段紧邻、逐 CW PAA 一致；`CP-MEDIA` | — |
| `block_read_meta` | — | op 11 只读回读 CW META；普通读后 `fc_buffer_get_meta` 复核；`CP-MEDIA` | — |
| `read_column` | — | op 42 显式列读：列 16/20/2048/2052，精确到 4 字节偏移；`CP-MEDIA` | `RISK-DP-29` |
| `block_lock_ops` | — | LOCK_STATUS → LOCK → STATUS → UNLOCK → ERASE/PROGRAM/READ；可逆；`CP-MEDIA` | — |
| `mixed_pipeline` | — | 流式提交/池满即抽干；全块写读 + 逐 CW 校验 + 三池归零；`CP-CONC` | `RISK-DP-07` |
| `buf_own_release` | — | `buf_own` 四档 + 建链未提交 + 空闲链完整性；`CP-LEDGER` | — |

> `block_lock_tight`（紧锁定）实现保留但 `#if 0`、不注册；OTP 族当前无用例。

### 4.4 `/bench/speed/fc_nand_rw`（1 条，2 档）

| 用例 | 参数 | 契约 / 覆盖点 | RISK |
|---|---|---|---|
| `fc_nand_rw` | `mode=normal/cache` | 从 `BLOCKS_PER_PLANE/2` 起 100 个 per-plane 块（25 MiB）FC 原始块读写；只判「跑不跑得完」+ 错误计数；破坏性 | `RISK-NANDP-03` |

### 4.5 OP 覆盖参考表

| 值 | 宏名称 | standalone | 说明 |
|---|---|---|---|
| 0 | `OP_RESET` | ✅ | 复位 NAND |
| 1 | `OP_READ_ID` | ✅ | 读 ID |
| 2 | `OP_READ_PARAMETER_PAGE` | ✅ | 读参数页 |
| 3 | `OP_READ_UNIQUE_ID` | ✅ | 读唯一 ID |
| 4 | `OP_GET_FEATURES` | ✅ | 读 Feature |
| 5 | `OP_SET_FEATURES` | ✅ | 写 Feature |
| 6 | `OP_READ_STATUS` | ✅ | 读状态 |
| 7 | `OP_READ_STATUS_ENHANCED` | ✅ | 增强读状态 |
| 8 | `OP_RANDOM_DATA_READ` | ❌ | 随机列读 |
| 9 | `OP_READ_ONFI_SIGNATURE` | ✅ | 90h + 地址 20h → 4 字节 "ONFI"（无 desc、写 CQ `param`） |
| 10 | `OP_PROGRAM_FOR_INTERNAL_DATA_MOVE` | ❌ | 内部搬移编程 |
| 11 | `OP_READ_META` | ✅ | 只读回读 CW 前 4 字节 META（自带加载、无 desc） |
| 12 | `OP_NORMAL_READ_PAGE_WITH_DATA_OUT` | ✅ | 标准读（有数据输出） |
| 13 | `OP_READ_PAGE_CACHE_SEQ` | ❌ | 缓存读顺序 |
| 14 | `OP_READ_PAGE_CACHE_RANDOM` | ❌ | 缓存读随机 |
| 15 | `OP_READ_PAGE_CACHE_LAST` | ❌ | 缓存读最后 |
| 16 | `OP_NORMAL_PROGRAM_PAGE` | ✅ | 标准编程 |
| 17 | `OP_PROGRAM_PAGE_CACHE` | ❌ | 缓存编程 |
| 18 | `OP_ERASE_BLOCK` | ✅ | 擦除 Block |
| 19 | `OP_READ_FOR_INTERNAL_DATA_MOVE` | ❌ | 内部搬移读 |
| 21 | `OP_BLOCK_UNLOCK_LOW` | ✅ | 解锁低段 |
| 22 | `OP_BLOCK_UNLOCK_HIGH` | ✅ | 解锁高段 |
| 23 | `OP_BLOCK_LOCK` | ✅ | 锁定 |
| 24 | `OP_BLOCK_LOCK_TIGHT` | ✅ | 紧锁定 |
| 25 | `OP_BLOCK_LOCK_STATUS` | ✅ | 锁定状态 |
| 26 | `OP_OTP_DATA_LOCK` | ❌ | OTP 锁定 |
| 27 | `OP_OTP_DATA_PROGRAM` | ❌ | OTP 编程 |
| 28 | `OP_OTP_DATA_READ` | ❌ | OTP 读 |
| 29 | `OP_READ_PAGE_MULTI_PLANE_WITHOUT_DATA_OUT` | ❌ | 多 Plane 读无数据 |
| 30 | `OP_READ_PAGE_MULTI_PLANE_WITH_DATA_OUT` | ❌ | 多 Plane 读有数据 |
| 31 | `OP_PROGRAM_PAGE_MULTI_PLANE` | ❌ | 多 Plane 编程 |
| 32 | `OP_ERASE_BLOCK_MULTI_PLANE` | ❌ | 多 Plane 擦除 |
| 33 | `OP_NORMAL_READ_PAGE_WITHOUT_DATA_OUT` | ✅ | 标准读（无数据输出） |
| 34 | `OP_READ_PAGE_CACHE_START_SEQ` | ✅ | 缓存读启动 + 第一页输出（含首条 31h） |
| 44 | `OP_TWO_PLANE_PROGRAM_CACHE` | ✅ | 双 plane cache 写中间页（80h-11h-80h-15h），CHECK_FAIL_CACHE 查 FAIL\|FAILC |
| 48 | `OP_TWO_PLANE_PROGRAM_CACHE_END` | ✅ | 双 plane cache 写末页（80h-11h-80h-10h），开始/结束均查 FAILC |
| 49 | `OP_TWO_PLANE_PROGRAM_CACHE_FIRST` | ✅ | 双 plane cache 写首页（只查 FAIL，避免残留 FAILC） |
| 50 | `OP_PROGRAM_PAGE_CACHE_FIRST` | ✅ | 单 plane cache 写批次首页（只查 FAIL，后续页回到 op 17 查 FAIL\|FAILC） |
| 45 | `OP_READ_PAGE_CACHE_START_SEQ_MP` | ✅ | 双 plane cache 读首 pair |
| 46 | `OP_READ_PAGE_CACHE_SEQ_MP` | ✅ | 双 plane cache 读顺序 pair |
| 47 | `OP_READ_PAGE_CACHE_LAST_MP` | ✅ | 双 plane cache 读末 pair |
| 35 | `OP_READ_PAGE_LOAD` | ❌ | 多 LUN 读 ISSUE 段（Phase V） |
| 36 | `OP_READ_PAGE_WAIT` | ❌ | 多 LUN 读 WAIT 段（Phase V：tR + 状态，无数据） |
| 43 | `OP_READ_PAGE_MULTI_PLANE_LOAD` | ❌ | 多 LUN 多 plane 读 LOAD 段（Phase V） |
| 37 | `OP_PROGRAM_PAGE_ISSUE` | ❌ | 多 LUN 编程 ISSUE 段 |
| 38 | `OP_PROGRAM_PAGE_WAIT` | ❌ | 多 LUN 编程 WAIT 段 |
| 39 | `OP_ERASE_BLOCK_ISSUE` | ❌ | 多 LUN 擦除 ISSUE 段 |
| 40 | `OP_ERASE_BLOCK_WAIT` | ❌ | 多 LUN 擦除 WAIT 段 |
| 41 | `OP_RANDOM_DATA_READ_MULTI_PLANE` | ❌ | 多 Plane 页缓冲随机读单 CW（06h-E0h） |
| 42 | `OP_READ_COLUMN` | ✅ | 按**显式列地址**（SQ `param.bytes[0..1]`）读 4 字节；够得到列 2048 的 spare 区（无 desc、写 CQ `param`） |

> 当前 `OP_COUNT = 51`（0..50）；槽位 20 是墓碑，不复用。`cmd_validate_op_range` 覆盖 0..50。

---

## 5. 风险追溯

> 只写「哪个 RISK 由哪些用例/人工流程覆盖」。**状态与历史见
> `docs/design/known_risks.md`**；未列出的 RISK 可能仍是 BOARD/OUT，本表不声称完整。

| RISK | 处置 | 用例 / 流程 |
|---|---|---|
| `RISK-IF-07` | UT-FC + BOARD | `chain_priority_lock`；读流式 trace 复核 |
| `RISK-DP-13` | UT-FC | `dp_ecc_step`、`dp_process_read_ecc_max` |
| `RISK-DP-15` | 不覆盖 | LDPC 完整解码；只测 `dp_process_read_ldpc_raw` 的跳过路径 |
| `RISK-DP-27` | 不覆盖 | LDPC 解码栈超 2 KiB；见 §7 |
| `RISK-DP-28` | 不覆盖 | CRC 初始化失败 release 静默；见 §7 |
| `RISK-NANDP-01` / `RISK-NANDP-04` / `RISK-NANDP-05` | 不覆盖 | 多 target / 每 target 初始化 / timing 共享；见 §7 |
| `RISK-NANDP-03` | BOARD | Mode 5 运行期 UNC；`/bench/speed/fc_nand_rw` 只观测 |

---

## 6. 运行与证据

### 6.1 构建

```text
make tests UT=FC
```

- 产物：`build/firmware/debug/bin/debug_ut.elf`；`UT=FC` / `UT=FTL` / `UT=HIL` 共用该文件，
  烧写前确认最后一次构建是 `UT=FC`。
- 串口横幅时间戳必须与构建时间一致（`__DATE__ __TIME__`）。

### 6.2 运行

```text
test --list
test /fc-nand/pure/                 # 纯逻辑，安全
test /fc-nand/data_path/             # 只碰 buffer，安全
test /fc-nand/lifecycle/             # 真 NAND，破坏性
test --log-visible info --iterations 5 /fc-nand/lifecycle/
test --log-visible info /bench/speed/fc_nand_rw   # 破坏性，单独跑
test                                  # 全套；不加过滤串才是全套
```

- 过滤串约束：套件名带尾斜杠；`/fc-nand/` 由 `ut_run_all()` 展开为全部叶子套件，可正常使用。
- `--seed N` 复现；`--iterations N` 只乘 `MUNIT_TEST_OPTION_NONE` 的用例。
- lifecycle 跑完必须 `format` + POR，恢复 live FTL 状态。

### 6.3 日志与证据

- 默认：用例名 + `[ OK ]/[ SKIP ]` + 耗时。
- `--log-visible info`：`[EV]` 简单前后对比（含抽样地址）。
- `--log-visible debug`：完整 inputs/outputs。
- `--log-project info/debug`：FC/NAND 项目日志（默认关闭）。
- 原始日志与 `meta.txt` 放 `build/test-results/<时间>_<build>_<target>/`。

### 6.4 SKIP 说明

Mode 5 下 `CONFIG_NAND_TIMING_MODE != 0`，以下 3 条按设计 SKIP（不是失败）：

- `init_sequence`
- `feature_write_read`
- `chain_priority_lock`

它们依赖「启动低速、运行高速」的自洽性；Mode 0 构建仍会执行。

**2026-11-13 Mode 0 已验证**：

```text
make clean && make tests UT=FC CFLAGS_EXTRA="-DCONFIG_NAND_TIMING_MODE=0"
```

- 启动日志出现 `high-speed mode disabled (CONFIG_NAND_TIMING_MODE=0)`。
- `test /fc-nand/` → **62/62，0 SKIP，823.49 ms**；三条 SKIP 用例均 OK。
- ⚠ `CFLAGS_EXTRA` 不参与对象目录命名，切换 Mode 0/Mode 5 必须 `make clean`；Mode 0 是专项回归，跑完恢复默认 Mode 5。
- 跑完 Live 映射已被原始块擦写破坏（见 §1 原则 6）。下次启动出现
  `[L2P] region N primary unreadable — backup OK` 属预期回退，不代表新缺陷；
  继续做 H2testw 前必须 `format` + POR。

---

## 7. 不覆盖项

- `CONFIG_FC_RAID=0`：`dp_raid_recover_no_random` / `dp_generate_stripe_parity` 不编译；RAID 恢复
  路径没有当前构建的 UT 判据。
- LDPC 完整解码：`dp_process_read_ldpc_raw` 只测「跳过 ECC」的编排，不证明 LDPC 算法；
  `RISK-DP-15` / `RISK-DP-27` 仍未闭合。
- CRC 初始化失败的 release 静默路径：`RISK-DP-28`。
- 多 target / 多 LUN / per-bank 初始化：`RISK-NANDP-01/04/05`；当前板单 target。
- Mode 0 专项：Mode 5 下 3 条 SKIP；需要 Mode 0 构建单独回归。
- OTP 族、`block_lock_tight`、真 POR、长时 soak、USB/HIL/FTL 行为：不在 FC UT。
- 性能：`/bench/speed/fc_nand_rw` 是基准，不作功能通过率。

### 7.1 `dp_ecc_step` 的覆盖边界

`dp_ecc_step` 验的是 **BCH(m=13, t=4) 的实现**，不是「算法对不对」——≤t 个错的唯一译码由代数
保证；测试守的是 BM/Chien/Forney、位翻转落点、`errloc` 映射、parity 布局。当前形态全绿只应
读作「在这些抽样下没复现出实现 bug」。六处缺口：

| # | 缺口 | 具体 |
|---|---|---|
| ① | 抽样非穷举 | 每 case 每轮只抽 1 组位置；空间 `C(4160,4) ≈ 10¹³` 量级 |
| ② | 抽样有偏 | bit-within-byte 只覆盖少数偏移 ⇒ 只错在 bit 7 的实现 bug 看不见 |
| ③ | `beyond-5` 不钉结果 | OK 分支只验 `ecc_bits ≤ 4`、不验数据 ⇒ 误纠被容忍 |
| ④ | parity 区是「软」的 | `p_k > 0` 时只比数据区 520B；解码器刻意不回写 parity 字节 |
| ⑤ | 单一信道模型 | 只有干净码字 + 均匀随机位错，不含突发/相关错 |
| ⑥ | 同源编解码 | 用 `fc_data_path_ecc_encode` 造码字，与解码同族 ⇒ 共享约定 bug 仍能通过 |

**提高计划（按性价比）**：

1. **定向补 case**：bit-within-byte 覆盖 0..7 / 跨字节 / 突发错 / t=4 全落 parity / `errloc`
   边界（4159/4160/4207）——抽不到的边界直接穷举。
2. **`--iterations` 加量**：`test --iterations 200 /fc-nand/data_path/ecc_step`。
3. **已知答案向量（KAT）**：用独立实现（上游 Linux `bch.c` 测试向量）比 parity 字节。
4. **系统层**：把 CRC 判定接进 FTL（`src/ftl/**`，按硬规则暂不动）。

> ⚠ ECC 单测全绿 ≠ 主机读到的数据一定对：超能力场景的误纠要靠步骤 3 的 CRC；FTL 侧用
> `fc_cmd_status_has_error` 全错误位判据（`fc_nand_design.md` §2.4）。

> 未实现 / 未验证项统一登记 `docs/design/known_iss.md`；本文不复制状态。

---

## 8. 维护规则

- **新增/删除用例**：同批更新 §4 对应表 + 测试源注释；不得只改源或只改文档。
- **风险状态变化**：只改 `docs/design/known_risks.md`；§5 仅在映射变化时改。
- **地址抽样契约**：改抽样顺序 / 盐 / `fmix32` 会改变 seed→地址映射；必须同批更新
  §2.4.1 与测试源注释。
- **运行结果**：写 `build/test-results/` 与 `known_risks.md` 历史；§9 只留最近快照。
- **禁止**：§5 写状态词；§4 复制实现细节；文档头写「N 函数 / M 次运行」快照。
- **测试源按 R4 由用户维护**；新增 mock / 注入前先说明为什么生产 API 不可直接驱动。
- **历史版本**：查 `docs/archive/fc_nand_ut_pre_refactor_20261113.md`；本文不写版本叙事。

---

## 9. 最近验收快照

> 只作时间点快照；状态以 `docs/design/known_risks.md` / `docs/design/known_iss.md` 为准。

| 日期 | 镜像横幅 | 命令 | 结果 | 证据 |
|---|---|---|---|---|
| 2026-11-13 | 13:37:20 | `test`（无过滤串，Mode 5） | `test` 全套 61 OK + 3 SKIP（64 runs；`/fc-nand/` 59 OK + 3 SKIP + `/bench/speed/fc_nand_rw` 2 OK），27.95 s；`/bench/speed/fc_nand_rw` normal 3685/3555 KB/s、cache 4340/3614 KB/s | `docs/design/known_iss.md` ISS-UT-02（已闭合） |
| 2026-11-13 | 18:36:56 | `test /fc-nand/`（Mode 0 专项） | **62/62，0 SKIP，823.49 ms** | Mode 0 串口日志 |

> 3 条 SKIP 见 §6.4；seed `0x5a4f21e4`。Mode 0 是专项回归（`CONFIG_NAND_TIMING_MODE=0`），不是默认档。
