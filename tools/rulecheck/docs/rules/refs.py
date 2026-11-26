"""设计文档的规则/流程引用：CLAUDE.md 与括号形式的规则编号。"""

from __future__ import annotations

import re
from pathlib import Path

from rulecheck.docs import policy
from rulecheck.docs.model import parse_document
from rulecheck.docs.rules.layering import collect_docs
from rulecheck.registry import Finding, Rule

CLAUDE_RX = re.compile(r"\bCLAUDE\.md\b")
RULE_ID_RX = re.compile(r"[（(]([HRCP]\d{1,2})[）)]")


def _check_file(root: Path, path: Path) -> list[Finding]:
    findings: list[Finding] = []
    try:
        rel = str(path.relative_to(root))
    except ValueError:
        rel = str(path)
    if policy.category_of(rel) != "design":
        return findings
    for doc_line in parse_document(path.read_text(encoding="utf-8", errors="ignore")):
        if doc_line.skip:
            continue
        if CLAUDE_RX.search(doc_line.text):
            findings.append(
                Finding(path=path, line=doc_line.number, message=doc_line.text.strip(), severity="warning", category="refs")
            )
            continue
        match = RULE_ID_RX.search(doc_line.text)
        if match:
            findings.append(
                Finding(
                    path=path,
                    line=doc_line.number,
                    message=f"规则编号 {match.group(1)}：{doc_line.text.strip()}",
                    severity="warning",
                    category="refs",
                )
            )
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
        design.write_text("按规则（H6）处理。\n", encoding="utf-8")
        if not any("H6" in item.message for item in check(root, None)):
            failures.append("rule id not caught")
        design.write_text("见 CLAUDE.md 的分层。\n", encoding="utf-8")
        if not any("CLAUDE.md" in item.message for item in check(root, None)):
            failures.append("CLAUDE.md not caught")
        design.write_text("C1/C2 是图里的命令分组，不是规则编号。\n", encoding="utf-8")
        if check(root, None):
            failures.append("non-parenthesized label flagged")
    return (not failures, "; ".join(failures))


RULE = Rule(
    rule_id="DOC-REF",
    title="设计文档禁止规则/CLAUDE.md 引用",
    kind="doc",
    check=check,
    selftest=selftest,
    roots=("docs",),
    suffixes=(".md",),
)
