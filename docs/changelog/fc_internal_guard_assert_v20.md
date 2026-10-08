# v20：FC 内部 / 数据路径 / 引擎的"配置错误 → ASSERT"（B 档）

**日期**: 2026-10-08
**相关设计**: `docs/design/assertion_guidelines.md` §九（撤销两行保留项）、§十五（新增）
**前置**: `fc_configure_guard_assert_v18.md`（v18 定调）、`fc_shell_guard_assert_v19.md`（v19 公开壳）

---

## 概述

v18 / v19 定调并落地了失败模式二分——**「申请不到」保留返回码（模仿硬件行为），「配置错误」一律 ASSERT（绝不允许带着错配置发到硬件）**——但只覆盖了 FC 的**公开 API 壳**（建链入口 + 5 个生命周期/buffer 壳）。

本轮（B 档）按同一判据扫 `src/fc/` 的**内部函数 / 数据路径 / 引擎**里同类毛病：把「调用者自铸值非法」当返回码。用户定夺三个岔路：**RAID-off 死代码一并转**、**BCH 库的 `-1` 转 ASSERT**、**`fc_command_validate` 整函数转**。

**审计修正**：v19 记的「`fc_data_path` 9 处」实为 **7 处**（`grep` 为准，全在 `fc_data_path_generate_stripe_parity`）。

## 1. 改动清单

### A. 默认构建覆盖（`CONFIG_FC_RAID=0`）

| # | 文件:函数 | 守卫 | 改动前 | 改动后 |
|---|---|---|---|---|
| 1 | `fc_command.c` `fc_command_validate` | `command == NULL` / `operation ≥ OP_COUNT` / 数据操作无 desc 链 | 三处 `return INVALID_ARGUMENT` | **三条 `ASSERT`**；恒返回 `OK` |
| 2 | `fc_queue.c` `fc_submission_queue_push` | `command_id == FC_COMMAND_ID_INVALID` | `return INVALID_ARGUMENT` | **`ASSERT`**；只留 `OK` / `QUEUE_FULL` |
| 3 | `fc_bch_engine.c` `fc_bch_decode` | `8U*len > (bch->n - bch->ecc_bits)` | `return -1` | **`ASSERT`** |
| 4 | `fc_bch_engine.c` `fc_bch_decode` | `!data \|\| !recv_ecc` | `return -1` | **`ASSERT(data)` + `ASSERT(recv_ecc)`** |

### B. RAID-off（`#if CONFIG_FC_RAID`，默认不编译）

| # | 文件:函数 | 守卫 | 改动后 |
|---|---|---|---|
| 5 | `fc_data_path.c` `fc_data_path_generate_stripe_parity` | 5× `desc/dsc == NULL` + 2× `buf == NULL` → `return INVALID_ARGUMENT` | 逐处 **`ASSERT(x != NULL)`**（函数不再返回 `INVALID_ARGUMENT`） |
| 6 | `fc_raid_engine.c` `fc_raid_accumulate` | `processed >= total` → `return NOT_READY` | **`ASSERT(processed < total)`** |
| 7 | `fc.c` `fc_stripe_submit` | desc 越界 → `NULL` → `release + goto stripe_error` | **`ASSERT(desc_id < FC_DESCRIPTOR_POOL_DEPTH)`**，直取 `&controller->descriptor_pool[desc_id]` |

## 2. 为什么（判据）

- **#1 `fc_command_validate`**：撤销 §九 的保留理由（"输入可能来自 host"）。查证 `operation` **全仓都是 FTL 的硬编码常量**——`sync_page_prepare` 的实参是 `OP_NORMAL_PROGRAM_PAGE` / `OP_NORMAL_READ_PAGE_WITH_DATA_OUT` / `OP_ERASE_BLOCK` 等（`ftl_fc_interface.c`），**从不是 host 输入**。三处前置违反皆判据 2 → `ASSERT`。函数退化为「恒 OK」的校验钩子：debug 下跑内层断言，release 下调用点 `fc_submit` 的 `ASSERT(... == FC_STATUS_OK)` 整条裁空，无运行时开销。
- **#2 `fc_submission_queue_push`**：入队 ID 全由 FC 自铸（`fc.c` 的 `fc_submit_to_queue` / 恢复路径），生产路径从不传 `FC_COMMAND_ID_INVALID` → 判据 2。同行的 `offset >= count`（`peek`/`pop_n` 的越界）仍是预期内的运行结果，**保留返回码**。
- **#3/#4 `fc_bch_decode`**：`-1` 既是「不可纠」哨兵又是参数错误返回——**撞车**会让参数错误被 `fc_ldpc_engine` 误判为不可纠、CW 静默置 UNC（比返回码更糟：静默数据损坏）。`len` 由唯一调用者 `fc_ldpc_engine.c:161` 传常量 `FC_BUFFER_DATA_SIZE`（520，恒 < 码长），`data` / `recv_ecc` 由上游保证 → 判据 2。转 `ASSERT` 后哨兵语义唯一。
- **#5 `fc_data_path_generate_stripe_parity`**：7 处 NULL 判是**死守卫**——`fc_descriptor_id_to_descriptor`（链内 id 恒在池内）与 `fc_buffer_get_base`（恒已占用）内部已各自 `ASSERT`，外层判不可能为真。
- **#6 `fc_raid_accumulate`**：`total` 由调用者 `fc_raid_start` 给定，超累加 = 计数 bug → 判据 2。
- **#7 `fc_stripe_submit`**：`desc_id` 由链遍历产生，越界即链损坏 → 判据 2。删掉 `goto stripe_error` 分支（该分支已不可能到达；`stripe_error` 标签仍被后续 NAND 失败路径使用）。

