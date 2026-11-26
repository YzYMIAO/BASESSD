"""docs-check 的负例自检（C7 / P13）。"""

from __future__ import annotations

import sys
import tempfile
from pathlib import Path

from rulecheck.docs.rules.layering import check


def _check_root(root: Path):
    return check(root, None)


def selftest() -> tuple[bool, str]:
    failures: list[str] = []

    def expect(root: Path, path: str, text: str, needle: str) -> None:
        p = root / path
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(text, encoding="utf-8")
        got = _check_root(root)
        if not any(needle in item.category for item in got):
            failures.append(f"negative case not caught: {needle} ({path})")

    with tempfile.TemporaryDirectory() as td:
        root = Path(td)
        design = "docs/design/ftl_design.md"
        (root / "docs/design").mkdir(parents=True)

        # Clean design doc passes.
        (root / design).write_text("# FTL design\n\n只描述不变量与数据流。\n", encoding="utf-8")
        if _check_root(root):
            failures.append("clean design fixture rejected")

        # Review mark lines are exempt: their code/ledger refs are deliberate.
        (root / design).write_text(
            "// #优化# RISK-FTL-01 见 src/ftl/ftl.c 这里要改\n", encoding="utf-8"
        )
        if _check_root(root):
            failures.append("review mark line was flagged")

        # The \ continuation belongs to the mark and is exempt too.
        (root / design).write_text(
            "// #优化# 见 src/ftl/ftl.c 这里要改\\\n"
            "// 还有 RISK-FTL-01 与 src/ftl/ftl.c\n",
            encoding="utf-8",
        )
        if _check_root(root):
            failures.append("review mark continuation was flagged")

        expect(root, design, "使用 STM32H743 的 DTCM。\n", "platform")
        expect(root, design, "见 `src/ftl/ftl.c`。\n", "code")
        # Only line-start // marks are exempt; a tag inside prose is still checked.
        expect(root, design, "文本 #优化# 见 src/ftl/ftl.c。\n", "code")
        # A bare mark without // is not exempt.
        expect(root, design, "#优化# RISK-FTL-01 见 src/ftl/ftl.c。\n", "code")
        expect(root, design, "2026-11-22 实测 318/318。\n", "temporal")
        expect(root, design, "工作区占用 128 KB。\n", "size")
        expect(root, design, "见 RISK-TOP-01 与 docs/archive/foo.md。\n", "ledger")

        # Marker suppresses the corresponding check.
        (root / design).write_text(
            "<!-- doc-policy: allow platform -->\nSTM32H743 / DTCM\n<!-- /doc-policy -->\n",
            encoding="utf-8",
        )
        if _check_root(root):
            failures.append("platform marker did not suppress")

        # Platform doc may contain platform terms.
        platform = "docs/design/platform_design.md"
        (root / platform).write_text("STM32H743 DTCM\n", encoding="utf-8")
        if any(item.category == "platform" for item in _check_root(root)):
            failures.append("platform doc flagged for platform terms")

    from rulecheck.docs.rules import glossary, links, refs, symbols, template, terms

    for module in (glossary, links, refs, symbols, template, terms):
        ok, message = module.selftest()
        if not ok:
            failures.append(f"{module.__name__.rsplit('.', 1)[-1]} rule: {message}")

    ok, message = template.selftest_modules()
    if not ok:
        failures.append(f"module template rule: {message}")

    if failures:
        for failure in failures:
            print(f"docs-test: FAIL: {failure}", file=sys.stderr)
        return False, "; ".join(failures)
    return True, ""


def run() -> int:
    ok, _ = selftest()
    if not ok:
        return 1
    print("docs-test: OK")
    return 0
