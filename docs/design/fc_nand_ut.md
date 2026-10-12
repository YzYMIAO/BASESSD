# FC-NAND 模块单元测试设计方案

**日期**: 2026-09-22

---

## 1. 概述

本文档记录 soft_ssd 项目中 FC（Flash Controller）与 NAND 底层驱动模块的 munit 单元测试设计方案。测试目标平台为 **STM32H743** 裸机，通过 `make tests` 交叉编译进固件，下载后通过 UART CLI 触发执行。

测试框架基于 **munit v0.4.1**（位于 `tests/framework/munit.c/h`），在嵌入式环境中做了以下适配：

| 适配项 | 说明 |
|---|---|
| **无 fork 隔离** | 裸机没有子进程，断言失败后 `munit_errorf_ex` 执行 `while(1)` 挂起，保留现场供调试器检查 |
| **stdout 重定向** | `_write()` → `uart_log_write()` 经 UART 输出，`printf` / `fprintf` 自动走串口 |
| **argc/argv 输入** | UART CLI 的 `uart_cli_tokenize()` 解析串口输入为 argv，调用 `munit_suite_main` |
| **无 `abort()`** | `munit_logf_ex` 中移除 `abort()`，断言失败的统一由 `while(1)` 处理 |
| **参数化（v29 补齐）** | 机器一直齐备（`MunitTest.parameters` + 笛卡尔积展开 + CLI `--param` / `--list-params` / `--single`），但漏移植了取值函数 `munit_parameters_get()`；`run_test_wild` 的递归实参另有误（≥2 个枚举时无限递归）。v29 一并修掉，见 `docs/changelog/fc_data_path_step_primitives_v29.md` §E |

---

## 2. 测试分层

参考 [UT Plan FC-NAND munit](ut-plan-fc-nand-munit.md)，测试分三个层级：

```
Level 1: Pure Logic ──── 零硬件依赖，纯函数测试
    ├── fc_command validate（合法输入，冒烟性质）
    ├── fc_queue push / pop / peek / full / empty / completion queue
    └── fc_scheduler init（选择逻辑已不在本层，见下）

Level 2: Data Path ──── 需 fc_init() 初始化池，无 NAND 硬件
    ├── 步骤级：单 CW 原语（crc_step / ecc_step / scramble）——不建链、不用 config 位
    ├── 管线级：fc_data_path_encode / process_read / raid_recover
    └── flat CW 位图建链几何（cw_bitmap_*，走 FTL 建链入口）

Level 3: FC Lifecycle ── 真实 NAND 硬件执行
    └── 15 个 block 级场景（init / feature / 链锁 / status / 内部搬移 / 擦写读 / cache / 多 plane / 随机列读 / 加载取数 / META 回读 / 块锁 / 背压流水线 / buffer 归属）
```

> **适配说明（v1.8）**：本层测试在 v13~v21 的接口/策略收敛后做过一轮适配，见 §7 修订历史与 `docs/changelog/fc_ut_adapt_v13_v21.md`。要点：
> - **校验类测试退场**：`fc_command_validate` 于 v20 整函数转 ASSERT（配置错误不再返回码），`validate_null` / `validate_desc_needed` / `validate_cw_mask` 无返回码可测，已删；`validate_op_range` 只保留「合法 OP 恒 OK」。
> - **调度器选择逻辑不可达**：v8 起 SQ/HSQ 选择搬进 `fc.c` 的 `static fc_poll_fill`，只能经 `fc_service()` 驱动真 NAND 才走到，Level 1 不再能覆盖 `select / select_chain / busy / starvation` 等 8 条，已删。
> - **`fc_command_t.command_id` 于 v15 删除**（ID 改指针派生），断言改用 `fc_command_to_id()`。
> - **建链入口于 v13 上移 FTL**：`fc_command_configure_descriptors`（复数）不复存在，测试改调 `ftl_cmd_configure_descriptors()`。
> - **2026-10-08 删 5 条**（判据：被测代码还有没有生产调用者）——`status_init_pool_cmd` / `status_init_stack_cmd` / `status_set_cw_valid`（`fc_command_status_init` / `_set_cw` 自 2026-09-20 起零生产调用者，CQ 完成项改由 `fc_complete_from_nand()` 内联组装，函数连同用例一并删）、`pop_n`（`fc_submission_queue_pop_n` 零生产调用者，调度器走逐条 `pop`）、`empty_full`（断言集是 `peek_empty` ∪ `overflow` 的子集）。Level 1 16→**11**，套件至此**零 setup**（不再需要 `fc_init()`）。

### 2.1 Level 1 — 纯逻辑测试

**测试目标**：`fc_command.c`、`fc_queue.c`、`fc_scheduler.c` 中的纯逻辑函数，以及 `nand/onfi_1_0.c` 的**纯解码函数**（`nand_onfi_parameter_page_decode`，v1.38）。全部用例自带夹具（栈上队列 / 栈上命令 / 栈上参数页），不依赖任何硬件、动态内存或资源池。

> **用例性质说明**：`validate_*` 两条是**冒烟**而非返回值验证——`fc_command_validate` v20 起函数体只剩 `return FC_STATUS_OK`，断言恒真；它们的价值是用合法输入把内层 ASSERT 跑一遍，拦住「以后有人加了过严的断言」。其余 10 条是真正的行为验证。

| 测试函数 | 被测函数 | 验证点 |
|---|---|---|
| `test_command_validate_op_range` | `fc_command_validate` | 0..OP_COUNT-1 全部合法（恒 OK，冒烟） |
| `test_command_validate_set_get_no_desc` | `fc_command_validate` | SET/GET_FEATURES 无 desc 链的内联例外（钉住断言短路分支） |
| `test_queue_push_pop` | fc_submission_queue push/pop | FIFO 顺序正确 |
| `test_queue_wrap_around` | fc_submission_queue push/pop | head/tail 回绕后 FIFO 正确（head=1, tail=0 跨环两侧） |
| `test_queue_overflow` | fc_submission_queue_push | 满队列返回 QUEUE_FULL，且 pop 后恢复可用 |
| `test_queue_peek` | fc_submission_queue_peek | 按偏移取到对应元素，且连查两次不消耗 |
| `test_queue_peek_empty` | fc_submission_queue_peek | 空队列返回 INVALID（`offset >= count` 边界） |
| `test_cqueue_init` | fc_completion_queue_init | depth 记录正确、count == 0 |
| `test_cqueue_push_pop` | fc_completion_queue push/pop | FIFO 顺序 + 整项拷贝 |
| `test_cqueue_overflow` | fc_completion_queue_push | 满队列返回 QUEUE_FULL |
| `test_scheduler_init` | `fc_scheduler_init` | sq / hsq 指针绑定（断言指针相等 = 只持引用不拷贝） |
| `test_onfi_parameter_page_decode` | `nand_onfi_parameter_page_decode` | **Part 4（v1.38）**：栈上按手册 Table 11 合成 256B 参数页 → 逐字段断言（签名/版本/型号串/几何/ECC 位数/时序模式位图）；再破坏签名，断言如实返回 0 而字段仍填满 |

**共计 12 个测试函数。**（合并前按模块分 command 2 + queue 8 + scheduler 1，现统一挂在单个 `logic` 套件下，见 §3；`test_onfi_parameter_page_decode` 是 v1.38 加入的**唯一非 FC 层**用例——纯 ONFI 协议解码，零硬件）

### 2.2 Level 2 — 数据路径测试

**测试目标**：`fc_data_path.c` 中的编解码管线。需要 `fc_init()` 初始化资源池，但不需要 NAND 硬件——全部在 buffer 层面做 round-trip 验证。

> **建链与释放（v1.8）**：本层建链走 FTL 策略层 `ftl_cmd_configure_descriptors()`（v13 起「按位图建整条链」属 FTL，FC 只剩单数逐条入口 `fc_command_configure_descriptor`）。释放一律 `fc_command_release()` 收尾——v16 起它沿 desc 链把 desc + buffer 一起还池；**不要再手动 `fc_buffer_release`**，那会二次释放同一 buffer 并使空闲链成环。

> **两种形态（v29）**：本层用例分两类——
> - **步骤级**：直接调 `fc_data_path_*` 的**单 CW 原语**（`fc_data_path.h` 的「单 CW 步骤原语」段），
>   只碰 `test_cw_scratch()` 给的一块 528B buffer。**不建链、不需要任何 config 位**——
>   「调了哪些原语」= 「开了哪些功能」。例如「只开 CRC」就是只调 `meta_write` + `crc_write`，
>   不必把 `config.ldpc` 设成 `LDPC_RAW` 去让 ECC 别把注入的 bit 纠回去。
> - **管线级**：建链后调 `fc_data_path_encode` / `process_read` / `raid_recover`，
>   用 `cmd->config` 的位组合表达「开哪些步骤」，验整条编排（gate 分支 / 链遍历 / `desc->status` 写回）。
>
> 判据：**验算法本身 → 步骤级；验编排 → 管线级**。前者不依赖 fixture 的建链正确性，是后者的地基。
> ⚠ 步骤级原语**绝不可**对已提交的命令手工调用——FC 自己会跑整条管线（见 §6.2 与 `fc_test_common.h`「五」）。

Level 2 不关心 OP 码（operation 字段），只测试 data_path 内部的数据变换逻辑。管线级三个被测函数：

| 函数 | 功能 | 读取的 config 字段 |
|---|---|---|
| `fc_data_path_encode` | 写路径：META → CRC? → Random 加扰? → ECC 编码 | `crc`, `random`, `ldpc` |
| `fc_data_path_process_read` | 读路径 Stage1：ECC 解码 → 条件解扰 → CRC 校验 → META 校验 | `ldpc`, `random`, `crc` |
| `fc_data_path_raid_recover` | 读路径 Stage2：RAID XOR 重建 → ECC 重试 → 解扰 | `ldpc`, `random` |

> **注意**：实现过程中发现 `fc_data_path.c` 原有的 encode/decode/raid_recover 三处 ECC 调用**没有读取 `config.ldpc` 字段**，始终执行 ECC。已在实现时补上 `ldpc == FC_ECC_MODE_LDPC_RAW` 时的跳过逻辑。
>
> **`config.raid` 数据路径不读**：encode 的 Stage 3（RAID XOR）已移除（`fc_data_path.c:191`），跨命令 stripe RAID 走独立的 `fc_data_path_generate_stripe_parity()`。全仓 `config.raid` 唯一读者是 `fc.c` 的分派钩子（决定走不走 `fc_stripe_submit`）。因此 `alloc_cmd_full()` 的 `raid_mode` 形参对本层是被动的——传 NORMAL / ENCODE 无差别。

### Level 2 地址抽样（v1.25）

**13 条用例**（`crc_step_detect` / `ecc_step` / `encode` / `encode_crc_disabled` / `process_read_ok` /
`process_read_unc` / `process_read_masked_chain` / `process_read_mata_error` / `process_read_crc_error` /
`process_read_ldpc_raw` / `raid_recover_no_random` / `generate_stripe_parity` / `cw_bitmap`）的地址
**在用例体内按种子抽**，与 §2.3 的 Level 3 同法（`l2_hash` / `l2_rand` / `l2_addr_draw`）。
取代此前的固定地址 `TEST_BLOCK`(100) / `TEST_PAGE`(32) / plane 0（经 `L2_META` / `L2_PAA` / `l2_cmd`，
本版删掉前两个宏）。

- **`l2_addr_draw(test, plane)`**：抽 block ∈ `[0, BLOCKS_PER_PLANE)`、page ∈ `[0, PAGES_PER_BLOCK)`；
  **plane 由调用者钉死**（不是随机量，见下）；`cw` 恒 0（`test_cmd_build` 断言）。就地打一条
  `test_trace_detail("addr block=… page=… plane=…")`（`--log-visible debug` 可见）——地址是
  `(seed, 用例名)` 的纯函数，不打就无从复现。
- **不设窗口**：Level 3 的 30%~70% 是为了避开**固定**元数据区；**本层不写介质**（见 §2.2 开头与文件头），
  那理由不成立 ⇒ 用整段 `[0, BLOCKS_PER_PLANE)` / `[0, PAGES_PER_BLOCK)`，与既有的 `scramble_whiten` 同口径。
- **必须自带 salt**（同 §2.3）：munit 每 `exec` 前 reseed 回 `runner->seed`（`munit.c:774`），导出的
  `munit_rand_*` **不带 salt** ⇒ 无盐时各用例的第一次抽样取值完全相同。故
  `l2_rand = munit_rand_uint32() ^ l2_hash(用例名) ^ l2_hash(用途标签)`。
- **⚠ 抽样必须过一道终混**（v1.25 的真正修正）：只做 XOR 会撞——`l2_hash` 是 DJB2（`h = h*33 + c`），
  而 `33 ≡ 1 (mod 32)` ⇒ **低 5 位只等于各字符之和 mod 32**，抽样又 `% 2048` / `% 64`（截低位用），
  两个哈希只要低位相同就必然撞。实测 `h("process_read_mata_error") = 0xE9E586CA` 与
  `h("process_read_ldpc_raw") = 0x0A6286CA` 的 XOR = `0xE3870000`（低 16 位全 0）⇒ 四条种子下逐位同址。
  修法：`l2_rand` 末尾加一道 murmur3 `fmix32` 终混。⚠ Level 3 的 `l3_rand` 结构相同，但实测六条无碰撞，
  **本轮不动**（动它会改 v40 已验证的 seed→地址映射）。
- **参数化用例的 case 同址是刻意的**：盐只有用例名、PRNG 每次 `exec` 重置 ⇒ 参数化用例
  （`crc_step_detect` / `ecc_step` / `encode` / `process_read_ok` / `cw_bitmap`）**各 case 抽到同一地址**。
  case 之间变的本来就不是地址，同址反而把变量隔离干净。
- **三处特殊**：① `process_read_mata_error` 的篡改目标是 `(addr.block + 1) % BLOCKS_PER_PLANE`——
  与命令**同 page / 同 plane、异 block**（PBA 里 block 是最高字段，改它不动低 3 位，正是本条要的
  「低 3 位同、高位异」，见 v34 的坑）；② `cw_bitmap` 的 plane 必须 = 场景表的 `addr_plane`（判据，不能随机）
  ⇒ 只随机 block / page；③ `generate_stripe_parity` 的 data 与 parity 两条命令**必须同址**（用例体把 data 的
  `desc->meta` 同步给 parity）⇒ 抽一次两处共用。
- **不加用例**：用例数仍 **51**；这批用例多数带 `MUNIT_TEST_OPTION_SINGLE_ITERATION`（`munit.c:765-767`
  把迭代数硬压成 1）⇒ `--iterations N` 对它们无效，用例体内抽样是**唯一**的加量杠杆。

### 参数组合

| 字段 | 有效取值 | 影响 |
|---|---|---|
| `random` | `FC_RANDOM_ENABLE(0)` / `FC_RANDOM_DISABLE(1)` | 是否加扰/解扰 |
| `crc` | `FC_CRC_ENABLE(0)` / `FC_CRC_DISABLE(1)` | encode 是否写 CRC 字段、读侧 Pass 3 是否校验 |
| `ldpc` | `FC_ECC_MODE_BCH(0)` / `FC_ECC_MODE_LDPC_RAW(3)` | LDPC_RAW 时两端都跳过 ECC |
| `multi_plane` + `cw_bitmap` | `0` + 单 plane 段位图 / `2` + 双 plane 段位图 | desc 链的 CW 集合与几何（v27：位图是**字面**的，0 = 空集，整段须显式给；v28：位图作 `ftl_cmd_configure_descriptors()` 的**实参**，不再是命令字段） |
| `cw` | 0 (1CW) / 3 (4CW) | desc 链长度，多 CW 边界 |
| ECC 结果 | 全部成功 / 有 UNC | process_read 是否走解扰 |
| 校验结果 | 全通过 / META 不符 / CRC 不符 | Pass 3 / Pass 4 是否置位 |

> **⚠ 命令 fixture 必须给真实页地址**（v1.11）：`alloc_cmd_full()` 统一设一个真实 `paa`/`pba`（v25 起形如 `paa_from_addr(&(nand_addr_t){.block = 50})`；v25 前是 `paa_make(32, 0, 50, 0, 0)`）。早期 fixture 不设地址（保持 `{0}` 清零），单 plane 建链分支会把 `desc->paa` 落成 `cw` 本身，BASIC 种子随之退化成 `seed == cw` → `{1,1,2,3}`，LFSR 低 16 位取这几个小值后 **keystream 除 byte0 外全 0**——加扰形同虚设，`random=ON` 的用例全部是空转。故 random=ON 的用例另加一条「encode 后 USER 区 ≠ 明文」断言自证加扰真的发生了。

### 测试用例

| 测试函数 | 参数组合 | 验证策略 |
|---|---|---|
| `encode` | **参数化** 3 case（`--param case <名>`） | 查**编码产物本身**（只 encode、不 process_read）；三条断言集相同、仅期望不同 ⇒ 参数化，见下 |
| `encode_crc_disabled` | crc=off, 4CW | encode 不写 CRC 字段（哨兵保留）；读侧 Pass 3 跳过 → 全 READY |
| `process_read_ok` | **参数化** 3 case | `l2_buffer_round_trip`：encode → process_read → 全 READY + 原文恢复；三条断言全在 helper 内 ⇒ 参数化，见下 |
| `process_read_unc` | random=on, 4CW, CW1 损坏 | 返回 IO_ERROR；desc status CW1=UNC, 其余=READY |
| `process_read_masked_chain` | random=on, **单 plane 段**, mask=0x05 | 链位 0,1 ↔ CW 0,2；**专钉 Pass 4 用 `paa_to_cw(desc->paa)` 而非链走位**——若改用链走位，第 2 条会假报 MATA_ERROR |
| `process_read_mata_error` | random=off, 4CW, encode 后改 `cmd->meta` | 返回值仍 OK，但每个 CW 置 `MATA_ERROR`（Pass 4） |
| `process_read_crc_error` | crc 写关读开, 4CW, [4,8) 预置哨兵 | 返回值仍 OK，但每个 CW 置 `CRC_ERROR`（Pass 3）；ECC 未扰动，触发确定 |
| `process_read_ldpc_raw` | ldpc=LDPC_RAW, 4CW | 两端跳过 ECC，数据原样往返，全 READY |

`encode` 的 3 个 case（三条都查：`cw_count` / META 已写 / CRC 已写 / parity 非零）：

| case | random | cw | 期望 USER 区 |
|---|---|---|---|
| `random-off-4cw` | off | 4CW | 仍为明文 |
| `random-on-4cw` | on | 4CW | **已非明文**（自证加扰真的发生，见 §2.2 的 fixture 教训） |
| `single-cw` | off | 1CW | 仍为明文 |

`process_read_ok` 的 3 个 case：`random-on-4cw` / `random-off-4cw` / `single-cw`——轴与 `encode` 相同，
但断言全在 `l2_buffer_round_trip()` 内（encode → process_read → 全 READY + 原文恢复）。

> **为什么这两组参数化、其余 5 条不**（判据见 `assertion_guidelines.md` §二十三）：参数化的前提是
> **同一套断言、只有期望值不同**。`encode` 三条只有两列期望不同（cw 数、USER 区是否仍是明文）；
> `process_read_ok` 三条的断言完全在 helper 里。而 `unc` / `masked_chain` / `mata_error` /
> `crc_error` / `ldpc_raw` 各自断言**不同的可观测量**（返回码 / 不同的 `status` 位 / 不同的哨兵区），
> 合并只能靠给每行挂 setup 回调——收益不足，保留独立。
> `mata_error` 与 `crc_error` 断言形状相同（逐 CW 查一个 `status` 位）但 setup 机制不同
> （改 `cmd->meta` vs 翻 `config.crc` + 预置哨兵），同样不并。

> **「返回值仍 OK」是刻意的**：`fc_data_path_process_read` 在 Pass 3/Pass 4 标脏后**不改返回值**（`fc_data_path.c:551` 恒返回 `FC_STATUS_OK`），所以判「读成功」必须查 `desc->status == FC_CW_STATUS_READY`，只看返回值会漏掉 CRC / META 校验回归。

| 测试函数 | 参数组合 | 验证策略 |
|---|---|---|
| `raid_recover_no_random` | random=off, 4CW, failed_idx=1 | 无加扰恢复（见下方局限说明） |
| `generate_stripe_parity` | random=on, 2cmd×4CW | 跨命令 stripe XOR parity 验证（真 stripe） |
_(以上两条仅在 CONFIG_FC_RAID=1 时编译，默认 0 不执行)_|

> **RAID 组只剩 2 条（v1.11）**：删掉了 `encode_raid_on` 与 `raid_recover_first` / `_mid` / `_last`。原因是它们**构造不出被测函数的真实前置条件**——`fc_data_path_raid_recover` 只收集单条命令的 desc 链（`fc_data_path.c:591`），而 parity CW 在另一条命令的链上（`fc.c:888/924-933`），跨命令适配是 `include/fc/fc_raid_engine.h:22` 的未办事项。那 4 条只能靠「无 parity + pattern 异或恰为 0（`0x10^0x11^0x12^0x13`）+ keystream 退化」三重巧合通过；修好 fixture 后 random=ON 的变体必然挂。保留的 `raid_recover_no_random` 走 random=OFF（无加扰，退化 stripe 成立），真 stripe 覆盖由 `generate_stripe_parity` 承担。判据见 `assertion_guidelines.md` §十七·判据 6。

### 步骤级用例（v29）

只调 `fc_data_path.h` 的单 CW 原语，**不建链**、不占 desc/buffer 池，只借 `test_cw_scratch(idx)` 的
528B 暂存区。三条用例全部 `MUNIT_TEST_OPTION_NONE` ⇒ 可用 `--iterations N` 多抽注入位置。

| 测试函数 | 形态 | 覆盖 |
|---|---|---|
| `crc_step_detect` | **参数化** 4 case（`--param case <名>`） | CRC 的**覆盖边界** |
| `ecc_step` | **参数化** 11 case | BCH(m=13, t=4) 的纠错能力 |
| `scramble_whiten` | 单函数内 `kind = 0..2` 循环 | 加扰白化度 + 自反性（判据依据见 §5.6） |

`crc_step_detect` 的 4 个 case——依据 `fc_data_path_crc_check`：存的 CRC 读自 `cw[4..8)`，
算的 CRC 吃 `cw[8..520)`（`FC_CRC_USER_SIZE = 512`）。META 与 parity 两头**结构性**在覆盖之外：

| case | 注入区间（CW 内偏移） | 期望 `crc_check` | 说明 |
|---|---|---|---|
| `user-1` | `[8, 520)` | `0` 必检出 | 被校验的数据 |
| `crc-field-1` | `[4, 8)` | `0` 必检出 | 坏的是**存的值**，不是数据 |
| `meta-1` | `[0, 4)` | `1` **必不检出** | 负例 |
| `parity-1` | `[520, 528)` | `1` **必不检出** | 负例 |

> **两条负例是重点**：没有它们，一个「把整块 528B 都拿去算 CRC」的错误实现照样能过。
> 区域内偏移由框架 PRNG 取（运行器打印 seed，失败可 `--seed` 原样重放）。

`ecc_step` 的 11 个 case：1..4 bit 错必**精确纠正**（钉在 t=4 上）；`beyond-5` 单列 BOUND——
BCH 允许误纠（实测约 0.28% 返回 OK 但数据是错的），故**不钉结果**，只钉两条恒成立的不变量
（失败必须原子 / 纠正数不得超过 t）。`scramble_whiten` 的三种输入（全 0x00 / 全 0xFF / PAA 重复）
走**同一组断言**，属迭代而非参数化；游程判据的**统计方法与概率依据**见 §5.6。

> ⚠ `ecc_step` 全绿只说明**没复现出实现 bug**，不是「算法被证明没问题」——
> 六处覆盖缺口与**四步提高计划**见 §5.7。

