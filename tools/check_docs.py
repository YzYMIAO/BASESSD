#!/usr/bin/env python3
"""Documentation layering checker (doc policy positive invariant).

The repository separates documentation by stability:

  design       platform-neutral contract: requirements, invariants, data flow,
               state machines, policies, trade-offs.  Must not embed the current
               implementation (SoC/memory domains, file/function/global names,
               dates, versions, test snapshots, risk/archive IDs).
  platform     current hardware/SoC/linker mapping. Platform details are expected.
  verification dated test design/results. Dates, throughput and counts are expected.
  ledger       known_open_items RISK/ISS ledger. IDs and dates are expected.
  revision     revision_history. History is expected.
  archive      docs/archive. Never checked.

Rules are path-based (see POLICY). Unclassified docs are ignored, so the policy
can be expanded one document at a time during the review.

Explicit allowances inside a design doc:

    <!-- doc-policy: allow platform -->
    ... platform note ...
    <!-- /doc-policy -->

    <!-- doc-policy: allow verification -->
    ... dated measurement / test snapshot ...
    <!-- /doc-policy -->

Struct/enum/union definitions, field semantics and key constant macros are
design content and are not flagged. File/function/global-symbol coordinates and
memory footprints are.

Usage:
    python3 tools/check_docs.py --root .
    python3 tools/check_docs.py --root . --report
    python3 tools/check_docs.py --self-test

Exit code 0 = clean, 1 = violations, 2 = tool/input error.
"""

from __future__ import annotations

import argparse
import fnmatch
import re
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

# ---------------------------------------------------------------------------
# Policy
# ---------------------------------------------------------------------------

POLICY: dict[str, dict[str, object]] = {
    "design": {
        "paths": [
            "docs/design/ftl_design.md",
            "docs/design/hil_design.md",
            "docs/design/fc_nand_design.md",
            "docs/architecture.md",
        ],
        "checks": ["platform", "code", "temporal", "size", "ledger"],
    },
    "platform": {
        "paths": [
            "docs/hardware_contract.md",
            "docs/boot_flow.md",
            "docs/design/platform_design.md",
            "docs/design/controller_target_features.md",
            "docs/design/usb_msc.md",
            "docs/design/common/fault_capture.md",
            "docs/design/common/timer.md",
            "docs/design/common/cli.md",
            "docs/design/common/logging.md",
        ],
        # Platform docs are expected to be implementation-specific.
        "checks": [],
    },
    "verification": {
        "paths": [
            "docs/guides/testing_guide.md",
            "docs/design/ftl_ut_design.md",
            "docs/design/hil_ut_design.md",
            "docs/design/fc_nand_ut.md",
            "docs/design/common/munit_adapter.md",
        ],
        # Verification docs are expected to carry dates/results/risk IDs.
        "checks": [],
    },
    "ledger": {
        "paths": ["docs/design/known_open_items.md"],
        "checks": [],
    },
    "revision": {
        "paths": ["docs/design/revision_history/*.md"],
        "checks": [],
    },
    "archive": {
        "paths": ["docs/archive/**"],
        "checks": [],
    },
}

# Public contract headers are allowed in design docs: they are the API, not an
# implementation coordinate.
CODE_ALLOW_RX = re.compile(r"\bsizeof\s*\(")

PUBLIC_HEADER_RX = re.compile(
    r"include/(?:ftl/ftl_interface|fc/fc_interface|nand/nand_interface|hil/controller)\.h"
)

PLATFORM_RX = re.compile(
    r"\b(?:STM32H743|Cortex-M7|ITCM|DTCM|AXI_RAM|AXI_NOCACHE|SRAM[0-9]|"
    r"FMC|HAL_[A-Za-z0-9_]+|GPIO[A-Z]?|USART[0-9]?|NOLOAD|\.noinit|\.bss|"
    r"\.dtcm_data|\.sram[0-9]_data|DTCM_DATA|SRAM[0-9]_DATA)\b"
    r"|链接脚本|内存域|linker script",
    re.IGNORECASE,
)

