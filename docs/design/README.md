# 设计文档索引（design/）

本目录是活跃权威设计文档（18 个）。先读哪个取决于任务 —— 导航表在
[`../README.md`](../README.md)。这里只给**阅读顺序**与**哪些是内部账本**，不复制设计内容。

## 阅读顺序

| 层 / 主题 | 主设计（先读） | 决策史 | 测试设计 |
|---|---|---|---|
| 系统总览 | [`../architecture.md`](../architecture.md) → [`../hardware_contract.md`](../hardware_contract.md) | — | — |
| FTL | `ftl_design.md` | `ftl_revision_history.md` | `ftl_ut_design.md` |
| FC-NAND | `fc_nand_design.md`（§26 行业对照 = 判据） | `fc_nand_revision_history.md` | `fc_nand_ut.md` |
| HIL / USB | `hil_design.md` → `usb_msc.md` | — | `hil_ut_design.md` |
| 平台 / 移植 | `platform_abstraction.md` | — | — |
| 测试通用方法 | `testing_guide.md`（§12 = 负例与注入框架） | — | `munit_adapter.md` |
| 设计原则 / 学习 | `storage_controller_design_essentials.md` → `storage_controller_learning.md` | — | — |
| 断言 / 故障捕获 | `assertion_guidelines.md` → `fault_capture.md` | — | — |

## 内部账本（维护者用，公开阅读可跳过）

- `known_risks.md`：缺陷 / 残余风险当前状态（**复查模块前先读**）；已闭合条目在文末。
- `known_iss.md`：未实现 / 未验证的设计项（feature backlog）。
- 各层 UT 设计文档的 **§7.2 负例判定台账**：M（红过且上板）/ N（缝已确认、差上板）/
  K（不可构造 + 理由）与状态词，按层分册，不集中在一处。
- `*_revision_history.md`：逐版本决策史与被否方案（回答「为什么不是另一种做法」）。
