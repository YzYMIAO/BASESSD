"""内部链接与锚点检查（所有 docs 文档）。"""

from __future__ import annotations

import re
import tempfile
from pathlib import Path

from rulecheck.docs import policy
from rulecheck.docs.model import parse_document
from rulecheck.docs.rules.layering import collect_docs
from rulecheck.registry import Finding, Rule

LINK_RX = re.compile(r"(?<!!)\[[^\]]*\]\(([^)]+)\)")
INLINE_CODE_RX = re.compile(r"`[^`]*`")
HEADING_RX = re.compile(r"^#{1,6}\s+(.+)$", re.M)
ANCHOR_RX = re.compile(r'<a\s+id="([^"]+)"')
EXTERNAL_PREFIX = ("http://", "https://", "mailto:", "tel:")


def _slug(heading: str) -> str:
    text = heading.strip().lower()
    text = re.sub(r"[^\w一-鿿\- ]", "", text)
    return text.replace(" ", "-")


def _anchors(path: Path) -> set[str]:
    text = path.read_text(encoding="utf-8", errors="ignore")
    anchors = set(ANCHOR_RX.findall(text))
    anchors.update(_slug(item) for item in HEADING_RX.findall(text))
    return anchors


def _split_target(target: str) -> tuple[str, str]:
    target = target.strip()
    if "#" not in target:
        return target, ""
    return target.split("#", 1)


def _check_file(root: Path, path: Path) -> list[Finding]:
    findings: list[Finding] = []
    try:
        rel = path.relative_to(root)
    except ValueError:
        rel = path
    if policy.category_of(str(rel)) == "archive":
        return findings  # 归档文档只读，不参与链接维护
    text = path.read_text(encoding="utf-8", errors="ignore")
    for doc_line in parse_document(text):
        if doc_line.skip:
            continue
        line = INLINE_CODE_RX.sub("", doc_line.text)
        for match in LINK_RX.finditer(line):
            target = match.group(1).strip()
            if target.startswith(EXTERNAL_PREFIX) or target.startswith(":"):
                continue
            file_part, anchor = _split_target(target)
            file_part = file_part.split(" ", 1)[0]
            target_path = (path.parent / file_part).resolve() if file_part else path
            if file_part and not target_path.exists():
                findings.append(
                    Finding(
                        path=path,
                        line=doc_line.number,
                        message=f"链接文件不存在：{target}",
                        category="link",
                    )
                )
                continue
            if anchor and target_path.is_file() and anchor not in _anchors(target_path):
                findings.append(
                    Finding(
                        path=path,
                        line=doc_line.number,
                        message=f"链接锚点不存在：{target}",
                        category="link",
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
        docs = root / "docs"
        docs.mkdir(parents=True)
        (docs / "a.md").write_text(
            '<a id="s1"></a>\n\n# A\n\n[ok](b.md#s2)\n', encoding="utf-8"
        )
        (docs / "b.md").write_text('<a id="s2"></a>\n\n# B\n', encoding="utf-8")
        if check(root, None):
            failures.append("valid link flagged")
        (docs / "a.md").write_text("[bad](missing.md)\n", encoding="utf-8")
        if not any("文件不存在" in item.message for item in check(root, None)):
            failures.append("missing file not caught")
        (docs / "a.md").write_text("[bad](b.md#nope)\n", encoding="utf-8")
        if not any("锚点不存在" in item.message for item in check(root, None)):
            failures.append("missing anchor not caught")
    return (not failures, "; ".join(failures))


RULE = Rule(
    rule_id="DOC-LINK",
    title="内部链接与锚点",
    kind="doc",
    check=check,
    selftest=selftest,
    roots=("docs",),
    suffixes=(".md",),
)
