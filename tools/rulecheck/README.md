# H 规则框架

统一入口：`tools/check_c.py`。规则是数据，不再一条 H 一个独立脚本。
`tools/check_types.py` / `tools/check_c_layout.py` 仅保留旧 CLI 兼容。

```text
rulecheck/
  registry.py      # Rule / Finding / 运行 / 输出 / 自检
  model.py         # 共享 Token / Document / Item / 复合类型扫描
  textual/         # 文本 / 结构规则（共享 Document / Item）
    h41_headers.py
    h50_types.py
    h51_layout.py
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

## 加一条 semantic 规则

在 `semantic/` 放 clang-tidy 配置或分析脚本；输出转换成 `Finding` 后注册到
`RULES`。它不进入 `textual/`，也不共享 `Document`。

## 约束

- `Finding` 只有 `path / line / message / severity / rule_id`；规则不要自建输出格式。
- 规则必须可单文件运行；跨文件规则在 `check(root, targets)` 内自行聚合。
- 新规则必须有负例；没有负例的规则不算完成（P13 / C7）。