**保留不动**：`fc_raid_start` 的 `active != 0` → `NOT_READY`、`fc_raid_accumulate` / `fc_raid_end` 的 `active == 0` → `INVALID_ARGUMENT`——运行时状态，判据 3。

## 3. 波及面

- `src/fc/`：`fc_command.c`、`fc_queue.c`、`fc_bch_engine.c`（+新增 `#include "common/assertion.h"`）、`fc_data_path.c`、`fc_raid_engine.c`、`fc.c`。
- `include/fc/`：`fc_queue.h`（push 契约）、`fc_bch_engine.h`（decode 返回契约）、`fc_data_path.h`（generate_stripe_parity 返回契约）、`fc_raid_engine.h`（accumulate 返回契约）。
- `docs/design/assertion_guidelines.md`：§九 撤销 `fc_command_validate` 行与 `fc_queue.c` 的 `FC_COMMAND_ID_INVALID` 半行（保留 `offset >= count`）；§十四 的 B 档"未跟进"标注已跟进；新增 §十五。
- **调用点无需改动**：`fc_command_validate` 唯一调用者 `fc_submit` 已 `ASSERT` 其结果；`fc_bch_decode` 唯一调用者 `fc_ldpc_engine.c:161`；`fc_submission_queue_push` 生产调用者 `fc.c` 全传 FC 自铸 ID；RAID-off 三处的调用者全在 `#if CONFIG_FC_RAID` 内，或（`fc_raid_*`）**全仓零调用者**（死 API）。
- **测试**：`tests/` 直接调 `fc_command_validate` 的用例会 abort（属既有 29 错误范畴，用户自管）。

## 4. 验证

| 项 | 结果 |
|---|---|
| `make debug` | 0 error / **0 warning** |
| `make release` | 0 error / **0 warning**（`(void)command;` 生效） |
| `make debug CFLAGS_EXTRA=-DCONFIG_FC_RAID=1` | 0 error / **0 warning**（覆盖 B 档 RAID-off 改动；`nm` 确认 `fc_raid_accumulate`/`fc_raid_start` 符号已进目标文件，证明代码真的被编译） |
| `make release CFLAGS_EXTRA=-DCONFIG_FC_RAID=1` | 0 error / **0 warning** |
| `make format-check` | passed（101 文件） |
| `make -k tests` | **29 error / 7 warning，与基线逐项一致**（`diff` 逐行相同；25 在 `test_fc_level1.c`、4 在 `test_fc_level3.c`，均为既有问题；`src/`+`include/` 零 error 零 warning） |
| 上板 | **未做**（用户按 `make tests` 上板） |

## 5. 未跟进项（C 档，另议）

- **C 档**（反向病：静默忽略）：`fc_data_path_encode` 的 `if (buffer != NULL)` 静默跳过 META/CRC/加扰/ECC、`fc.c` 三个 state getter 的越界静默 clamp、`pop_n` 不校验 `count`、`fc_bch_engine.c` / `fc_ldpc_qc_engine.c` 缺入参 ASSERT、`fc_ldpc_engine.c` 的 `data_length` 无上界。
- **遗留**：`fc_data_path.c` 里对 `fc_buffer_get_base` 的 `if (buffer != NULL)`（多处）**恒真**，成死守卫——归 C 档。
- **另注**：`fc_get_resource_stats` 的 doc/行为不符（v19 已登记），本轮未动。