> **参数化 vs 迭代的判据**（见 `assertion_guidelines.md` §二十三）：
> **不同分支 / 不同期望 → 参数化**（每个 case 一次独立 setup/tear_down，运行器在测试名下打印 `case=…`）；
> **同一断言、不同输入 → 迭代**（`--iterations N`，同一段代码多抽几次）。

### flat CW 位图建链（v7，v1.8 改为走 FTL 建链入口）

`cw_bitmap` 是**参数化**用例（3 case，`--param case <名>`）——三条场景共用「建链 → 收链 →
断言 desc 数 → 逐条断言 (plane, cw)」的骨架，差异全在 **段宽 / `addr.plane` / 位图** 三个字段
⇒ 判据 7。期望链以显式表给出（原「循环 + 公式」形态换成逐位比表）：

| case | conf（段宽） | addr.plane | 位图 | 期望链 (plane, cw) |
|---|---|---|---|---|
| `flat-mixed` | MULTI | 0 | `0x29` | (0,0)(0,3)(1,1) |
| `plane1-only` | SINGLE | 1 | `0xF0` | (1,0)(1,1)(1,2)(1,3) |
| `span-multi-plane` | MULTI | 0 | `0xFF` | (0,0)…(0,3)(1,0)…(1,3) |

> ⚠ **v27 起位图是字面位图**（置 1 = 访问该 CW，**0 = 空集**）——`span-multi-plane` 一行的前身
> `cw_mask_zero_multi_plane` 即由这一条驱动改名。夹具侧对应
> `test_plane_cw_bitmap(plane_bitmap)`（原 `test_plane_cw_mask`）。
> **v28 起位图作 `ftl_cmd_configure_descriptors(cmd, cw_bitmap)` 的实参**——夹具里
> `cmd->param.cw_bitmap = test_plane_cw_bitmap(...)` 改成把值直接传给建链入口。
> **Level 2 / Level 3 的迁移均已完成**（v26 起建链走 `test_cmd_build()`；Level 3 于 v31 迁完，
> 见 `docs/changelog/fc_ut_level3_migration_v31.md`）。
>
> `plane1-only` 的段必须是**单 plane 且 `addr.plane = 1`**：用 multi-plane op 配只覆盖 plane 1 的
> 位图会被 `test_cmd_build` 的「逐 plane 覆盖」断言挡下——那正是静默编程未覆盖 plane 的形态。

验证的是 **desc 链几何**（plane / CW 展开与链序），不是 NAND 行为，故留在 Level 2，只调
`ftl_cmd_configure_descriptors(cmd, cw_bitmap)` 后直接走链核对。

**共计 12 个测试函数（默认构建）——其中 `crc_step_detect` / `ecc_step` / `encode` /
`process_read_ok` / `cw_bitmap` 五条是参数化，展开后实际跑 31 个用例。（CONFIG_FC_RAID=1 时
额外含 `raid_recover_no_random` + `generate_stripe_parity`，共 14 个）**

### 2.3 Level 3 — FC 全链路测试

**测试目标**：FC 命令完整生命周期（alloc → configure → submit → service → poll → release）在真实 NAND 硬件上执行。

Level 3 不验证数据变换正确性（那是 Level 2 的职责），只验证 FC 层命令编排的正确性：
- 命令能完整走完 FC 生命周期（不崩溃、不泄漏）
- 各阶段状态转换正确、`completion->command_id` 与提交的命令匹配
- 资源（command / descriptor / buffer）无泄漏

> **形态变更（v1.8）**：早期设计是「39 个 OP 码参数化展开 × user-specified block」的 `test_lifecycle_op`。现文件已改为 **9 个手写 block 级场景用例**（见下表），OP 覆盖表降级为参考附录。每个用例自带前置状态准备（擦除 / 写入 / 加载页缓冲），因此不再需要 standalone 参数。

#### 依赖与约定

- **NAND 硬件**：走 `fc_submit → fc_service → nand_fc_start/step` 异步路径；`setup_level3()` 里幂等重申 `nand_dispatch_set_armed(1)`（v1.8：旧 `nand_dispatch_set_device_ready` 已整体改名为 armed 闩锁）。
- **建链**：走 FTL 策略层 `ftl_cmd_configure_descriptors()`（v13 起）。
- **释放**：只需 `fc_command_release()`（v16 起一并回收 desc + buffer）。
- **ID 断言**：`fc_command_t` 无 `command_id` 字段（v15 删），断言用 `fc_command_to_id(cmd, &expect_id)` 反查。
- **PAA 约定**：`ch=0 ce=0 lun=0`，page=0..63，plane 0/1；**block / plane 在用例体内随机抽**（见下「随机地址与种子」节，窗口 = 每 plane 块数的 30%~70% ⇒ `614..1433`；内部搬移的目标块同窗口另抽，`src ≠ dest`）。
- **cache 的 plane 边界**（决定 helper 为什么只做单 plane，两层依据别混）：**ONFI 1.0 根本没有 multi-plane**——Rev 1.0 Table 14 命令集里 cache 只有单 plane 的 `Read Cache` 31h / `Read Cache End` 3Fh / `Read Cache Enhanced` 00h-31h / `Page Cache Program` 80h-15h，全文 `plane` 一词出现 **0 次**（1.0 唯一的「多设备」是 multiple LUN，走 `Interleaved` D1h / 11h）。而**本器件（Micron MT29F4G08）确实有 two-plane cache 编程**——datasheet Table 7 `TWO-PLANE PROGRAM PAGE CACHE MODE 80h-11h-80h-15h`；但**没有 two-plane cache 读**（读侧只有单 plane 31h / 3Fh）。本项目 op 表两种都没有 ⇒ helper 只做单 plane。
- **cache 整块读写**：夹具新增 `test_prog_block_cache()` / `test_read_block_cache()`（**单 plane**——依据见上「cache 的 plane 边界」；`config` 只提供模式位，`operation` 由 helper 逐步覆盖，调用者填什么都被忽略）。写侧前 63 页 `OP_PROGRAM_PAGE_CACHE`（15h）、末页 `OP_NORMAL_PROGRAM_PAGE`（10h）收尾。读侧**不能**由 `test_read_page` 循环拼出——31h / 3Fh 无地址周期（设备自增），且 31h 输出的是**上一轮加载的页**，故 helper 显式编排 `START(p0) → SEQ ×63 → LAST`，`meta` 取**真正输出页**的 PBA（第 i 次 SEQ 取 page i-1 的 PBA）；共 65 条命令输出 64 页。两者都是同步逐条提交 ⇒ 只验数据正确性（cache 命令序列走得通、编码管线不串），**不验流水线收益**。
- **cache 读随机跳页（v1.26 加、v1.30 订正语义）**：`test_read_cache_random(block, plane_bitmap, prev_page, addr_page, config)` 覆盖 cache 管线的**跳页入口** `OP_READ_PAGE_CACHE_RANDOM`（00h-addr-31h，带 5 个地址周期）。⚠ **v1.26 的语义理解是错的**（当成「载入 + 输出合一、输出页 = 地址页」，上板当场 `MATA_ERROR` 被证伪）。手册原文：*reads the specified block and page **into the data register** while the **previous page is output from the cache register*** ⇒ **地址页只进 data register（预取），当场输出的是 cache 寄存器里的「上一页」**，地址页要等**下一次 31h** 才出；且该命令**必须在 00h-30h 或 00h-31h 之后**下发（die 地址须与前一命令一致）⇒ 它等价于「带显式下一页地址的 31h」，**不能当独立入口单发**。故 helper 自建最小管线：`START(prev_page)` → `RANDOM(addr_page)`（**输出 prev_page**，`meta` 取 prev_page 的 PBA）→ `SEQ(31h)`（**输出 addr_page**，这一步才证「地址真的生效」）→ `LAST(3Fh)`（输出 addr_page+1 并收尾）；`prev_page` / `addr_page` 都不得是块尾（手册：cache 读不能跨块边界）。FC 侧本 op 在 `fc_is_page_read` 里（`src/fc/fc.c:125`）⇒ 走 `process_read`；NAND 侧 `req_cache_read` + `completion_per_cw`，数据散落在页缓冲（`nand_dispatch.c:662` 显式列出本 op）。`block_cache_ops` Phase 3 用它从 page 16 跳到 page 32。
- **加载 / 取数两段式（v1.27）**：生产读路径一直是「先加载、再逐 CW 取数」两段——`fc_dispatch_rewrite_operation`（`src/fc/fc.c:1221`）下发前把 `OP_NORMAL_READ_PAGE_WITH_DATA_OUT`(12) 改写成 `OP_NORMAL_READ_PAGE_WITHOUT_DATA_OUT`(33)（多 plane：30→29），`nand_dispatch_task_cw_chain`（`src/nand/nand_dispatch.c:574`）再对加载型 op 逐 CW 发取数 op `OP_RANDOM_DATA_READ`(8, 05h-E0h) / `OP_RANDOM_DATA_READ_MULTI_PLANE`(41, 06h-E0h)。**但 op 33/29/8/41 从未作为独立命令被点名过**——它们的 uop 序列、page-read 分类、页缓冲散落全无测试背书。本版把它们**显式拆成两条 FC 命令**下发：`test_load_page()` 发**不带 desc** 的纯加载命令（op 表 `has_data = 0` ⇒ 不需 desc 链、无 CW 位图，只把目标页搬进器件页缓冲、**不输出到总线**），`test_random_data_read()` 再发**带整段掩码 desc 链**的取数命令（单条命令由 CW 链自动遍历掩码内所有 CW）并逐 CW PAA 校验。为支撑这条路径：① op 表 op 33/29 的 `has_data` 由 `1U` 改 `0U`（`src/nand/onfi_1_0.c`）；② `fc_is_page_read` 补 op 41（与 op 8 对称，否则直接下发 41 拿到未解码数据）；③ `fc_build_dispatch_request` 新增「无 desc 加载」分支（op 33/29 从 `command->paa` 填 `plane_paa[]`）；④ `req_read_page` 空掩码时 `data = NULL`（消除残留指针隐患）。⚠ **生产路径零影响**：分支按 FC 命令的 op（12/30）选，op 33/29 只作 rewrite 目标出现；`fc_is_page_read(41)` 在生产里也从不被查（命令 op 恒 30）。⚠ 两段必须**紧邻**——加载与取数是两条独立命令，中间若有别的命令碰器件页缓冲，取数会读到错页。`block_load_fetch` 用它验单 plane（op 33 → op 8）与双 plane（op 29 → op 41）。
- **META 只读回读（v1.28）**：新增 op `OP_READ_META`（槽位 **11**，原 `OP_READ_MODE` 复用，`OP_COUNT` 保持 42）——**只读**回读指定页、指定 CW 的 **4 字节 META**（CW 布局 `[META:4][CRC:4][USER:512][PARITY:8]`，META 在加扰区之前、**明文**），供上层扫坏块建表。**CW 由 `command->paa` 的 cw 字段选**（PAA 原生含 cw → `plane_paa[]` → `paa_to_cw() * CW_SIZE` 作列地址），**无 desc 链、不占 buffer**（`has_data = 0` ⇒ `ftl_cmd_needs_desc_chain` / `fc_command_validate` 都不要求链）。op 自带页加载（`70h` 轮询 → `00h`+列/行 → `30h` → tR → `DATA_OUT`），uop 复用 `g_nand_normal_read_page_uops`（与普通读页**逐字节相同**），唯一差别是 `request->data_length = 4` ⇒ 只输出 META 四个字节。4 字节落控制器侧暂存 `g_fc_return_staging[command_id]`（与 `OP_GET_FEATURES` **同款**，已在板上跑通），完成时拷进 CQ 的 `fc_param_t param`。**不进 ECC / 数据路径**（op 11 不在 `fc_is_page_read` ⇒ 走非 page-read 分支），**不参与 CW 链**（不是 load/fetch op），**不被 rewrite**（`fc_dispatch_rewrite_operation` 只动 op 12/30）。`block_read_meta` 用它验单 plane：擦除 + 编程后用 `test_read_meta()` 逐 CW 读回，比对 `expected = (prog_cmd->meta & ~PLANE_CW_MASK) | (plane << PBA_PLANE_SHIFT) | cw`（同时验证**页地址**与 **CW 选择**两条轴）。⚠ op 11 **首次上板**；`00h`-addr-`30h` 后只输出 4 字节属**部分页输出**，与 GET_FEATURES 的 4 字节输出同族但作用在页读路径，需上板确认器件接受。
- **裸页 config**：本文件用 `L3_CONF(operation, crc)` 复合字面量给出「裸页」形态——与改造前 `program_page` / `read_page` / `erase_block` 字段逐字一致（**不加扰** + **LDPC RAW** + **不 RAID**；crc 逐处给，裸页读写沿用池默认的 `FC_CRC_ENABLE`，cache 与 multi-plane 路径显式 `FC_CRC_DISABLE`）。`ch/ce/lun` 由 `test_cmd_build` 从 addr 覆盖，`multi_plane` 由 operation 派生。
- **不走页 helper 的命令**（READ_ID / GET_FEATURES / 状态查询 / 内部搬移 / RANDOM_DATA_READ）用夹具的三件零件自己拼：`test_cmd_build()`（建链）+ `test_submit_wait()`（提交轮询，**命令仍归调用者**）+ `fc_command_release()`；平面段位图用 `test_plane_cw_bitmap(plane_bitmap)` 正推，`addr.plane` 用 `test_first_plane(plane_bitmap)`。
- **读回校验分两档**：整段读用 `test_verify_page_paa(data, block, page, plane_bitmap)`（按地址**重算**期望，覆盖段内全部 CW）；部分掩码读（如 RANDOM_DATA_READ 只抽 1 个 CW）用本文件的 `verify_chain_paa(cmd)`（只看**链上实际有的** desc，期望取 `desc->paa`）——此时段内其余 CW 的源槽根本没被这次读填过。
- **擦除页的判据只能是逐 CW `FC_CW_STATUS_SUCCESS`**：擦除页 META = 0xFF…，Pass 4 ① 自洽必然不过 ⇒ 该 CW 带 `MATA_ERROR`，**聚合 status 的错误掩码非零**。故 `read_block_verify_erased` 走本文件的 `read_page_verify(..., erased=1)`（在 release 前逐 CW 查 SUCCESS 位），不能用 `test_read_block` 的聚合判据。

#### 随机地址与种子（v1.22）

10 条地址相关用例（`internal_data_move` / `block_erase_verify` / `block_write_read` / `block_cache_ops` / `block_multi_plane` / `block_random_data` / `block_load_fetch` / `block_read_meta` / `block_lock_ops` / `mixed_pipeline`）**在用例体内**抽地址，**不挂 `MunitParameterEnum`**——参数表是 NULL 结尾的 `const char*` 数组，表达不了 820 宽的窗口，只能写死几个候选值（v1.15 就是 `100`/`104` 两档）。

**窗口**（`test_fc_level3.c` 顶部，**编译期**从 `BLOCKS_PER_PLANE` 算出，不写死数字）：

| 常数 | 值（`BLOCKS_PER_PLANE` = 2048） | 含义 |
|---|---|---|
| `L3_BLOCK_LO` | `2048 * 30 / 100` = **614** | 窗口下界 |
| `L3_BLOCK_HI` | `2048 * 70 / 100` = **1433** | 窗口上界 |

地址里的 `block` 字段是 **per-plane** 号，窗口取每 plane 块数的 30%~70% 以避开**固定**元数据区（本仓 per-plane 编号下落在 0..41 一带：L2P 0..31 / Anchor 33,34 / BBT 36,39 / Checkpoint 37,38 / Journal 40,41，见 `src/ftl/metadata.c` 的 `ftl_is_metadata_block`）。

**抽样函数**（都在 `test_fc_level3.c`，用途标签区分同一用例内的不同抽取）：

| 函数 | 抽什么 |
|---|---|
| `l3_block_draw(test)` | 窗口内一个块号（闭区间） |
| `l3_plane_draw(test)` | `[0, PLANE_NUM)` 一个 plane |
| `l3_dest_draw(test, src)` | 内部搬移目标块：同窗口另抽，撞上 `src` 就**环移一格**（不额外抽 ⇒ 抽样次数固定） |
| `l3_pages_draw(test, out)` | n ∈ [2,8] 个**互不重复**的页（拒绝采样），返回 n |
| `l3_cws_draw(test, out)` | n ∈ [1,4] 个**互不重复**的 CW（拒绝采样），返回 n |

**抽样顺序即复现契约**（各用例体内依次）：`internal_data_move` ① plane ② block ③ dest；`block_erase_verify` / `block_write_read` / `block_cache_ops` ① plane ② block；`block_multi_plane` ① block（plane 由 multi-plane op 派生）；`block_random_data` ① plane ② block ③ 页集（先抽个数再抽元素）④ CW 集（同）；`mixed_pipeline` ① block（plane 由 multi-plane op 派生，页全遍历）——主循环（页 × CW 探针）**不再消耗随机流**。上限 8×4 = 32 个探针 = 旧 `dense` 档的最坏值 ⇒ **板上耗时上界不变**。

**必须加 salt**：munit 在每次 `exec` 前把 PRNG 重置回 `runner->seed`（`munit.c:774`），而导出的 `munit_rand_*` **不带 salt**（带 salt 的 `munit_rand_at_most` 是 `static`，`munit.c:503`）⇒ 用例体内的**第一次**抽样在所有用例之间取值完全相同（六条会撞在同一个块上、整轮只有一个 plane）。故 `l3_rand(test, tag) = munit_rand_uint32() ^ l3_hash(test) ^ l3_hash(tag)`：**用例名**让用例之间互异，**用途标签**让同一用例内各用途互异。`l3_hash` 是 DJB2，与 `munit.c:386` 的 `munit_str_hash` 逐字同实现（那份是 `static`，只能照抄）。

**复现契约 = (seed, 用例名, 第几次抽样)**：reseed 是**每用例一次**，故复现单条用例**不需要**重放其它用例。`runner.seed` 一次运行一个值（默认由 `time(NULL)` 派生、开场打印 `Running test suite with seed 0x…`，`--seed N` 可复现）；抽到的值一律经 `test_trace_summary` 打出来（`--log-visible info`），否则有 seed 也对不上是哪个块。

**覆盖旋钮是 `--iterations N`**（不是参数表）：六条随机化行用 `MUNIT_TEST_OPTION_NONE`（**保留**，别换成 `SINGLE_ITERATION`——`munit.c:765-767` 会把迭代数硬压成 1，旋钮就锁死了）。`test --iterations 20 /fc-nand/lifecycle/` ⇒ 六条随机化用例的**用例体各跑 20 次**（各抽 20 组地址）。

⚠ **报告里的条数不变**：`--iterations` 只乘**用例体**，不乘**条目**——`munit_test_runner_exec` 在 `do{…}while(++i<iterations)` 里逐次累加的是**局部** `report.successful`（`munit.c:803`），而 `runner->report.successful++` 在调用点**每条只加一次**（`munit.c:989`）⇒ 仍打 `9 of 9`。上板实测（2026-10-11，seed `0x5a4f21e4`）：`--iterations 5` 打 `9 of 9`，但 `internal_data_move` 打了 5 组 `[EV] move …`、总耗时 522 ms → 2.61 s；且单条打印的 `[ N ms ]` 变成**每次迭代的平均值**（`munit.c:981` 的 `report.wall_clock / report.successful`），故九条的单条时间几乎逐字不变（`internal_data_move` 144.93 → 144.83 ms）。**判断加量有没有生效看总耗时，不要看条数。**

⚠ `--iterations` 是 **runner 级全局量**，且 `runner->suite` **恒指根套件**（`munit.c:1359`）⇒ 子套件自己的 `iterations` 字段无效，**别去改 `ut_main.c`**（改了会把 Level 1/2 也乘 N）；它按调用范围生效，只跑 `/fc-nand/lifecycle/` 就只乘 Level 3。

**`internal_data_move` 的 Step 5 按源坐标比对**：copyback 搬的是源页内容——`READ_FOR_INTERNAL_DATA_MOVE` / `PROGRAM_FOR_INTERNAL_DATA_MOVE` 都**不是** page read/write，数据路径不介入、不重铸 ⇒ 目标页的 USER 仍是**源块坐标**的 PAA。故 Step 5 逐页 `test_read_page(dest, …, tag, …)` + `test_verify_page_paa(g_source_data, src, page, …)`（期望取**源块**），**不能**用 `test_read_block(dest)`——后者按入参 block 重算期望（`fc_test_common.c:644-675`）。数据路径侧照常通过：META 低 3 位 = plane/cw（源/目标同 plane、同 cw 枚举）、META 与命令 meta **同页** ⇒ Pass 4 两项都过；CRC 因 USER 原样搬过而一致。⚠ **v34 起 meta 是页 PBA**：读命令声明的 `tag` 必须是**源页 PBA**——META 存的是「内容的身份」而非「内容现在在哪」，搬移不经过数据路径，源页写入时铸的 PBA 原样跟到了目标块；声明目标页 PBA 反而会被 Pass 4 ② 判成「读到了别的页」。⚠ **v1.22 起 dest 也是抽出来的**（不再是 `src + 2`）⇒ 上面这条更容易写错。

**`internal_data_move` 的 Step 5 按源坐标比对**：copyback 搬的是源页内容——`READ_FOR_INTERNAL_DATA_MOVE` / `PROGRAM_FOR_INTERNAL_DATA_MOVE` 都**不是** page read/write，数据路径不介入、不重铸 ⇒ 目标页的 USER 仍是**源块坐标**的 PAA。故 Step 5 逐页 `test_read_page(dest, …, tag, …)` + `test_verify_page_paa(g_source_data, src, page, …)`（期望取**源块**），**不能**用 `test_read_block(dest)`——后者按入参 block 重算期望（`fc_test_common.c:644-675`）。数据路径侧照常通过：META 低 3 位 = plane/cw（源/目标同 plane、同 cw 枚举）、META 与命令 meta **同页** ⇒ Pass 4 两项都过；CRC 因 USER 原样搬过而一致。⚠ **v34 起 meta 是页 PBA**：读命令声明的 `tag` 必须是**源页 PBA**——META 存的是「内容的身份」而非「内容现在在哪」，搬移不经过数据路径，源页写入时铸的 PBA 原样跟到了目标块；声明目标页 PBA 反而会被 Pass 4 ② 判成「读到了别的页」。

#### 测试用例（15 个，其中 10 条地址随机化）

