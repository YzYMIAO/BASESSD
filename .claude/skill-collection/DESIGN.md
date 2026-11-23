# 为什么是这种 skill 架构

## 1. 生命周期是顶层

```text
立项 → 设计 → 开发 → 验证 → 审查 → 发布
                 ↑
               维护（缺陷 / 变更）
```

技能首先回答「项目现在处于哪一步」，再回答「这一层具体怎么做」。
所以目录按生命周期分组，不按技能数量平铺。

## 2. 层 skill 是「开发」的子流程

`ftl` / `fc` / `hil` / `platform` 不是「立项 / 设计」的同级阶段，
而是 `develop/` 下的层子流程。`board` 属于 `verify/`，`fault` 属于 `maintain/`。

## 3. collection + symlink

- `skill-collection/` 是集合仓库：技能源、文档、测试、样例都在这里。
- `.claude/skills/` 只放 symlink：保持 Claude Code `/` 菜单名字干净，不出现路径前缀。
- 与 `drama-skills/skills/<skill>` + `~/.codex/skills/<skill>` symlink 同一模型。

## 4. 默认只建 SKILL.md

子目录按需创建，判据：

```text
多个 skill / 工程共用      → docs/ 或 sh/ / tools/
只属于一个 skill、按需读    → references/
只属于一个 skill、要执行    → scripts/
只属于一个 skill、要复制产物 → assets/
客户端 UI 元数据           → agents/（本仓不用）
```

不提前造空目录；不把知识抄进 skill；不维护第二份命令表。

## 5. 单一来源

| 内容 | 唯一位置 |
|---|---|
| 全局判据 R/C/P | `CLAUDE.md` |
| C 规则 H/S 一句话 | `.claude/rules/c-coding.md` |
| C 规则细则 | `docs/guides/coding_rules.md` |
| FTL/FC/HIL/平台约束 | `.claude/rules/*.md` |
| 设计契约 / 测量 | 根 `docs/` |
| 技能流程 | `skill-collection/skills/**/SKILL.md` |

skill 只写流程、入口、边界、产出、完成判据；符号、命令表、行号等易漂移事实一律指向源码或 docs。

## 6. 新增 skill 的判据

同时满足才加：

1. 重复发生（不是一次性发布/调研）。
2. 多步 + 有决策（不是单条规则）。
3. 有明确产出（brief / ADR / 测试矩阵 / 变更单 / release checklist）。
4. 不与现有 skill 抢触发（description 有 stop list）。

否则：规则进 `.claude/rules/`，知识进 `docs/`，样本进 `examples/`。
