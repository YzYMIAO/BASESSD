#!/usr/bin/env python3
"""H 规则统一入口。

这里只做三件事：选规则、展开路径、汇总 Finding。具体规则在
`tools/rulecheck/textual/`；语义规则后续接 clang-tidy，不改本入口。

用法：
    python3 tools/check_c.py --all
    python3 tools/check_c.py --rule H51
    python3 tools/check_c.py --rule H50 --rule H41 --report
    python3 tools/check_c.py --path src/ftl/ftl.c
    python3 tools/check_c.py --self-test
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from rulecheck.registry import expand_targets, print_findings, run_rules, run_selftests  # noqa: E402
from rulecheck.textual import RULES  # noqa: E402


def select_rules(ids: list[str]) -> list:
    if not ids or "all" in ids:
        return RULES
    wanted = {item.strip().upper() for raw in ids for item in raw.split(",") if item.strip()}
    selected = [rule for rule in RULES if rule.rule_id.upper() in wanted]
    unknown = wanted - {rule.rule_id.upper() for rule in selected}
    if unknown:
        raise SystemExit(f"未知规则：{', '.join(sorted(unknown))}")
    return selected


def main() -> int:
    parser = argparse.ArgumentParser(description="H 规则统一检查入口")
    parser.add_argument("--root", default=".", help="仓库根目录")
    parser.add_argument("--all", action="store_true", help="跑全部规则（默认）")
    parser.add_argument("--rule", action="append", default=[], help="只跑指定规则（可重复 / 逗号分隔）")
    parser.add_argument("--path", action="append", default=[], help="只检查指定文件或目录")
    parser.add_argument("--report", action="store_true", help="显示 warning 与规则统计")
    parser.add_argument("--strict", action="store_true", help="warning 也算失败")
    parser.add_argument("--self-test", action="store_true", help="跑各规则自检")
    parser.add_argument("--list", action="store_true", help="列出已注册规则")
    args = parser.parse_args()

    root = Path(args.root).resolve()
    if not root.is_dir():
        print(f"check-c: root 不存在：{root}", file=sys.stderr)
        return 2

    rules = select_rules([] if args.all else args.rule)
    if args.list:
        for rule in rules:
            print(f"{rule.rule_id}\t{rule.kind}\t{rule.title}")
        return 0
    if args.self_test:
        return run_selftests(rules)

    try:
        targets = expand_targets(root, args.path)
    except FileNotFoundError as error:
        print(f"check-c: 路径不存在：{error}", file=sys.stderr)
        return 2

    result = run_rules(root, rules, targets, args.report)
    print_findings(result.findings, args.report)
    errors = len(result.errors)
    warnings = len(result.warnings)
    print(f"check-c: {'OK' if errors == 0 and (not args.strict or warnings == 0) else 'FAIL'} "
          f"(rules={','.join(rule.rule_id for rule in rules)}; errors={errors}; warnings={warnings})")
    if errors > 0:
        return 1
    if args.strict and warnings > 0:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
