---
name: basessd-project(立项)
description: 新功能/新项目立项：目标、范围、约束、主流调研、风险与验收。用户说「立项」「要不要做」「先评估」「新功能从哪开始」时使用。不用于实现、代码审查或发布。
---

# 立项

## Quick Start
用 basessd-project 评估「<新功能>」：先答主流怎么做 + 出处，再写一页 brief。

## 入口
- 需求还没定清；或新功能/新项目刚开始。
- 已读 `CLAUDE.md §〇` / P0；不清楚主流 → 先查 `docs/reference/`。
- 需要方案落地 → 转 `basessd-design(设计)`。

## 工作流
1. 目标与边界：解决什么问题、不解决什么。
2. 主流调研：主流主控怎么做、出处；DRAM 型 vs DRAM-less。
3. 约束：STM32H743 + MT29F4G08、内存/时序/工具链、R2/R4/R5。
4. 风险：登记 `known_open_items.md`（R11）。
5. 验收：可测判据 + 需要上板的项。
6. 计划：拆到设计 / 开发 / 验证阶段。

## 产物格式
- 一页 brief：目标 / 范围 / 约束 / 出处 / 风险 / 验收 / 计划。
- 风险条目按 R11 五要素登记。

## 边界
- 方案设计 → `basessd-design(设计)`。
- 层改动 → `basessd-develop-ftl/fc/hil/platform`。
- 代码审查 → `basessd-review(审查)`；发布 → `basessd-release(发布)`。

## 命令
无；需要构建/上板时见 `basessd-verify-board(上板)`。

## 按需知识
- `CLAUDE.md §〇` / P0
- `docs/design/storage_controller_design.md`
- `docs/reference/README.md`
- `docs/design/known_open_items.md`

## 修订
brief 变更时同步风险与验收；不把计划写成承诺。

## 常见失败

- 没答「主流怎么做」就立项。
- 范围只有愿望清单，没有「不做什么」。
- 风险不登记，后续重复发现。
- 把计划写成承诺。

## 完成
brief 能回答「做什么、为什么、出处、风险、验收」；风险已登记；未提交未推送（R3）。

- [ ] brief 七字段齐全
- [ ] 主流做法有出处 / 标「本仓自拟」
- [ ] 风险已登记（R11）
- [ ] 验收可测、需上板项已列
