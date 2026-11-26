"""术语表覆盖检查：设计文档的术语节必须覆盖核心缩写。"""

from __future__ import annotations

import re
import tempfile
from pathlib import Path

from rulecheck.docs import policy
from rulecheck.docs.rules.layering import collect_docs
from rulecheck.registry import Finding, Rule

TERMS = ("L2P", "PBA", "PAA", "META", "SB", "BBT", "GC", "WL", "CP", "VWC")


def _glossary_section(text: str) -> str | None:
    lines = text.splitlines()
    start = None
    level = 0
    for i, line in enumerate(lines):
        if line.startswith("#") and ("术语速查" in line or "术语表" in line):
            level = len(line) - len(line.lstrip("#"))
            start = i + 1
            break
    if start is None:
        return None
    body = []
    for line in lines[start:]:
        if line.startswith("#"):
            this_level = len(line) - len(line.lstrip("#"))
            if this_level <= level:
                break
        body.append(line)
    return "\n".join(body)


def _check_file(root: Path, path: Path) -> list[Finding]:
    try:
        rel = str(path.relative_to(root))
    except ValueError:
        rel = str(path)
    if policy.category_of(rel) != "design":
        return []
    section = _glossary_section(path.read_text(encoding="utf-8", errors="ignore"))
    if section is None:
        return []
    findings: list[Finding] = []
    for term in TERMS:
        if not re.search(rf"\b{re.escape(term)}\b", section):
            findings.append(
                Finding(
                    path=path,
                    line=1,
                    message=f"术语表缺少 `{term}`",
                    severity="warning",
                    category="glossary",
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
    failures: list[str] = []
    with tempfile.TemporaryDirectory() as td:
        root = Path(td)
        design = root / "docs/design/ftl_design.md"
        design.parent.mkdir(parents=True)
        design.write_text("### 8.D 术语速查\n\n" + "\n".join(TERMS) + "\n", encoding="utf-8")
        if check(root, None):
            failures.append("complete glossary flagged")
        design.write_text("### 8.D 术语速查\n\nL2P PBA PAA META SB BBT GC WL CP\n", encoding="utf-8")
        if not any("VWC" in item.message for item in check(root, None)):
            failures.append("missing glossary term not caught")
    return (not failures, "; ".join(failures))


RULE = Rule(
    rule_id="DOC-GLOSSARY",
    title="术语表核心缩写覆盖",
    kind="doc",
    check=check,
    selftest=selftest,
    roots=("docs",),
    suffixes=(".md",),
)
