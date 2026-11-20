# 设计文档索引（design/）

本目录是活跃权威设计文档（主设计 11 篇 + [`common/`](common/) 公共设施 6 篇 + [`revision_history/`](revision_history/) 决策史 2 篇）。先读哪个
取决于任务 —— 导航表在 [`../README.md`](../README.md)。这里只给**阅读顺序**与
**哪些是内部账本**，不复制设计内容。

## 阅读顺序

| 层 / 主题 | 主设计（先读） | 决策史 | 测试设计 |
|---|---|---|---|
| 系统总览 | [`../architecture.md`](../architecture.md) → [`../hardware_contract.md`](../hardware_contract.md) | — | — |
| 启动流程 / Boot policy | [`../boot_flow.md`](../boot_flow.md)、`../src/boot/`（当前未接入 main） | — | — |
| FTL | `ftl_design.md` | `revision_history/ftl_revision_history.md` | `ftl_ut_design.md` |
| FC-NAND | `fc_nand_design.md`（§26 行业对照 = 判据） | `revision_history/fc_nand_revision_history.md` | `fc_nand_ut.md` |
| HIL / USB | `hil_design.md` → `usb_msc.md` | — | `hil_ut_design.md` |
| 平台 / 移植 | `platform_design.md` | — | — |
| 测试通用方法 | `../guides/testing_guide.md`（§12 = 负例与注入框架） | — | — |
| 公共设施 / 横切规则 | [`common/README.md`](common/README.md)（assert / fault / logging / cli / timer / munit） | — | `common/munit_adapter.md` |
| 设计原则 / 学习 | `storage_controller_design.md`（设计要点手册） | — | — |
| **存储主控 SoC 介绍** | `controller_target_features.md`（硬件功能块 + FTL/固件功能块 + 消费/企业差异 + 当前平台案例 + 候选平台） | — | — |

## 内部账本（维护者用，公开阅读可跳过）

- `known_open_items.md`：风险（`RISK-*`）与未实现/未验证缺口（`ISS-*`）的统一台账；开头是未闭合看板，未闭合条目按模块 1~16 分节，已闭合/不实现见附录 B。**复查模块前先读。**
- 各层 UT 设计文档的 **§7.2 负例判定台账**：M（红过且上板）/ N（缝已确认、差上板）/
  K（不可构造 + 理由）与状态词，按层分册，不集中在一处。
- `revision_history/*_revision_history.md`：逐版本决策史与被否方案（回答「为什么不是另一种做法」）。
