#!/usr/bin/env python3
"""Mermaid 语法校验（可选，依赖 mmdc）。

从 docs/ 下抽取 ```mermaid 代码块，逐个交给 Mermaid CLI 渲染到临时 SVG。
mmdc 不存在时打印 SKIP 并返回 0，不进入默认 docs-check。

Usage:
    python3 tools/check_mermaid.py --root .
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


def iter_blocks(path: Path):
    inside = False
    start = 0
    lines: list[str] = []
    for number, line in enumerate(path.read_text(encoding="utf-8", errors="ignore").splitlines(), 1):
        stripped = line.strip()
        if not inside and stripped.startswith("```mermaid"):
            inside = True
            start = number
            lines = []
            continue
        if inside and stripped == "```":
            yield start, "\n".join(lines) + "\n"
            inside = False
            continue
        if inside:
            lines.append(line)


def main() -> int:
    parser = argparse.ArgumentParser(description="Mermaid diagram syntax checker")
    parser.add_argument("--root", default=".", help="repository root")
    args = parser.parse_args()

    mmdc = shutil.which("mmdc")
    if mmdc is None:
        print("docs-mermaid: SKIP (mmdc not found; install @mermaid-js/mermaid-cli to enable)")
        return 0

    root = Path(args.root).resolve()
    docs = root / "docs"
    failures = 0
    with tempfile.TemporaryDirectory() as td:
        out = Path(td) / "out.svg"
        for path in sorted(docs.rglob("*.md")):
            for line, body in iter_blocks(path):
                source = Path(td) / "in.mmd"
                source.write_text(body, encoding="utf-8")
                result = subprocess.run(
                    [mmdc, "-i", str(source), "-o", str(out)],
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    timeout=60,
                )
                if result.returncode != 0:
                    failures += 1
                    print(f"docs-mermaid: {path.relative_to(root)}:{line}: render failed", file=sys.stderr)
                    detail = result.stderr.decode("utf-8", errors="replace").strip().splitlines()
                    if detail:
                        print(f"  {detail[-1]}", file=sys.stderr)

    if failures:
        print(f"docs-mermaid: {failures} failure(s)", file=sys.stderr)
        return 1
    print("docs-mermaid: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