| 测试函数 | 场景 | 验证点 |
|---|---|---|
| `init_sequence` | RESET → READ_ID → READ_PARAMETER_PAGE → GET_FEATURES → READ_UNIQUE_ID(3) | 初始化序列各步生命周期完成；READ_ID 前 5B / 参数页 ONFI 签名 / **唯一 ID 16B（v1.36 补测 op 3：只打证据、不断言内容——手册未给死值）** |
| `feature_write_read` | GET_FEATURES ×4 → 01h 读-改-写回环 | Feature 写进去的值读得回来（见下） |
| `chain_priority_lock` | SQ 8 命令链（前 7 MORE / 末条 END）+ 同 LUN HSQ 命令 | **链锁**：链未结束期间该 LUN 的填充权归链所在队列独占——HSQ 插不进链（终值 = HSQ 写的值）；对照 Step 2 证明 HSQ 平时**能**插队（见下） |
| `status_query` | READ_STATUS / READ_STATUS_ENHANCED | 状态查询生命周期 + CW status |
| `internal_data_move` | READ_FOR_INTERNAL_DATA_MOVE → PROGRAM_FOR_INTERNAL_DATA_MOVE | 源 block → 目标 block 搬移后 64 页逐字节一致 |
| `block_erase_verify` | ERASE → 全 0xFF 校验 | 擦除后逐页全 0xFF |
| `block_write_read` | PROGRAM → READ 全 block | 写读数据一致 |
| `block_cache_ops` | CACHE_START(p0) → CACHE_SEQ ×63 → CACHE_LAST；+ CACHE_START(p16) → CACHE_RANDOM(p32) → SEQ → LAST | cache 读管线（30h/31h/3Fh）；写侧前 63 页 15h + 末页 10h；Phase 3 用 00h-addr-31h 跳页入口：输出 p16、地址页 p32 在下一步出 |
| `block_multi_plane` | MP_ERASE → MP_PROGRAM → MP_READ | 两 plane 各自 pattern 独立验证（desc 0..3=plane0，4..7=plane1） |
| `block_random_data` | READ + RANDOM_DATA_READ 抽样 | 抽页集/列集后随机列读，验证列地址解码 |
| `block_load_fetch` | LOAD(33/29，无 desc) → FETCH(8/41，带 desc) | 加载/取数两段式显式下发：单 plane（33→8）+ 双 plane（29→41）逐 CW PAA 一致 |
| `block_read_meta` | ERASE → PROGRAM → READ_META(11) ×2 CW；+ **普通读 → `fc_buffer_get_meta` 复核（v1.36）** | 无 desc 只读回读 CW 前 4 字节 META，与编程时写入的 PBA 逐字节一致（CW0 / CW2）；**链内 META 复核**：普通读完成后经 `fc_buffer_get_meta(cmd, cw)` 从 CW 基址 offset 0 取 META，与 op 11 结果逐位一致（gc.c 的真实消费路径），且不在链上的 CW 号返回 NULL |
| `block_lock_ops` | LOCK_STATUS(25) → LOCK(23) → LOCK_STATUS → ERASE(证据) → UNLOCK_LOW(21) + UNLOCK_HIGH(22) → LOCK_STATUS → ERASE/PROGRAM/READ | 块锁族（**不含**永久锁 24）走通且**可逆**：各 op 无错误位，锁序列后块仍完全可用；「LOCK 是否真挡住擦除」「状态字节怎么变」只打印证据、不断言 |
| `mixed_pipeline` | 整块先写后读（**流式提交 / 池满即抽干**）：Phase 0 multi-plane 擦块 → Phase 1 流式 PROGRAM 全块 64 页 → Phase 2 流式 READ 全块并逐 CW 校验 | **背压闭环**：连续下发不等完成，每成功下发 4 条（或本条失败）才 polling + 取 CQ，在途命令顶到池上限 ⇒ 建链失败 ⇒ 抽 CQ 腾资源 ⇒ **重试同一条**；每条完成项按 `command_id` 配回槽位、无错误位、READ 逐页一致；两 phase 收尾三池归零 |
| `buf_own_release` | `OP_READ_ID` 单 CW 链 × 四档 `buf_own`（00 默认 / 01 / 10 / 11）+ 「建链未提交」+ 空闲链完整性守卫 | **资源账闭合**：`fc_get_resource_stats().buffer_used` 的增量——00 建链 +1、完成仍 +1、release 回基线；01/11 完成即回基线且再 release 幂等；10 完成仍 +1、release 回基线（读法 A 回归守卫）；建链未提交也不泄漏；连续申请满池 id 两两不同（抓双重释放造成的重复入链）（见下） |

> 前 3 条（`init_sequence` / `feature_write_read` / `status_query`）是定长命令序列、地址不参与，**不随机化**（保持 `SINGLE_ITERATION` + `NULL`）；`chain_priority_lock` 也是**固定脚本**（feature 地址 `01h` 不参与抽样），用 `MUNIT_TEST_OPTION_NONE` 但体内不抽地址；`buf_own_release` 同样**固定**（只验资源账，地址恒 `block 0 / plane 0 / page 0`，介质内容不参与）；其余 10 条**在用例体内**抽地址（见上「随机地址与种子」节）。每条用例的 setup/teardown 与 Level 2 共用同一份 `test_setup` / `test_tear_down`（`fc_test_common.c`）。
>
> `block_lock_tight`（紧锁定，不可逆锁死整片 NAND）实现保留但以 `#if 0` 隔开、不注册。

#### 流式提交 / 池满即抽干（v1.32 起，v1.33 重写）

绝大多数用例是「**发一条 → `fc_service` 轮询 → `fc_poll_completion` 只取一条 → 释放**」——简单、可复现，但**从不给流水线加压**：任何时刻 NAND 层最多 1 条在途（`chain_priority_lock` 例外，它一次连投一条 8 命令链以验链锁，但链内仍逐条串行推进）。`mixed_pipeline` 换成**连续下发、不等完成**的形态，靠资源耗尽自然形成背压：

- **流式提交**：逐页 `fc_submit`，**不等完成**；只在「已成功下发 4 条」或「本条下发失败（池满 / SQ 满）」时才做一次 polling + 取 CQ。在途命令因此涨到池上限，完成项又不断回流腾资源——这是真实主控的背压节奏。
- **粒度 = 整块先写后读**：Phase 0 擦块 → Phase 1 写全块 64 页 → **抽干** → Phase 2 读全块并逐 CW 校验。先写后读（不是读写擦交替）⇒ 读的每一页必然已写完，**无数据冒险**。
- **multi-plane**：一条命令覆盖 `PLANE_NUM` 个 plane ⇒ 遍历维度只剩 `ch→ce→lun→page`，且每条命令吃 **`PLANE_NUM*CW_NUM` 份 desc + buffer**。这一步是必须的——**buffer 是绑定池**，阈值 = `FC_BUFFER_POOL_DEPTH / (PLANE_NUM*CW_NUM)`：单 plane 是 32 条，multi-plane 降到 **16 条**。
- **建链失败是可恢复的**：夹具为此新增 `test_cmd_try_build()`（同配置校验，但资源申请失败**返回 NULL** 而非 abort；失败时已整条 `fc_command_release`，调用者无需再释放）。配置错误（坐标越界 / 掩码漏 plane）**仍是 ASSERT**——那是调用者 bug，不是资源问题。
- **批量收取**：新增 FC 公开 API

  ```c
  fc_status_t fc_poll_completions(fc_command_status_t *completions, uint16_t max_count, uint16_t *count);
  ```

  **弹到上限或 CQ 空为止**，实际条数写入 `*count`；有取到 = `OK`，一条没取到 = `QUEUE_EMPTY`（含 `max_count==0`）。单条版 `fc_poll_completion` 原样保留，两条路径并存。
- **上限由调用方给**：`max_count` 不写死任何 FC 常量。测试里传 `batch = CH_NUM * CE_NUM * LUN_NUM`——这正是 `nand_dispatch_poll` 单次调用的推进上限（三层循环 ch→ce→lun、全遍历不早退，`nand_dispatch.c:755`）。今天 `= 1`，换多 die 器件同一份代码自动变大。
- **拓扑无关**：`batch`（以及 `L3_STRESS_POLL_EVERY` 这类节奏常量）都是**测试侧**的编译期表达式，FC 不知道也不该知道拓扑；FC 只给机制（弹到上限/空），策略（一次收几条）留给上层。对齐行业模型：NVMe 的 SQ/CQ 由主机驱动、条目靠 CID 配对，控制器不规定主机一次收几条。
- **配对靠 `command_id`**：`fc_command_status_t.command_id` 是命令的稳定身份（v15 起由命令指针派生），测试用它把完成项配回本地槽表，再取回 `fc_command_t*` 释放——**先 poll 后 release**，兑现「命令不得还在 CQ 里」的契约。
- **重试同一条**：池满 / SQ 满时**本页还没发出去**，polling 腾出资源后**用同一 `addr` 重建**——测的就是「资源回流后同一条能建成」这条不变式。
- **退出循环后收干净**：遍历完再 `while (drained < submitted) { fc_service(); reap(); }`，把剩余 CQ 一起处理完。
- **逐页证据（`--log-visible debug`）**：每页打一行 `p19 retry +poll submitted=19 drained=3 | cmd=16/64 desc=128/256 buf=128/128 sq=15 cq=1`，直接看出「发到第几条开始 retry、retry 后哪个池被腾空」。默认日志档**输出逐字不变**（`test_trace_detail` = `MUNIT_LOG_DEBUG`）。

> ⚠ `fc_service()` 本轮**只要交付过 ≥1 条完成，就整段跳过填充 pass**（`fc_poll_internal`，`src/fc/fc.c:1793`）⇒ `retry` **不等于**必有一次完成：某次 poll 可能只走了填充 pass、一个完成都没交付（上板实测 `p20` 就连 `retry` 了两次）。这是刻意行为，不是缺陷。
>
> ⚠ **背压是否触发，判据不依赖它**：用例的硬判据是「全块写-读完成 + 逐 CW 校验通过 + 三池归零」；`retry` 只出现在 debug 证据行里。若调深 buffer 池导致不再触顶，用例**仍应绿**——否则改池深就会假红。

#### 链锁用例（v52）

`chain_priority_lock`（`MUNIT_TEST_OPTION_NONE`，注册进 `g_fc_lifecycle_tests[]`、紧跟 `feature_write_read`）验 §2.7 的**链锁**——链未结束期间该 LUN 的填充权归链所在提交队列独占。用 `01h`（Timing mode）观察：`x = 01h`、`y = 02h`（都在手册 Table 15 的 `00h..05h` 内）。记法 `set0`/`get0` = `high_priority=0`（SQ）、`set1`/`get1` = `high_priority=1`（HSQ）。

- **Step 0**：GET 原值，断言 `== 00h`（手册默认模式 0）。
- **Step 1**：SQ 基线 `set0(x) → get0`，断言读回 `x`。
- **Step 2（HSQ 真能插队）**：`set0(x) → get0 → set1(y)` 三条一起提交后抽干；`get0` 与终值都是 `x`——`set1(y)` 先落盘、随后被 `set0(x)` 覆盖 ⇒ HSQ 抢先，证明「HSQ 有插入能力」。
- **Step 3（★ 链锁：链没结束 HSQ 插不进）**：连续 `fc_submit` 8 条 SQ 链（前 7 条 `chain=FC_CHAIN_MORE`、末条 `FC_CHAIN_END`，set/get 交替），`fc_service()` **一次**（前 4 条进 FIFO，`chain_owner=SQ`），再投 HSQ `set1(y)`/`get1(y)`，抽干；`get1` 读到 `y`、**终值 `y`**。
- **Step 4**：还原 `01h` 原值。

**判据的区分力**：无链锁时 HSQ 的 `y` 会插在链的第 4、5 条之间，链的后 4 条（含 `set0(x)`）会在 `y` 之后执行 ⇒ 终值 **`x`**；有链锁 ⇒ 终值 **`y`**。Step 2 与 Step 3 合起来才说明问题：**HSQ 确实能抢占**（Step 2 读回 `x` 而非 `y`）、**但链一旦开始就抢不进来**（Step 3 终值 `y`）。⚠ **生产路径零影响**——FTL 恒 `FC_CHAIN_END` ⇒ `chain_owner` 恒 NONE，只有本用例（及未来真用链的调用者）会走到新分支。夹具新增 `g_l3_prio_slots[]` + `l3_prio_reset/submit/drain`（按 `command_id` 配回槽位、GET 存值、release，与 `mixed_pipeline` 的 CID 配对同路）。

#### buffer 归属用例（v53）

`buf_own_release`（`MUNIT_TEST_OPTION_NONE`，注册进 `g_fc_lifecycle_tests[]`、紧跟 `mixed_pipeline`）验 §12.10 的 **`fc_cmd_conf_t.buf_own : 2`**——本命令的数据 buffer 谁申请、谁归还。**判据 = `fc_get_resource_stats().buffer_used` 的增量**（测试不 include `fc_internal.h`，只走公开统计接口）。载体是 `OP_READ_ID` 单 CW 链（单 plane、`has_data=1`、无需建块，同 `init_sequence` Step 2）；**只验资源账，不比对介质内容**（数据正确性由 `init_sequence` 覆盖）。

- **Step 1 `00`（默认，必须逐字不变）**：`test_cmd_build` → 建链即 `+1` → 完成仍 `+1` → `fc_command_release` → 回基线。
- **Step 2 `01`**：`test_cmd_build` → 建链 `+1` → **完成即回基线**（FC 当场归还）→ 再 `fc_command_release` 仍基线（**幂等**，无双重归还）。
- **Step 3 `11`**：先 `fc_buffer_alloc()`（`+1`）→ 手工建链（**5 参** `fc_command_configure_descriptor_with_buffer`）→ 完成即回基线 → 再 release 仍基线。
- **Step 4 `10`**：先 `fc_buffer_alloc()`（`+1`）→ 手工建链 → 完成**仍 `+1`** → `fc_command_release` → 回基线 ← **读法 A 的回归守卫**（release 恒归还链上未归还的 buffer，不管谁申请的）。
- **Step 5「建了链但没提交」**：`01` 建链后直接 `fc_command_release` → 回基线（**不泄漏**——没走完成时 buffer 仍有效，链释放照常归还）。
- **Step 6 空闲链完整性守卫**：连续 `fc_buffer_alloc()` 满池 `FC_BUFFER_POOL_DEPTH` 次，断言 id **两两不同**（双重释放会让同一 id 被链入 free list 两次），再全部 `fc_buffer_release` 回基线。

模式 `10`/`11` **不能**用 `test_cmd_build`（它走 `ftl_cmd_configure_descriptors`，即 4 参入口 = FC 申请那条路）；夹具侧照 `test_cmd_try_build` 的样子手工建：`fc_command_alloc` → 填 `config`（含 `buf_own`）/`ch`/`ce`/`lun`/`module`/`paa` → 单次 `fc_command_configure_descriptor_with_buffer`（本文件的 `l3_buf_own_build_upper`）。跑完仍用现有 `test_submit_wait` 抽干，**不新造基础设施**。⚠ **生产路径零影响**——FTL 全走默认 `00`。

**`feature_write_read` 的地址一律取自器件手册**（MT29F4G08ABADAWP Table 13）：
`01h` Timing mode / `80h` 驱动强度 / `81h` R/B# 下拉 / `90h` Array operation mode；
`00h`、`02h–7Fh`、`82h–FFh` 是**保留段**，读写皆未定义——**不得自造地址**
（该用例第一版照抄 FTL `read_recovery.h` 的 `RR_FEATURE_READ_RETRY = 0x89`，落在保留段，上板直接红）。

- **Phase 1**：对四个地址各 GET 一次，**只读**，一个器件状态都不动。
- **Phase 2**：在 **`01h`** 上做读-改-写回环——`GET 原值(应 00h) → SET 模式 1(01h) → GET 校验 → SET 原值 → GET 校验`，
  探针取 Table 15 给死的 `01h`（模式 0..5 = `00h..05h`，模式 0 是出厂默认）。
- **为什么回环选 `01h` 而不是 `90h`**——取决于**还原失败的代价**。手册说 feature 断电才复位、
  连 `RESET(FFh)` 都清不掉，所以还原失败必然把器件留在非默认态：停在**模式 1 是良性的**
  （模式是「器件能接收的上限速度」，而主机 FMC 时序按模式 0 配、比器件上限慢，照常工作）；
  停在 `90h` 的 `08h` 则**内部 ECC 一直开着**，后面所有裸页读写会静默出错。
  `80h` / `81h` 一律**只读**（改驱动强度影响信号完整性；改 R/B# 下拉写重了 WAIT_READY 会读到超时）。
- **还原的 SET 排在断言之前**，断言 abort 时也不会把器件留在非默认态。

#### OP 覆盖参考表（附录）

下表列出各 OP 是否有 NAND 状态机前置依赖（`standalone=❌` 表示独立执行时 NAND 可能返回错误，需前置 READ/PROGRAM）。当前 15 个用例各自准备前置状态，不直接依赖本表。

> **v1.27 起 op 33/29/8/41 被直接点名**：`block_load_fetch` 用 `test_load_page()`（op 33/29，**不带 desc**）显式加载页缓冲，再用 `test_random_data_read()`（op 8/41，带 desc）取数。此前这四个 op 只在生产读路径里**间接**跑到（rewrite + CW 链），现有了直接下发的用例。注意本表的 `standalone` 列描述的是**器件前置依赖**（op 8/41 取数前必须有加载 ⇒ 仍 ❌），与本用例「先加载、再取数」的两段式一致，不因被点名而改变。
>
> **v1.28 起 op 11 是新增项**：`OP_READ_META` 复用原 `OP_READ_MODE` 的槽位 11（`OP_COUNT` 不变，仍 42；空位只剩 **9 / 20**），是本套第一个**无 desc 链、直接写 CQ `param`** 的只读 op，由 `block_read_meta` 点名。`standalone = ✅`——它**自带页加载**（`00h`-addr-`30h`），不依赖前一条命令留下的页缓冲。
>
> **v1.31 起 op 21/22/23/25 被直接点名**：`block_lock_ops` 一条用例走完块锁族（**不含**永久锁 `OP_BLOCK_LOCK_TIGHT` / 24），见 `docs/changelog/fc_ut_block_lock_v49.md`。这 4 个 op 此前**零用例、零生产调用者**，且落地时发现两处 src 缺陷（`plane_paa[]` 未填 ⇒ 23h/24h/7Ah 恒寻址 block 0；7Ah 状态字节在 FC 边界被丢弃），已一并修（`src/nand/onfi_1_0.c`、`src/nand/nand_dispatch_tables.c`、`src/fc/fc.c`）。⚠ **op 25 的 `has_data` 由 1 翻成 0**——7Ah 的状态字节走 DQ 总线直读（`NAND_UOP_DATA_OUT_STATUS` → `completion->nand_status` → CQ `param.bytes[0]`），**不经数据缓冲**，标 `has_data=1` 是语义谎言（先例 = v45 的 op 33/29）。本表的 `standalone` 列对这 4 个 op 仍为 ✅——它们都不依赖前一条命令留下的页缓冲（23h/24h/7Ah 自带 3 个地址周期，2Ah 无地址周期）。

>
> **v1.32 起 `mixed_pipeline` 不新增任何 op**：它是**形状**用例（批量提交/批量收取 + 读写擦交替），复用现成的 `OP_NORMAL_PROGRAM_PAGE`(16) / `OP_NORMAL_READ_PAGE_WITH_DATA_OUT`(12) / `OP_ERASE_BLOCK`(18)，本表**不变**。见 `docs/changelog/fc_ut_stress_v50.md`。
>
> **v1.33 起 `mixed_pipeline` 改用 multi-plane 变体**（仍是**形状**用例，本表仍**不变**）：Phase 0 `OP_ERASE_BLOCK_MULTI_PLANE`(32)、Phase 1 `OP_PROGRAM_PAGE_MULTI_PLANE`(31)、Phase 2 `OP_READ_PAGE_MULTI_PLANE_WITH_DATA_OUT`(30)。这三个 op 此前已由 `block_multi_plane` 点名，本版只是把它们用在**压力路径**上——一条命令吃 `PLANE_NUM*CW_NUM` 份 desc/buffer，**buffer 绑定阈值因此从 32 条降到 16 条**，64 页内必然触顶 ⇒ 压出背压。见 `docs/changelog/fc_ut_stress_v51.md`。
>
> **v1.36 起 op 3（`OP_READ_UNIQUE_ID`）被 `init_sequence` 点名**（见 `docs/changelog/fc_ut_read_unique_id_buffer_meta_v54.md`）：此前它**零用例**（`grep OP_READ_UNIQUE_ID tests/` 无命中），而 op 表里它与 op 2/4 共用同一 UOP 模板（`EDh` vs `ECh`/`EEh`）却从未在板上跑过。`init_sequence` 新增 Step 5 直接下发（`has_data=1`、256B 进 CW0、位图 `0x01`），硬判据只到「命令 + CW 状态全 SUCCESS」——唯一 ID 是出厂烧录的 16 字节，**手册未给死值** ⇒ 16 字节只打证据、不断言内容（全 `0xFF` 即视为器件未实现）。本表 `standalone` 列对 op 3 仍为 ✅（不依赖前一条命令的页缓冲）。
>
> **v1.37 起 op 9 是新增项**（见 `docs/changelog/fc_ut_read_onfi_signature_v59.md`）：`OP_READ_ONFI_SIGNATURE` 复用已删除槽位 9（原 `OP_RANDOM_DATA_INPUT`，功能早已由 op 10 覆盖）⇒ **`OP_COUNT` 保持 42，空位只剩 20**（上面 v1.28 批注里说的「9 / 20」中的 9 已被本版占用；20 是墓碑，**不得复用**）。手册 Figure 27：同一条 `90h` 把地址段给 **`20h`**（不是 Figure 26 的 `00h`）就只回 4 字节 `4F 4E 46 49` = `"ONFI"`。**无 desc 链**（`.has_data = 0U` ⇒ `ftl_cmd_needs_desc_chain` / `fc_command_validate` 都不要求链）、**uop 复用 `g_nand_read_id_uops`**（`NAND_UOP_COLUMN_0` 写的是 `request->column` 而非 `uop->operand` ⇒ 同一数组同时服务 00h / 20h），4 字节经控制器侧暂存 `g_fc_return_staging` 落 CQ 的 `param`（与 `OP_GET_FEATURES` / `OP_READ_META` 同款）。由 `init_sequence` 新增 **Step 4** 点名。⚠ 与 `OP_READ_PARAMETER_PAGE`(2) 的 "ONFI" **不是同一条路径**：EC 走 desc 链、字节落 USER 缓冲；本 op 走 90h、无链、字节落 CQ `param`——`init_sequence` 里两步并排对照。`standalone = ✅`（器件级命令，不依赖任何介质状态）。

> **v1.39 起 op 42 是新增项**（见 `docs/changelog/read_column_v63.md`）：`OP_READ_COLUMN`——**按显式列地址**读一页的 4 个字节。**`OP_COUNT` 42 → 43，全仓首次抬升**（0..41 已满，唯一空位是墓碑 20，**不得复用**）。起因：`bbt` 打出的坏块图恒为全 `FF`，因为**出厂坏块标记在列 2048**（spare 首字节，x8 为 `00h`），而全仓唯一能指定列的 op 11（`OP_READ_META`）的列 = `paa_to_cw(paa) * CW_SIZE`，cw 只有 2 位 ⇒ 列上限 1584，**够不到 spare 区**。与 op 11 的唯一区别：**列地址不走 `paa` 的 cw 字段，改由命令级 SQ `param.bytes[0..1]`（小端）传入**——为此 dispatch 层新增 `uint16_t column` 承载字段（`feature_addr` 只有 8 位、装不下 2048，**不合并**）；**结果仍走 CQ `param.bytes[0..3]`**，与 op 11/9 同款控制器侧暂存 `g_fc_return_staging` 机制。**无 desc 链**（`.has_data = 0U`）、**uop 复用 `g_nand_normal_read_page_uops`**（与普通读页逐字节相同，差别仅在列来源与输出长度 4）。`standalone = ✅`（自带 `00h`-addr-`30h` 页加载）。

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
| 9 | `OP_READ_ONFI_SIGNATURE` | ✅ | 90h + 地址 20h → 4 字节 "ONFI"（无 desc、写 CQ `param`，v1.37） |
| 10 | `OP_PROGRAM_FOR_INTERNAL_DATA_MOVE` | ❌ | 内部搬移编程 |
| 11 | `OP_READ_META` | ✅ | 只读回读 CW 前 4 字节 META（自带加载、无 desc，v1.28） |
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
| 34 | `OP_READ_PAGE_CACHE_START` | ❌ | 缓存读启动（cache 管线） |
| 35 | `OP_READ_PAGE_LOAD` | ❌ | 多 LUN 读 ISSUE 段（Phase V） |
| 36 | `OP_READ_PAGE_FETCH` | ❌ | 多 LUN 读 FETCH 段（Phase V） |
| 37 | `OP_PROGRAM_PAGE_ISSUE` | ❌ | 多 LUN 编程 ISSUE 段 |
| 38 | `OP_PROGRAM_PAGE_WAIT` | ❌ | 多 LUN 编程 WAIT 段 |
| 39 | `OP_ERASE_BLOCK_ISSUE` | ❌ | 多 LUN 擦除 ISSUE 段 |
| 40 | `OP_ERASE_BLOCK_WAIT` | ❌ | 多 LUN 擦除 WAIT 段 |
| 41 | `OP_RANDOM_DATA_READ_MULTI_PLANE` | ❌ | 多 Plane 页缓冲随机读单 CW（06h-E0h） |
| 42 | `OP_READ_COLUMN` | ✅ | 按**显式列地址**（SQ `param.bytes[0..1]`）读 4 字节；够得到列 2048 的 spare 区（无 desc、写 CQ `param`，v1.39，`OP_COUNT` 首次抬到 43） |

