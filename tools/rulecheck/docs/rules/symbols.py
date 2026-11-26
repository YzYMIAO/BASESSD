"""设计文档中的裸代码符号提示（warning，不失败）。

契约级符号可放进 `tools/doc_symbols_allow.txt`；局部必要的符号用
`<!-- doc-policy: allow symbols -->` 包住。规则只报告每个文件里的首次出现，
避免同一符号刷屏。
"""

from __future__ import annotations

import re
from pathlib import Path

from rulecheck.docs import policy
from rulecheck.docs.model import parse_document
from rulecheck.docs.rules.layering import collect_docs
from rulecheck.registry import Finding, Rule

SYMBOL_RX = re.compile(r"`([a-z][a-z0-9]*_[a-z0-9_]+)`")
ALLOW_FILE = "tools/doc_symbols_allow.txt"


def _allowlist(root: Path) -> set[str]:
    path = root / ALLOW_FILE
    if not path.is_file():
        return set()
    return {
        line.strip()
        for line in path.read_text(encoding="utf-8", errors="ignore").splitlines()
        if line.strip() and not line.lstrip().startswith("#")
    }


def _check_file(root: Path, path: Path, allow: set[str]) -> list[Finding]:
    findings: list[Finding] = []
    try:
        rel = str(path.relative_to(root))
    except ValueError:
        rel = str(path)
    if policy.category_of(rel) != "design":
        return findings
    for doc_line in parse_document(path.read_text(encoding="utf-8", errors="ignore")):
        if doc_line.skip or doc_line.text.lstrip().startswith("|"):
            continue
        if "symbols" in doc_line.allowed:
            continue
        symbols = [
            match.group(1)
            for match in SYMBOL_RX.finditer(doc_line.text)
            if match.group(1) not in allow
        ]
        if len(set(symbols)) < 3:
            continue
        sample = " / ".join(sorted(set(symbols))[:3])
        findings.append(
            Finding(
                path=path,
                line=doc_line.number,
                message=f"一行 {len(set(symbols))} 个裸符号（{sample}…）；先写概念，必要时加 {ALLOW_FILE} 或段内例外",
                severity="warning",
                category="symbols",
            )
        )
    return findings


def check(root: Path, targets: list[Path] | None) -> list[Finding]:
    paths = targets if targets is not None else collect_docs(root)
    allow = _allowlist(root)
    findings: list[Finding] = []
    for path in paths:
        findings.extend(_check_file(root, path, allow))
    return findings


def selftest() -> tuple[bool, str]:
    import tempfile

    failures: list[str] = []
    with tempfile.TemporaryDirectory() as td:
        root = Path(td)
        design = root / "docs/design/ftl_design.md"
        design.parent.mkdir(parents=True)
        design.write_text("见 `l2p_flush_kick` / `md_commit_region` / `wb_handle`。\n", encoding="utf-8")
        if not check(root, None):
            failures.append("symbol density not reported")
        (root / "tools").mkdir()
        (root / ALLOW_FILE).write_text("l2p_flush_kick\nmd_commit_region\nwb_handle\n", encoding="utf-8")
        if check(root, None):
            failures.append("allowlist not honored")
        design.write_text(
            "<!-- doc-policy: allow symbols -->\n见 `wb_handle` / `md_commit_region` / `l2p_flush_kick`。\n<!-- /doc-policy -->\n",
            encoding="utf-8",
        )
        (root / ALLOW_FILE).write_text("", encoding="utf-8")
        if check(root, None):
            failures.append("doc-policy symbols not honored")
    return (not failures, "; ".join(failures))


RULE = Rule(
    rule_id="DOC-SYMBOL",
    title="设计文档裸代码符号提示",
    kind="doc",
    check=check,
    selftest=selftest,
    roots=("docs",),
    suffixes=(".md",),
)
