---
name: basessd-verify(验证)
description: 验证设计：测试矩阵、负例、证据与验收判据。用户说「测什么」「验收」「负例怎么设计」「验证方案」时使用。不用于上板执行、代码审查或发布。
---

# 验证 / 验收

## Quick Start
用 basessd-verify 为「<改动>」列测试矩阵：每条需求/风险对应一个用例与判据。

## 入口
- 实现已完成；或需要为方案定验收。
- 已读 `CLAUDE.md` R2 / C7 / C8 与 `docs/guides/testing_guide.md`。
- 只是跑板 → `basessd-verify-board(上板)`。

## 工作流
1. 从需求 / 风险 / 契约导出测试项。
2. 每条给判据：期望值、失败长什么样。
3. 设计负例：必须能失败（C7）；恒真断言不算。
4. 分层：UT / 上板 / 主机侧；建链/位图/地址必须上板（R2）。
5. 证据：编译输出、串口原文、计数/日志。
6. 验收：通过 / 失败 / 未验证，按 R1 标注。

## 产物格式
- 测试矩阵：用例 → 判据 → 证据。
- 负例清单；未覆盖项写明原因。
- 台账按 R11 更新。

## 边界
- 上板执行 → `basessd-verify-board(上板)`。
- 运行时故障 → `basessd-maintain-fault(故障)`。
- 代码审查 → `basessd-review(审查)`。

## 命令
`make tests UT=<FC|FTL|HIL>`；上板见 `basessd-verify-board`。

## 按需知识
- `docs/guides/testing_guide.md`
- `docs/design/*_ut_design.md`
- `docs/design/common/assertion_guidelines.md`
- `docs/design/known_open_items.md`

## 修订
需求/契约变化时重导测试项；不复用失效用例。

## 常见失败

- 只验一条路径。
- 判据恒真 / 两边不同源。
- 证据不成立（不是当场镜像 / 旧记录 / 缓存）。
- 把技术成功写成功能生效（C8）。

## 完成
每个风险有用例或明确不测理由；负例能失败；证据齐全；状态如实（R1）。

- [ ] 每个风险有项或明确不测理由
- [ ] 判据能失败、证据可复现
- [ ] 状态如实（R1）
