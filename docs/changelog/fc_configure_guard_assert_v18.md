# v18：建链入口的"配置错误 → ASSERT"定调

**日期**: 2026-10-08
**相关设计**: `docs/design/assertion_guidelines.md` §七（例外撤销）、§十三（新增）
**前置**: `fc_build_chain_convergence_v17.md`（v17 建链收敛）

---

## 概述

`fc_command_configure_descriptor` 的失败模式二分：

| 类别 | 例子 | 处理 |
|---|---|---|
| **申请不到**（模仿硬件行为） | desc / buffer 池空 | **返回码** `RESOURCE_EMPTY`（在意料之中） |
| **配置出错**（调用者 bug） | 句柄非法 / 生命周期不符 / 坐标越界 / 重复挂同一 n / `row_paa` plane 不符 | **`ASSERT`**（绝不允许带着错配置发到硬件） |

据此，该入口不再返回 `INVALID_ARGUMENT`，只返回 `OK` / `RESOURCE_EMPTY`。

## 1. 改动（`src/fc/fc_interface.c` 校验段）

| 守卫 | 改动前 | 改动后 |
|---|---|---|
| `command != NULL` | `ASSERT` | 不变 |
| 指针身份 + 生命周期（`fc_command_to_id` / `ALLOCATED`） | `if (...) return INVALID_ARGUMENT` | **两条 `ASSERT`** |
| plane 段 / CW 范围 | `if (...) return INVALID_ARGUMENT` | **一条 `ASSERT`** |
| 重复 n（`fc_command_cw_attached`） | `ASSERT` | 不变 |
| `paa_to_plane(row_paa) == plane` | `ASSERT` | 不变 |
| desc / buffer 池空 | `return RESOURCE_EMPTY` | 不变 |

## 2. 为什么（判据）

- **§一"值从哪来"**：`command` 是 `fc_command_alloc()` 铸出的句柄，`plane`/`cw`/`row_paa` 是 FTL / RAID 自己算出的坐标——全是调用者自铸值，属判据 2。
- **§七 的例外只是"文档契约"**：而该契约无调用者区分消费——三处调用点（`ftl_cmd_configure_descriptors` / `ftl_cmd_setup_ppa_pair` / RAID parity 建链）全是 `!= FC_STATUS_OK → 沿链释放 + 报错`。
- **配置错误该在污染点炸**：`INVALID_ARGUMENT` 让调用者优雅退出，但错配置本身没有被修正；`ASSERT` 在**污染点**定位 bug，更强。
- **"申请不到"不同**：它模仿硬件行为（描述符 / 缓冲池有限，分配失败是真实运行状态），返回码让上层走清理流程是正当的。

## 3. 波及面

- `src/fc/fc_interface.c`：校验段两个 `if ... return INVALID_ARGUMENT` → `ASSERT`；`(void)command_id;` 吞掉 release 未使用变量；函数头返回契约改写。
- `include/fc/fc_interface.h`：`fc_command_configure_descriptor` 的返回契约去掉 `INVALID_ARGUMENT`，前置条件标注"违反一律 ASSERT"。
- `docs/design/assertion_guidelines.md`：§七 标注例外撤销、§十一附 2 标注、新增 §十三。
- **调用点无需改动**：三处都是 `!= FC_STATUS_OK` 一视同仁，去掉 `INVALID_ARGUMENT` 后语义不变（只可能收到 `RESOURCE_EMPTY`）。

## 4. 验证

| 项 | 结果 |
|---|---|
| `make debug` | 0 error / **0 warning** |
| `make release` | 0 error / **0 warning**（`(void)command_id;` 生效） |
| `make format-check` | passed（101 文件） |
| `make -k tests` | **29 error / 7 warning，与基线逐项一致**（25 在 `test_fc_level1.c`、4 在 `test_fc_level3.c`，均为 v15 删 `command_id` 等既有问题；`src/`+`include/` 零 error 零 warning） |
| 上板 | **未做**（用户按 `make tests` 上板） |

## 5. 未跟进项

- **`fc_submit`**：身份/状态检查按同一判据也应转 `ASSERT`（"队列满"等资源结果仍保留返回码）。
- **`fc_command_release`**：幂等早退是运行时分支（对已 FREE 命令释放是合法 no-op），**不转 ASSERT**。

两项待用户确认后再动。
