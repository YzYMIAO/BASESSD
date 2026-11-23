---
name: basessd-maintain
description: 维护 / 变更 / 回归：缺陷分类、影响范围、兼容性、回归清单与台账。用户说「维护」「改完回归什么」「变更影响」「兼容性」时使用。不用于故障定位、层实现或发布。
---

# 维护 / 变更

## Quick Start
用 basessd-maintain 处理「<缺陷/变更>」：先分类，再定影响面与回归清单。

## 入口
- 已有缺陷、变更请求或回归需求。
- 运行时故障 → 先 `basessd-maintain-fault`。
- 层实现 → 对应 `basessd-develop-*`。

## 工作流
1. 分类：缺陷 / 变更 / 回归 / 文档。
2. 影响面：层、接口、介质格式、配置。
3. 兼容性：`FTL_MEDIA_LAYOUT_VERSION`、API、主机行为。
4. 回归清单：按影响面选既有 UT / 上板项。
5. 台账：R11 状态推进；关闭迁 `known_closed_items.md`。
6. 版本/记录：需要时更新 revision_history / changelog。

## 产物格式
- 变更单：原因、影响面、兼容性、回归清单、证据。
- 台账状态更新。

## 边界
- 故障定位 → `basessd-maintain-fault`。
- 层实现 → `basessd-develop-ftl/fc/hil/platform`。
- 发布 → `basessd-release`。

## 命令
按影响面走 `make tests UT=...`；上板见 `basessd-verify-board`。

## 按需知识
- `CLAUDE.md` R11
- `docs/design/known_open_items.md` / `known_closed_items.md`
- `docs/design/revision_history/`
- `docs/guides/testing_guide.md`

## 修订
变更单只追加；旧决策不改写（H37）。

## 常见失败

- 只看改动点，不看跨模块影响。
- 忽略介质格式 / API / 主机行为兼容性。
- 回归只跑改动路径。
- 台账不推进，下轮重复发现。

## 完成
影响面与兼容性写清；回归有证据；台账已更新；未提交未推送。

- [ ] 影响面与兼容性写清
- [ ] 回归有证据
- [ ] 台账已更新（R11）
