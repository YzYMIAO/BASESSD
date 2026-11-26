"""术语首次出现格式（warning，不失败）。

只检查一组常用术语：首次出现应写成 `English term (中文译名)`。
术语表仍是各文档附录；这里只做提示，不替代人工评审。
"""

from __future__ import annotations

import re
from pathlib import Path

from rulecheck.docs import policy
from rulecheck.docs.model import parse_document
from rulecheck.docs.rules.layering import collect_docs
from rulecheck.registry import Finding, Rule

TERMS = (
    "garbage collection",
    "wear leveling",
    "write amplification",
    "write-back",
    "program-before-map",
    "deferred write completion",
    "logical-to-physical",
    "read disturb",
    "predictive failure analysis",
    "volatile write cache",
    "read recovery",
    "over-provisioning",
    "super block",
)
BILINGUAL_RX = re.compile(r"\([^)]*[一-鿿][^)]*\)")


def _check_file(root: Path, path: Path) -> list[Finding]:
    try:
        rel = str(path.relative_to(root))
    except ValueError:
        rel = str(path)
    if policy.category_of(rel) != "design":
        return []

    findings: list[Finding] = []
    seen: set[str] = set()
    for doc_line in parse_document(path.read_text(encoding="utf-8", errors="ignore")):
        if doc_line.skip or doc_line.text.lstrip().startswith(("# ", "## ", "### ", "- [")):
            continue
        for term in TERMS:
            if term in seen:
                continue
            pattern = re.compile(rf"\b{re.escape(term)}\b", re.IGNORECASE)
            match = pattern.search(doc_line.text)
            if not match:
                continue
            seen.add(term)
            tail = doc_line.text[match.end() :]
            if not BILINGUAL_RX.match(tail.lstrip()):
                findings.append(
                    Finding(
                        path=path,
                        line=doc_line.number,
                        message=f"术语 `{match.group(0)}` 首次出现应给中文译名（English (中文)）",
                        severity="warning",
                        category="terms",
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
        design.write_text("garbage collection (垃圾回收) 是核心。\n", encoding="utf-8")
        if check(root, None):
            failures.append("bilingual first use flagged")
        design.write_text("garbage collection 是核心。\n", encoding="utf-8")
        if not check(root, None):
            failures.append("missing translation not caught")
    return (not failures, "; ".join(failures))


RULE = Rule(
    rule_id="DOC-TERM",
    title="术语首次出现格式",
    kind="doc",
    check=check,
    selftest=selftest,
    roots=("docs",),
    suffixes=(".md",),
)
