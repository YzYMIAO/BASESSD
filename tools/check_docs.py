#!/usr/bin/env python3
"""文档规则检查 CLI。

规则在 `tools/rulecheck/docs/`：文档解析、策略表与各规则模块分开；
本文件只负责参数、输出与退出码。

Usage:
    python3 tools/check_docs.py --root .
    python3 tools/check_docs.py --root . --report
    python3 tools/check_docs.py --root . --self-test
    python3 tools/check_docs.py --root . --rule DOC-LAYER

Exit code 0 = clean, 1 = violations, 2 = tool/input error.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from rulecheck.docs import RULES  # noqa: E402
from rulecheck.docs import policy, selftest  # noqa: E402
from rulecheck.docs.rules.layering import collect_docs, scan_file  # noqa: E402
from rulecheck.registry import run_rules  # noqa: E402


def _relative(root: Path, path: Path) -> str:
    try:
        return str(path.relative_to(root))
    except ValueError:
        return str(path)


def report(root: Path) -> int:
    counts: dict[str, dict[str, int]] = {}
    for path in collect_docs(root):
        rel = _relative(root, path)
        if not policy.checks_for(rel):
            continue
        counts.setdefault(rel, {label: 0 for label in policy.CHECK_LABEL})
        for finding in scan_file(root, path):
            counts[rel][finding.category] += 1

    if not counts:
        print("docs-report: no classified docs")
        return 0

    print(f"{'document':50s} " + " ".join(f"{k:>12s}" for k in policy.CHECK_LABEL))
    for path, row in sorted(counts.items()):
        total = sum(row.values())
        if total == 0:
            continue
        print(f"{path:50s} " + " ".join(f"{row[k]:12d}" for k in policy.CHECK_LABEL))

    from rulecheck.registry import run_rules

    findings = run_rules(root, RULES, None, False).findings
    if findings:
        by_rule: dict[str, list[int]] = {}
        for item in findings:
            row = by_rule.setdefault(item.rule_id, [0, 0])
            row[0 if item.severity == "warning" else 1] += 1
        print(f"\n{'rule':24s} {'warning':>8s} {'error':>8s}")
        for rule_id, (warn, err) in sorted(by_rule.items()):
            print(f"{rule_id:24s} {warn:8d} {err:8d}")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="Documentation rule checker")
    parser.add_argument("--root", default=".", help="repository root")
    parser.add_argument("--report", action="store_true", help="print per-doc counts and exit 0")
    parser.add_argument("--self-test", action="store_true", help="run discrimination self-test")
    parser.add_argument("--rule", action="append", default=[], help="run only the given rule id")
    parser.add_argument("--path", action="append", default=[], help="run only on the given doc/directory (repeatable)")
    parser.add_argument("--strict", action="store_true", help="treat warnings as failures")
    args = parser.parse_args()

    if args.self_test:
        return selftest.run()

    root = Path(args.root).resolve()
    if not (root / "docs").is_dir():
        print(f"docs-check: not a repository root: {root}", file=sys.stderr)
        return 2

    if args.report:
        return report(root)

    targets = None
    if args.path:
        targets = []
        for raw in args.path:
            path = (root / raw).resolve()
            if path.is_dir():
                targets.extend(sorted(path.rglob("*.md")))
            elif path.is_file():
                targets.append(path)
            else:
                print(f"docs-check: path not found: {raw}", file=sys.stderr)
                return 2

    rules = RULES
    if args.rule:
        wanted = set(args.rule)
        rules = [rule for rule in RULES if rule.rule_id in wanted]
        missing = wanted - {rule.rule_id for rule in rules}
        if missing:
            print(f"docs-check: unknown rule(s): {sorted(missing)}", file=sys.stderr)
            return 2

    findings = run_rules(root, rules, targets, False).findings
    errors = [item for item in findings if item.severity != "warning"]
    warnings = [item for item in findings if item.severity == "warning"]
    shown = errors + (warnings if args.strict else [])
    if shown:
        for finding in sorted(shown, key=lambda item: (str(item.path), item.line)):
            label = policy.CHECK_LABEL.get(finding.category, finding.category)
            print(
                f"docs-check: {_relative(root, finding.path)}:{finding.line}: "
                f"[{label}] {finding.message.strip()}",
                file=sys.stderr,
            )
        print(f"docs-check: {len(shown)} violation(s)", file=sys.stderr)
        return 1

    print(f"docs-check: OK files={len(targets) if targets is not None else len(collect_docs(root))}")
    if warnings:
        print(f"docs-check: {len(warnings)} warning(s) (use --report / --strict)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