CODE_RX = re.compile(
    r"\b(?:src|include)/[A-Za-z0-9_./-]+"
    r"|`[a-z_][a-z0-9_]{2,}\s*\("
    r"|\b[a-z_][a-z0-9_]{2,}\(\)"
    r"|\b[gs]_[a-z0-9_]{2,}\b"
    r"|\.(?:c|h):\d+"
)

TEMPORAL_RX = re.compile(
    r"20\d{2}-\d{2}-\d{2}"
    r"|\bv\d+\.\d+\b"
    r"|built\s+(?:Jan|Feb|Mar|Apr|May|Jun|Jul|Aug|Sep|Oct|Nov|Dec)"
    r"|KB/s|MB/s|MiB/s"
    r"|\bseed\b"
    r"|\d+/\d+\s*(?:passed|通过|tests successful)"
    r"|H2testw|CrystalDiskMark"
    r"|\d{2}:\d{2}.*镜像|镜像.*\d{2}:\d{2}"
)

SIZE_RX = re.compile(r"\b\d[\d,]*\s*(?:KB|KiB|MB|MiB)\b")
SIZE_CONTEXT_RX = re.compile(r"内存|占用|对象|结构体|workspace|实测|合计")
LEDGER_RX = re.compile(
    r"RISK-[A-Z]+-\d+|ISS-[A-Z]+-\d+|docs/archive|revision_history"
)

MARKER_RX = re.compile(r"<!--\s*doc-policy:\s*allow\s+([a-z-]+)\s*-->")
END_MARKER = "<!-- /doc-policy -->"

CHECK_LABEL = {
    "platform": "平台/内存域",
    "code": "实现坐标",
    "temporal": "日期/版本/测试快照",
    "size": "内存占用大小",
    "ledger": "风险/归档引用",
}


@dataclass
class Violation:
    path: str
    line: int
    category: str
    text: str


def _matches(path: str, pattern: str) -> bool:
    return fnmatch.fnmatch(path, pattern)


def category_of(path: str) -> str | None:
    for category, cfg in POLICY.items():
        for pattern in cfg["paths"]:  # type: ignore[index]
            if _matches(path, pattern):
                return category
    return None


def checks_for(path: str) -> set[str]:
    category = category_of(path)
    if category is None:
        return set()
    return set(POLICY[category]["checks"])  # type: ignore[index]


def _strip_fenced_blocks(lines: list[str]) -> list[bool]:
    """Return a per-line flag: True if the line is inside a fenced code block."""
    inside = False
    flags: list[bool] = []
    for line in lines:
        stripped = line.lstrip()
        if stripped.startswith("```"):
            flags.append(True)
            inside = not inside
            continue
        flags.append(inside)
    return flags


def scan_file(root: Path, path: str) -> list[Violation]:
    checks = checks_for(path)
    if not checks:
        return []

    full = root / path
    if not full.exists():
        return []

    lines = full.read_text(encoding="utf-8", errors="ignore").splitlines()
    fenced = _strip_fenced_blocks(lines)

    allowed: set[str] = set()
    violations: list[Violation] = []

    def active(check: str) -> bool:
        return check in checks and check not in allowed

    for idx, line in enumerate(lines, 1):
        marker = MARKER_RX.search(line)
        if marker:
            allowed.add(marker.group(1))
            continue
        if END_MARKER in line:
            allowed.clear()
            continue
        if fenced[idx - 1]:
            continue

        if active("platform") and PLATFORM_RX.search(line):
            violations.append(Violation(path, idx, "platform", line))
            continue
        if active("code"):
            code_line = PUBLIC_HEADER_RX.sub("", line)
            code_line = CODE_ALLOW_RX.sub("", code_line)
            if CODE_RX.search(code_line):
                violations.append(Violation(path, idx, "code", line))
                continue
        if active("temporal") and TEMPORAL_RX.search(line):
            violations.append(Violation(path, idx, "temporal", line))
            continue
        if active("size") and SIZE_RX.search(line) and SIZE_CONTEXT_RX.search(line):
            violations.append(Violation(path, idx, "size", line))
            continue
        if active("ledger") and LEDGER_RX.search(line):
            violations.append(Violation(path, idx, "ledger", line))

    return violations


