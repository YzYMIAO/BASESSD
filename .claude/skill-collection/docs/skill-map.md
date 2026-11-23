# Skill 地图与分层契约

> 本文件是技能集合的层次地图，不是 skill；不会被 Skill 工具加载。

## 1. 分层

| 层 | 位置 | 回答 | 加载时机 |
|---|---|---|---|
| L0 常驻 | `CLAUDE.md` | 必须/禁止做什么 | 每次会话 |
| L1 路径规则 | `.claude/rules/*.md` | 在这个文件/领域里必须遵守什么 | 读写匹配文件 |
| L2 生命周期 | `skill-collection/skills/<lifecycle>/` | 项目现在处于哪一步、该产出什么 | 模型判断 |
| L3 层/操作 | `skill-collection/skills/<lifecycle>/<skill>/` | 这一层具体怎么改/跑/查/审 | 模型判断 |
| L4 知识 | 根 `docs/` | 为什么、契约、测量 | 按任务 |
| L5 参考 | `docs/reference/`、`examples/vendor/` | 外部资料、第三方样本 | 按需 |

## 2. 生命周期树

```text
skills/
├── project/                       立项
│   └── basessd-project            现有
├── design/                        设计
│   ├── basessd-design-arch        现有
│   └── basessd-design             现有
├── develop/                       开发
│   ├── basessd-develop-ftl        现有
│   ├── basessd-develop-fc         现有（P0）
│   ├── basessd-develop-hil        现有（P0）
│   └── basessd-develop-platform   现有（P0）
├── verify/                        验证
│   ├── basessd-verify-board       现有
│   └── basessd-verify             现有
├── maintain/                      维护
│   ├── basessd-maintain-fault     现有
│   └── basessd-maintain           现有
├── review/                        审查
│   └── basessd-review             现有
└── release/                       发布
    └── basessd-release            现有
```

## 3. 流程

```text
立项 → 设计 → 开发 → 验证 → 审查 → 发布
                 ↑
               维护（缺陷 / 变更）
```

| 生命周期 | 子流程 |
|---|---|
| 立项 | `project` |
| 设计 | `design` + `arch` |
| **开发** | **`ftl` / `fc` / `hil` / `platform`** |
| 验证 | `verify` + `board` |
| 维护 | `maintain` + `fault` |
| 审查 | `review` |
| 发布 | `release` |

## 4. 边界

| skill | 不做什么 → 转给 |
|---|---|
| `basessd-design-arch` | 实现 / 审查 / 排障 → 层 skill / review / fault |
| `basessd-develop-ftl` | FC/NAND → fc；平台 → platform；审查 → review |
| `basessd-develop-fc` | FTL → ftl；平台 → platform；上板 → board |
| `basessd-develop-hil` | FTL/FC 内部逻辑 → 对应层 |
| `basessd-develop-platform` | 业务层语义 → 对应层 |
| `basessd-verify-board` | 代码审查 → review；故障定位 → fault |
| `basessd-maintain-fault` | 静态审查 → review；计划内改动 → 层 skill |
| `basessd-review` | 架构决策 → arch；实现 → 层 skill |
| `basessd-project` | 实现 → design/develop；审查 → review |
| `basessd-design` | 分层 → design-arch；实现 → develop |
| `basessd-verify` | 上板执行 → verify-board；故障 → maintain-fault |
| `basessd-maintain` | 故障定位 → maintain-fault；实现 → develop；发布 → release |
| `basessd-release` | 开发 → develop；测试 → verify |

## 5. 命名与写法

- 生命周期：`basessd-<stage>(中文)`；子流程：`basessd-<stage>-<domain>(中文)`。
  例：`basessd-develop-ftl`、`basessd-verify-board`。
- **不用 `/`**（路径分隔符），用 `-` 或中文。
- description = **what + when + when not**（stop list）。
- L2 路由器 30~50 行；L3 层流程 60~120 行；只写流程/入口/边界/产出/完成。
- 引用一层深；知识放根 `docs/`，脚本优先 `sh/` / `tools/`。

## 6. 维护

- 新增/删除/改名 skill：同步 `README.md`、本文件、`CLAUDE.md` P4、`check_rules.py`。
- 每次改完：`make rules-check rules-test`；用 `.claude/review/skill-trigger-matrix.md` 做触发回归。
