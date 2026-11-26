# H 规则框架

统一入口：`tools/check_c.py`。规则是数据，不再一条 H 一个独立脚本。
`tools/check_types.py` / `tools/check_c_layout.py` 仅保留旧 CLI 兼容。

```text
rulecheck/
  registry.py      # Rule / Finding / 运行 / 输出 / 自检
  model.py         # 共享 Token / Document / Item / 复合类型扫描
  textual/         # C 文本 / 结构规则（共享 Document / Item）
    h41_headers.py
    h50_types.py
    h51_layout.py
  docs/             # 文档规则（与 C 规则共用 registry）
    model.py        # Markdown 行模型：围栏 / doc-policy 例外 / 评审标记
    policy.py       # 文档类型 → 启用检查
    rules/layering.py    # 平台/代码坐标/日期/尺寸/台账
    rules/links.py       # 内部链接与锚点
    rules/refs.py        # 规则编号 / CLAUDE.md 引用
    rules/symbols.py     # 裸符号密度（warning）
    rules/template.py    # ADR 字段 + 模块七段（warning）
    rules/terms.py       # 全称术语首次双语（warning）
    rules/glossary.py    # 术语表核心缩写覆盖（warning）
  semantic/        # 预留：clang-tidy / clang AST 规则
  manual/          # 预留：人审判据登记
```

## 运行

```sh
python3 tools/check_c.py --all
python3 tools/check_c.py --rule H51
python3 tools/check_c.py --rule H50 --rule H41 --report
python3 tools/check_c.py --path src/ftl/ftl.c
python3 tools/check_c.py --all --self-test
```

`make c-rules-check` / `make c-rules-test` 是同一入口的 make 包装。

## 规则分层

| 类型 | 判据 | 载体 |
|---|---|---|
| textual | 词法 / 条目 / 文件布局 | Python `Document` |
| semantic | 类型、调用链、资源配对、数据流 | clang-tidy / clang AST |
| manual | 设计取舍、模块边界 | 审查 checklist |

**不要把 semantic 规则硬塞进 textual**；文本脚本对宏展开和跨函数语义没有区分力。

## 加一条 textual 规则

1. 在 `textual/` 新建模块，导出 `RULE = Rule(...)`：

```python
from rulecheck.registry import Finding, Rule

def check(root, targets):
    return [Finding(path=..., line=..., message="违反 Hxx：...")]

RULE = Rule(
    rule_id="Hxx",
    title="一句话标题",
    kind="textual",
    check=check,
    selftest=selftest,
)
```

2. 在 `textual/__init__.py` 的 `RULES` 加一项。
3. 在规则模块内加正例 + 负例；`selftest()` 返回 `(ok, 描述)`。
4. 不新增 CLI、不改 `registry.py`。

## 加一条文档规则

1. 在 `docs/rules/` 新建模块，导出 `RULE = Rule(rule_id="DOC-...", kind="doc",
   roots=("docs",), suffixes=(".md",), check=..., selftest=...)`。
2. 在 `docs/__init__.py` 的 `RULES` 加一项。
3. 需要按文档类型启用时，只改 `docs/policy.py`，不在规则里写路径。
4. 在模块内写正例 / 负例；`docs-test` 会调用。

```sh
python3 tools/check_docs.py --root . --rule DOC-LAYER
python3 tools/check_docs.py --root . --report
python3 tools/check_docs.py --root . --strict
python3 tools/check_docs.py --root . --path docs/design/ftl_design.md --strict
python3 tools/check_mermaid.py --root .   # 可选，需 mmdc
```

## 加一条 semantic 规则

在 `semantic/` 放 clang-tidy 配置或分析脚本；输出转换成 `Finding` 后注册到
`RULES`。它不进入 `textual/`，也不共享 `Document`。

## 约束

- `Finding` 只有 `path / line / message / severity / rule_id`；规则不要自建输出格式。
- 规则必须可单文件运行；跨文件规则在 `check(root, targets)` 内自行聚合。
- 新规则必须有负例；没有负例的规则不算完成（P13 / C7）。