---

## 3. 测试套件组织

```c
/* test_fc_level1.c — 单个子套件 logic，共 12 个测试，全部零 setup */

static MunitTest g_fc_logic_tests[] = {
  /* Part 1: fc_command */
    {       "validate_op_range",        test_command_validate_op_range, NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {"validate_set_get_no_desc", test_command_validate_set_get_no_desc, NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},

 /* Part 2: fc_submission_queue */
    {                "push_pop",                   test_queue_push_pop, NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {             "wrap_around",                test_queue_wrap_around, NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {                "overflow",                   test_queue_overflow, NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {                    "peek",                       test_queue_peek, NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {              "peek_empty",                 test_queue_peek_empty, NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},

 /* Part 2b: fc_completion_queue */
    {             "cqueue_init",                      test_cqueue_init, NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {         "cqueue_push_pop",                  test_cqueue_push_pop, NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {         "cqueue_overflow",                  test_cqueue_overflow, NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},

 /* Part 3: fc_scheduler */
    {          "scheduler_init",                   test_scheduler_init, NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},

    {                      NULL,                                  NULL, NULL, NULL,             MUNIT_TEST_OPTION_NONE, NULL},
};

static MunitSuite g_fc_nand_suite = {
    (char *)"/fc-nand/",
    NULL,
    (MunitSuite[]){
        { (char *)"logic/",     g_fc_logic_tests,     NULL, 1, MUNIT_SUITE_OPTION_NONE },
        { (char *)"data_path/", g_fc_data_path_tests, NULL, 1, MUNIT_SUITE_OPTION_NONE },
        { (char *)"lifecycle/", g_fc_lifecycle_tests, NULL, 1, MUNIT_SUITE_OPTION_NONE },
        { NULL, NULL, NULL, 0, MUNIT_SUITE_OPTION_NONE },
    },
    1,
    MUNIT_SUITE_OPTION_NONE,
};
```

> **⚠ 前缀必须以 `/` 结尾**：munit 把前缀与用例名**首尾相接**，不补分隔符
> （`munit.c:1285` 下探子套件、`munit.c:1103` 拼用例名），而本仓的**用例名不带前导斜杠**。
> 根前缀 `"/fc-nand/"` 已带尾斜杠，子套件漏了就会打印成
> `/fc-nand/lifecycleblock_multi_plane`，且过滤串 `/fc-nand/lifecycle/…` **一条都匹配不上**。

> **2026-10-08 合并**：Level 1 原先按模块切成 `command`(2) / `queue`(8) / `scheduler`(1)
> 三个子套件，现合并为单个 `logic`——**一个文件一个套件**，与 Level 2（`data_path`）/
> Level 3（`lifecycle`）一致。合并理由：合并后 `command` 只剩 2 条、`scheduler` 只剩 1 条，
> 过滤粒度已细到等于「单跑一条用例」；而 Level 1 全部 11 条都是纯 RAM 微秒级，按模块过滤
> 省不出任何东西。真正需要过滤的边界是**依赖面**（零依赖 / 需池 / 需真 NAND），那由根套件的
> Level 1/2/3 三层保住。合并后单条用例仍可按全路径跑（`/fc-nand/logic/push_pop`）。
> 套件名 `logic` 取自本层别名「纯逻辑测试」（§2.1），与 `data_path` / `lifecycle` 同属
> 描述性命名的风格。

```c
/* test_fc_level2.c — data_path 子套件，共 12 个测试（CONFIG_FC_RAID=1 时 14 个） */
extern MunitTest g_fc_data_path_tests[];

/* test_fc_level3.c — lifecycle 子套件，15 个 block 级场景用例 */
extern MunitTest g_fc_lifecycle_tests[];
```

---

## 4. 测试入口与集成

### 4.1 裸机入口

在 `tests/unit/ut_main.c` 中提供 `ut_run_all()`，供 UART CLI 调用：

```c
#include "munit.h"

extern MunitTest g_fc_logic_tests[];
extern MunitTest g_fc_data_path_tests[];
extern MunitTest g_fc_lifecycle_tests[];

static MunitSuite g_fc_nand_suite = {
    (char *)"/fc-nand/", NULL,
    (MunitSuite[]){
        { (char *)"logic/",     g_fc_logic_tests,     NULL, 1, MUNIT_SUITE_OPTION_NONE },
        { (char *)"data_path/", g_fc_data_path_tests, NULL, 1, MUNIT_SUITE_OPTION_NONE },
        { (char *)"lifecycle/", g_fc_lifecycle_tests, NULL, 1, MUNIT_SUITE_OPTION_NONE },
        { NULL, NULL, NULL, 0, MUNIT_SUITE_OPTION_NONE },
    },
    1, MUNIT_SUITE_OPTION_NONE,
};

int ut_run_all(int argc, char *argv[])
{
    return munit_suite_main(&g_fc_nand_suite, NULL, argc, argv);
}
```

配套头文件 `tests/unit/ut_main.h` 声明该接口供 `usart.c` 包含。

### 4.2 UART CLI 集成

在 `stm32h743/usart.c` 中加入 `test` 命令：

```c
#if defined(FC_NAND_TEST_ENABLE)
#    include "ut_main.h"
static int uart_cli_test(int argc, char *argv[]);

/* ... 命令表中注册 test 命令 ... */
{ "test", "test [--seed N] [--log-visible info|debug] [--list] [test_name...]", "run munit unit test suite", 1U, 10U, uart_cli_test },

static int uart_cli_test(int argc, char *argv[])
{
    return ut_run_all(argc, argv);
}
#endif
```

UART 串口输入示例：

```
ssd> test --seed 0x1234 /fc-nand/logic/validate_op_range
ssd> test --list
ssd> test --log-visible info /fc-nand/lifecycle/               # Level 3：每条抽一组地址，打印抽到的值
ssd> test --log-visible info --iterations 5 /fc-nand/lifecycle/ # 每条抽 5 组（覆盖旋钮）
ssd> test --seed 0x1a2b3c4d /fc-nand/lifecycle/                # 复现上一轮
ssd> test --list-params /fc-nand/data_path/                    # Level 2 仍有参数化用例（case=…）
ssd> test --single --param case meta-1 /fc-nand/data_path/     # CLI 值优先于随机挑值
ssd> test                                                      # 全套（**不加过滤串**才是全套）
```

> **过滤串的两条约束**（`munit_test_runner_run_suite`，`munit.c:1294-1306`）：
> ① **套件名要带尾斜杠**——`/fc-nand/lifecycle/` ✅、`/fc-nand/lifecycle` ❌；
> ② **根套件名单独用不了**——`/fc-nand/` **一条都不匹配**（过滤串比子套件路径短，`strncmp`
> 撞上 `'\0'` 就判不等）。跑全套用**不带过滤串**的 `test`。
> ② 是 fork 既有的匹配缺陷（与 `munit.c:1275` 自述的「按完整路径前缀匹配」不符），
> 本轮**只记录不改**——修它要动 `tests/framework/munit.c`。

### 4.3 构建集成

在 `src/fc/makefile.mk` 中条件加入测试源码：

```makefile
ifeq ($(FC_NAND_TEST_ENABLE),1)
TEST_C_SRCS += $(PROJECT_ROOT)/tests/framework/munit.c
TEST_C_SRCS += $(PROJECT_ROOT)/tests/unit/test_fc_level1.c
TEST_C_SRCS += $(PROJECT_ROOT)/tests/unit/test_fc_level2.c
TEST_C_SRCS += $(PROJECT_ROOT)/tests/unit/test_fc_level3.c
TEST_C_SRCS += $(PROJECT_ROOT)/tests/unit/ut_main.c
MODULE_INCS += $(PROJECT_ROOT)/tests/framework
MODULE_INCS += $(PROJECT_ROOT)/tests/unit
endif
```

---

## 5. 测试编写规范

### 5.1 文件组织

```
tests/
├── framework/
│   ├── munit.c            # 测试框架实现（已修复 15 项）
│   ├── munit.h            # 测试框架头文件
│   └── README.md          # 框架边界说明
├── unit/
│   ├── ut_main.c          # 裸机统一入口
│   ├── ut_main.h          # 入口声明（供 usart.c 包含）
│   ├── fc_test_common.c   # Level 2/3 共享夹具（setup/teardown、test_cmd_build、源数据表、页 helper）
│   ├── fc_test_common.h   # 夹具对外声明（含「调了哪些原语 = 开了哪些功能」的步骤级契约）
│   ├── test_fc_level1.c   # Level 1: 12 个测试（logic 单套件）
│   ├── test_fc_level2.c   # Level 2: 12 个测试（步骤级 + encode/process_read/raid_recover + cw_bitmap）
│   └── test_fc_level3.c   # Level 3: 15 个 block 级场景（真实 NAND）
```

> 注：测试全部随 `make tests`（FC_NAND_TEST_ENABLE=1）编入固件、在 STM32H743 上经
> UART CLI 触发；无 PC/host 独立编译入口。

### 5.2 断言使用

- **优先使用 `munit_assert_*` 宏**，不直接使用 `assert.h`
- 类型明确时使用带类型的断言（`munit_assert_uint32`、`munit_assert_size` 等）
- 字符串比较使用 `munit_assert_string_equal` / `not_equal`
- 内存块比较使用 `munit_assert_memory_equal` / `not_equal`
- 指针 NULL 检查使用 `munit_assert_null` / `not_null`
- **禁用 `MUNIT_ENABLE_ASSERT_ALIASES`**，避免污染`assert`宏

### 5.3 测试函数签名

```c
static MunitResult test_fc_queue_push_pop(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;
    /* setup (if needed) */
    /* exercise */
    /* verify */
    return MUNIT_OK;
}
```

- 测试函数使用 `static` 链接属性
- 参数/返回值使用 `void`，除非需要 fixture
- 成功返回 `MUNIT_OK`，失败通过断言宏或显式返回 `MUNIT_FAIL`

### 5.4 参数化测试

`MunitTest.parameters` 挂 `MunitParameterEnum[]`（`{name, values}` 的 NULL 结尾数组，`values` 也是
NULL 结尾），runner 对每个参数**做笛卡尔积展开**，每组一次独立的 `setup` / `tear_down`。
用例体内用 `munit_parameters_get(params, "name")` 取本组的值（查不到返回 NULL）。

```c
static const char *l2_crc_case_names[] = {"user-1", "crc-field-1", "meta-1", "parity-1", NULL};

/* MunitTest.parameters 是**非 const** 指针（munit.h:432）⇒ 枚举数组不能声明成 const */
static MunitParameterEnum l2_crc_params[] = {{"case", l2_crc_case_names}, {NULL, NULL}};

static MunitResult test_crc_step_detect(const MunitParameter params[], void *data)
{
    const l2_crc_case_t *c = l2_crc_case_find(munit_parameters_get(params, "case"));

    munit_assert_true(c != NULL); /* --param 传了表外的名字 ⇒ 显式失败 */
    ...
}
```

- 值**只能是字符串**（`MunitParameterEnum.values` 是 `const char **`）——名字类参数直接 `strcmp`；
  数值参数得自己 `strtoul`。
- **`--param` 的值不做合法性校验**，原样塞进 `params[]` ⇒ 用例必须自己挡表外的名字（见上面的断言）。
- 板子上钉单个场景：`test --param case meta-1`；列全部参数：`--list-params`。
- **什么时候参数化**：不同**分支 / 期望**才参数化；同一断言、不同输入用迭代（`--iterations N`）。
  判据见 `assertion_guidelines.md` §二十三。
- **随机化参数化（v1.15）**：`--single` 让每个参数枚举按种子**随机挑一个值**（salt = 用例名哈希，`munit.c:1198`），`--seed N` 复现上一轮；不加 `--single` 走全组合枚举。**Level 2 的参数化用例**（`case=…`）走这条路。
- **用例体内随机（v1.22，与上一条并存）**：**Level 3 的 6 条地址相关用例**改在**用例体内**抽地址——参数表是 NULL 结尾的 `const char*` 数组，表达不了 820 宽的窗口（见 §2.3「随机地址与种子」）。机制：`l3_rand(test, tag) = munit_rand_uint32() ^ hash(用例名) ^ hash(用途标签)`。
  ⚠ **必须自带 salt**：munit 在每次 `exec` 前把 PRNG 重置回 `runner->seed`（`munit.c:774`），而导出的 `munit_rand_int_range` / `munit_rand_uint32` **不带 salt**（带 salt 的 `munit_rand_at_most` 是 `static`，`munit.c:503`）⇒ 直接调用的话，**用例体内的第一次抽样在所有用例之间取值完全相同**。这不是「不能调 `munit_rand_*`」，而是「调了必须自己加盐」。Level 2 的 `test_scramble_whiten` / `test_crc_step_detect` 直接用 `munit_rand_int_range` 是因为**只有那一条用例在抽**，撞不上（v1.25 起 Level 2 的 13 条地址相关用例也改走带盐的用例体内抽样，见 §2.2「Level 2 地址抽样」）。

### 5.5 证据打印（v1.18）

**要解决的问题**：跑完只看到一片 `[ OK ]`，**看不出这条用例到底证明了什么**。
本套此前已经栽过两次「绿灯不可信」——v34 修掉一条恒真断言（篡改被 `lba_to_meta()` 整段擦除），
v35 修掉两处 `cw_bitmap` 建链缺陷。所以需要一个**开关**，开了就打出断言所依据的**实际观测值**。

#### 三个原语（`tests/unit/fc_test_common.{h,c}`）

```c
void test_trace_begin(void);                                  /* 每条用例开头调一次 */
MUNIT_PRINTF(1, 2) void test_trace_summary(const char *format, ...);  /* INFO：这条用例证明了什么 */
MUNIT_PRINTF(1, 2) void test_trace_detail(const char *format, ...);   /* DEBUG：逐页 / 逐 CW 明细 */
```

- `test_trace_begin()` **每用例显式调一次**，不挂 `test_setup`——Level 1 的 12 条用例
  `setup == NULL`，靠 setup 会漏。它只做两件事：复位行首标志、在可见时先补一个换行（见下）。
- `test_trace_summary` 打**摘要**（页数、字节数、实读字节、回环的每一步）；
  `test_trace_detail` 打**明细**（夹具的逐页 prog/read、`verify_chain_paa` 的逐 desc）。
  共享夹具里的明细让 Level 3 全部 9 条用例自动获得逐页证据。

#### 开关：munit 的 `--log-visible info|debug`，**不用**项目自己的 `log.h`

`include/common/log.h` 是另一套：`LOGI` **默认就开着**（`g_log_level = LOG_INFO`）会立刻刷屏，
`LOGD` 被**编译期** `DEBUG` 宏闸住（运行时打不开），且 `g_log_level` 是**全局固件状态**，
调完会一直留着影响 FTL/NAND 的生产日志。munit 的开关是 `test` 的一个参数、默认关、
两级阈值天然对应 INFO/DEBUG、只作用于 munit 自己的输出。

框架侧为此加了 1 个只读访问器 `munit_log_visible()`（见 `munit_adapter.md` §2.4）。

#### 实现要点

- **不走 `munit_logf`**：它会给**每一行**加 `Info:<文件>:<行号>` 前缀（`munit.c:147-178`），
  逐页明细会被撑到 ~110 字符、UART 阻塞时间翻倍。证据行**直接写 `MUNIT_OUTPUT_FILE`**，自带紧凑前缀。
- **默认档逐字不变是硬约束**：可见性判断放在函数**最前面**，
  `if (!munit_log_visible(level)) return;` ⇒ 默认档（visible=`ERROR`）连那个换行都不输出。
- **换行要「急切」**：munit 的参数前缀行（`    block=100, plane=0`）**不带换行**就开跑用例
  （`munit.c:900-926`），用例体内任何输出都会插进结果行中间。`test_trace_begin()` 在可见时
  **立刻**补一个 `'\n'`（不等第一条证据行），否则 `init_sequence` 里 nand 驱动打的
  `[INF] nand: READ_ID ...` 会粘在结果行上。

#### ⚠ 三个坑

1. **`--log-fatal` 不是「可见级」的同义词**。它与 `--log-visible` 共用取值解析，
   而 `munit_logf_ex` 在 `level >= fatal` 时 `abort()` ⇒ `--log-fatal info` 会让每条证据行 abort。
   默认 `fatal = ERROR` 安全，别动。
2. **可见级跨 `test` 调用残留**。`munit_log_level_visible` 是文件级 static，
   `munit_suite_main` 不重置 ⇒ 一旦 `test --log-visible info`，之后光打 `test` 也仍出证据，
   直到重启或 `--log-visible error`。
3. **UART TX 是阻塞的**（环 4096 B / 115200 波特 ≈ 11.5 KB/s，`usart.c` 的 `__WFE()` 自旋，不丢字节）
   ⇒ `--log-visible debug` 跑全套（53 case × 64 页，v1.22 前是 77）是**数十秒纯 TX**，
   **debug 档只对单条用例跑**（`test --log-visible debug /fc-nand/lifecycle/block_write_read`）。
   另外 `make tests` 带 `-DMUNIT_ENABLE_TIMING`，证据打印在用例体内 ⇒
   **开证据后 `[ N ms ]` 被 UART 阻塞污染，不可再当性能基线**。

#### 板上实测（2026-10-10，v1.18 形态 = 77 case）

| 档 | 结果 | 证据行 |
|---|---|---|
| 默认（`--log-visible error`） | 77/77，输出**逐字不变** | 0 |
| `--log-visible info` | 77/77 | 131 |
| `--log-visible debug` | 77/77 | 4936（131 摘要 + 4805 明细） |

> ⚠ **v1.22 起用例数 77 → 51**（Level 3 由 35 → 9，参数化展开被用例体内随机取代；Level 1 = 11、Level 2 = 31 不变）。上表是 v1.18 形态的实测记录，**未重跑**；证据行数也随之变化（Level 3 的 6 条各多一句 `info` 摘要）。

### 5.6 游程统计判据的理论依据（v1.19）

`scramble_whiten` 的判据 ①②③ 建立在「随机数据的游程该长什么样」之上。这里把统计方法与它的概率推导写清，
既解释阈值从哪来，也标出判据的**盲区**。加扰「目标 = 统计独立」的理论背景见 `fc_nand_design.md` §5.4。

#### 统计方法（`l2_bit01_analyze`，`test_fc_level2.c`）

一趟**流式**扫描，不存 bit，只维护两个游标 `cur`（当前段的位值）/ `run`（当前段已累积长度）：

- `cur` 从**首位**初始化（不是从 0）——否则首段是 1 时会漏计一次；
- 每读一个 bit：与 `cur` 相同 → `run++`；不同 → **结算**旧段（`hist[cur][桶]++`、`runs++`），再 `cur = b; run = 1`；
- **循环后必须补收尾**——最后一段没有后继，不补则段数恒少 1（游程编码的经典 off-by-one）；
- `≥16` **钳位进第 15 桶，不丢弃**——全 0xFF 的明文就是**一个长度 4096 的游程**、PAA 重复有 128 个 `≥16` 的 0 游程，
  正是加扰要治的病，丢掉等于把病灶从报告里抹了。

派生量（都走**整数**，newlib-nano 没有 `%f`）：`mean_x100 = 总位数 × 100 ÷ 段数`（平均段长 ×100）、
`bias = |ones − 总位数/2|`。

#### 为什么段数 ≈ 位数的一半

随机数据**每一步有 1/2 概率翻转**（相邻两位相同 / 不同各半）：

```
段数 = 1 + 翻转次数 ≈ 1 + (4096−1) × 1/2 ≈ 2048       （= 位数的一半）
```

等价地，**平均段长 = 4096 / 2048 = 2**。段长服从几何分布：

| 段长 k | 概率 `2^-k` | 在 2048 段里约 |
|---|---|---|
| 1 | 1/2 | 1024 段 |
| 2 | 1/4 | 512 段 |
| 3 | 1/8 | 256 段 |
| 4 | 1/16 | 128 段 |

——**一半的段只有 1 个 bit**，所以 2048 这个数看着大，其实是碎片。三个极端对照：

| 序列 | 翻转率 | 平均段长 | 段数 |
|---|---|---|---|
| 全 0 | 0% | 4096 | 1 |
| **随机** | **50%** | **2.0** | **≈2048** |
| `0101…` | 100% | 1.0 | 4096 |

#### 判据 ② 是双侧的

`mean_x100 ∈ [170, 240]`（中心 200 = 平均段长 2.0），**两边都是病**：

- **低了**（如 `mean_x100 = 100`，平均段长 1.0）：被**强制交替**（周期 2）；
- **高了**（> 240，平均段长 > 2.4）：**长游程**（偏置，加扰没起作用）。

#### ⚠ 判据 ①②③ 检的是症状，有盲区

三条判据都是**统计量**，测不出**周期性**。反例：`00110011…`（每段长度恒为 2）——

- 判据 ①：`ones = 2048` ✅（均衡）；
- 判据 ②：平均段长正好 **2.0** ✅；
- 判据 ③：没有 `≥16` 的段 ✅。

**三条全过，但它是一个周期 4 的固定图案。** 这不是 bug，而是有限统计量的固有边界——真正的病根
（「独立」）无法被任何有限个统计量完全覆盖。实际不影响正确性：加扰器输出是 LFSR 驱动的，
`all-0x00` 那一路的输出就是裸 LFSR 序列（最大长度伪随机），**不是** `00110011…`。

### 5.7 `ecc_step` 的覆盖边界与提高计划（v1.20）

`ecc_step` 验的是 **BCH(m=13, t=4) 的实现**，不是「算法对不对」——后者是**代数保证**：t 纠错码的
**设计距离** d ≥ 2t+1 = 9，故 ≤ t 个错的接收字落在**唯一**码字的 t 球内，译码结果唯一，**不需要测**。
测试守的是实现——BM / Chien / Forney、位翻转落点、`errloc` 映射、parity 布局。这条不是空话：
`fc_bch_engine.c:582-585` 就记着一个真实的坑（回代写成 `tmp = (tmp<<1)|bit` 会让 t=4 实际退化成 t=2）。

在这个定位下，**当前形态全绿只应读作「在这些抽样下没复现出实现 bug」**。六处缺口：

| # | 缺口 | 具体 |
|---|---|---|
| ① | **抽样非穷举** | 每 case 每轮只抽 **1 组**位置；空间 `C(4160,4) ≈ 10¹³` 量级 |
| ② | **抽样有偏** | 单轮里 bit-within-byte 只覆盖少数偏移（实测只出现 `{0,1,2,5}`）⇒ 只错在 bit 7 的实现 bug 看不见 |
| ③ | **`beyond-5` 不钉结果** | OK 分支只验 `ecc_bits ≤ 4`、**不验数据** ⇒ **误纠被测试容忍**，绿了也不说明「5 个错必被检出」 |
| ④ | **parity 区是「软」的** | `p_k > 0` 时只比数据区 520B；解码器**刻意不回写** parity 字节（`fc_ldpc_engine.c:182` 只翻 `errloc < 4160`）⇒ parity 侧只由**计数**背书，没验它真被修好 |
| ⑤ | **单一信道模型** | 只有「干净码字 + 均匀随机位错」，不含**突发 / 相关错**（真实介质错是相关的） |
| ⑥ | **同源编解码** | `l2_ecc_cw_build` 用 `fc_data_path_ecc_encode` 造码字，与解码同族 ⇒ **共享约定的 bug 两边一起错仍能往返通过**，无独立参考向量 |

