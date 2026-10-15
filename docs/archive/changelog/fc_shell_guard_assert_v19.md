# v19：FC 公开 API 壳的"配置错误 → ASSERT"（A 档）

**日期**: 2026-10-08
**相关设计**: `docs/design/assertion_guidelines.md` §七（例外清零）、§九（buffer 保留项撤销）、§十四（新增）
**前置**: `fc_configure_guard_assert_v18.md`（v18 建链入口定调）

---

## 概述

v18 定调了失败模式二分——**「申请不到」保留返回码（模仿硬件行为），「配置错误」一律 ASSERT（绝不允许带着错配置发到硬件）**——但只改了 `fc_command_configure_descriptor` **一个**入口。

按同一判据回扫 `src/fc/` 全量后，其余**公开 API 壳**仍是老写法：把「调用者自铸句柄 / ID 非法」这类配置错误当返回码。本轮（A 档）收敛这五个公开壳；B 档（内部死守卫）、C 档（静默忽略）另行处理。

## 1. 改动（`src/fc/fc_interface.c`）

| # | 函数 | 改动前 | 改动后 |
|---|---|---|---|
| 1 | `fc_submit` | `if (to_id==0 \|\| state!=ALLOCATED \|\| validate!=OK \|\| !owned) return INVALID_ARGUMENT;` | **四条 `ASSERT`**；只留 `OK`/`QUEUE_FULL` |
| 2 | `fc_command_release` | `if (fc_command_to_id(...)==0U) return INVALID_ARGUMENT;` | **`ASSERT`**；恒返回 `OK` |
| 3 | `fc_buffer_release` | `if (fc_buffer_is_occupied(...)==0U) return INVALID_ARGUMENT;` | **`ASSERT`**；恒返回 `OK` |
| 4 | `fc_buffer_id_to_real_buffer` | 占用守卫 + 恒假 `base==NULL` → `return NULL` | 占用守卫 → **`ASSERT`**；**删**恒假分支 |
| 5 | `fc_buffer_get_base` | 占用守卫 → `return NULL` | 占用守卫 → **`ASSERT`** |
| 6 | `fc_buffer_is_occupied`（static helper） | — | 加 **`__attribute__((unused))`** + 注释 |

**要点**

- **#1**：`command_id` 变 ASSERT-only → `(void)command_id;`。`fc_command_validate` 是非 static（`fc_command.h:249`），NDEBUG 下被裁空**不**触发 `-Wunused-function`。
- **#2**：`fc_command_release_internal_api` **恒返回 OK**（`fc.c:560` + 代码，含 FREE→OK 幂等早退）→ 本壳改后不再可能返回 `INVALID_ARGUMENT`；幂等语义原样保留在 internal。
- **#4**：`fc_buffer_id_to_real_buffer_internal` 只做数组下标，恒不返 NULL；占用已由上一行 ASSERT 保证 ⇒ `if (base == NULL)` 是**恒假死判**，删除。`base` 局部仍用于 `return base + FC_BUFFER_USER_OFFSET;`，无 unused 风险。
- **#6**：`fc_buffer_is_occupied` 是 **static 且全仓仅这 3 个调用者**（:303/:315/:331）；全转 ASSERT 后 NDEBUG 下零调用者 → `-Wunused-function`。用同文件先例 `fc_command_cw_attached`（:92）的 `__attribute__((unused))` 处理；其内部调用的 `fc_buffer_id_is_valid`（:51）因函数体仍编译而保持"被引用"，无级联警告。

## 2. 为什么（判据）

- **§一"值从哪来"**：实参全是调用者自铸/自持值——`fc_command_alloc` 铸出的指针、它自己设的状态、它自己填的 config、`fc_command_configure_descriptor` 分配的 buffer_id——属**判据 2**。
- **§七 例外清零**：v18 撤销了 `fc_command_configure_descriptor`，本轮撤销 `fc_submit` / `fc_command_release`，§七 的"顶层生命周期入口保留返回码"例外**至此全无**。
- **§九 buffer 保留项撤销**：原列"`fc_buffer_is_occupied(...) == 0U` / `fc_buffer_release` 未占用 = 生命周期状态"，现按判据 2 改判**配置错误 → ASSERT**。
- **配置错误该在污染点炸**：`INVALID_ARGUMENT` / `NULL` 让调用者优雅退出，但错配置本身没被修正；`ASSERT` 在**污染点**定位 bug。

## 3. 波及面

- `src/fc/fc_interface.c`：5 个公开壳 + 1 个 static helper（见 §1）；`fc_submit`/`fc_command_release` 各加 `(void)command_id;`。
- `include/fc/fc_interface.h`：这 5 个声明的返回契约改写（去 `INVALID_ARGUMENT` / 注明 NULL 不再出现、前置违反一律 ASSERT）。
- `docs/design/assertion_guidelines.md`：§七 加"修订（二）"、§九 划掉 buffer 保留行、§十三 未跟进项标注已跟进、新增 §十四。
- **调用点无需改动**：
  - FTL 的 `fc_submit(...) != FC_STATUS_OK`（ftl.c / gc.c / read_recovery.c / ftl_fc_interface.c）去掉 `INVALID_ARGUMENT` 后语义不变；
  - `fc_command_release` 全 15 处是 `(void)` 丢弃返回值；
  - `fc_buffer_id_to_real_buffer` 的 FTL 调用点（ftl_fc_interface.c:181/243）**不查 NULL**（前置已有 `buffer_id == INVALID` 守卫），改 ASSERT 安全；
  - **无任何测试依赖这 5 个壳的返回码**（`grep tests/` 确认无 `INVALID_ARGUMENT` / `== NULL` 断言）。

## 4. 验证

| 项 | 结果 |
|---|---|
| `make debug` | 0 error / **0 warning** |
| `make release` | 0 error / **0 warning**（`__attribute__((unused))` + `(void)command_id;` 生效） |
| `make format-check` | passed（101 文件） |
| `make -k tests` | **29 error / 7 warning，与基线逐项一致**（25 在 `test_fc_level1.c`、4 在 `test_fc_level3.c`，均为 v15 删 `command_id` 等既有问题；`src/`+`include/` 零 error 零 warning） |
| 上板 | **未做**（用户按 `make tests` 上板） |

## 5. 未跟进项（B / C 档，另议）

- **B 档**（同类返回码）：`fc_data_path_generate_stripe_parity` 的 7× `desc==NULL` 死守卫、`fc_bch_engine.c:918/927` 的 `-1` 与"不可纠"哨兵撞车、`fc_command_validate` 的 `desc_id==INVALID`、`fc_queue.c` 的 `command_id==INVALID`。
- **C 档**（反向病：静默忽略）：`fc_data_path_encode` 的 `if (buffer != NULL)` 静默跳过 META/CRC/加扰/ECC、`fc.c` 三个 state getter 的越界静默 clamp、`pop_n` 不校验 `count`、`fc_bch_engine.c:231` / `fc_ldpc_qc_engine.c:179` 缺入参 ASSERT、`fc_ldpc_engine.c:104` 的 `data_length` 无上界。
- **遗留**：本改后 `fc_data_path.c` 里对 `fc_buffer_get_base` 的 `if (buffer != NULL)`（:113/132/153/179 等）**恒真**，成死守卫——归 C 档。
- **另注**：`fc_get_resource_stats`（`fc_interface.c:486`）的 doc 写 `FC_STATUS_OK / INVALID_ARGUMENT`，但实现恒返 `OK`（NULL 分支已 ASSERT）——doc/行为不符，属文档瑕疵，本轮未动。
