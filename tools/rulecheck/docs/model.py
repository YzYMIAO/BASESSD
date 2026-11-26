"""Markdown 行模型：围栏代码、doc-policy 例外、评审标记。

所有文档规则共用这一层，避免每条规则各自解析 Markdown。
"""

from __future__ import annotations

import re
from dataclasses import dataclass

MARKER_RX = re.compile(r"<!--\s*doc-policy:\s*allow\s+([a-z-]+)\s*-->")
END_MARKER = "<!-- /doc-policy -->"

# Review marks carry deliberate code/ledger references; they are doc-local
# review notes, not implementation coordinates. Tag validity is another
# checker's job, so this model only skips the line and its continuations.
REVIEW_MARK_RX = re.compile(r"^\s*//\s*#[^#\s]+#")


@dataclass(frozen=True)
class DocLine:
    number: int
    text: str
    fenced: bool = False
    skip: bool = False
    allowed: frozenset[str] = frozenset()


def parse_document(text: str) -> list[DocLine]:
    """按行解析；保持与旧 check_docs 相同的例外语义。

    - 围栏代码块内的行不参与规则；
    - `doc-policy: allow <check>` 之后、`/doc-policy` 之前的行免该检查；
    - 行首 `// #...#` 评审标记及其 `\\` 续行跳过。
    """
    lines = text.splitlines()
    result: list[DocLine] = []
    inside_fence = False
    allowed: set[str] = set()
    review_cont = False

    for number, line in enumerate(lines, 1):
        if review_cont:
            review_cont = line.rstrip().endswith("\\")
            result.append(DocLine(number, line, skip=True))
            continue

        marker = MARKER_RX.search(line)
        if marker:
            allowed.add(marker.group(1))
            result.append(DocLine(number, line, skip=True))
            continue
        if END_MARKER in line:
            allowed.clear()
            result.append(DocLine(number, line, skip=True))
            continue

        stripped = line.lstrip()
        if stripped.startswith("```"):
            result.append(DocLine(number, line, fenced=True, skip=True))
            inside_fence = not inside_fence
            continue
        if inside_fence:
            result.append(DocLine(number, line, fenced=True, skip=True))
            continue

        if REVIEW_MARK_RX.match(line):
            review_cont = line.rstrip().endswith("\\")
            result.append(DocLine(number, line, skip=True))
            continue

        result.append(DocLine(number, line, allowed=frozenset(allowed)))

    return result
