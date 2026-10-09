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
    └── 9 个 block 级场景（init / feature / status / 内部搬移 / 擦写读 / cache / 多 plane / 随机列读）
```

> **适配说明（v1.8）**：本层测试在 v13~v21 的接口/策略收敛后做过一轮适配，见 §7 修订历史与 `docs/changelog/fc_ut_adapt_v13_v21.md`。要点：
> - **校验类测试退场**：`fc_command_validate` 于 v20 整函数转 ASSERT（配置错误不再返回码），`validate_null` / `validate_desc_needed` / `validate_cw_mask` 无返回码可测，已删；`validate_op_range` 只保留「合法 OP 恒 OK」。
> - **调度器选择逻辑不可达**：v8 起 SQ/HSQ 选择搬进 `fc.c` 的 `static fc_poll_fill`，只能经 `fc_service()` 驱动真 NAND 才走到，Level 1 不再能覆盖 `select / select_chain / busy / starvation` 等 8 条，已删。
> - **`fc_command_t.command_id` 于 v15 删除**（ID 改指针派生），断言改用 `fc_command_to_id()`。
> - **建链入口于 v13 上移 FTL**：`fc_command_configure_descriptors`（复数）不复存在，测试改调 `ftl_cmd_configure_descriptors()`。
> - **2026-10-08 删 5 条**（判据：被测代码还有没有生产调用者）——`status_init_pool_cmd` / `status_init_stack_cmd` / `status_set_cw_valid`（`fc_command_status_init` / `_set_cw` 自 2026-09-20 起零生产调用者，CQ 完成项改由 `fc_complete_from_nand()` 内联组装，函数连同用例一并删）、`pop_n`（`fc_submission_queue_pop_n` 零生产调用者，调度器走逐条 `pop`）、`empty_full`（断言集是 `peek_empty` ∪ `overflow` 的子集）。Level 1 16→**11**，套件至此**零 setup**（不再需要 `fc_init()`）。

### 2.1 Level 1 — 纯逻辑测试

**测试目标**：`fc_command.c`、`fc_queue.c`、`fc_scheduler.c` 中的纯逻辑函数。全部用例自带夹具（栈上队列 / 栈上命令），不依赖任何硬件、动态内存或资源池。

> **用例性质说明**：`validate_*` 两条是**冒烟**而非返回值验证——`fc_command_validate` v20 起函数体只剩 `return FC_STATUS_OK`，断言恒真；它们的价值是用合法输入把内层 ASSERT 跑一遍，拦住「以后有人加了过严的断言」。其余 9 条是真正的行为验证。

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

**共计 11 个测试函数。**（合并前按模块分 command 2 + queue 8 + scheduler 1，现统一挂在单个 `logic` 套件下，见 §3）

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
| `scramble_whiten` | 单函数内 `kind = 0..2` 循环 | 加扰白化度 + 自反性 |

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
走**同一组断言**，属迭代而非参数化。

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
- **PAA 约定**：`ch=0 ce=0 lun=0`，page=0..63，plane 0/1；**block / plane 由参数给出**（见下「参数化与随机种子」节，块池 `100`/`104`；内部搬移目标恒 = 源 + 2 ⇒ `102`/`106`）。
- **cache 的 plane 边界**（决定 helper 为什么只做单 plane，两层依据别混）：**ONFI 1.0 根本没有 multi-plane**——Rev 1.0 Table 14 命令集里 cache 只有单 plane 的 `Read Cache` 31h / `Read Cache End` 3Fh / `Read Cache Enhanced` 00h-31h / `Page Cache Program` 80h-15h，全文 `plane` 一词出现 **0 次**（1.0 唯一的「多设备」是 multiple LUN，走 `Interleaved` D1h / 11h）。而**本器件（Micron MT29F4G08）确实有 two-plane cache 编程**——datasheet Table 7 `TWO-PLANE PROGRAM PAGE CACHE MODE 80h-11h-80h-15h`；但**没有 two-plane cache 读**（读侧只有单 plane 31h / 3Fh）。本项目 op 表两种都没有 ⇒ helper 只做单 plane。
- **cache 整块读写**：夹具新增 `test_prog_block_cache()` / `test_read_block_cache()`（**单 plane**——依据见上「cache 的 plane 边界」；`config` 只提供模式位，`operation` 由 helper 逐步覆盖，调用者填什么都被忽略）。写侧前 63 页 `OP_PROGRAM_PAGE_CACHE`（15h）、末页 `OP_NORMAL_PROGRAM_PAGE`（10h）收尾。读侧**不能**由 `test_read_page` 循环拼出——31h / 3Fh 无地址周期（设备自增），且 31h 输出的是**上一轮加载的页**，故 helper 显式编排 `START(p0) → SEQ ×63 → LAST`，`meta` 取**真正输出页**的 LBA（第 i 次 SEQ 取 `(i-1) << CW_WIDTH`）；共 65 条命令输出 64 页。两者都是同步逐条提交 ⇒ 只验数据正确性（cache 命令序列走得通、编码管线不串），**不验流水线收益**。
- **裸页 config**：本文件用 `L3_CONF(operation, crc)` 复合字面量给出「裸页」形态——与改造前 `program_page` / `read_page` / `erase_block` 字段逐字一致（**不加扰** + **LDPC RAW** + **不 RAID**；crc 逐处给，裸页读写沿用池默认的 `FC_CRC_ENABLE`，cache 与 multi-plane 路径显式 `FC_CRC_DISABLE`）。`ch/ce/lun` 由 `test_cmd_build` 从 addr 覆盖，`multi_plane` 由 operation 派生。
- **不走页 helper 的命令**（READ_ID / GET_FEATURES / 状态查询 / 内部搬移 / RANDOM_DATA_READ）用夹具的三件零件自己拼：`test_cmd_build()`（建链）+ `test_submit_wait()`（提交轮询，**命令仍归调用者**）+ `fc_command_release()`；平面段位图用 `test_plane_cw_bitmap(plane_bitmap)` 正推，`addr.plane` 用 `test_first_plane(plane_bitmap)`。
- **读回校验分两档**：整段读用 `test_verify_page_paa(data, block, page, plane_bitmap)`（按地址**重算**期望，覆盖段内全部 CW）；部分掩码读（如 RANDOM_DATA_READ 只抽 1 个 CW）用本文件的 `verify_chain_paa(cmd)`（只看**链上实际有的** desc，期望取 `desc->paa`）——此时段内其余 CW 的源槽根本没被这次读填过。
- **擦除页的判据只能是逐 CW `FC_CW_STATUS_SUCCESS`**：擦除页 META = 0xFF…，Pass 4 ① 自洽必然不过 ⇒ 该 CW 带 `MATA_ERROR`，**聚合 status 的错误掩码非零**。故 `read_block_verify_erased` 走本文件的 `read_page_verify(..., erased=1)`（在 release 前逐 CW 查 SUCCESS 位），不能用 `test_read_block` 的聚合判据。

#### 参数化与随机种子（v1.15）

6 条地址相关用例（`internal_data_move` / `block_erase_verify` / `block_write_read` / `block_cache_ops` / `block_multi_plane` / `block_random_data`）挂 `MunitParameterEnum`，参数词汇表：

| 参数 | 取值 | 用在 |
|---|---|---|
| `block` | `100` / `104` | 除 multi-plane 外的地址相关用例 |
| `plane` | `0` / `1` | `block_erase_verify` / `block_write_read` / `block_cache_ops` / `block_random_data` |
| `sample` | `edges` / `spread` / `middle` / `dense` | 仅 `block_random_data`（页集 + 列集；`spread` 逐字等于改造前现状） |

其余 3 条（`init_sequence` / `feature_write_read` / `status_query`）是定长命令序列、地址不参与，**不参数化**（保持 `SINGLE_ITERATION` + `NULL`）。

**随机性由 munit 的种子驱动，不落在用例体内**：
- `--single` 让每个参数枚举**随机挑一个值**，salt 是**用例名哈希**（`munit.c:1139`）⇒ 同一轮里各用例挑到的组合互不相同；`runner.seed` 一次运行一个值（默认由 `time(NULL)` 派生、开场打印 `Running test suite with seed 0x…`，`--seed N` 可复现）。
- 不加 `--single` ⇒ 按参数表**全组合枚举**（笛卡尔积，`munit.c:1160` 的 `run_test_wild` 递归展开）：`block` 档 2 组、`block×plane` 档 4 组、`block×plane×sample` 档 16 组。`--single` 下每条恒 1 组。
- CLI `--param name value` **优先于**随机挑值（`munit.c:1102-1120`，`filled == 1` 先短路）；框架**不校验**值 ⇒ 表外名字直接进用例，取值处（`l3_param_block` / `l3_param_plane` / `l3_sample_case_find`）用断言兜底。
- ⚠ **用例体内不要用 `munit_rand_*`**：`exec` 在用例体前把 PRNG 重置回 `runner->seed`（`munit.c:715`），而导出的 `munit_rand_int_range` 不带 salt（带 salt 的 `munit_rand_at_most` 在 `munit.c` 里是 `static`，不导出）⇒ 两个用例体内的**第一个**随机数完全相同。随机性只走「参数取值」这一条路。

**`internal_data_move` 的 Step 5 按源坐标比对**：copyback 搬的是源页内容——`READ_FOR_INTERNAL_DATA_MOVE` / `PROGRAM_FOR_INTERNAL_DATA_MOVE` 都**不是** page read/write，数据路径不介入、不重铸 ⇒ 目标页的 USER 仍是**源块坐标**的 PAA。故 Step 5 逐页 `test_read_page(dest, …)` + `test_verify_page_paa(g_source_data, src, page, …)`（期望取**源块**），**不能**用 `test_read_block(dest)`——后者按入参 block 重算期望（`fc_test_common.c:644-675`）。数据路径侧照常通过：META 低 3 位 = plane/cw（源/目标同 plane、同 cw 枚举）、META 的 LBA 与命令 meta 同页 ⇒ Pass 4 两项都过；CRC 因 USER 原样搬过而一致。

#### 测试用例（9 个，其中 6 条参数化）

| 测试函数 | 场景 | 验证点 |
|---|---|---|
| `init_sequence` | RESET → READ_ID → READ_PARAMETER_PAGE | 初始化序列各步生命周期完成；READ_ID 前 5B / 参数页 ONFI 签名 |
| `feature_write_read` | SET_FEATURES → GET_FEATURES | Feature 写回一致 |
| `status_query` | READ_STATUS / READ_STATUS_ENHANCED | 状态查询生命周期 + CW status |
| `internal_data_move` | READ_FOR_INTERNAL_DATA_MOVE → PROGRAM_FOR_INTERNAL_DATA_MOVE | 源 block → 目标 block 搬移后 64 页逐字节一致 |
| `block_erase_verify` | ERASE → 全 0xFF 校验 | 擦除后逐页全 0xFF |
| `block_write_read` | PROGRAM → READ 全 block | 写读数据一致 |
| `block_cache_ops` | CACHE_START(p0) → CACHE_SEQ ×63 → CACHE_LAST | cache 读管线（30h/31h/3Fh）；写侧前 63 页 15h + 末页 10h |
| `block_multi_plane` | MP_ERASE → MP_PROGRAM → MP_READ | 两 plane 各自 pattern 独立验证（desc 0..3=plane0，4..7=plane1） |
| `block_random_data` | READ + RANDOM_DATA_READ 抽样 | 按 `sample` 取页集/列集随机列读，验证列地址解码 |

> 前 3 条（`init_sequence` / `feature_write_read` / `status_query`）不参数化；其余 6 条挂 `block` / `plane` / `sample`（见上「参数化与随机种子」节）。每条用例的 setup/teardown 与 Level 2 共用同一份 `test_setup` / `test_tear_down`（`fc_test_common.c`）。
>
> `block_lock_tight`（紧锁定，不可逆锁死整片 NAND）实现保留但以 `#if 0` 隔开、不注册；`unlock_erase` 同样 `#if 0` 停用。

