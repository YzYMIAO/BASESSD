# 归档区（docs/archive）

> **本区存放已闭合 / 一次性 / 历史阶段文档——日常开发不读。** 活跃权威文档在
> [`../design/`](../design/)。需要追溯某次变更的落地细节 / 上板验证记录时才来这里。

## 目录

```
docs/archive/
├── README.md          ← 本文件
├── changelog/         ← 原 docs/changelog/：FC / FTL 各版本逐版本落地记录（70 个）
└── <dated 6 个>       ← 原 docs/design/ 的一次性阶段文档
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

## 边界

- 活跃权威设计文档一律在 **`../design/`**（18 个），不要往这里放。
- 新增「已落地变更」的记录：若只是单版本落地/上板记录，直接放这里 `changelog/`；
  若是对当前设计仍有约束力的决策，进 `../design/` 对应文档。