def iter_docs(root: Path) -> list[str]:
    docs = root / "docs"
    if not docs.is_dir():
        return []
    return sorted(
        str(p.relative_to(root))
        for p in docs.rglob("*.md")
        if p.is_file()
    )


def check_root(root: Path) -> list[Violation]:
    violations: list[Violation] = []
    for path in iter_docs(root):
        violations.extend(scan_file(root, path))
    return violations


def report(root: Path) -> int:
    counts: dict[str, dict[str, int]] = {}
    for path in iter_docs(root):
        category = category_of(path)
        checks = checks_for(path)
        if category is None or not checks:
            continue
        counts.setdefault(path, {k: 0 for k in CHECK_LABEL})
        for v in scan_file(root, path):
            counts[path][v.category] += 1

    if not counts:
        print("docs-report: no classified docs")
        return 0

    print(f"{'document':50s} " + " ".join(f"{k:>12s}" for k in CHECK_LABEL))
    for path, row in sorted(counts.items()):
        total = sum(row.values())
        if total == 0:
            continue
        print(f"{path:50s} " + " ".join(f"{row[k]:12d}" for k in CHECK_LABEL))
    return 0


def self_test() -> int:
    """C7/P13: every rule must reject a negative fixture; markers must suppress."""
    failures: list[str] = []

    def expect(root: Path, path: str, text: str, needle: str) -> None:
        p = root / path
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(text, encoding="utf-8")
        got = check_root(root)
        if not any(needle in v.category for v in got):
            failures.append(f"negative case not caught: {needle} ({path})")

    with tempfile.TemporaryDirectory() as td:
        root = Path(td)
        design = "docs/design/ftl_design.md"
        (root / "docs/design").mkdir(parents=True)

        # Clean design doc passes.
        (root / design).write_text("# FTL design\n\n只描述不变量与数据流。\n", encoding="utf-8")
        if check_root(root):
            failures.append("clean design fixture rejected")

        expect(root, design, "使用 STM32H743 的 DTCM。\n", "platform")
        expect(root, design, "见 `src/ftl/ftl.c`。\n", "code")
        expect(root, design, "2026-11-22 实测 318/318。\n", "temporal")
        expect(root, design, "工作区占用 128 KB。\n", "size")
        expect(root, design, "见 RISK-TOP-01 与 docs/archive/foo.md。\n", "ledger")

        # Marker suppresses the corresponding check.
        (root / design).write_text(
            "<!-- doc-policy: allow platform -->\nSTM32H743 / DTCM\n<!-- /doc-policy -->\n",
            encoding="utf-8",
        )
        if check_root(root):
            failures.append("platform marker did not suppress")

        # Platform doc may contain platform terms.
        platform = "docs/design/platform_design.md"
        (root / platform).write_text("STM32H743 DTCM\n", encoding="utf-8")
        if any(v.category == "platform" for v in check_root(root)):
            failures.append("platform doc flagged for platform terms")

        # Report mode must not fail.
        if report(root) != 0:
            failures.append("report mode failed")

    if failures:
        for f in failures:
            print(f"docs-test: FAIL: {f}", file=sys.stderr)
        return 1

    print("docs-test: OK")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="Documentation layering checker")
    parser.add_argument("--root", default=".", help="repository root")
    parser.add_argument("--report", action="store_true", help="print per-doc counts and exit 0")
    parser.add_argument("--self-test", action="store_true", help="run discrimination self-test")
    args = parser.parse_args()

    if args.self_test:
        return self_test()

    root = Path(args.root).resolve()
    if not (root / "docs").is_dir():
        print(f"docs-check: not a repository root: {root}", file=sys.stderr)
        return 2

    if args.report:
        return report(root)

    violations = check_root(root)
    if violations:
        for v in violations:
            print(
                f"docs-check: {v.path}:{v.line}: [{CHECK_LABEL[v.category]}] {v.text.strip()}",
                file=sys.stderr,
            )
        print(f"docs-check: {len(violations)} violation(s)", file=sys.stderr)
        return 1

    print(f"docs-check: OK files={len(iter_docs(root))}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