> ⚠ 更要紧的一层：`beyond-5` 这类超能力场景下的**误纠**是 ECC **自己抓不到**的，要靠 Pass 3 的 CRC；
> 而本工程 FTL 只判 `FC_CW_STATUS_NAND_FAIL`（见 `fc_nand_design.md` §8.2）⇒
> **ECC 单测全绿 ≠ 主机读到的数据一定对**。

**提高计划（四步，按性价比排序）**：

| 步 | 动作 | 补哪个缺口 | 代价 |
|---|---|---|---|
| 1 | **定向补 case**：bit-within-byte 覆盖 `0..7` / 跨字节边界 / **突发错**（相邻位）/ t=4 全落 parity 区 / `errloc` 边界（bit 4159 / 4160 / 4207） | ②⑤ | 改 `l2_ecc_cases` 表 + 少量夹具；**抽样抽不到的边界直接穷举掉**，性价比最高 |
| 2 | **`--iterations` 加量**：板子上 `test --iterations 200 /fc-nand/data_path/ecc_step` | ① | ≈0（该用例已是 `MUNIT_TEST_OPTION_NONE`） |
| 3 | **已知答案向量（KAT）**：用**独立**实现（上游 Linux `bch.c` 的测试向量）比 parity 字节 | ⑥ | 需引一份外部向量表 |
| 4 | **系统层**：把 CRC 判定接进 FTL | 「误纠到达主机」那一层 | ⚠ 在 `src/ftl/**`，**按硬规则暂不动** |

> 本版**只记录，不改测试**：①②⑤ 的定向用例、③ 的 parity 回写语义、KAT 表都留待后续。
> 其中 ④ 若要动，属 `fc_ldpc_engine.c` 的**契约变更**，不在测试层解决。

---

## 6. 注意事项

### 6.1 BCH 引擎全局变量

`g_bch` 是全局状态，`fc_ecc_init_bch` 只在首次分配时初始化，跨测试用例持续存在。Level 2 测试需要在第一个测试前完成初始化，不应在每个测试的 setup 中重复初始化。

### 6.2 资源池状态

`fc_command_pool` / `fc_descriptor_pool` / `fc_buffer_pool` 是全局静态池。Level 1 测试涉及纯逻辑函数时不直接依赖池内容；Level 3 测试需要 `fc_init()` 预先初始化。

**调试注意（2026-10-04 补充）**：命令池/描述符池**没有 section 属性**，落默认 `.bss` → `AXI_RAM`（基址 `0x24000000`），MPU region 3（`src/platform/stm32h743/sys.c:14`）是 **Normal Write-Back 可缓存**。Cortex-M7 的 D-Cache 与调试器**不一致**——SWD 读内存绕过 D-Cache，所以**调试器 watch / memory 窗口读到的池内容可能是陈旧的**（CPU 看到 cache 新值，调试器看到 SRAM 旧值）。要确认字段真实值，用 CPU 侧打印，或在观察点前 `SCB_CleanDCache()` 再读。详见 `debug_cmd_operation_rewrite_20261004.md`。

### 6.3 CQ entry slot 限制

CQ entry 只有 4 个 status slot（`status0..3`），`CW_NUM=4`。超出的 slot 应填 `FC_CW_STATUS_UNUSED`。

### 6.4 断言失败的现场保留

裸机环境下断言失败后会卡在 `while(1)`，不会继续执行后续测试。因此：
- 一个测试文件内建议的测试粒度为**一个 test 函数只验证一个行为**
- 使用 `--fatal-failures` 可以遇错即停（裸机下这是默认行为）

### 6.5 生命周期插桩（默认关闭）

`test_fc_level3.c` 的 `run_lifecycle` 上方有一段 `#if 0` 块（`cmd_snapshot` / `cmd_diff_report` / `CMD_FIELD_U`）：提交前给 `cmd` 拍快照、release 前逐字段比对并打印差异，用于排查"命令提交后被谁改写"。`run_lifecycle` 内两处调用已注释并指向该块。整块默认不编译。

`setup_level3()` 的 `[RES] L3_start: cmd=…/64 desc=…/256 …` 资源快照行同样已注释，排查池泄漏时再打开。

---

## 7. 修订历史