#### OP 覆盖参考表（附录）

下表列出各 OP 是否有 NAND 状态机前置依赖（`standalone=❌` 表示独立执行时 NAND 可能返回错误，需前置 READ/PROGRAM）。当前 9 个用例各自准备前置状态，不直接依赖本表。

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
| 10 | `OP_PROGRAM_FOR_INTERNAL_DATA_MOVE` | ❌ | 内部搬移编程 |
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

---

## 3. 测试套件组织

```c
/* test_fc_level1.c — 单个子套件 logic，共 11 个测试，全部零 setup */

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
        { (char *)"logic",     g_fc_logic_tests,     NULL, 1, MUNIT_SUITE_OPTION_NONE },
        { (char *)"data_path", g_fc_data_path_tests, NULL, 1, MUNIT_SUITE_OPTION_NONE },
        { (char *)"lifecycle", g_fc_lifecycle_tests, NULL, 1, MUNIT_SUITE_OPTION_NONE },
        { NULL, NULL, NULL, 0, MUNIT_SUITE_OPTION_NONE },
    },
    1,
    MUNIT_SUITE_OPTION_NONE,
};
```

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

/* test_fc_level3.c — lifecycle 子套件，9 个 block 级场景用例 */
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
        { (char *)"logic",     g_fc_logic_tests,     NULL, 1, MUNIT_SUITE_OPTION_NONE },
        { (char *)"data_path", g_fc_data_path_tests, NULL, 1, MUNIT_SUITE_OPTION_NONE },
        { (char *)"lifecycle", g_fc_lifecycle_tests, NULL, 1, MUNIT_SUITE_OPTION_NONE },
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
{ "test", "test [--seed N] [--list] [test_name...]", "run munit unit test suite", 1U, 10U, uart_cli_test },

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
ssd> test --list-params /fc-nand/lifecycle/            # 看 Level 3 的参数词汇表
ssd> test --single /fc-nand/lifecycle/                 # 每用例按种子随机抽一组参数
ssd> test --single --seed 0x1a2b3c4d /fc-nand/lifecycle/
ssd> test --single --param block 104 /fc-nand/lifecycle/  # CLI 值优先于随机挑值
ssd> test
```

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
│   ├── test_fc_level1.c   # Level 1: 11 个测试（logic 单套件）
│   ├── test_fc_level2.c   # Level 2: 12 个测试（步骤级 + encode/process_read/raid_recover + cw_bitmap）
│   └── test_fc_level3.c   # Level 3: 9 个 block 级场景（真实 NAND）
├── integration/
│   ├── test_fc_nand.c     # 旧硬件集成测试（**未编入 `tests` 目标**；仍引用已删的复数建链入口，待废弃）
│   └── test_fc_nand.h
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
- **随机化参数化（v1.15）**：`--single` 让每个参数枚举按种子**随机挑一个值**（salt = 用例名哈希，`munit.c:1139`），`--seed N` 复现上一轮；不加 `--single` 走全组合枚举。Level 3 的 6 条地址相关用例走这条路（见 §2.3「参数化与随机种子」）。⚠ 用例体内**不要**自己调 `munit_rand_*`——`exec` 会重置 PRNG（`munit.c:715`），且导出的取数函数不带 salt。

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
| 2026-10-10 | v1.15 | **Level 3 去转发层 + 随机化参数化（munit 种子驱动）**（见 `docs/changelog/fc_ut_level3_param_v32.md`）。① **去层**：删 `test_fc_level3.c` 里 4 个纯转发 helper——`write_block` / `write_block_cache` / `read_block_verify`（唯一附加值是把 `plane` 索引换成 `TEST_PLANE_BM(plane)` 位图 + 烘焙一个 `L3_CONF`，在夹具「一律位图」约定外引入第二套索引约定）、`run_lifecycle`（两行内联），调用点直接调夹具原语 `test_prog_block` / `test_prog_block_cache` / `test_read_block`。② **参数化**：6 条地址相关用例挂 `MunitParameterEnum`（`block` 100/104、`plane` 0/1、`sample` edges/spread/middle/dense），取值就地解析（`l3_param_block` / `l3_param_plane` / `l3_sample_case_find`，表外 `--param` 用断言挡）；**随机性全走 munit 种子**——`--single` 按用例名哈希挑值、`--seed` 复现、不加则全组合枚举；用例体内**不**调 `munit_rand_*`（`exec` 重播种 + 导出函数不带 salt）。③ **判据变更**（加强）：整段读从 `read_page_verify`（逐 CW READY 位 + 链驱动 PAA）换成 `test_read_block`（聚合 `FC_CW_STATUS_ERROR_MASK == 0` + 按坐标重算 `test_verify_page_paa`）；擦除态仍留在本文件（聚合判据不适用）。④ **修既有缺陷**：`internal_data_move` Step 5 原先期望取目标块坐标、而 copyback 保留源块坐标 ⇒ 必然对不上（v26「数据 = 地址」后暴露）；改为逐页 `test_read_page(dest)` + `test_verify_page_paa(g_source_data, src, …)` 按**源坐标**比对。⑤ `usart.c` 的 `test` 帮助串补 `--single`。`make format-check` 过（101 文件）、`make clean && make tests` 0 warning 0 error、链接 **201580 B**；RAID=1 档（直调子 make）0/0、**209180 B**；**上板未验** |
| 2026-10-09 | v1.14 | **Level 3 迁移到共享夹具——v27~v30 遗留的编译阻塞点闭合**（见 `docs/changelog/fc_ut_level3_migration_v31.md`）。`test_fc_level3.c` 曾引用夹具里已删的 7 个符号（`test_page_cmd_ex` / `test_page_cmd` / `test_run_lifecycle` / `test_fill_user_from_source` / `test_verify_against_source` / `TEST_OP_READ` / `test_write_page`），`make tests` 编不过。本轮：① 建链一律走 `test_cmd_build()`，页 helper 参数对齐（`test_read_page` 补 config、`test_erase_block` 的 `0U` → `L3_CONF(...)`）；② 夹具新导出三件零件 `test_submit_wait()`（提交轮询，**命令仍归调用者**）/ `test_plane_cw_bitmap()`（plane 段 → flat CW 位图）/ `test_first_plane()`，供 READ_ID / GET_FEATURES / 状态查询 / 内部搬移 / RANDOM_DATA_READ 等**形态对不上页 helper** 的用例自拼；③ `L3_CONF(operation, crc)` 复合字面量给出与改造前逐字一致的「裸页」形态（不加扰 + LDPC RAW + 不 RAID）；④ 新 `verify_chain_paa()` 取代 `test_verify_against_source`——**链驱动**逐 desc 查 `desc->paa`（旧函数比的是**与页无关**的源表，「读错页」不可见；且部分掩码读（RANDOM_DATA_READ 只抽 1 CW）不能用 `test_verify_page_paa`，段内未捕获的槽是陈旧值）；⑤ 新 `read_page_verify(..., erased)` 把「逐 CW `FC_CW_STATUS_SUCCESS`」与「聚合错误掩码」分开——擦除页 META=0xFF… 必然带 `MATA_ERROR`，聚合判据不可用；⑥ `block_cache_ops` 的 Phase 2 换成夹具的 `test_read_block_cache()`（31h/3Fh 无地址周期、31h 输出上一轮加载页，无法由 `test_read_page` 循环拼出）。9 条场景数不变。`make format-check` 过、`-fsyntax-only` 全部 unit 测试文件 0/0；**未编译、上板未验** |
| 2026-10-09 | v1.13 | **Level 2 三组用例按判据 7 参数化**（承接 v1.12 的 §二十三）。① `encode_random_off` / `_random_on` / `_single_cw` → 单条参数化 `encode`（3 case）；② `process_read_ok_random_on` / `_random_off` / `_single_cw` → 单条参数化 `process_read_ok`（3 case）；③ `cw_bitmap_flat_mixed` / `_plane1_only` / `_span_multi_plane` → 单条参数化 `cw_bitmap`（3 case，期望链以显式表给出，原「循环 + 公式」形态换成逐位比表）。判据：①② 断言集相同、只有期望不同；③ 差异全在**段宽 / `addr.plane` / 位图**三个字段。`unc` / `masked_chain` / `mata_error` / `crc_error` / `ldpc_raw` **刻意不并**——各自断言不同的可观测量。Level 2 计数 18→**12**（RAID=1 时 20→**14**；参数化展开后实际跑 31 个用例，与合并前一致）。顺带修正表中两条陈旧参数（`masked_chain` 实为**单 plane 段**、`mata_error` 实为 4CW）与两处 `§二十二`→`§二十三` 的引用。⚠ 仍**未编译**（Level 3 在旧符号上），上板未验 |
| 2026-10-09 | v1.12 | **Level 2 步骤级用例 + 参数化 + munit 移植修复的文档同步**（见 `docs/changelog/fc_data_path_step_primitives_v29.md`）。① §2.2 补「两种形态」（**步骤级** 调单 CW 原语 / **管线级** 建链走 config 位）与**步骤级用例**小节：`crc_step_detect`（参数化 4 case，含 `meta-1` / `parity-1` 两条**负例**——钉 CRC 只覆盖 USER `[8,520)`，META 与 parity 在覆盖外）、`ecc_step`（参数化 11 case）、`scramble_whiten`；② Level 2 计数 15→**18**（RAID=1 时 17→**20**）；③ §1 / §5.4 补参数化机制：`munit_parameters_get()` 是 v29 补回的上游漏移植函数（另修 `run_test_wild` 递归实参、`runner->suite->iterations`），CLI `--param name value` 一直是通的；④ §5.1 文件树补 `fc_test_common.{c,h}`；⑤ v27/v28 的 `cw_mask`→`cw_bitmap` 改名与「位图作建链实参」已同步（§2.2 参数组合表、建链几何表）。⚠ **Level 3 仍在旧符号上**（`test_page_cmd_ex` 等），`make tests` 编不过——v27/v28/v29 三版共同的遗留中间态 |
| 2026-10-08 | v1.11 | **Level 2 重构：fixture 修正 + RAID 退化用例清理 + 覆盖补齐，10→15（RAID=1 时 17）**（见 `docs/changelog/fc_ut_level2_rework_v22.md`）。① **fixture 修正**：新增 `alloc_cmd_full()` 统一建链并设 `paa = paa_make(32, 0, 50, 0, 0)`——原 fixture 不设 `paa`，BASIC 种子退化成 `{1,1,2,3}`、keystream 除 byte0 外全 0，所有 `random=ON` 用例的加扰是空转；random=ON 用例补「encode 后 USER 区 ≠ 明文」自证断言。② **RAID 组删 4 留 2**：删 `encode_raid_on` / `raid_recover_first` / `_mid` / `_last`（构造不出真 stripe，只靠三重巧合通过；判据见 `assertion_guidelines.md` §十七·判据 6），保留 `raid_recover_no_random` 与真 stripe 的 `generate_stripe_parity`。③ **覆盖补齐 5 条**：`encode_crc_disabled`、`process_read_masked_chain`（专钉 Pass 4 的 `paa_to_cw`）、`process_read_mata_error`、`process_read_crc_error`、`process_read_ldpc_raw`。④ **断言加强**：`process_read_ok_*` / `_single_cw` 补 `desc->status == READY`（process_read 标脏后仍返回 OK）。⑤ **去冗余**：删 `fill_pattern` 包装与 `print_resource_stats` 注释块，`get_cw_buffer` / `get_cw_base` / `get_cw_count` 收敛到 `cw_desc()` 之上。⚠ 本 fork 的 `MunitSuite` 结构体没有 `setup` 字段，套件级 setup 不存在，故仍逐用例挂 `setup_level2`（靠 `g_level2_inited` 幂等）。`make -k tests` 0 error/0 warning、链接 197344 B；RAID=1 档 0/0、204328 B；format-check 过；上板未验 |
| 2026-10-08 | v1.10 | **Level 1 套件合并：`command` / `queue` / `scheduler` 三子套件 → 单个 `logic`**（11 条用例不变）。理由：Level 2/3 已是「一个文件一个套件」，Level 1 是唯一例外；合并后 `command`(2) / `scheduler`(1) 的过滤粒度已细到等于「单跑一条用例」，而本层全部用例是纯 RAM 微秒级，按模块过滤无收益；真正需要过滤的边界是**依赖面**，由根套件 Level 1/2/3 三层保住。`test_scheduler_init` 用例名 `init` → `scheduler_init`（合并后去歧义）。改动：`tests/unit/test_fc_level1.c`（3 数组→1）、`tests/unit/ut_main.c`（3 extern + 3 表项 → 1 + 1）、本文档 §2.1/§3/§5.1/§4.2、`munit_adapter.md`、`wsl_debug_environment.md` 的 CLI 示例路径 `/fc-nand/command/...` → `/fc-nand/logic/...`。单条用例仍可按全路径跑（`/fc-nand/logic/push_pop`） |
| 2026-10-08 | v1.9 | **Level 1 清理：删 5 条用例 + 对应死代码，16→11**。判据统一为「被测代码还有没有生产调用者」：`fc_command_status_init` / `fc_command_status_set_cw` 自 2026-09-20 起零调用者（CQ 完成项由 `fc_complete_from_nand()` 内联组装）→ **函数 + 头声明 + 3 条用例一并删**；`fc_submission_queue_pop_n` 零调用者（调度器走逐条 `pop`）→ 函数 + 声明 + 用例删；`test_queue_empty_full` 断言集是 `peek_empty` ∪ `overflow` 子集 → 删。套件至此零 `setup`（`setup_fc_suite` 随 `status_init` 两条用例删除），文件不再 include `fc/fc.h`。同时给保留的 11 条逐个补「功能 / 测什么 / 设计思路」注释。四档 0 error 0 warning、`make tests` 链接通过（193956 B）、format-check 过；上板未验 |
| 2026-10-08 | v1.8 | **三级 UT 适配 v13~v21 的接口/策略收敛**（见 `docs/changelog/fc_ut_adapt_v13_v21.md`）：Level 1 45→**16**（删 `validate_*` 3 条——`fc_command_validate` v20 整函数转 ASSERT；删 `scheduler_select_*` 8 条——选择逻辑 v8 起搬进 `static fc_poll_fill`，Level 1 不可达；`status_init` 改池内/栈上两例走 `fc_command_to_id`）；Level 2 15→**10**（+RAID 16），建链改 `ftl_cmd_configure_descriptors`、补 `cw_mask_*` 三条说明、删手动 buffer 释放；Level 3 由「39 OP 参数化」改为 **9 个 block 级场景**，OP 表降为附录；`make tests` 由 29 error/7 warning → **0/0 链接通过** |
| 2026-10-06 | v1.7 | OP 覆盖表补齐 34~41（Phase V ISSUE/WAIT/FETCH + `OP_READ_PAGE_CACHE_START` + 新增 `OP_RANDOM_DATA_READ_MULTI_PLANE`），计数 34→39（`OP_COUNT` 现为 42，含 9/11/20 三个空位）。**待补用例**：`cw_mask` 非满 mask 读（见 `fc_nand_design.md` §25） |
| 2026-10-04 | v1.6 | §6.2 补命令池/描述符池可缓存性与调试器 D-Cache 不一致注意；新增 §6.5 生命周期插桩（`#if 0` 默认关闭）+ `[RES] L3_start` 资源行已注释；见 `debug_cmd_operation_rewrite_20261004.md` |
| 2026-09-22 | v1.5 | 消除全部编译警告：munit.h/c `const` 限定符修复（MunitTest/MunitParameter/MunitSuite 等），test_fc_level3.c `g_op_names` const 修复 |
| 2026-09-22 | v1.4 | 最终审视修复：饥饿保护测试改为 select_chain、补 completion_queue 测试、补 submit/poll/configure NULL 测试、补 starvation 测试。Level 1 从 31→45 测试。上传代码提交 Phase P |
| 2026-09-22 | v1.3 | Level 3 实现：34 个 OP 全生命周期测试编译通过 |
| 2026-09-22 | v1.2 | Level 2 实现：15 个测试编译通过。config.ldpc 修复 |
| 2026-09-22 | v1.1 | Level 1 实现：31 个测试编译通过。文件合并、UART CLI 集成 |
| 2026-09-22 | v1.0 | 初始版本 |