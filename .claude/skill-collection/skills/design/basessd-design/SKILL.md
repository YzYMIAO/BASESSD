---
name: basessd-design
description: 功能/方案设计：契约、备选方案、取舍、失败模式与验证方式。用户说「设计一下」「方案怎么选」「写 ADR」「定接口」时使用。不用于实现、代码审查或发布。
---

# 设计 / ADR

## Quick Start
用 basessd-design 为「<功能>」写 ADR：先列备选与出处，再定契约与失败模式。

## 入口
- 立项 brief 已定；或接口/语义需要方案。
- 已读 `CLAUDE.md` C1/P10 与对应 `docs/design/*_design.md`。
- 只是「放哪一层」→ `basessd-design-arch`。

## 工作流
1. 写契约六行（P10）：坐标 / 表 / 计数 / 归属 / 表示链 / 判据。
2. 列备选方案 + 出处（§〇 / `docs/reference/`）；说不出出处标「本仓自拟」。
3. 取舍：代价、失败模式、恢复路径。
4. 验证方式：哪些可测、哪些必须上板。
5. 记录 ADR：决策、被否方案、理由。
6. 交接：实现 → 对应 develop skill。

## 产物格式
- ADR / 设计草案：契约、备选、取舍、失败模式、验证方式。
- 新风险按 R11 登记。

## 边界
- 分层 / 接口 → `basessd-design-arch`。
- 实现 → `basessd-develop-ftl/fc/hil/platform`。
- 审查 → `basessd-review`。

## 命令
无；需要验证时见 `basessd-verify`。

## 按需知识
- `CLAUDE.md` C1 / P10
- `docs/design/revision_history/`
- `docs/design/*_design.md`
- `docs/design/storage_controller_design.md`

## 修订
ADR 变更要写新记录，不改写旧决策（H37）。

## 常见失败

- 只写「怎么做」，不写被否方案与代价。
- 契约缺坐标/归属，实现时各自定义。
- 失败模式只写正常路径。
- 把设计写成实现细节。

## 完成
契约六行齐全；备选与取舍有出处；失败模式与验证方式明确；未提交未推送。

- [ ] 契约六行齐全
- [ ] 备选与取舍有出处
- [ ] 失败模式与验证方式明确
- [ ] ADR 只追加，不改旧记录
