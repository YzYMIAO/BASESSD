# BASE SSD skill collection

> 本目录是技能的**集合仓库**（类似 `drama-skills`）。
> `.claude/skills/` 只放指向本目录的 **symlink**；真正的 skill 在这里。
> L0 常驻判据在 `CLAUDE.md`，L1 路径约束在 `.claude/rules/`，L4 工程知识在根 `docs/`。

## 结构

```text
.claude/skill-collection/
├── skills/
│   ├── project/                 立项：project
│   ├── design/                  设计：design + arch + docs
│   ├── develop/                 开发：ftl / fc / hil / platform
│   ├── verify/                  验证：verify + board
│   ├── maintain/                维护：maintain + fault
│   ├── review/                  审查：review
│   └── release/                 发布：release
├── docs/
│   ├── skill-map.md             层次地图
│   ├── skill-authoring.md       写作规范（待补）
│   └── trigger-design.md        触发描述（待补）
├── examples/vendor/             第三方样本（gitignored）
├── tests/                       技能契约 / 触发测试（待补）
└── tools/                       symlink / 目录校验（待补）
```

每个 skill 默认只有 `SKILL.md`；有具体内容再加：

```text
<skill>/
├── SKILL.md              必需
├── agents/               可选：openai.yaml（Codex UI 元数据）
├── references/           可选：本 skill 专属资料（优先根 docs/）
├── scripts/              可选：确定性脚本（优先 sh/ 或 tools/）
└── assets/               可选：模板 / 样例
```

## 发现模型

```text
.claude/skills/basessd-develop-ftl
  -> ../skill-collection/skills/develop/basessd-develop-ftl
```

扁平 symlink 保持 `/` 菜单名字干净；层次由本目录的 lifecycle 分组表达。

## 新增 skill

1. 在 `skills/<lifecycle>/<skill>/` 建 `SKILL.md`；必须有 `## 产物格式` / `## 常见失败` / `## 完成`；名字只用小写字母/数字/连字符；引用的 `docs/...` / `.claude/...` 必须存在。
2. 在 `.claude/skills/` 建同名 symlink。
3. 更新 `docs/skill-map.md`、`CLAUDE.md` P4 表、`tools/check_rules.py` 的 `EXPECTED_SKILLS`。
4. 跑 `make rules-check rules-test`；补触发矩阵。

## 校验

```sh
make rules-check rules-test
make docs-check
```
