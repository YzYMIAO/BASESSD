# 文档地图（docs/）

> 本文帮你**按任务只读该读的文档**。活跃权威文档在 [`design/`](design/)；
> 已闭合 / 一次性 / 历史记录在 [`archive/`](archive/)（**日常不读**）；
> 外部资料离线备份在 [`reference/`](reference/)。

## 按任务该读哪几个

| 任务 | 读（按序） |
|---|---|
| FTL 改动 / 新功能 | `design/ftl_design.md`（主设计）→ `design/ftl_revision_history.md`（逐版本决策史）→ `design/known_iss.md`（未实现项）→ `design/known_risks.md`（风险） |
| FTL 对外接口 | `include/ftl/*.h`（函数契约在头文件）+ `design/ftl_design.md` §5.1（模块契约/调用约束）；历史契约见 `archive/ftl_host_interface.md` |
| FC-NAND 层改动 | `design/fc_nand_design.md`（§26 行业对照 = 判据） |
| **NAND OP 选型 / cache / copy-back / GC 多 plane** | `design/known_iss.md` §2（ISS-OP，方案已归档）→ `design/fc_nand_design.md` §17.6/§25.8 → `design/ftl_design.md` ADR-15 |
| FC 对外接口 | `design/fc_nand_design.md` §2.9/§2.10（`archive/fc_api_usage.md` 为历史使用指南） |
| 写 / 跑 UT | `design/fc_nand_ut.md` + `design/munit_adapter.md`；FTL UT（已重建上板）见 `design/ftl_ut_design.md`；HIL UT（15 例，已上板）见 `design/known_iss.md` §1 |
| 平台 / 移植 / 调试链 | `design/platform_abstraction.md` + `design/scheduler_design.md`（统一定时调度器计划）+ `design/wsl_debug_environment.md` + `hardware_contract.md` |
| 断言 / 日志 / 故障捕获 | `design/logging_design.md`（预期/非预期 + 打印预算 + 飞行记录器）+ `design/assertion_guidelines.md` + `design/fault_capture.md` |
| 设计原则 / 学习 | `design/storage_controller_design_essentials.md` + `design/storage_controller_learning.md`；CW 布局事实已并入 `design/ftl_design.md` §2.1/§2.2 |
| **项目水平 / 定位评估** | `design/project_positioning.md`（对照 OpenSSD / SPDK FTL 的定性评估；不是基准报告） |
| **模块接口 / 解耦判据** | `design/ftl_design.md` §5.1.1/§5.1.2（handle 契约 / 字段所有权 / 超时 / 多 LUN；全文在 `archive/ftl_module_contracts.md`） |
| **USB MSC / U 盘** | `design/usb_msc_udisk.md`（可行性 / TinyUSB / BOT 单命令 / 与主控差距）→ `design/usb_msc_verification.md`（上板验证手册 + 判据 + 脚本）→ `design/testing_guide.md`（分层测试 / 工具 / 性能方法 / 实测） |
| 多 LUN / 共享 L2P cache / 内存域 | `design/ftl_design.md` §2.1/§5.4 + `design/known_iss.md` §4（实施计划已归档） |
| **未实现 / 未验证项（feature backlog）** | `design/known_iss.md` |
| **复查已知风险 / 判断是否该修** | **先读 `design/known_risks.md`**（缺陷/残余风险；已闭合条目在文末）→ 再进对应模块文档 |
| **质疑某条项目规则（`CLAUDE.md`）的出处** | `archive/claude_md_history.md`（规则何时、因为什么改过；日常不读） |

## 目录

```
docs/
├── README.md                  ← 本文件（导航）
├── archive/                   ← 历史区（日常不读）：changelog 70 + dated 6 + 计划/历史 10，见 archive/README.md
├── design/                    ← 活跃权威设计文档（18 个）+ known_iss/known_risks
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

- **活跃** = 代码注释 / 测试 / 构建仍引用它（判据：`grep -rn <文档名> src include tests` 有命中），
  或它是某一层/某类 UT 的权威设计文档。
- **归档** = 已闭合的阶段性记录（单版本落地 / 上板验证 / 一次性调试）、**已被主干取代的计划稿**、
  以及纯历史（版本变更史 / 规则变更史）。
  新增已落地变更记录 → 放 `archive/changelog/`；对当前设计仍有约束力的决策 → 进 `design/` 对应文档；
  未实现/未验证的设计项 → 进 `design/known_iss.md`，不要留在计划稿里。
