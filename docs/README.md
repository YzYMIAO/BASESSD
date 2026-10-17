# 文档地图（docs/）

> 本文帮你**按任务只读该读的文档**。活跃权威文档在 [`design/`](design/)；
> 已闭合 / 一次性 / 历史记录在 [`archive/`](archive/)（**日常不读**）；
> 外部资料离线备份在 [`reference/`](reference/)。

## 按任务该读哪几个

| 任务 | 读（按序） |
|---|---|
| FTL 改动 / 新功能 | `design/ftl_design.md` → `design/ftl_optimization_plan.md`（波次）→ `design/ftl_revision_history.md`（决策史） |
| FTL 对外接口 | `design/ftl_api_usage.md` + `design/ftl_host_interface.md` |
| FC-NAND 层改动 | `design/fc_nand_design.md`（§26 行业对照 = 判据） |
| FC 对外接口 | `design/fc_api_usage.md` |
| 写 / 跑 UT | `design/ftl_ut_munit.md` + `design/fc_nand_ut.md` + `design/munit_adapter.md` |
| 平台 / 移植 / 调试链 | `design/platform_abstraction.md` + `design/wsl_debug_environment.md` + `hardware_contract.md` |
| 断言 / 日志 / 故障捕获 | `design/assertion_guidelines.md` + `design/fault_capture.md` |
| 设计原则 / 学习 | `design/storage_controller_design_essentials.md` + `design/cw_level_mapping_design.md` |
| 多 LUN / 共享 L2P cache / 内存域 | `design/multi_lun_generalization.md` |

## 目录

```
docs/
├── README.md                  ← 本文件（导航）
├── archive/                   ← 历史区（日常不读）：changelog 70 个 + dated 6 个，见 archive/README.md
├── design/                    ← 活跃权威设计文档（19 个）
├── reference/                 ← 资料区：论文 PDF / 网页存档离线备份
├── diagrams/                  ← 示意图
├── architecture.md            ← 软件架构总览
├── boot_flow.md               ← 固件启动流程（复位 → main）
├── hardware_contract.md       ← 硬件契约（STM32H743 + MT29F4G08）
├── nand_model.md              ← NAND 组织模型
├── claude_code_usage.md       ← Claude Code 日常用法（模型切换）
└── ftl_design.md              ← 桩：指到 design/ftl_design.md
```

## 边界

- **活跃** = 代码注释 / 测试 / 构建仍引用它（判据：`grep -rn <文档名> src include tests` 有命中）。
- **归档** = 已闭合的阶段性记录（单版本落地 / 上板验证 / 一次性调试）。
  新增已落地变更记录 → 放 `archive/changelog/`；对当前设计仍有约束力的决策 → 进 `design/` 对应文档。
