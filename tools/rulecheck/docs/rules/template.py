"""模板完整性：ADR 字段（warning，不失败）。"""

from __future__ import annotations

import re
from pathlib import Path

from rulecheck.docs import policy
from rulecheck.docs.model import parse_document
from rulecheck.docs.rules.layering import collect_docs
from rulecheck.registry import Finding, Rule

ADR_RX = re.compile(r"^### (ADR-\d+)\b")
ROW_RX = re.compile(r"^\|\s*([^|]+?)\s*\|")
REQUIRED = ("背景", "选项", "选择", "理由", "主流", "代价", "重估触发", "出处")


def _check_file(root: Path, path: Path) -> list[Finding]:
    findings: list[Finding] = []
    try:
        rel = str(path.relative_to(root))
    except ValueError:
        rel = str(path)
    if policy.category_of(rel) not in ("design", "architecture"):
        return findings

    current: str | None = None
    keys: set[str] = set()
    start_line = 0

    def flush(line: int) -> None:
        if current is None:
            return
        missing = [key for key in REQUIRED if key not in keys]
        if missing:
            findings.append(
                Finding(
                    path=path,
                    line=start_line,
                    message=f"{current} 缺字段：{' / '.join(missing)}",
                    severity="warning",
                    category="template",
                )
            )

    for doc_line in parse_document(path.read_text(encoding="utf-8", errors="ignore")) + [None]:
        if doc_line is None:
            flush(0)
            break
        if doc_line.skip:
            continue
        match = ADR_RX.match(doc_line.text)
        if match:
            flush(doc_line.number)
            current = match.group(1)
            keys = set()
            start_line = doc_line.number
            continue
        if current is None:
            continue
        row = ROW_RX.match(doc_line.text)
        if row:
            keys.add(row.group(1).strip())
    return findings


def check(root: Path, targets: list[Path] | None) -> list[Finding]:
    paths = targets if targets is not None else collect_docs(root)
    findings: list[Finding] = []
    for path in paths:
        findings.extend(_check_file(root, path))
    return findings


def selftest() -> tuple[bool, str]:
    import tempfile

    failures: list[str] = []
    with tempfile.TemporaryDirectory() as td:
        root = Path(td)
        design = root / "docs/design/ftl_design.md"
        design.parent.mkdir(parents=True)
        good = """### ADR-1 示例
| 项 | 内容 |
|---|---|
| 背景 | x |
| 选项 | x |
| 选择 | x |
| 理由 | x |
| 主流 | x |
| 代价 | x |
| 重估触发 | x |
| 出处 | x |
"""
        design.write_text(good, encoding="utf-8")
        if check(root, None):
            failures.append("complete ADR flagged")
        design.write_text(good.replace("| 选项 | x |\n", ""), encoding="utf-8")
        if not any("选项" in item.message for item in check(root, None)):
            failures.append("missing ADR field not caught")
    return (not failures, "; ".join(failures))


RULE = Rule(
    rule_id="DOC-ADR",
    title="ADR 字段完整性",
    kind="doc",
    check=check,
    selftest=selftest,
    roots=("docs",),
    suffixes=(".md",),
)


MODULE_RX = re.compile(r"^### (3\.\d+)\b")
MODULE_SUB_RX = re.compile(r"^#### 3\.\d+\.\d+\s+(.+)$")
MODULE_REQUIRED = (
    ("背景与需求", ("背景",)),
    ("职责与边界", ("职责",)),
    ("结构与不变量", ("结构",)),
    ("流程", ("流程", "路径")),
    ("失败语义", ("失败",)),
    ("设计取舍", ("取舍", "主流")),
    ("常量与阈值", ("常量",)),
)


def _check_modules(root: Path, path: Path) -> list[Finding]:
    findings: list[Finding] = []
    try:
        rel = str(path.relative_to(root))
    except ValueError:
        rel = str(path)
    if policy.category_of(rel) != "design":
        return findings

    modules: list[tuple[str, int, list[str]]] = []
    current: tuple[str, int, list[str]] | None = None
    for doc_line in parse_document(path.read_text(encoding="utf-8", errors="ignore")):
        if doc_line.skip:
            continue
        match = MODULE_RX.match(doc_line.text)
        if match:
            if current is not None:
                modules.append(current)
            current = (match.group(1), doc_line.number, [])
            continue
        if current is None:
            continue
        sub = MODULE_SUB_RX.match(doc_line.text)
        if sub:
            current[2].append(sub.group(1))
    if current is not None:
        modules.append(current)

    for name, line, titles in modules:
        joined = " / ".join(titles)
        missing = [label for label, keywords in MODULE_REQUIRED if not any(key in joined for key in keywords)]
        if missing:
            findings.append(
                Finding(
                    path=path,
                    line=line,
                    message=f"{name} 缺模板小节：{' / '.join(missing)}",
                    severity="warning",
                    category="template",
                )
            )
    return findings


def check_modules(root: Path, targets: list[Path] | None) -> list[Finding]:
    paths = targets if targets is not None else collect_docs(root)
    findings: list[Finding] = []
    for path in paths:
        findings.extend(_check_modules(root, path))
    return findings


def selftest_modules() -> tuple[bool, str]:
    import tempfile

    failures: list[str] = []
    with tempfile.TemporaryDirectory() as td:
        root = Path(td)
        design = root / "docs/design/ftl_design.md"
        design.parent.mkdir(parents=True)
        design.write_text(
            "### 3.1 demo\n"
            "#### 3.1.1 背景与需求\n"
            "#### 3.1.2 职责与边界\n"
            "#### 3.1.3 结构与不变量\n"
            "#### 3.1.4 流程\n"
            "#### 3.1.5 失败语义\n"
            "#### 3.1.6 设计取舍\n"
            "#### 3.1.7 常量与阈值\n",
            encoding="utf-8",
        )
        if check_modules(root, None):
            failures.append("complete module flagged")
        design.write_text("### 3.1 demo\n#### 3.1.1 背景与需求\n", encoding="utf-8")
        if not any("缺模板小节" in item.message for item in check_modules(root, None)):
            failures.append("missing module section not caught")
    return (not failures, "; ".join(failures))


MODULE_RULE = Rule(
    rule_id="DOC-MODULE",
    title="模块七段模板完整性",
    kind="doc",
    check=check_modules,
    selftest=selftest_modules,
    roots=("docs",),
    suffixes=(".md",),
)
