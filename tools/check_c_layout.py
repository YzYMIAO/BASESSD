#!/usr/bin/env python3
"""H51 兼容入口。

H51 已迁到 `rulecheck.textual.h51_layout` / `rulecheck.model`；本文件只保留
旧 CLI 形状，供 make layout-check / layout-test 和历史命令使用。
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from rulecheck.textual.h51_layout import (  # noqa: E402
    check_file,
    print_result,
    run_self_test,
    source_files,
)


def main() -> int:
    parser = argparse.ArgumentParser(description="H51 文件内布局检查")
    parser.add_argument("--root", default=".", help="仓库根目录")
    parser.add_argument(
        "--path",
        action="append",
        default=[],
        help="只检查指定文件或目录（可重复）",
    )
    parser.add_argument("--report", action="store_true", help="输出条目清单与 warning")
    parser.add_argument("--inventory", action="store_true", help="打印解析出的条目")
    parser.add_argument("--self-test", action="store_true", help="运行内置自检")
    args = parser.parse_args()

    if args.self_test:
        return run_self_test()

    root = Path(args.root).resolve()
    if not root.is_dir():
        print(f"layout-check: 根目录不存在：{root}", file=sys.stderr)
        return 2

    targets: list[Path] = []
    for raw in args.path:
        target = Path(raw)
        if not target.is_absolute():
            target = root / target
        if target.is_dir():
            targets.extend(sorted(target.rglob("*.c")))
            targets.extend(sorted(target.rglob("*.h")))
        elif target.is_file():
            targets.append(target)
        else:
            print(f"layout-check: 路径不存在：{target}", file=sys.stderr)
            return 2
    if not targets:
        targets = source_files(root)

    failed = 0
    for path in targets:
        result = check_file(path)
        if result.violations:
            failed += 1
        print_result(result, args.report or args.inventory)
        if args.inventory:
            for item in result.items:
                print(f"  {item.describe()}")

    if failed:
        print(f"layout-check: FAIL ({failed} file(s))")
        return 1
    print("layout-check: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
