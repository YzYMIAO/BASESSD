# 归档区（docs/archive）

> **本区存放已闭合 / 一次性 / 历史阶段文档——日常开发不读。** 活跃权威文档在
> [`../design/`](../design/)。需要追溯某次变更的落地细节 / 上板验证记录时才来这里。

## 目录

```
docs/archive/
├── README.md          ← 本文件
├── changelog/         ← 原 docs/changelog/：FC / FTL 各版本逐版本落地记录（70 个）
├── <dated 6 个>       ← 原 docs/design/ 的一次性阶段文档
└── <计划/历史 10 个>   ← 2026-10-22 文档重组移入（见下节）
```

## changelog/（原 `docs/changelog/`，2026-10-15 移入）

FC / FTL 层**逐版本**变更记录（「为什么这样做 / 决策反转 / 上板验证」），按家族：

| 家族 | 数量 | 说明 |
|---|---|---|
| `fc_*` | 51 | FC 层 v13~v63 建链/UT/地址/加扰/锁 各版本 |
| `ftl_*` | 10 | FTL 层 v23~v75 部分版本 / 断言瘦身（`ftl_assert_redundant_trim`） |
| `nand_*` | 4 | NAND 层（地址构造 / dispatch / ONFI 打印 / poll sink） |
| `platform_*` | 1 | ISR 分散化 v61 |
| 其他 | 5 | `assertion_log_rollout` / `boot_banner_v62` / `diag_crash_cli_board_verify_v71` / `internal_move_op_merge` / `read_column_v63` |

> 版本号序列即搜索键：`docs/archive/changelog/fc_ut_level2_rework_v22.md` 之类，按名检索。

## dated 阶段文档（原 `docs/design/`，2026-10-15 移入）

| 文件 | 内容 |
|---|---|
| `debug_cmd_operation_rewrite_20261004.md` | 一次性调试会话（命令池可缓存性 / op 18→17 调查） |
| `debug_pio_dmb_20260927.md` | 一次性调试会话（PIO __DMB） |
| `debug_sbrk_heap_stack_20261004.md` | 一次性调试会话（_sbrk 堆栈守卫） |
| `ftl_full_landing_plan_20261003.md` | 一次性落地计划（已全部落地） |
| `ftl_wave0_board_verify.md` | FTL 波 0 上板验证清单（v0.70 已过） |
| `ftl_v075_board_verify.md` | v0.75 整机回归验证清单（§0~§12 全绿） |

## 2026-10-22 文档重组移入（10 个）

> 原则：有效设计已并入主干（`../design/`）；未实现/未验证项已抽到
> [`../design/known_iss.md`](../design/known_iss.md)；其余历史/计划稿在此保留全文。

| 文件 | 原用途 | 处置 |
|---|---|---|
| `cw_level_mapping_design.md` | CW 级布局与设计决策 | §1.3/1.4 口径表、§2.2 尺寸推导、approach ③ 已并入 `ftl_design.md` §2.1/§2.2 |
| `multi_lun_generalization.md` | 多 LUN 泛化实施计划 | 内存预算/共享 cache 已并入 `ftl_design.md`；Phase D 等进 `known_iss` §4 |
| `ftl_optimization_plan.md` | FTL 优化波次与遗留表 | §8 未决项进 `known_iss` §3；其余为历史 |
| `nand_op_enablement_plan.md` | NAND OP 启用方案 | P1/P2/P3/Phase 0/D1-D5 进 `known_iss` §2 |
| `hil_ut_design.md` | HIL UT 与系统验收设计 | 用例清单进 `known_iss` §1（未实现） |
| `ftl_host_interface.md` | FTL 对外接口历史契约 | FLUSH/TRIM 语义已补进 `ftl_design.md` §3.9.10 |
| `fc_api_usage.md` | FC 对外接口使用指南 | 主干 `fc_nand_design.md` §2.9/§2.10 已覆盖 |
| `ftl_module_contracts.md` | FTL 模块 handle 契约 | §6 时间归属 / §8 字段所有权并入 `ftl_design.md` §5.1.2；全文保留 |
| `ftl_ut_munit.md` | FTL L0 UT 设计与用例清单 | UT 冻结状态进 `known_iss` ISS-UT-01；全文保留 |
| `claude_md_history.md` | `CLAUDE.md` 规则变更史 | 纯历史 |

## 边界

- 活跃权威设计文档一律在 **`../design/`**（17 个，含 `ftl_revision_history.md`），不要往这里放。
- 新增「已落地变更」的记录：若只是单版本落地/上板记录，直接放这里 `changelog/`；
  若是对当前设计仍有约束力的决策，进 `../design/` 对应文档。
