# 归档区（docs/archive）

> **本区存放已闭合 / 一次性 / 历史阶段文档——日常开发不读。** 活跃权威文档在
> [`../design/`](../design/)。需要追溯某次变更的落地细节 / 上板验证记录时才来这里。

## 目录

```
docs/archive/
├── README.md          ← 本文件
├── changelog/         ← 原 docs/changelog/：FC / FTL 各版本逐版本落地记录（70 个）
├── <dated 6 个>       ← 原 docs/design/ 的一次性阶段文档
├── <计划/历史 10 个>   ← 2026-10-22 文档重组移入（见下节）
└── <2026-11-13 重构归档> ← UT 修订史 / 断言历史轮次 / 设计讨论 / 合并后的原文档
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
> [`../design/known_open_items.md`](../design/known_open_items.md)；其余历史/计划稿在此保留全文。

| 文件 | 原用途 | 处置 |
|---|---|---|
| `cw_level_mapping_design.md` | CW 级布局与设计决策 | §1.3/1.4 口径表、§2.2 尺寸推导、approach ③ 已并入 `ftl_design.md` §2.1/§2.2 |
| `multi_lun_generalization.md` | 多 LUN 泛化实施计划 | 内存预算/共享 cache 已并入 `ftl_design.md`；Phase D 等进 `known_open_items` 模块 10 |
| `ftl_optimization_plan.md` | FTL 优化波次与遗留表 | §8 未决项进 `known_open_items`；其余为历史 |
| `nand_op_enablement_plan.md` | NAND OP 启用方案 | P1/P2/P3/Phase 0/D1-D5 进 `known_open_items` 模块 10 |
| `hil_ut_design.md` | HIL UT 与系统验收设计 | 用例清单进 `known_open_items` 模块 14（未实现） |
| `ftl_host_interface.md` | FTL 对外接口历史契约 | FLUSH/TRIM 语义已补进 `ftl_design.md` §3.9.10 |
| `fc_api_usage.md` | FC 对外接口使用指南 | 主干 `fc_nand_design.md` §2.9/§2.10 已覆盖 |
| `ftl_module_contracts.md` | FTL 模块 handle 契约 | §6 时间归属 / §8 字段所有权并入 `ftl_design.md` §5.1.2；全文保留 |
| `ftl_ut_munit.md` | FTL L0 UT 设计与用例清单 | UT 冻结状态进 `known_open_items` ISS-UT-01；全文保留 |
| `claude_md_history.md` | `CLAUDE.md` 规则变更史 | 纯历史 |

## 2026-11-13 重构归档

> 本轮把一次性评估、纯修订史、已合并的诊断/调度/USB 文档移入 archive；活跃设计文档
> 相应缩减，正文不在这里复制。

| 文件 | 原位置 / 内容 | 处置 |
|---|---|---|
| `project_positioning_20261029.md` | 项目水平定位评估（一次性，2026-10-29） | 原定位评估全文保留在此 |
| `negative_test_plan.md` | 可执行负例清单 | 并入 `../guides/testing_guide.md` §11 |
| `logging_design.md` | 日志设计 | 并入 `../design/common/fault_capture.md` §7 |
| `scheduler_design.md` | 统一定时调度器设计 | 并入 `../design/common/timer.md` |
| `usb_msc_udisk.md` / `usb_msc_verification.md` | USB 设计与上板验证 | 拆为 `../design/usb_msc.md`（设计）+ `../guides/usb_msc_verify.md`（验证） |
| `assertion_rounds.md` | 断言规范 §八~§二十三 历史落地轮次 | 主文件保留标题与指针 |
| `fc_design_discussions.md` | FC §11/§14~§16 设计讨论与重构记录 | 主文件保留标题与指针 |
| `fc_nand_status_history.md` | FC §8/§9 状态/待完善项历史表 | 主文件改为指针 |
| `ftl_design_verification_history.md` | FTL §7.1 历史未验证状态 | 主文件改为指针 |
| `ut_revision_history.md` | `munit_adapter.md` §4 / `fc_nand_ut.md` §7 修订史 | 主文件改为指针 |
| `wsl_debug_history.md` | WSL 调试环境 §8 历史问题 | 主文件保留 §7 当前排障 |
| `testing_guide_itcm_ab.md` | testing_guide §9.4 ITCM/DTCM A/B 细节 | 主文件保留当前结论 |
| `testing_guide_results_history.md` | testing_guide §7.1/§7.2/§7.5/§9.1~§9.3 实测数据 | 主文件保留当前结论 |
| `fc_nand_ut_pre_refactor_20261113.md` | 重构前 FC-NAND UT 设计（含版本史/旧板端证据） | 主文档已按 FTL UT 骨架重写 |
| `known_iss_batches.md` | known_iss 已闭合/不实现条目原文 | 主文件保留索引表 |

## 边界

- 活跃权威设计文档一律在 **`../design/`**（10 篇主设计 + `common/` + `revision_history/` + `known_open_items.md`），不要往这里放。
- 新增「已落地变更」的记录：若只是单版本落地/上板记录，直接放这里 `changelog/`；
  若是对当前设计仍有约束力的决策，进 `../design/` 对应文档。
