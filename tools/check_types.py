#!/usr/bin/env python3
"""H50 兼容入口。

H50 的解析器已迁到 `rulecheck.model`，命名 / 形态判据在
`rulecheck.textual.h50_types`。本文件只保留旧 CLI 形状：

    python3 tools/check_types.py --root .
    python3 tools/check_types.py --root . --include-tests

退出码：0 = 干净，1 = 有违规，2 = 输入错误。
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from rulecheck.textual.h50_types import scan_files, validate  # noqa: E402


def collect_files(root: Path, include_tests: bool) -> list[Path]:
    roots = [root / "include", root / "src"]
    if include_tests:
        roots.append(root / "tests")
    files: list[Path] = []
    for base in roots:
        if not base.exists():
            continue
        for path in base.rglob("*"):
            if not path.is_file() or path.suffix not in {".h", ".c"}:
                continue
            rel = path.relative_to(root)
            if any(part in {"third_party", "build", "cmsis", "hal"} for part in rel.parts):
                continue
            files.append(path)
    return sorted(files)


def main() -> int:
    parser = argparse.ArgumentParser(description="H50 复合类型定义 / 命名检查")
    parser.add_argument("--root", type=Path, default=Path("."), help="项目根目录")
    parser.add_argument("--include-tests", action="store_true", help="同时扫描 tests/（默认关闭）")
    args = parser.parse_args()
    root = args.root.resolve()

    files = collect_files(root, args.include_tests)
    if not files:
        print("types-check: no source files found", file=sys.stderr)
        return 2

    report = scan_files(root, files)
    validate(report)
    if report.errors:
        for path, line, message in report.errors:
            print(f"{path}:{line}: {message}", file=sys.stderr)
        print(f"types-check: FAILED ({len(report.errors)} violation(s))", file=sys.stderr)
        return 1
    print("types-check: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