| 日期 | 版本 | 变更描述 |
|---|---|---|
| 2026-10-12 | v1.39 | **新增 `OP_READ_COLUMN`（槽位 42，`OP_COUNT` 首次 42 → 43）：按「显式列地址」读 4 字节，够得到 spare 区列 2048；`bbt` 改为非破坏性出厂坏块扫描（默认只读，`--erase` 才擦）**（见 `docs/changelog/read_column_v63.md`）。**src + 文档**（`include/nand/onfi_1_0.h`、`src/nand/onfi_1_0.c`、`include/nand/nand_dispatch.h`、`src/nand/nand_dispatch_internal.h`、`src/nand/nand_dispatch.c`、`src/nand/nand_dispatch_tables.c`、`src/fc/fc.c`、`include/ftl/ftl_fc_interface.h`、`src/ftl/ftl_fc_interface.c`、`include/ftl/bbt.h`、`src/ftl/bbt.c`、`src/controller/cli.c`），**`tests/` 一行不动**。起因：用户报「`bbt` 的坏块图看不出任何东西」，核出根因两条——**① 先擦后读**（`bbt_probe_block_bad` 先 `ftl_sync_erase_block` 再读；器件手册 Micron **AN1819** 逐字 *"the bad block information is **erasable** and cannot be recovered once erased"*、*"…check the first spare area location on the first page of each block **prior to** performing any PROGRAM or ERASE operations"* ⇒ 擦完再读必为全 `FF`；`bbt.h`/`bbt.c` 原写的「出厂坏块标记是**擦不掉的硬标记**」**是错的**，已删）；**② 列地址根本到不了 2048**（出厂标记在**列 2048** = spare 首字节，x8 为 `00h`，而唯一能指定列的 op 11 的列 = `paa_to_cw(paa) * CW_SIZE`，cw 只有 **2 位** ⇒ 列只能是 {0, 528, 1056, 1584}；`PAGE_SPARE_SIZE` 零读者）。**用户拍板三条接口约束**：**不用 desc 链**（`has_data = 0`）／**列地址走 SQ 的 `command->param.bytes[0..1]`（小端）**、**不用 `paa` 的 cw 字段**／**结果回 CQ 的 `completion.param.bytes[0..3]`**（`param[0]` 即坏块标记）；一次读 4 字节（列 2048..2051 = spare[0..3]），与已上板验过的 op 11 **完全同形**，复用控制器侧暂存 `g_fc_return_staging`，**不新增出参 API**。**① `src/`**——`onfi_1_0.h` 加 `OP_READ_COLUMN (42U)`（0..41 已满，唯一空位 20 是墓碑「不得复用」⇒ **`OP_COUNT` 42 → 43，全仓首次抬升**）+ 时序图注释；`onfi_1_0.c` op 表加一行（`00h`/`30h`、`address_cycles = 5U`、`.has_data = 0U`、`.data_length = 4U`、**uop 复用 `g_nand_normal_read_page_uops`**，逐字抄 op 11）；`nand_dispatch.h` / `nand_dispatch_internal.h` 各加 `uint16_t column`（**独立字段**——`feature_addr` 是 8 位、装不下 2048，且与 FC 层同名字段对齐，不合并）、`nand_dispatch.c` 的 `task_load` 加一行搬运；`nand_dispatch_tables.c` 新增 `req_read_column()`（与 `req_meta_read` 的唯一区别是 `.column = task->column` 而非 `paa_to_cw(...) * CW_SIZE`）并注册进 req/completion **两张表**（⚠ 两张 `[OP_COUNT]` 表**都必须填**——`nand_dispatch.c` 的调用点是裸函数指针，漏填 = NULL 调用 = HardFault；漏 op-map = `uops == NULL` ⇒ `INVALID_ARGUMENT`）；`fc.c` 的 `fc_build_dispatch_request` 新增一支（照 op 11 写，**额外**填 `plane_paa[plane] = command->paa`（行地址需要；op 9 那支不需要所以不填）与 `request->column = param.bytes[0] \| (param.bytes[1] << 8)`；**必须建这一支**，否则落末尾 `else` ⇒ 位图空集 ⇒ `cw_buffers[]` 一格不搬 ⇒ `DATA_OUT` 写向野地址，与 v59 同一个坑），`fc_complete_from_nand` 的 staging→`param.bytes` 拷贝条件扩为五 op。`ftl_cmd_needs_desc_chain` **不用改**（`has_data == 0` 自动短路）；`fc_is_page_read`/`fc_is_page_write`/`fc_dispatch_rewrite_operation` **都不动**。**② FTL + CLI**——`ftl_fc_interface.h/.c` 新增 `ftl_sync_read_column(addr, column, out[4])`（照 `ftl_sync_read_meta`，列写进 `cmd->param.parameter0/1`，`ASSERT(column + 4 <= PAGE_SIZE)`；契约同 `read_meta`：**刻意不判 NAND 失败**）；`bbt_probe_block_bad()` 改**先读后擦**（① 读列 `PAGE_MAIN_SIZE`，`mark[0] != 0xFF` ⇒ 坏块立即返回；② `do_erase` 非 0 才擦）；`bbt_print_bad_map(alloc, do_erase)` 加形参；CLI `bbt [--erase]`（`max_argc` 1 → 2）。**③ 测试**——**一行不动**：`test_command_validate_op_range` 遍历 `0..OP_COUNT-1`，新 op `has_data == 0` 自动通过；**用例数不变（仍 58）**。`make format-check` 过、`make clean && make all` 0 warning 0 error（`.bin` **181296 B**）、`make clean && make tests` 0 warning 0 error（`.bin` **259404 B**）。**⚠ 上板未验**。⚠ 上板步骤：`bbt`（**期望仍全 `FF`——本片芯片已被旧版先擦后读的 `bbt` 整片擦过，真出厂标记已丢，不是 bug**；只验读路径不崩不擦芯片）→ `wr 0 8` + `fl` + `bbt`（**期望出现少量非 `FF` 格子**：FC 把 528B CW 连续铺在列 `cw*528`，CW3 覆盖列 1584..2111 ⇒ 列 2048 落在 CW3 的 USER 区，被写过且加扰；这是**误报**，只用于证明链路真读到了程序化的数据）→ `bbt --erase`（**破坏性，放最后**）→ `make clean && make tests` + `test`（期望 58/58）。⚠ **`bbt_scan_factory()`（开机路径）本次不动**——另有「LDPC_RAW 不报 ECC 失败」的老问题，只在注释里留 TODO；⚠ **`make all` 后直接 `make tests` 是空操作**（共用 `OBJ_DIR`、CFLAGS 不被跟踪）⇒ 换目标必须先 `make clean` |
| 2026-10-11 | v1.38 | **开机读 ONFI 参数页（ECh，手册 Table 11，256B）并打印设备信息——纯函数解码 + 结构体**（见 `docs/changelog/nand_onfi_param_page_print_v60.md`）。**src + 测试 + 文档**（`include/nand/onfi_1_0.h`、`src/nand/onfi_1_0.c`、`include/nand/nand_mt29f4g08abadawp.h`、`src/nand/nand_mt29f4g08abadawp.c`、`tests/unit/test_fc_level1.c`），**`src/ftl/**` 一行不动**。起因：用户贴出手册 **Table 11「Parameter Page Data Structure」** 问「这个是不是应该弄一个函数读取并打印信息？」，随后明确**「在初始化使用，打印设备信息」**；并选定「解码做成**纯函数 + 结构体**」「**补单测**（并入既有套件，不新建文件）」「打印**精选字段 ~3 行**」。**① `src/`**——`onfi_1_0.h` 新增 §8：`NAND_ONFI_PARAM_PAGE_SIZE (256U)`、`NAND_ONFI_PARAM_MODEL_LEN (20U)`、22 个 `NAND_ONFI_PARAM_OFF_*` 字段偏移宏、`nand_onfi_le16/le32` 两个 `static inline`（ONFI 多字节字段**小端**，全仓此前无 LE helper）、结构体 `nand_onfi_param_info_t`、`nand_onfi_parameter_page_decode()` 声明；`onfi_1_0.c` 实现该函数（**纯函数**：零 HAL / 零日志 / 零硬件，返回签名是否成立）；`nand_mt29f4g08abadawp.h/.c` 加 typed 同步壳 `nand_fc_read_parameter_page(page[256], completion)`（照 `nand_fc_read_id` 写法）+ `static void nand_onfi_param_print()`（3 行 `LOGI`）+ `nand_init` 插 **Step 6b**（ID 校验之后、`#if CONFIG_NAND_TIMING_MODE_4` 之前：读 → 解码 → 打印）。⚠ **非致命**：读失败或签名（bytes 0–3 ≠ `4F 4E 46 49`）不符只 `LOGW` 一行、**继续启动**（ID 已匹配 ⇒ 器件是好的，参数页读打嗝不该 brick 开机）。⚠ 打印**每行必须 < 192 字符**（`src/common/log.c` 的 `log_vprintf` 落 `char message[192]`，超长截断）⇒ 用 `%u/%lu/%02X/%c/%s`，**不用** `%ll`/`%zu`/`%f`（newlib-nano）。⚠ 时序字段 `tprog/terase/tr/tccs` **只打原值、不打单位后缀**（Table 11 未给量纲，按「协议断言必须先查证」不猜）；**CRC（bytes 254–255）不做**（多项式不在仓库内）。**② `tests/`**——并入 `test_fc_level1.c` 的 `logic` 套件新增 **Part 4**：一条纯 RAM 用例 `onfi_param_page_decode`——用新增的 `test_put_le16/le32()` 在栈上按 Table 11 合成 256B 参数页（`ONFI` 签名 / rev `0x0002` / model `MT29F4G08ABADAWP` 空格补齐 / page `2048+64` / partial `512+16` / block `64p` / blocks `4096` / lun `1` / addrcyc `0x23` / ecc `4` / timingmodes `0x003F` …）→ 逐字段断言 + 证据行；再破坏签名，断言解码函数如实**返回 0** 而字段仍填满。⚠ 计数器须 `uint16_t i;`——`uint8_t` 会让 `i < 256` **恒真**（死循环），编译期由 `-Wtype-limits` 抓出。**用例数 57 → 58**（Level 1 **11 → 12**）。`make format-check` 过、`make clean && make tests` 0 warning 0 error、链接 **248352 B**（v59 是 242776 B）。**✅ 上板已验**（2026-10-11，seed `0x5a4f21e4`）：**58 of 58 (100%)**、0 skipped、870.45 ms，`onfi_param_page_decode` **[ OK ] [ 26.79 us ]**，既有 57 条逐字不变；开机三行 `nand: ONFI …` 全对（`model=MT29F4G08ABADAWP`、`page=2048+64`、`partial=512+16`、`block=64p blocks=4096`、`lun=1`、`addrcyc=0x23`、`ecc=4bit`、`timingmodes=0x003F` ⇒ **同步路径首次读参数页的机械成立性闭合**，读失败降级分支未被触发）。⚠ **本用例真拦下一个缺陷**：第一跑红在 `info.model == "MT29F4G08ABADAWP"`——器件把型号串用**空格右填充**到 20 字节、解码侧原样拷贝 ⇒ 得到带 4 个尾空格的串（开机日志里同样肉眼可见）。**修在解码侧**（拷完 20 字节后回退吃掉 `0x20` 再补 NUL，**非改断言**），重建 248352 B 后全绿。⚠ 零生产调用者（解码函数只被 Step 6b 调）；不用参数页的时序支持位去驱动 Mode 4（用户只要求打印）；不加 CLI 命令 |
| 2026-10-11 | v1.37 | **新增 `OP_READ_ONFI_SIGNATURE`（槽位 9）：90h + 地址 20h 的 4 字节 ONFI 签名，写入 CQ 的 `param`**（见 `docs/changelog/fc_ut_read_onfi_signature_v59.md`）。起因：用户对照 MT29F4G08 手册提问「这个支持了吗」——**Figure 26**（`90h` + 地址 `00h` → 5 字节厂商/器件 ID）**已支持**（`OP_READ_ID`(1)，开机日志 `ID=2C DC 90 95 56`）；**Figure 27**（`90h` + 地址 `20h` → 4 字节 `4F 4E 46 49` = `"ONFI"`）**不支持**（全仓无一处把 `0x20` 送进 90h 的地址段）。用户拍板「只返回 4 个参数，是不是可以考虑放在返回的命令状态参数里」⇒ 4 字节走 CQ 的 `fc_command_status_t.param.bytes[]`，**不新增出参 API**；并选定「只做 FC 路径的一等 op、不加启动期自检」（启动走同步 `nand_fc_execute`，其 completion 无 `param` 通道）。**① `src/`**——`include/nand/onfi_1_0.h` 把「操作码 9 已删除」墓碑换成 `OP_READ_ONFI_SIGNATURE (9U)`（**复用槽位 ⇒ `OP_COUNT` 保持 42**；槽位 20 是墓碑，不动）并加 `NAND_READ_ID_ADDR_ONFI_SIGNATURE (0x20U)`；`src/nand/onfi_1_0.c` op 表加一行（字段照 op 1，但 `.has_data = 0U`、`.data_length = 4U`、**uop 复用 `g_nand_read_id_uops`**——`NAND_UOP_COLUMN_0` 的 handler 写的是 `request->column` 低字节而非 `uop->operand`，故同一数组同时服务 90h+00h 与 90h+20h）；`src/nand/nand_dispatch_tables.c` 新增 `req_onfi_signature()`（列地址恒 `0x20`、行地址恒 0、`data_length = 4`）并注册进 req/completion 两张表（⚠ 两张 `[OP_COUNT]` 表**都必须填**——`nand_dispatch.c:420` 是裸函数指针调用，漏填 = NULL 调用 = HardFault）；`src/fc/fc.c` 的 `fc_build_dispatch_request` 新增一支（与 `OP_READ_META` **同型**：位图只置本 plane 的 CW0、`cw_buffers[]` 挂 `g_fc_return_staging[command_id]`，**不填 `plane_paa[]`**——无地址段需要；**必须建这一支**，否则落末尾 `else` ⇒ 位图空集 ⇒ `cw_buffers[]` 一格不搬 ⇒ `DATA_OUT` 写向野地址），`fc_complete_from_nand` 的 staging→`param.bytes` 拷贝条件扩为三 op。**② `tests/`**——并入既有 `init_sequence` 新增 **Step 4**（`test_cmd_build(addr, 0U, 0U, &L3_CONF(OP_READ_ONFI_SIGNATURE, FC_CRC_ENABLE))` → 断言 `param.bytes[0..3] == 4F 4E 46 49`），原 Step 4/5 顺延为 5/6，证据摘要扩 `SIG(90h+20h)=…`；**夹具不用改**（`test_cmd_try_build` 的 `ASSERT(addr->cw == 0U || op == OP_READ_META)` 走前半支）。**用例数不变（仍 57）**。`make format-check` 过、`make clean && make tests` 0 warning 0 error、链接 **242776 B**（v58 是 241584 B）；**上板已验**（2026-10-11，57/57、870.83 ms，`init_sequence` 绿 ⇒ 器件真在 90h+20h 后回 `"ONFI"`）。⚠ **零生产调用者**（同 `OP_GET_FEATURES` 现状）、`src/ftl/**` 一行不动、不进 ECC/数据路径（不在 `fc_is_page_read`）、不被 rewrite（只动 op 12/30/41）。⚠ **曾需上板确认（已闭合）**：`0x20` 只在手册 Figure 27 一处出现，且 4 字节输出在 `90h` 命令之后（不是 `00h`-`30h` 之后）是否成立——上板后 `init_sequence` 绿，器件实测返回 `"ONFI"`，断言未红 |
| 2026-10-11 | v1.36 | **两条补测并入既有用例：`OP_READ_UNIQUE_ID`（op 3）进 `init_sequence`、`fc_buffer_get_meta` 进 `block_read_meta`**（见 `docs/changelog/fc_ut_read_unique_id_buffer_meta_v54.md`）。**纯测试 + 文档**，`src/**` 一行不动。起因：用户从「性价比最高的补测清单」里点名这两条——`OP_READ_UNIQUE_ID` **只读、零风险**，`fc_buffer_get_meta` 是 **gc 搬移**的真实消费路径——并要求「能并入现有测试就并入，不能并入才加用例」。两条**都能并入**，故**用例数不变（仍 57）**、不新增注册行、不碰 `ut_main.c`。**① `OP_READ_UNIQUE_ID`（3，EDh）**：此前**零用例**（`grep OP_READ_UNIQUE_ID tests/` 无命中），而 op 表里它与 `OP_READ_PARAMETER_PAGE`(2)/`OP_GET_FEATURES`(4) 共用同一 UOP 模板（`EDh` vs `ECh`/`EEh`）、`has_data=1`、256B、1 地址周期，却从未在板上跑过。并入 `init_sequence` **Step 5**（与 Step 3 参数页**同型**）：`test_cmd_build(addr, 0x01U, 0U, &L3_CONF(OP_READ_UNIQUE_ID, FC_CRC_ENABLE))` → 提交 → `test_verify_cw_status_all_success == 1`。**断言强度刻意只到「命令 + CW 状态全 SUCCESS」**：唯一 ID 是**出厂烧录的 16 字节**，本器件手册未给死值 ⇒ **不假设内容**（见「协议断言必须先查证」）；16 字节走新增的证据摘要行 `UNIQUE_ID(16B)=…` 打出（`MUNIT_LOG_INFO`，默认档可见），供人工比对（全 `0xFF` 即视为器件未实现）。**② `fc_buffer_get_meta(cmd, cw_index)`**：返回命令中 **CW 号 == cw_index** 的 CW 基址（offset 0 = META 4 字节），按 `paa_to_cw(desc->paa)` 匹配而非链走位；**唯一生产调用者 = `src/ftl/gc.c:96`**（读页 → `get_meta(cmd,0)` → 4 字节扇区地址 → `>> CW_WIDTH` 得页 LBA）。并入 `block_read_meta`：在既有 op 11 回读循环之后，加一段**普通读**（`test_cmd_build` + 完整 plane 位图，走 desc 链）→ 读回后对 CW0/CW2 调 `fc_buffer_get_meta`，比对**同一** `expected = (base & ~PLANE_CW_MASK) | (plane << PBA_PLANE_SHIFT) | cw`——与 op 11 的结果**互为交叉检查**（一条「无链只读回读」、一条「链内 buffer」）；并断言 `fc_buffer_get_meta(cmd, CW_NUM)` **返回 NULL**（cw 不在链上）。读路径 Pass 4 已把 buffer[0..4] 读作 META（`fc_data_path.c:618-619`）⇒ 普通读后 buffer 内 offset 0 仍是页内明文 META，可直接复核。**验证**：`make format-check` 过、`make clean && make tests` 0 warning 0 error、`.bin` **240436 B**（v53 是 238980 B）；**✅ 上板已验**（2026-10-11，seed `0x5a4f21e4`）：完整 `test` **57 of 57 (100%)**、0 skipped——`init_sequence` **[ OK ] [ 328.22 us ]**（含 Step 5，**op 3 首次上板被器件接受**）、`block_read_meta` **[ OK ] [ 40.16 ms ]**（含链内复核）、其余 55 条逐字不变。 |
| 2026-10-11 | v1.35 | **新增用例 `buf_own_release`，验 §12.10 的 `fc_cmd_conf_t.buf_own : 2`（buffer 归属：谁申请 / 谁归还）**（见 `docs/changelog/fc_buf_own_v53.md`）。**src + 测试 + 文档**（`include/fc/fc_command.h`、`include/fc/fc_interface.h`、`src/fc/fc_interface.c`、`src/fc/fc.c`、`tests/unit/test_fc_level3.c`），**`src/ftl/**` 一行不动**。**① 编码**：bit1 = 申请方（0 = FC，1 = 上层）、bit0 = 释放方（0 = 上层，1 = FC），四档 `00` FC申请/上层释放（**默认**）/ `01` FC申请/FC释放 / `10` 上层申请/上层释放 / `11` 上层申请/FC释放；字段从原 `reserved : 3` 取 2 bit（剩 `reserved : 1`，结构仍 4 字节）⇒ 命令出池 `(fc_command_t){0}` 清零即得 `00` = **既有生产行为** ⇒ FTL 零改动。**② `src/`**：建链机制体改名 `fc_command_configure_descriptor_with_buffer(cmd, plane, cw, row_paa, buffer_id)`（5 参，上层申请时用调用者 `fc_buffer_alloc()` 的 id），原 4 参入口退化为 `_with_buffer(..., FC_BUFFER_ID_INVALID)` 薄包装（FTL ×2 / RAID parity / 既有单测零改动）；`fc_complete_from_nand` 在 Step 3 之后、`fc_complete_internal` 之前加 **Step 4**——释放位 = FC 释放（`01`/`11`）时沿 desc 链归还全部 buffer 并把各 desc 的 `buffer_id` 抹 `FC_BUFFER_ID_INVALID`（幂等，随后链释放自然跳过）。**释放原语一行不改**（读法 A：`fc_command_release` 恒归还链上所有未归还的 buffer，不管谁申请的）。RAID parity 补一行 `buf_own = FC_BUF_OWN_FC_ALLOC_FC_FREE`（`#if CONFIG_FC_RAID`，恒关、无法编译验证）。**③ `tests/`**：新用例 `buf_own_release`（`MUNIT_TEST_OPTION_NONE`，注册进 `g_fc_lifecycle_tests[]`、紧跟 `mixed_pipeline`）——载体 `OP_READ_ID` 单 CW 链，**判据 = `fc_get_resource_stats().buffer_used` 的增量**；Step 1 `00` 建链 +1 / 完成仍 +1 / release 回基线；Step 2 `01` 完成即回基线 + 再 release 幂等；Step 3 `11` 上层 `fc_buffer_alloc` + 5 参建链，完成即回基线；Step 4 `10` 完成仍 +1、release 回基线（**读法 A 回归守卫**）；Step 5 建链未提交也不泄漏；Step 6 连续申请满池断言 id 两两不同（抓双重释放造成的重复入链）。模式 `10`/`11` 不能走 `test_cmd_build`（那是 4 参入口），夹具侧新增本文件内的 `l3_buf_own_build_upper`（照 `test_cmd_try_build` 手工建），不新造基础设施。**用例数 56 → 57**（Level 3 **14 → 15**，其中地址随机化 **10** 不变——本用例是固定脚本）。`make format-check` 过、`make clean && make tests` 0 warning 0 error、`.bin` **238980 B**（v52 是 234988 B）。**✅ 上板已验**（2026-10-11，seed `0x5a4f21e4`）：`/fc-nand/lifecycle/buf_own_release` **[ OK ] [ 2.05 ms ]**（`base=0`）、完整 `test` **57 of 57 (100%)**、0 skipped（`mixed_pipeline` 165.26 ms、`chain_priority_lock` 597.13 us，既有 56 条逐字不变） |
| 2026-10-11 | v1.34 | **新增用例 `chain_priority_lock`，并把 §2.7 的「链锁」补进 FC 机制**（见 `docs/changelog/fc_chain_priority_lock_v52.md`）。**src + 测试 + 文档**（`include/fc/fc_scheduler.h`、`src/fc/fc.c`、`tests/unit/test_fc_level3.c`），`src/ftl/**` 一行不动。**① 缺口**：链长于 per-LUN FIFO 深（`FC_SCHEDULER_CHAIN_MAX = NAND_DISPATCH_CHAIN_DEPTH = 4`）时被 `fc_poll_chain_length` 截成多批、分多次填充 pass 下发，而 `fc_poll_fill` 的 `suspended[]` **每趟 `memset` 清零** ⇒ 批与批之间无跨 pass 状态，另一条提交队列（HSQ/SQ）的命令可插进链的已下发部分与未下发部分之间（`nand_dispatch_enqueue` 只保证**一批内**原子）。用户明确这是**需求**：「防止 fifo 里面有要执行的链，但有的链还没加进来，且 HSQ 上有命令，防止被插入」。**② `src/`（约 12 行）**：`fc_scheduler_t` 新增跨 pass 持久的 `uint8_t chain_owner[CH_NUM][CE_NUM][LUN_NUM]`（取值 `fc_chain_owner_t` NONE/SQ/HSQ，`fc_init_internal` 的 `*controller = (fc_t){0}` 天然清零）；`fc_poll_fill_queue` 加 `owner` 形参、三处——**准入**（`chain_owner != NONE && != 本队列` → 回推跳过，**只回推、不置 suspended**，否则把链自己的续批一起锁死）/ **更新**（下发成功后看**批尾** `chain.ids[need-1].config.chain`：`MORE` ⇒ 锁继续转本队列、`END` ⇒ 解锁）/ **失败清锁**（整链注 `NAND_FAIL` 时清 `chain_owner`，免留残锁）。锁的粒度是「**哪条提交队列能喂这个 LUN**」而非「LUN 不能用」：链所在队列照常能喂（续批正从这条队列取），另一条队列一律回推，直到链 END 下发——**发 SQ 链时 HSQ 插不进、发 HSQ 链时 SQ 插不进**。与既有「共享 `suspended` 防优先级倒挂」**正交**。**③ 为什么安全**：FTL 全仓恒 `FC_CHAIN_END`（§23.9）⇒ `chain_owner` **恒 NONE** ⇒ 准入判断恒假、更新恒写 NONE ⇒ **生产路径逐字不变**。**④ `tests/`**：新用例 `chain_priority_lock`（`MUNIT_TEST_OPTION_NONE`，注册进 `g_fc_lifecycle_tests[]`、紧跟 `feature_write_read`）——用 `01h`（Timing mode）观察，`x = 01h`、`y = 02h`；Step 2 验「HSQ 真能插队」（`set0(x)→get0→set1(y)` ⇒ 读回 `x`）；Step 3 ★ 连续提交 8 命令 SQ 链（前 7 条 `FC_CHAIN_MORE`、末条 `FC_CHAIN_END`）→ `fc_service()` 一次（前 4 条进 FIFO、`chain_owner=SQ`）→ 投 HSQ `set1(y)/get1(y)` → 抽干 → **终值 `y`**（无锁时会是 `x`，这就是判据的区分力）。夹具新增 `g_l3_prio_slots[]` + `l3_prio_reset/submit/drain`（按 `command_id` 配回槽位、GET 存值、release，同 `mixed_pipeline` 的 CID 配对）。**用例数 55 → 56**（Level 3 **13 → 14**，其中地址随机化 **10** 不变——本用例是固定脚本）。`make format-check` 过、`make clean && make tests` 0 warning 0 error、`.bin` **234988 B**（v51 是 232260）。**✅ 上板已验**（2026-10-11，seed `0x5a4f21e4`）：`/fc-nand/lifecycle/chain_priority_lock` **[ OK ] [ 597.37 us ]**、完整 `test` **56 of 56 (100%)**、既有 55 条逐字不变。⚠ **前置契约**（只写进注释、未强制）：链成员必须在**同一条提交队列**里**连续入队**——现有链机制本就依赖它，`fc_submit` 一次一条、连续性无人保证 |
| 2026-10-11 | v1.33 | **`mixed_pipeline` 重写为「流式提交 / 池满即抽干」的背压形态**（见 `docs/changelog/fc_ut_stress_v51.md`）。**纯测试 + 文档**，`src/**` 一行不动。起因：v1.32 的形状（每轮批量提交 `W=8` 条、op 轮转读写擦、各打专用块 → 抽干 → 共 `R=4` 轮）被用户判定**没有施加任何压力**，并给出期望节奏（逐字）：「写page0获取命令下发，polling，没有完成的 / 写page1获取命令下发，polling，没有完成的 / …… 写pagex获取命令下发，polling有命令完成，处理释放 / …… 写pagey获取命令失败没有资源，polling，有命令完成处理释放 / 写pagey获取命令成功，polling，有命令完成处理释放 / …… 退出循环后看还有多少CQ没处理，一起处理了 / 完成一次写block」。**① 两次上板量出两点**——(a) 第一版按此节奏重写但**逐页** polling，上板实测在途只爬到 `buf=104/128`（26 条）、够不着 buffer 阈值 32 条 ⇒ **全程零 `retry`**，背压闭环从未进入（`fc_service` 单次最多交付 1 条完成、且交付过就跳过填充 pass ⇒ 逐页 poll 时在途净增速仅 ≈ 0.2 条/页，128 页也到不了顶）；(b) 用户给最终形状 = **multi-plane、单块、每成功下发 4 个 page（或本条失败）做一次 polling + completions**。两点同时变：命令形态单 plane（4 desc + 4 buf）→ **multi-plane（8 desc + 8 buf）**、一块页数 128（2 plane × 64）→ **64**（plane 由 op 一次覆盖）⇒ buffer 阈值 **32 → 16 条**、在途爬升率 ≈ 0.2 → **≈ 0.84 条/页** ⇒ 64 页内必到顶。**② `tests/`**——夹具把 `test_cmd_build` 拆成 **`test_cmd_try_build`（实体）+ `test_cmd_build`（断言薄壳）**：前者配置校验（坐标越界 / `cw==0` 例外 / multi-plane 掩码覆盖）原样保留**仍是 ASSERT**，但**资源申请失败（`fc_command_alloc` 返 NULL、`ftl_cmd_configure_descriptors` 非 `FTL_OK`）返回 NULL 而非 abort**，且失败时已整条 `fc_command_release`（沿链回收 desc/buffer + 归还命令槽，幂等）⇒ 调用者拿到 NULL 即可、无需再释放、不泄漏半条链；**对既有 54 条零影响**（`test_cmd_build` 行为逐字不变，完整套 55 of 55 印证）。用例 `mixed_pipeline` 整体重写（**注册行不变**，仍在 `g_fc_lifecycle_tests[]`；抽样只留 `l3_block_draw` 一次，不再抽 plane——plane 由 multi-plane op 派生）：新宏 `L3_STRESS_BATCH`(=`CH*CE*LUN`，单次收 CQ 上限) / `L3_STRESS_SLOTS`(=`FC_COMMAND_POOL_DEPTH`，在途登记表容量) / `L3_STRESS_BUDGET`(=`TEST_SERVICE_LOOP_MAX`，每页重试护栏) / `L3_STRESS_POLL_EVERY`(=4)；新件（全 `static`）`g_l3_stress_slots[]` + `l3_stress_slots_reset/alloc/find`（完成项只带 `command_id`，靠 `fc_command_to_id` 配回命令 + 地址）、`l3_stress_try_build(op, addr, plane_bm)`（multi-plane 命令；PROGRAM 填源数据、READ 预填 `0xCC` 残留哨兵）、`l3_stress_reap()`（`fc_poll_completions(buf, batch, &n)` → 逐条配槽 → 断言无 `FC_CW_STATUS_ERROR_MASK` → READ **先** `test_capture_cmd` + `test_verify_page_paa` → **最后才** `fc_command_release`，顺序不能反）、`l3_stress_stream(op, block)`。流程：Phase 0 multi-plane 擦块（硬断言无错误位）→ Phase 1 `l3_stress_stream(PROGRAM)` 写全块 64 页 → Phase 2 `l3_stress_stream(READ)` 读全块 64 页并逐 CW 校验；每页内核 = `while (sent == 0) { try_build → NULL 则只 poll / submit 非 OK 则 release 后 poll / 成功则登记槽位并 `since_poll++`；`if (sent == 0 || since_poll >= 4) { fc_service(); drained += l3_stress_reap(); }`；打逐页证据行；`++guard` 护栏 }`，**重试用同一 `addr`**（测「资源回流后同一条能建成」这条不变式），遍历完再 `while (drained < submitted)` 把剩余 CQ 一起处理完（兑现用户「退出循环后看还有多少CQ没处理，一起处理了」）。**逐页证据**用 `test_trace_detail`（`MUNIT_LOG_DEBUG`）⇒ **默认档输出逐字不变**，`--log-visible debug` 下每页一行 `p19 retry +poll submitted=19 drained=3 \| cmd=16/64 desc=128/256 buf=128/128 sq=15 cq=1`。**③ 上板实测**（2026-10-11，seed `0x5a4f21e4`）——`p00..p18` 只 `sub`、`buf` 从 8 涨到 **128/128**（= 16 条，**与算出的 buffer 阈值精确吻合**；`desc` 只到 `128/256` ⇒ **buffer 是绑定池、desc 不是**）；`p19` 起进入稳态 `retry +poll → sub`，`p20` 因首次 poll 只走填充 pass（无完成交付）而连 `retry` 两次——正是 `fc_poll_internal`「交付过完成就跳过填充」那条语义；两个 phase 收尾都打 `phase done: submitted=64 drained=64 \| cmd=0 desc=0 buf=0 sq=0 cq=0 (应为全 0)` ⇒ **一条不漏全还回去了**。`/fc-nand/lifecycle/mixed_pipeline` **[ OK ] [ 164.47 ms ]**（v50 是 39.48 ms，多出的正是上百次 `retry` + `poll`）、完整 `test` **55 of 55 (100%)**、其余 54 条耗时与 v50 逐条对得上。**用例数不变（仍 55）**。`make format-check` 过、`make clean && make tests` 0 warning 0 error、`.bin` **232260 B**（text 230480 + data 1736 = 232216）。⚠ **`retry` 出现是「设计生效」的唯一证据，但不是硬判据**——本用例硬判据 = 全块写-读完成 + 逐 CW 校验通过 + 三池归零；默认档下即使背压没触发用例也会绿，**要看背压必须显式加 `--log-visible debug`**（刻意：判据不该绑定某个池深的具体数值，否则调池深就假红）。⚠ buffer 阈值 = `FC_BUFFER_POOL_DEPTH / (PLANE_NUM*CW_NUM)` = 16 条，改 `PLANE_NUM`/`CW_NUM`/池深都会挪 `retry` 起点，但**不影响用例是否通过**。⚠ 拓扑无关仍只是「写对了」（`LUN_NUM=1` ⇒ `batch=1`、串行推进）。⚠ 沿用 Level 3 的 30%~70% 窗口（614..1433），`src/ftl/**` 一行不动 |
| 2026-10-11 | v1.32 | **新增压力用例 `mixed_pipeline` + FC 批量收取 API `fc_poll_completions()`**（见 `docs/changelog/fc_ut_stress_v50.md`）。用户要一条**压力测试**，形状 = **批量提交若干 → 批量收取若干**、读/写/擦**交替**，且收取条数与 `nand_dispatch_poll` 的推进上限对齐。**① `src/` 新增一个公开 API（纯增量，4 处）**——(a) `include/fc/fc_interface.h` 声明、(b) `src/fc/fc_interface.c` 壳（`ASSERT(completions!=NULL)` / `ASSERT(count!=NULL)`）、(c) `include/fc/fc_internal.h` internal 声明、(d) `src/fc/fc.c` 第 10 节实现：`while (n < max_count && fc_completion_queue_pop(&cq, &completions[n]) == FC_STATUS_OK) n++; *count = n; return (n != 0U) ? FC_STATUS_OK : FC_STATUS_QUEUE_EMPTY;`。**关键 = 不新增任何常量**（尤其没有 `FC_POLL_COMPLETION_MAX`）——上限完全由调用方给，兑现用户「**FC 不应该定死这个参数，这个参数由上层指定**」；循环天然被 CQ 占用数封顶（≤ `FC_CQ_DEPTH`），`max_count` 给多大都不越界，`max_count==0` 时 `*count=0`、返回 `QUEUE_EMPTY`，无需特判；返回语义对齐单条版（有取到 = `OK`，一条没取到 = `QUEUE_EMPTY`）；`fc_poll_completion` **原样保留**，对既有路径零影响。**② `tests/`**——夹具把静态 `test_capture_user_paa` **提升为导出** `test_capture_cmd(const fc_command_t*)`（`fc_test_common.c:198`，读路径收尾：FC 已在 `fc_complete_from_nand` 里跑过 `process_read` 原地解码，此处把 desc 的 USER 区逐槽拷回 `g_source_data`；批量读要「先捕获、再校验、最后才 release」，所以必须能在提交-抽取循环里单独调用）；新用例 `mixed_pipeline`（`MUNIT_TEST_OPTION_NONE`，**直接复用同文件的 `l3_rand`/`l3_block_draw`/`l3_plane_draw`**——它们是 `static`，另立文件就得复制一份）注册进 `g_fc_lifecycle_tests[]`（**零注册改动**：不碰 `makefile.mk`、不碰 `ut_main.c`）。形状：`W = NAND_DISPATCH_CHAIN_DEPTH * 2 = 8`（**必须 > per-LUN FIFO 深 4**，否则 SQ 留不住命令、压不出调度行为）、`R = 4` 轮；每轮 `W` 条一次 `fc_submit` 进 SQ（不逐条轮询），op 按 `k` 轮转 `PROGRAM / READ / ERASE`，**三种 op 各打自己的专用块**（读块 Phase 0 已写好、写块只写、擦块只擦）⇒ 同窗口在途无数据冒险；抽取循环 `while (本轮已收 < W) { fc_service(); fc_poll_completions(buf, batch, &n); for j: 按 buf[j].command_id 查槽表 → 断言无 FC_CW_STATUS_ERROR_MASK → READ 则 test_capture_cmd + test_verify_page_paa → fc_command_release（**先 poll 后 release**，兑现「命令不得还在 CQ 里」的契约）}`，超预算（`TEST_SERVICE_LOOP_MAX * W` 量级）未收齐 ⇒ 报「流水线卡住」。**拓扑无关**：`batch = CH_NUM*CE_NUM*LUN_NUM`（= `nand_dispatch_poll` 单次推进上限，`nand_dispatch.c:755` 三层循环全遍历不早退）与 `W` 都是**测试侧编译期表达式**，FC 不知道拓扑、只给机制（弹到上限/空），策略留给上层——对齐行业 NVMe 的 SQ/CQ 由主机驱动、条目靠 CID 配对、控制器不规定主机一次收几条。今天 `batch=1`（`LUN_NUM=1`）、测试仍串行推进，多 die 器件上同一份代码自动变 `batch>1` 真并行（**这正是设计目标，但本板无法验证并行正确性**）。**用例数 54 → 55**（Level 3 **12 → 13**，其中地址随机化 **9 → 10**）。`make format-check` 过、`make clean && make tests` 0 warning 0 error、链接 **232860 B**；**✅ 上板已验**（2026-10-11：`mixed_pipeline` [ OK ]、完整 `test` **55 of 55 (100%)**）。⚠ `fc_service()` 本轮**只要交付过 ≥1 条完成就整段跳过填充 pass**（`src/fc/fc.c:1793`）⇒ 推进节奏是「交付轮 / 填充轮」交替，`W=8>4` 正是为此选的（**刻意**，非缺陷）。⚠ 沿用 Level 3 的 30%~70% 窗口（614..1433），`src/ftl/**` 一行不动 |
| 2026-10-11 | v1.31 | **块锁族（除永久锁）合并为一条用例 `block_lock_ops`；顺带补两处 src 缺陷**（见 `docs/changelog/fc_ut_block_lock_v49.md`）。用户要求把除永久锁（`OP_BLOCK_LOCK_TIGHT`，24 / 2Ch）以外的块锁 op **合并成一条用例** ⇒ 覆盖 op 21 `BLOCK_UNLOCK_LOW`(23h) / 22 `BLOCK_UNLOCK_HIGH`(24h) / 23 `BLOCK_LOCK`(2Ah) / 25 `BLOCK_LOCK_STATUS`(7Ah)。盘点：这 4 个 op **零生产调用者、零用例**（`grep OP_BLOCK_*` 只命中 op 表、req/completion 表、`onfi_1_0.h` 定义、`test_fc_level3.c` 两个 `#if 0` 死块）。**① `src/` 三文件四处**——(a) `src/fc/fc.c` 的 `fc_build_dispatch_request` **新增「块锁族」分支**（插在 `OP_READ_META` 分支之后、`nand_operation_has_data` 分支之前）：填 `request->plane` + `plane_paa[plane]`，**不置 `cw_bitmap`**。修的是**真缺陷**——此前这 5 个 op 落进末尾 `else`，而 `request` 在 `fc.c:1263` 被 `{0}` 整体清零 ⇒ `plane_paa[0]=0` ⇒ `req_no_data` 算出的 `row[0] = paa_to_nand_row(0)`，而 `ROW_0` 的 uop 就是 `*config->address = request->row[operand]`（`nand_fmc.c`）⇒ **23h/24h/7Ah 永远指向 block 0**，`cmd->paa` 白设；(b) `src/nand/onfi_1_0.c` op 25 的 `.has_data` `1U`→`0U`、`.data_length` `1U`→`0U`——7Ah 的状态字节走 **DQ 总线直读**，不经任何数据缓冲，标 `has_data=1` 是语义谎言（**先例 = v45** 的 op 33/29）；(c) `src/nand/nand_dispatch_tables.c` req 表 `[OP_BLOCK_LOCK_STATUS]`：`req_single` → `req_no_data`（`req_single` 在空掩码下取 `cw_buffers[0]` = 上次残留指针，**先例 = v45 改动 D**）；(d) `src/fc/fc.c` 的 `fc_complete_from_nand` 非 page-read 分支新增一支：`OP_BLOCK_LOCK_STATUS` ⇒ `completion.param.bytes[0] = nand_completion->nand_status;`——修的是**第二个真缺陷**：`nand_fmc_exec_data_out_status`（`nand_fmc.c:399-412`）把字节写进 `completion->nand_status`（**不写 `request->data`**），该值一路传到 FC 但 `fc_complete_from_nand` **从不读它** ⇒ 字节在 FC 边界被丢弃。**② `tests/`**——夹具新增 `test_block_lock_cmd(block, plane, config)`（覆盖 21/22/23，无 desc 链，断言无 `FC_CW_STATUS_ERROR_MASK` 位）与 `test_block_lock_status(block, plane, config)`（op 25，**在 `fc_command_release` 之前**取 `completion.param.bytes[0]` 返回）；新用例 `block_lock_ops`（`MUNIT_TEST_OPTION_NONE`，地址由 `l3_plane_draw`/`l3_block_draw` 抽样，`page = PAGES_PER_BLOCK/2`）八阶段：Phase 0 基线（erase+prog+read+校验，**硬断言**）→ Phase 1/3/6 三次 `7Ah` **证据打印**（解码 Lock#/LT#/LT）→ Phase 2 `2Ah` 锁（硬断言无错误位）→ **Phase 4 再擦一次，只观察不断言**（有错误位 = LOCK 生效、无 = 本板 LOCK 引脚为低致 2Ah 空转）→ Phase 5 `23h`+`24h` 解锁（23h@block 覆盖 0..block、24h@block 覆盖 block..last ⇒ 合起来恢复全器件）→ **Phase 7 再 erase+prog+read+校验 = 本用例唯一硬判据「可逆」**。**断言强度是刻意的**：手册 MT29F4G08 的 LOCK 引脚「上电为低则所有 BLOCK LOCK 命令被禁用；为高则默认锁死全部 block」，而本板现有用例一直能擦除 ⇒ 块未被保护 ⇒ 引脚几乎肯定是**低** ⇒ 2Ah 大概率空转，硬断言「LOCK 挡住擦除」在本板会当场红、且红的原因不是代码错。**③ 清理**：删 `unlock_block()`（:353，被 `test_block_lock_cmd` 取代）与 `#if 0` 死块 `test_unlock_erase`（:864-916，调用了 v32 已删的 helper）；`test_block_lock_tight` 的 `#if 0` **保留**（记录「永久锁为什么不做」）。**用例数 53 → 54**（Level 3 **11 → 12**）。`make format-check` 过、`make clean && make tests` 0 warning 0 error、链接 **229492 B**；**上板未验**。⚠ 影响面已核实为零：这 5 个 op 无生产调用者，A3 分支对其它 op 不可达，A1 只让 `ftl_cmd_needs_desc_chain(25)` 从 1 变 0（FTL 侧无人用 op 25）。⚠ `OP_BLOCK_LOCK_TIGHT`(24) 与 OTP(26/27/28) **不做**——锁到断电/资源不可恢复，同口径 |
| 2026-10-11 | v1.30 | **订正 `OP_READ_PAGE_CACHE_RANDOM` 的语义——「跳页」而非「随机读单页」**（见 `docs/changelog/fc_ut_cache_random_v48.md`）。**纯测试 + 注释/文档**（`tests/unit/fc_test_common.{c,h}`、`tests/unit/test_fc_level3.c`、`include/nand/onfi_1_0.h` 的 op 注释、`docs/design/*`），**`src/**` 一行不动**（uop 序列本身没错）。起因：上板跑 `block_cache_ops`，Phase 3 在 `fc_test_common.c:717` 报 `MATA_ERROR`（`16`）。**先证不是 v47**：host 侧穷举 plane×page×block×cw 核对 `cmd->paa = paa_from_addr(addr)` 与 `pba_to_paa(pba_from_addr(addr))`、以及新旧 `desc->paa` 两条公式，**0 处不符**；`src/fc/fc.c` 在本路径上只是换名。**根因 = v1.26 的语义理解错了**（v44 changelog 自己预言过这个风险）：手册原文是 *reads the specified block and page **into the data register** while the **previous page is output from the cache register*** ⇒ **地址页只进 data register（预取），当场输出的是 cache 寄存器里的「上一页」**（输出页 ≠ 地址页），且该命令**必须在 00h-30h 或 00h-31h 之后**下发（die 地址须与前一命令一致）⇒ 它是「带显式下一页地址的 31h」，**不能当独立入口单发**（Phase 3 原来就把它当独立入口、且在 Phase 2 的 `3Fh` 之后单发）。改法：夹具新增 `test_cache_start()`（把 Phase 2 内联的 START 抽成 helper）与 `test_assert_read_meta()`（**自描述断言**——直接比对 `desc->meta`（Pass 4 写入的**实际读回** META），失败信息里 munit 会打出 actual/expected，一眼看出「实际输出了哪一页」）；`test_read_cache_random` 签名 `(block, plane_bitmap, page_index, config)` → **`(block, plane_bitmap, prev_page, addr_page, config)`**，体内自建最小管线 `START(prev) → RANDOM(addr)`（**期望输出 prev**，`meta` 取 prev 的 PBA）→ `SEQ(31h)`（**期望输出 addr**，这才证地址生效）→ `LAST(3Fh)`（输出 addr+1 收尾）；`prev`/`addr` 都不得是块尾。`block_cache_ops` Phase 3 改为从 page 16 跳到 page 32。**用例数不变（仍 53）**。`make format-check` 过、`make clean && make tests` 0 warning 0 error；**上板未验**（⚠ 本次上板失败**不是** v47 引起，但 Phase 3 一挂整套就中断 ⇒ **v44~v47 的其余部分仍未上板验过**，需重跑完整 `test`） |
| 2026-10-11 | v1.29 | **`fc_command_t.pba` → `.paa`：PBA 退出 FC 层**（见 `docs/changelog/fc_command_paa_v47.md`）。起因：SQ 里的 PBA 经 FC 接口一路带到 FC-NAND 层，而 FC 层在 10+ 处 `pba_to_paa()` 现场转换——PBA 与 PAA **位序不同**（`pba_to_paa` 是真正的字段重排），每处转换都是一次静默算错地址的机会。改为：**FTL 在自己的空间组合 PBA，在边界处 `pba_to_paa()` 一次，赋给 `cmd->paa`**；`src/fc/**` 里 `pba_to_paa()` / `pba_to_*()` 归零。建链入口 `fc_command_configure_descriptor()` 一并改收 `nand_paa_t row_paa`；`desc->paa` 由 `paa_with_cw(paa_with_plane(row_paa, plane), cw)` 造出（PAA 的 plane 与 cw **不相邻**，v24 的 plane_cw 掩码技巧只在 LBA/PBA 空间有效），`desc->meta` 仍是 LBA 公式、不变。**PAA 无 ch/ce ⇒ 路由一律走 `config.ch/ce/lun`**（写死为结构不变式）。RAID 段（`#if CONFIG_FC_RAID`，恒关）一并机械改 PAA、**无法编译验证**。**本版是 FC 层地址重构，测试侧只做等价改名**：`test_cmd_build` 的 `cmd->pba = pba_from_addr(addr)` → `cmd->paa = paa_from_addr(addr)`（`paa_from_addr` 忽略 ch/ce——夹具已把 ch/ce/lun 镜像进 `config`，无损失）；`test_fc_level3.c` 的 BLOCK_UNLOCK 用例与 `#if 0` 快照块同步；`test_fc_level2.c` **不动**（其中的 `pba_from_addr(...)` 全是 `meta` 实参）。**用例数不变（仍 53）**。`make format-check` 过、`make clean && make tests` 0 warning 0 error、链接 **225212 B**；固件 **166708 B**；**上板未验**（⚠ PAA/PBA 位序不同 ⇒ 任何遗漏的转换都**静默算错地址**，只有真实器件能判，建议跑一次完整 `test`） |
| 2026-10-11 | v1.28 | **新增 `OP_READ_META`（槽位 11）：无 desc 链只读回读 CW 的 4 字节 META，写入 CQ 的 `param`**（见 `docs/changelog/fc_ut_read_meta_v46.md`）。起因：坏块扫描需要**只读**回读页内 META（CW 布局 `[META:4][CRC:4][USER:512][PARITY:8]`，前 4 字节 = META，在加扰区之前、**明文**），据此判工厂坏块标记；现状只有 `ftl_sync_read_page_raw`（op 12 LDPC_RAW，**整页 + ECC**）能靠返回码判坏块，`include/ftl/bbt.h:62` 一直在等一个「读 spare/OOB 的 FC 接口」。**① `src/`**——`include/nand/onfi_1_0.h` 把「操作码 11 已删除」的墓碑注释换成 `OP_READ_META (11U)`（**复用原 `OP_READ_MODE` 槽位 ⇒ `OP_COUNT` 保持 42**）；`src/nand/onfi_1_0.c` op 表加一行 `[OP_READ_META]`（`.has_data = 0U`、`.data_length = 4U`、`.op_type = 0U`、`.pair_op = 0U`，**uop 复用 `g_nand_normal_read_page_uops`** ⇒ 线上序列与普通读页**逐字节相同**，唯一差别是输出长度 4）；`src/nand/nand_dispatch_tables.c` 新增 `req_meta_read()`（**CW 由 `command->pba` 的 cw 字段选**：`pba_to_paa` 保留 cw 位 → `plane_paa[]` → `paa_to_cw() * CW_SIZE` 作列地址；行地址取自同一 PAA；`data_length = 4`）并注册进 req/completion 两张表；`src/fc/fc.c` 的 `fc_build_dispatch_request` 新增一支（与 `OP_GET_FEATURES` **同型**：填 `plane_paa[]`、位图只置本 plane 的 CW0、`cw_buffers[]` 挂 `g_fc_return_staging[command_id]`），`fc_complete_from_nand` 的 staging→`param.bytes` 拷贝条件由「仅 GET_FEATURES」扩为「GET_FEATURES 或 READ_META」。**不进 ECC / 数据路径**（op 11 不在 `fc_is_page_read` ⇒ 非 page-read 分支），**不参与 CW 链**（不是 load/fetch op），**不被 rewrite**（`fc_dispatch_rewrite_operation` 只动 op 12/30）。**② `tests/`**——`test_cmd_build` 的 `ASSERT(addr->cw == 0U)` 放宽为「允许 `OP_READ_META` 带 cw」（该 op 的 cw 就是列选择器）；夹具新增 `test_read_meta(block, plane, page, cw, out[4])`（建链 → 提交 → 查无错误位 → 拷 `param.bytes[0..3]`）；新用例 `block_read_meta`（擦除 + 编程后对 CW0/CW2 读回，比对 `expected = (prog_cmd->meta & ~PLANE_CW_MASK) \| (plane << PBA_PLANE_SHIFT) \| cw`——同时验证**页地址**与 **CW 选择**两条轴）。用例数 **52 → 53**（Level 3 **10 → 11**）。`make format-check` 过、`make clean && make tests` 0 warning 0 error、链接 **225140 B**；固件 `make` 0/0、**166628 B**；**上板未验**。⚠ op 11 **首次上板**：`00h`-addr-`30h` 后只输出 4 字节属**部分页输出**，与 GET_FEATURES 的 4 字节输出同族但作用在页读路径，器件是否接受需上板确认；⚠ 4 字节落点是 `.bss` 的 `g_fc_return_staging`（先例 = GET_FEATURES 已用同一暂存在板上跑通，若存在 D-Cache 隐患则两者同源，非本版引入）；⚠ 本 op 带 `CHECK_FAIL` ⇒ 读失败时命令报 `NAND_FAIL`，正好对应「读页失败 = 坏块」；⚠ **FTL 未接入**（`src/ftl/**` 一行不动，`bbt_scan_factory` 仍走 op 12），本 op 供后续/自建扫描用 |
| 2026-10-11 | v1.27 | **加载 / 取数两段式显式下发（op 33/29 支持「不带 desc」+ op 8/41 带校验取数）**（见 `docs/changelog/fc_ut_load_fetch_v45.md`）。起因：op 33/29/8/41 虽在生产读路径里**每读必跑**（12→33 / 30→29 的 rewrite + CW 链逐 CW 发 8/41），却**从未作为独立命令被点名**——uop 序列、page-read 分类、页缓冲散落零测试背书。改动分两层：**① `src/`（首次让加载型 op 可无 desc 下发）**——op 表 op 33/29 的 `has_data` `1U`→`0U`（`src/nand/onfi_1_0.c`；`ftl_cmd_needs_desc_chain` / `fc_command_validate` 随之不再要求链）；`fc_is_page_read` 补 `OP_RANDOM_DATA_READ_MULTI_PLANE`（与 op 8 对称）；`fc_build_dispatch_request` 新增「无 desc 加载」分支（op 33/29 从 `command->pba` 填 `plane_paa[]`，不建位图/不占 buffer）；`req_read_page` 空掩码时 `data = NULL`（消除 `cw_buffers[]` 残留指针隐患）。**生产路径零影响**（分支按 FC 命令的 op 12/30 选，33/29 只作 rewrite 目标；`fc_is_page_read(41)` 生产里从不被查）。**② `tests/`**——夹具新增 `test_load_page()`（不带 desc 的纯加载）与 `test_random_data_read()`（带整段掩码 desc 的取数，逐 CW PAA 校验），`test_is_multi_op` 补 op 29/41；新用例 `block_load_fetch`（单 plane op 33→op 8；双 plane op 29→op 41）。用例数 **51 → 52**（Level 3 **9 → 10**）。`make format-check` 过、`make clean && make tests` 0 warning 0 error、链接 **223572 B**；**上板未验**。⚠ 两段必须紧邻（中间若有别的命令碰器件页缓冲，取数会读到错页）；⚠ op 33/29 首次作为独立命令下发，器件是否接受 `00h-30h` 加载后无后续 30h 输出即驻留，需上板逐字确认——若红，正是要暴露的结论 |
| 2026-10-11 | v1.26 | **补 `OP_READ_PAGE_CACHE_RANDOM` 覆盖——cache 读的第二条入口**（见 `docs/changelog/fc_ut_cache_random_v44.md`）。起因：盘点 Level 3 的 OP 码覆盖时发现 cache 读只用 START 入口（00h-addr-30h），`OP_READ_PAGE_CACHE_RANDOM`（00h-addr-31h）虽在生产可达（`fc_is_page_read` 收它、NAND 侧 `req_cache_read` + `nand_dispatch.c:662` 显式处理）却**零测试背书**。改动：① 夹具新增 `test_read_cache_random(block, plane_bitmap, page_index, config)`——带 5 个地址周期（传哪页读哪页，与 SEQ/LAST 的「地址恒 0、设备自增」相反），「载入 + 输出」合一 ⇒ `meta` 取**同一页** PBA；逐 CW PAA 校验；② `block_cache_ops` 加 Phase 3，回读中间页（`PAGES_PER_BLOCK/2`）。**只动 `tests/**`，`src/**` 一行不动**；用例数不变（仍 51，9 条 Level 3 未增减）。`make format-check` 过、`make clean && make tests` 0 warning 0 error、链接 **220908 B**；**上板未验**。⚠ 该 op 此前从未上过板：若器件不接受 `00h-addr-31h` 独立执行，本版会当场红——这正是要验的 |
| 2026-10-11 | v1.25 | **Level 2 地址随机化：13 条用例改用例体内 salted PRNG**（见 `docs/changelog/fc_ut_level2_addr_random_v43.md`）。**测试 + 文档**（`tests/unit/test_fc_level2.c` + 本文档 + 新 changelog），**`src/**` 一行不动**（含 `src/ftl/**`）。① **起因与自我订正**：v1.24 的「顺带观察」曾称 Level 2 `scramble_whiten` 仍写死 `block=1328`/`2042`——**错**（该函数早就用 `munit_rand_int_range`；那三个值是三条探针各抽一次）。真正写死的是 `TEST_BLOCK`(100) / `TEST_PAGE`(32) / plane 0，经 `L2_META` / `L2_PAA` / `l2_cmd` 服务 13 条用例。② **做法**（对齐 v1.22 的 Level 3）：删 `L2_META` / `L2_PAA` 两宏，新增 `l2_hash`（DJB2，与 `munit.c:386` 逐字同）/ `l2_rand` / `l2_addr_draw(test, plane)`（`cw` 恒 0，就地打 `test_trace_detail("addr …")`）；`l2_cmd` / `l2_buffer_round_trip` / `l2_ecc_cw_build` / `do_raid_recover_test` 各加 `const nand_addr_t *addr` 首参（显式传递：忘了传是**编译错误**，好过静默用错地址）；13 条用例体各抽一次。**不设窗口**（本层不写介质，Level 3 的 30%~70% 理由不成立）⇒ 用整段 `[0, BLOCKS_PER_PLANE)` / `[0, PAGES_PER_BLOCK)`。③ **⚠ 本版真正的技术发现——抽样必须过一道终混**：只做 XOR 会撞（`l2_hash` 是 DJB2，`33 ≡ 1 (mod 32)` ⇒ 低 5 位只等于各字符之和 mod 32，而抽样 `% 2048`/`% 64` 截低位用）；实测 `h("process_read_mata_error")=0xE9E586CA` 与 `h("process_read_ldpc_raw")=0x0A6286CA` 的 XOR = `0xE3870000`（低 16 位全 0）⇒ 四条种子下逐位同址。修法：`l2_rand` 末尾加 murmur3 `fmix32` 终混。⚠ Level 3 的 `l3_rand` 结构相同但实测六条无碰撞，**本轮不动**（动它会改 v40 已验证的 seed→地址映射）。④ **三处特殊**：`process_read_mata_error` 篡改目标 `(addr.block+1)%BLOCKS_PER_PLANE`（同 page/plane、异 block，保低 3 位）、`cw_bitmap` 的 plane 必须 = 场景表 `addr_plane`（只随机 block/page）、`generate_stripe_parity` 的 data/parity **同址**（抽一次共用）。⑤ **参数化用例各 case 同址是刻意的**（盐只有用例名、PRNG 每次 exec 重置；case 之间变的本来就不是地址）。**用例数不变（仍 51）**——这批用例多数带 `SINGLE_ITERATION`（`munit.c:765-767` 硬压成 1）⇒ `--iterations N` 对它们无效，用例体内抽样是唯一的加量杠杆。`make format-check` 过、`make clean && make tests` 0 warning 0 error、链接 **220132 B**；宿主机复刻 4 种子 × 13 用例的不变式（互不重复 / 界内 / mata_error 篡改低 3 位同）**全部通过**；**上板已验**（2026-10-11，seed `0x5a4f21e4`：`test --log-visible debug /fc-nand/data_path/` **31 of 31 (100%)**，11 条编译进去的用例地址**逐条与宿主机预测吻合**（`crc_step_detect` 282/41、`ecc_step` 1945/2、`encode` 170/30、`encode_crc_disabled` 1098/62、`process_read_ok` 949/38、`process_read_unc` 283/0、`masked_chain` 800/60、`mata_error` 951/3、`crc_error` 1672/40、`ldpc_raw` 1710/49、`cw_bitmap` 528/60），`--seed 0x5a4f21e4` 第二轮地址**逐字复现**，`mata_error` 打 `0x00077018 vs 0x00076E18`（block 951→952、低 3 位同））。⚠ 遗留：`fc_test_common.h:157` 的 `TEST_PAGE` 现已零读者（本版不动共享夹具头文件，只记录） |
| 2026-10-11 | v1.24 | **上板验证 v1.22 / v1.23 + 订正 `--iterations` 语义**（见 `docs/changelog/fc_ut_board_verify_v42.md`）。**纯文档，`tests/**` 与 `src/**` 一行不动**。① **上板结论**：`--list` 32 行路径全带分隔斜杠、`test /fc-nand/lifecycle/` 9 条、`--iterations 5` 9 条、单条 debug 1 条、全套 **51/51 (100%)**；六条随机地址全在 **614..1433**、`src ≠ dest`、`block_random_data` 两个掩码的 popcount 全部等于打印个数（逐条核到页号，与日志 `read p46` / `read p49` 对上）；同 seed 下 run 2 / run 3 首次迭代 / run 5 的地址**逐字相同** ⇒ 复现契约成立。唯一一条 `[ERR]` 是**上次会话**留在 `.noinit` 的 `[FAULT] previous abort`，非本轮失败。② **订正事实错误**：v1.22 曾写「`--iterations 20` ⇒ Level 3 跑 `3 + 6×20 = 123` 条」——**错**。`--iterations` 只乘**用例体**（`munit.c:780`/`:827` 的 `do…while`），不乘**条目**（`:803` 累加的是局部 `report->successful`，而 `:989` 的 `runner->report.successful++` 每条只加一次）⇒ **条数恒为 9（全套 51）**；打印的单条 `[ N ms ]` 是**每次迭代的平均值**（`:981`）。上板佐证：总耗时 522.62 ms → 2.61 s（≈×5），九条单条时间几乎逐字不变（`internal_data_move` 144.93 → 144.83 ms）。**判断加量看总耗时，不看条数。** ③ §2.3 已按此改写并加实测数据；v1.22 / v1.23 两行的「上板未验」改「上板已验」。④ 顺带观察（未处理）：~~Level 2 的 `scramble_whiten` 仍写死 `block=1328`/`2042`~~ ⚠ **此观察有误，已撤回**（2026-10-11，v1.25）：`scramble_whiten`（`test_fc_level2.c:502-508`）**早就**用 `munit_rand_int_range` 抽 block / plane / page / cw，日志里的 `1328`/`2042`/`1018` 是三条探针**各抽一次**的**三个不同值**，不是写死的常量；真正写死的是 `TEST_BLOCK`(100) / `TEST_PAGE`(32) / plane 0，已在 v1.25 改成按种子抽样。`--list` 只列 32 行而实跑 51 条（Level 2 参数化展开），非缺陷 |
| 2026-10-11 | v1.23 | **子套件前缀补尾斜杠：打印路径与过滤串修正**（`tests/unit/ut_main.c`，见 `docs/changelog/fc_ut_suite_prefix_slash_v41.md`）。munit 把套件前缀与用例名**首尾相接**（`munit.c:1285` 下探子套件、`:1103` 拼用例名），**不补分隔符**；根前缀 `"/fc-nand/"` 带尾斜杠，而三个子套件写成 `"logic"` / `"data_path"` / `"lifecycle"` ⇒ 打印成 `/fc-nand/lifecycleblock_multi_plane`，且**文档里一路写的过滤串 `/fc-nand/lifecycle/…` 一条都匹配不上**（本仓用例名不带前导斜杠）。改动 = 三个字面量补尾斜杠 + 一段警示注释，**测试代码零逻辑改动**（用例名 / 断言 / 用例数不变，仍 51）。⚠ 副作用：**不带**尾斜杠的套件过滤串（`/fc-nand/lifecycle`）由「匹配」变「不匹配」；`--single` 下参数按用例名哈希挑值（`munit.c:1198`）会因全路径变化而挑到不同的值（仅 `--single` 档可见）。⚠ 另一处**既有**缺陷只记录不改：`/fc-nand/`（根套件名）**一条都不匹配**（过滤串短于子套件路径 ⇒ `strncmp` 撞 `'\0'` 即判不等），跑全套要用**不带过滤串**的 `test`。`make format-check` 过、`make clean && make tests` 0 warning 0 error、链接 **219836 B**；**上板已验**（2026-10-11：`test --list` 32 行路径**全部带分隔斜杠**、粘连形式 0 处；`test /fc-nand/lifecycle/` 跑出 **9 条**，见 `docs/changelog/fc_ut_board_verify_v42.md`） |
| 2026-10-11 | v1.22 | **Level 3 地址随机化：用例体内 salted PRNG，窗口 30%~70%**（见 `docs/changelog/fc_ut_level3_seed_random_v40.md`）。① **去参数化**：删 `test_fc_level3.c` 的 `MunitParameterEnum`（`l3_blocks` `100`/`104`、`l3_planes`、`l3_samples` edges/spread/middle/dense）与取值函数 `l3_param_block` / `l3_param_plane` / `l3_sample_case_find`，以及 `#define L3_BLOCK_DEST_OFF (2U)`。② **改用例体内抽样**：新增窗口常数 `L3_BLOCK_LO` / `L3_BLOCK_HI`（编译期由 `BLOCKS_PER_PLANE` 算出 30%/70% ⇒ **614..1433**，避**固定**元数据区 0..41）与 `l3_hash`（DJB2，与 `munit.c:386` 逐字同）/ `l3_rand` / `l3_block_draw` / `l3_plane_draw` / `l3_dest_draw` / `l3_pages_draw` / `l3_cws_draw`。③ **为什么必须加盐**：munit 每 `exec` 前 reseed 回 `runner->seed`（`munit.c:774`）、导出的 `munit_rand_*` **不带 salt**（`munit_rand_at_most` 是 `static`，`munit.c:503`）⇒ 无盐时六条会抽到同一个块、整轮只有一个 plane。`l3_rand = munit_rand_uint32() ^ hash(用例名) ^ hash(用途标签)`。④ **随机化的量**：block、plane、内部搬移目标块（同窗口另抽，`src ≠ dest`，撞上则环移一格）、`block_random_data` 的页集（n∈[2,8]）与 CW 集（n∈[1,4]），拒绝采样；上限 8×4 = 32 探针 = 旧 `dense` 档最坏值 ⇒ 板上耗时上界不变。⑤ **覆盖旋钮改 `--iterations N`**（六条保留 `MUNIT_TEST_OPTION_NONE`）：用例数 **77 → 51**（Level 3 **35 → 9**；Level 1 = 11、Level 2 = 31 不变）。⑥ 证据打印补齐（`--log-visible info`）：`internal_data_move` / `block_erase_verify` / `block_cache_ops` / `block_random_data` 各打一句摘要，`block_random_data` 打 64 位页掩码（拆两个 `%08lX`）+ CW 掩码 + 探针数，掩码 popcount 自证互不重复。⑦ `usart.c` 的 `test` 帮助串补 `--iterations N`（**唯一的 `src/**` 改动**，其余 `src/ftl/**` 一行不动；帮助串变长 ⇒ AAS 重排整张 CLI 表，纯空白）。`make format-check` 过、`make clean && make tests` 0 warning 0 error、链接 **219836 B**；固件 `make` 0/0、**166108 B**；**上板已验**（2026-10-11：`/fc-nand/lifecycle/` 9/9、全套 **51/51**，六条 block 全在 614..1433、`src ≠ dest`、两个掩码 popcount == 打印个数，见 `docs/changelog/fc_ut_board_verify_v42.md`） |
| 2026-10-10 | v1.21 | **Level 2 用例审查：12 个函数补注释 + multi-plane 窄位图的生产侧守卫**（见 `docs/changelog/fc_multiplane_narrow_bitmap_v39.md`）。① **注释**（纯注释、零逻辑改动）：`test_fc_level2.c` 给 `l2_verify_parity_nonzero`、五张场景表的 `*_case_find`、`test_crc_step_detect`、`test_ecc_step`、`test_encode`、`test_process_read_ok`、`do_raid_recover_test`、`test_raid_recover_no_random`、`test_cw_bitmap` 共 12 个此前无注释的函数补「功能 / 测什么 / 为什么这么判」。② `test_process_read_masked_chain` 的 ⚠ 注释更新：那条「multi-plane 必须逐 plane 覆盖」的约束**此前只在夹具里**，v39 起 `fc_build_request_data_planes` 也守同一不变式 ⇒ 生产侧不再是裸奔。③ **生产侧新守卫**（`src/fc/fc.c`，本套唯一被它影响的路径是**测试夹具已挡掉的**非法输入）：multi-plane 命令的 desc 链没覆盖 plane 段里每个 plane 时，debug 档 `ASSERT` 停机、release 档返回 `FC_STATUS_INVALID_ARGUMENT` 拒绝，经 `fc_build_dispatch_request`（`void` → `fc_status_t`）透传到两条派发路径，`fc_poll_fail_command` 注 `NAND_FAIL` 完成项。**断言与用例数不变（77）**。`make format-check` 过、`make clean && make tests` 0/0、链接 **219484 B**；release 档 100884 B（2 处**既有**告警，均不在改动文件内）；**上板未验**。⚠ 新守卫**无单测覆盖**（夹具 `test_cmd_build` 在上游就挡住了非法输入），是代码走查确认的 |
| 2026-10-10 | v1.19 | **把「加扰 = 统计独立」的理论写入设计文档 + 注释代码（纯文档，无逻辑改动）**（见 `docs/changelog/fc_scrambler_theory_v37.md`）。背景：一次关于 `scramble_whiten` 输出的讨论暴露了「加扰 = 追求 `0101…` 均衡」的误解。① 主设计文档 `fc_nand_design.md` 新增 **§5.4「加扰的目标：统计独立」**——**条件** 1/2（独立）vs **无条件** 1/2（均衡）、三种序列对照、`段数 ≈ n/2` 与几何分布 `P(len=k)=2^-k` 的推导、`0101…` 为何对 NAND 有害（零结构 / CCI 最坏 / 与阵列物理周期谐振 ⇒ 系统性错误，ECC 怕系统性）。② 本文档新增 **§5.6「游程统计判据的理论依据」**——`l2_bit01_analyze` 的状态机（`cur` 从首位初始化 / 循环后补收尾 / `≥16` 钳位）、`段数 ≈ n/2`、判据 ② **双侧**（低了 = 强制交替 / 高了 = 长游程），以及判据 ①②③ 的**盲区**（`00110011…` 三条全过却仍是周期图案）。③ 注释：`test_fc_level2.c` 给 `l2_bit01_analyze` 补前导注释、给 `test_scramble_whiten` 补理论依据；`fc_random_engine.{c,h}` 点明「目标是独立，均衡只是副产品」。**不改断言、不改用例数（77）、不改任何 `src/**` 逻辑**（`fc_random_engine.c` 只动注释）。`make format-check` 过、`make clean && make tests` 0/0、链接 **219012 B**（与 v36 逐字节同）；**纯文档 + 注释，无需上板** |
| 2026-10-10 | v1.20 | **`ecc_step` 的覆盖边界与提高计划（纯文档，只记录不改测试）**。背景：一次「这样是不是确认算法没问题」的追问。① 新增 **§5.7**——点明 `ecc_step` 验的是 BCH(m=13,t=4) 的**实现**而非「算法对不对」（后者是代数保证：设计距离 d ≥ 2t+1 = 9，≤ t 个错唯一可译，不需要测）；列**六处覆盖缺口**（① 抽样非穷举 ② 抽样有偏 ③ `beyond-5` 不钉结果且 OK 分支不验数据 ⇒ 误纠被容忍 ④ parity 区只由计数背书、解码器刻意不回写 ⑤ 单一信道模型无突发错 ⑥ 同源编解码无 KAT）；给出**四步提高计划**（定向补 case → `--iterations` 加量 → KAT → CRC 判定接进 FTL），并标明第 4 步在 `src/ftl/**`、按硬规则暂不动。② §2.2 的 `ecc_step` 段加指针。**不改任何断言、不改用例数（77）、不改 `src/**`** |
| 2026-10-10 | v1.18 | **单测证据打印 + `feature_write_read` 改读-改-写回环**（见 `docs/changelog/fc_ut_evidence_trace_v36.md`）。① **证据打印**：新增 §5.5，`tests/unit/fc_test_common.{c,h}` 加 `test_trace_begin/summary(INFO)/detail(DEBUG)` 三个原语，**复用 munit fork 已有的 `test --log-visible info\|debug`**（框架侧只加 1 个只读访问器 `munit_log_visible()`，见 `munit_adapter.md` v1.4）；不走 `munit_logf`（每行加 `Info:文件:行号` 会把明细撑爆）而直接写 `MUNIT_OUTPUT_FILE`；可见性判断放最前 ⇒ **默认档输出逐字不变**；`test_trace_begin()` 的换行改**急切**，修掉 `init_sequence` 里 `[INF] nand: READ_ID` 粘在结果行上的既有缺陷（info/debug 档）。夹具 `test_prog_page` / `test_read_page` / `test_read_block_cache` / `verify_chain_paa` 带 DEBUG 明细，Level 3 九条自动获得逐页证据；Level 1 加 11 条摘要、Level 2 迁移既有 6 处 `munit_logf` + 给自证函数加摘要。② **`feature_write_read` 由「只 GET」改成回环**：Phase 1 只读手册 Table 13 的四个地址（`01h`/`80h`/`81h`/`90h`），Phase 2 在 **`01h`（Timing mode）** 上做读-改-写（Table 15 取值 `00h..05h`），还原的 SET 排在断言**之前**。⚠ 该用例第一版照抄 FTL `RR_FEATURE_READ_RETRY` 写 **`0x89`**——那是本器件的**保留地址**，上板直接红（`readback[0] == probe[0] (0 == 1)`）。③ `usart.c` 的 `test` 帮助串补 `--log-visible`。**生产代码 `src/**` 零改动**。断言与用例数不变（77）。`make format-check` 过、`make clean && make tests` 0/0、链接 **219012 B**；**上板已验**：默认/info/debug 三档各 **77/77**，回环 `原值 00 -> SET 01 -> GET 01 -> 还原 00` |
| 2026-10-10 | v1.17 | **修 Level 3 两处 `cw_bitmap` 建链缺陷——v27 语义翻转的漏扫调用点**（见 `docs/changelog/fc_ut_level3_bitmap_v35.md`）。背景：v23~v26 的 `cw_bitmap == 0` = 「本 plane 段全部 CW」，v27 改为**字面位图**（0 = 空集）并加了两条断言（`ftl_cmd_resolve_cw_bitmap`），但**没回扫调用点**；受影响的 Level 3 自 v31 起只保证编译通过、从未上板，故潜伏至今。① `test_read_block_cache` 的 cache **START** 传 `0U` → 改 `test_plane_cw_bitmap(plane_bitmap)`：v31 注释「无数据阶段 ⇒ 无数据链」是错的——op 表里它的 `has_data = 1U`（与 `WITHOUT_DATA_OUT` 同口径，「加载型」也算占数据路径），而 `fc_command_validate` 断言「has_data ⇒ desc_id 有效」⇒ **必须建链**。② `block_random_data` 的随机列读位图 `1U << cw` → `1U << (plane * CW_NUM + cw)`：cw_bitmap 是 **flat 绝对编号**，plane 1 的 CW0 是 bit 4 不是 bit 0；plane=0 侥幸合法，参数化新增的 plane=1 用例才炸。③ `fc_test_common.h` §三 与 `test_fc_level3.c` 文件头的 `1U << cw` 写法一并改正。**生产代码 `src/**` 零改动**（两个 ASSERT 都是被测代码正确拦住了测试的错误）。用例数不变（77）。`make clean && make tests` 0/0、链接 **213156 B**；**上板已验：77/77 (100%) 全绿**——本套首次全套通过。另更正 v34 记录里「全套 FC UT 0 失败」一句（当时只单跑了 2 条用例） |
| 2026-10-10 | v1.16 | **单测的 META 由「页索引（自造 LBA）」换成**页 PBA**（见 `docs/changelog/fc_ut_meta_pba_v34.md`）。① 夹具 `test_prog_page` / `test_read_cache_step` 的 meta 改 `pba_from_addr(本页地址)`；**`test_read_page` 加 `meta` 形参**（「命令声明自己期待读到哪一页」正是该 tag 的意义，故提到调用点可见；全仓仅两个调用点），`test_read_block` 内传本页 PBA。② `test_fc_level2.c` 的 `L2_META` 改 `pba_from_addr`（15 个使用点不动）；`test_scramble_whiten` 直接用 `addr` 的 PBA 并删掉 `& ~3`（该掩码原本为 `page << 2` 让出 plane/cw 位，PBA 自带低 3 位，理由消失）。③ `test_fc_level3.c` 的 `read_page_verify` / 两个内部搬移 op / `RANDOM_DATA_READ` 换 PBA；`internal_data_move` Step 5 读命令声明**源页 PBA**（见上）。④ **修既有 bug**：`process_read_mata_error` 原先用 `(TEST_PAGE + 1U) << CW_WIDTH`，`+4` 落在 `PLANE_CW_MASK` 的 **bit 2（plane 位）**，被 `lba_to_meta()` 掩掉重铸 ⇒ 断言恒不成立（上板必挂；v33 修好 CRC 后才跑得到这里）。改为篡改 block 维，宿主程序逐 CW 数值验证。**生产代码 `src/**` 零改动，FC/FTL 契约仍是 LBA**；`make clean && make tests` 0/0、链接 **213132 B**；**上板已验**（`process_read_mata_error` / `internal_data_move` 均过，全套 FC UT 0 失败） |
| 2026-10-10 | v1.15 | **Level 3 去转发层 + 随机化参数化（munit 种子驱动）**（见 `docs/changelog/fc_ut_level3_param_v32.md`）。① **去层**：删 `test_fc_level3.c` 里 4 个纯转发 helper——`write_block` / `write_block_cache` / `read_block_verify`（唯一附加值是把 `plane` 索引换成 `TEST_PLANE_BM(plane)` 位图 + 烘焙一个 `L3_CONF`，在夹具「一律位图」约定外引入第二套索引约定）、`run_lifecycle`（两行内联），调用点直接调夹具原语 `test_prog_block` / `test_prog_block_cache` / `test_read_block`。② **参数化**：6 条地址相关用例挂 `MunitParameterEnum`（`block` 100/104、`plane` 0/1、`sample` edges/spread/middle/dense），取值就地解析（`l3_param_block` / `l3_param_plane` / `l3_sample_case_find`，表外 `--param` 用断言挡）；**随机性全走 munit 种子**——`--single` 按用例名哈希挑值、`--seed` 复现、不加则全组合枚举；用例体内**不**调 `munit_rand_*`（`exec` 重播种 + 导出函数不带 salt）。③ **判据变更**（加强）：整段读从 `read_page_verify`（逐 CW READY 位 + 链驱动 PAA）换成 `test_read_block`（聚合 `FC_CW_STATUS_ERROR_MASK == 0` + 按坐标重算 `test_verify_page_paa`）；擦除态仍留在本文件（聚合判据不适用）。④ **修既有缺陷**：`internal_data_move` Step 5 原先期望取目标块坐标、而 copyback 保留源块坐标 ⇒ 必然对不上（v26「数据 = 地址」后暴露）；改为逐页 `test_read_page(dest)` + `test_verify_page_paa(g_source_data, src, …)` 按**源坐标**比对。⑤ `usart.c` 的 `test` 帮助串补 `--single`。`make format-check` 过（101 文件）、`make clean && make tests` 0 warning 0 error、链接 **201580 B**；RAID=1 档（直调子 make）0/0、**209180 B**；**上板未验** |
| 2026-10-09 | v1.14 | **Level 3 迁移到共享夹具——v27~v30 遗留的编译阻塞点闭合**（见 `docs/changelog/fc_ut_level3_migration_v31.md`）。`test_fc_level3.c` 曾引用夹具里已删的 7 个符号（`test_page_cmd_ex` / `test_page_cmd` / `test_run_lifecycle` / `test_fill_user_from_source` / `test_verify_against_source` / `TEST_OP_READ` / `test_write_page`），`make tests` 编不过。本轮：① 建链一律走 `test_cmd_build()`，页 helper 参数对齐（`test_read_page` 补 config、`test_erase_block` 的 `0U` → `L3_CONF(...)`）；② 夹具新导出三件零件 `test_submit_wait()`（提交轮询，**命令仍归调用者**）/ `test_plane_cw_bitmap()`（plane 段 → flat CW 位图）/ `test_first_plane()`，供 READ_ID / GET_FEATURES / 状态查询 / 内部搬移 / RANDOM_DATA_READ 等**形态对不上页 helper** 的用例自拼；③ `L3_CONF(operation, crc)` 复合字面量给出与改造前逐字一致的「裸页」形态（不加扰 + LDPC RAW + 不 RAID）；④ 新 `verify_chain_paa()` 取代 `test_verify_against_source`——**链驱动**逐 desc 查 `desc->paa`（旧函数比的是**与页无关**的源表，「读错页」不可见；且部分掩码读（RANDOM_DATA_READ 只抽 1 CW）不能用 `test_verify_page_paa`，段内未捕获的槽是陈旧值）；⑤ 新 `read_page_verify(..., erased)` 把「逐 CW `FC_CW_STATUS_SUCCESS`」与「聚合错误掩码」分开——擦除页 META=0xFF… 必然带 `MATA_ERROR`，聚合判据不可用；⑥ `block_cache_ops` 的 Phase 2 换成夹具的 `test_read_block_cache()`（31h/3Fh 无地址周期、31h 输出上一轮加载页，无法由 `test_read_page` 循环拼出）。9 条场景数不变。`make format-check` 过、`-fsyntax-only` 全部 unit 测试文件 0/0；**未编译、上板未验** |
| 2026-10-09 | v1.13 | **Level 2 三组用例按判据 7 参数化**（承接 v1.12 的 §二十三）。① `encode_random_off` / `_random_on` / `_single_cw` → 单条参数化 `encode`（3 case）；② `process_read_ok_random_on` / `_random_off` / `_single_cw` → 单条参数化 `process_read_ok`（3 case）；③ `cw_bitmap_flat_mixed` / `_plane1_only` / `_span_multi_plane` → 单条参数化 `cw_bitmap`（3 case，期望链以显式表给出，原「循环 + 公式」形态换成逐位比表）。判据：①② 断言集相同、只有期望不同；③ 差异全在**段宽 / `addr.plane` / 位图**三个字段。`unc` / `masked_chain` / `mata_error` / `crc_error` / `ldpc_raw` **刻意不并**——各自断言不同的可观测量。Level 2 计数 18→**12**（RAID=1 时 20→**14**；参数化展开后实际跑 31 个用例，与合并前一致）。顺带修正表中两条陈旧参数（`masked_chain` 实为**单 plane 段**、`mata_error` 实为 4CW）与两处 `§二十二`→`§二十三` 的引用。⚠ 仍**未编译**（Level 3 在旧符号上），上板未验 |
| 2026-10-09 | v1.12 | **Level 2 步骤级用例 + 参数化 + munit 移植修复的文档同步**（见 `docs/changelog/fc_data_path_step_primitives_v29.md`）。① §2.2 补「两种形态」（**步骤级** 调单 CW 原语 / **管线级** 建链走 config 位）与**步骤级用例**小节：`crc_step_detect`（参数化 4 case，含 `meta-1` / `parity-1` 两条**负例**——钉 CRC 只覆盖 USER `[8,520)`，META 与 parity 在覆盖外）、`ecc_step`（参数化 11 case）、`scramble_whiten`；② Level 2 计数 15→**18**（RAID=1 时 17→**20**）；③ §1 / §5.4 补参数化机制：`munit_parameters_get()` 是 v29 补回的上游漏移植函数（另修 `run_test_wild` 递归实参、`runner->suite->iterations`），CLI `--param name value` 一直是通的；④ §5.1 文件树补 `fc_test_common.{c,h}`；⑤ v27/v28 的 `cw_mask`→`cw_bitmap` 改名与「位图作建链实参」已同步（§2.2 参数组合表、建链几何表）。⚠ **Level 3 仍在旧符号上**（`test_page_cmd_ex` 等），`make tests` 编不过——v27/v28/v29 三版共同的遗留中间态 |
| 2026-10-08 | v1.11 | **Level 2 重构：fixture 修正 + RAID 退化用例清理 + 覆盖补齐，10→15（RAID=1 时 17）**（见 `docs/changelog/fc_ut_level2_rework_v22.md`）。① **fixture 修正**：新增 `alloc_cmd_full()` 统一建链并设 `paa = paa_make(32, 0, 50, 0, 0)`——原 fixture 不设 `paa`，BASIC 种子退化成 `{1,1,2,3}`、keystream 除 byte0 外全 0，所有 `random=ON` 用例的加扰是空转；random=ON 用例补「encode 后 USER 区 ≠ 明文」自证断言。② **RAID 组删 4 留 2**：删 `encode_raid_on` / `raid_recover_first` / `_mid` / `_last`（构造不出真 stripe，只靠三重巧合通过；判据见 `assertion_guidelines.md` §十七·判据 6），保留 `raid_recover_no_random` 与真 stripe 的 `generate_stripe_parity`。③ **覆盖补齐 5 条**：`encode_crc_disabled`、`process_read_masked_chain`（专钉 Pass 4 的 `paa_to_cw`）、`process_read_mata_error`、`process_read_crc_error`、`process_read_ldpc_raw`。④ **断言加强**：`process_read_ok_*` / `_single_cw` 补 `desc->status == READY`（process_read 标脏后仍返回 OK）。⑤ **去冗余**：删 `fill_pattern` 包装与 `print_resource_stats` 注释块，`get_cw_buffer` / `get_cw_base` / `get_cw_count` 收敛到 `cw_desc()` 之上。⚠ 本 fork 的 `MunitSuite` 结构体没有 `setup` 字段，套件级 setup 不存在，故仍逐用例挂 `setup_level2`（靠 `g_level2_inited` 幂等）。`make -k tests` 0 error/0 warning、链接 197344 B；RAID=1 档 0/0、204328 B；format-check 过；上板未验 |
| 2026-10-08 | v1.10 | **Level 1 套件合并：`command` / `queue` / `scheduler` 三子套件 → 单个 `logic`**（11 条用例不变）。理由：Level 2/3 已是「一个文件一个套件」，Level 1 是唯一例外；合并后 `command`(2) / `scheduler`(1) 的过滤粒度已细到等于「单跑一条用例」，而本层全部用例是纯 RAM 微秒级，按模块过滤无收益；真正需要过滤的边界是**依赖面**，由根套件 Level 1/2/3 三层保住。`test_scheduler_init` 用例名 `init` → `scheduler_init`（合并后去歧义）。改动：`tests/unit/test_fc_level1.c`（3 数组→1）、`tests/unit/ut_main.c`（3 extern + 3 表项 → 1 + 1）、本文档 §2.1/§3/§5.1/§4.2、`munit_adapter.md`、`wsl_debug_environment.md` 的 CLI 示例路径 `/fc-nand/command/...` → `/fc-nand/logic/...`。单条用例仍可按全路径跑（`/fc-nand/logic/push_pop`） |
| 2026-10-08 | v1.9 | **Level 1 清理：删 5 条用例 + 对应死代码，16→11**。判据统一为「被测代码还有没有生产调用者」：`fc_command_status_init` / `fc_command_status_set_cw` 自 2026-09-20 起零调用者（CQ 完成项由 `fc_complete_from_nand()` 内联组装）→ **函数 + 头声明 + 3 条用例一并删**；`fc_submission_queue_pop_n` 零调用者（调度器走逐条 `pop`）→ 函数 + 声明 + 用例删；`test_queue_empty_full` 断言集是 `peek_empty` ∪ `overflow` 子集 → 删。套件至此零 `setup`（`setup_fc_suite` 随 `status_init` 两条用例删除），文件不再 include `fc/fc.h`。同时给保留的 11 条逐个补「功能 / 测什么 / 设计思路」注释。四档 0 error 0 warning、`make tests` 链接通过（193956 B）、format-check 过；上板未验 |
| 2026-10-08 | v1.8 | **三级 UT 适配 v13~v21 的接口/策略收敛**（见 `docs/changelog/fc_ut_adapt_v13_v21.md`）：Level 1 45→**16**（删 `validate_*` 3 条——`fc_command_validate` v20 整函数转 ASSERT；删 `scheduler_select_*` 8 条——选择逻辑 v8 起搬进 `static fc_poll_fill`，Level 1 不可达；`status_init` 改池内/栈上两例走 `fc_command_to_id`）；Level 2 15→**10**（+RAID 16），建链改 `ftl_cmd_configure_descriptors`、补 `cw_mask_*` 三条说明、删手动 buffer 释放；Level 3 由「39 OP 参数化」改为 **9 个 block 级场景**，OP 表降为附录；`make tests` 由 29 error/7 warning → **0/0 链接通过** |
| 2026-10-06 | v1.7 | OP 覆盖表补齐 34~41（Phase V ISSUE/WAIT/FETCH + `OP_READ_PAGE_CACHE_START` + 新增 `OP_RANDOM_DATA_READ_MULTI_PLANE`），计数 34→39（`OP_COUNT` 现为 42，含 9/11/20 三个空位；⚠ **槽位 11 已于 v1.28 用作 `OP_READ_META`**，空位剩 9/20）。**待补用例**：`cw_mask` 非满 mask 读（见 `fc_nand_design.md` §25） |
| 2026-10-04 | v1.6 | §6.2 补命令池/描述符池可缓存性与调试器 D-Cache 不一致注意；新增 §6.5 生命周期插桩（`#if 0` 默认关闭）+ `[RES] L3_start` 资源行已注释；见 `debug_cmd_operation_rewrite_20261004.md` |
| 2026-09-22 | v1.5 | 消除全部编译警告：munit.h/c `const` 限定符修复（MunitTest/MunitParameter/MunitSuite 等），test_fc_level3.c `g_op_names` const 修复 |
| 2026-09-22 | v1.4 | 最终审视修复：饥饿保护测试改为 select_chain、补 completion_queue 测试、补 submit/poll/configure NULL 测试、补 starvation 测试。Level 1 从 31→45 测试。上传代码提交 Phase P |
| 2026-09-22 | v1.3 | Level 3 实现：34 个 OP 全生命周期测试编译通过 |
| 2026-09-22 | v1.2 | Level 2 实现：15 个测试编译通过。config.ldpc 修复 |
| 2026-09-22 | v1.1 | Level 1 实现：31 个测试编译通过。文件合并、UART CLI 集成 |
| 2026-09-22 | v1.0 | 初始版本 |