#!/usr/bin/env python3
"""Generate compile_commands.json from this project's Makefile.

Why not bear: bear's wrapper uses a local gRPC channel and fails in some
sandboxed/WSL setups ("Connection reset by peer"). This project's compile
rules are one-line `gcc ... -c SRC -o OBJ`, so a forced dry-run
(`make -B -n V=1 all`) is enough to recover the exact command lines without
touching the build tree.

Only C sources are recorded (assembly entries are not useful for clangd).
The output is deterministic (sorted by file, first command wins).
"""

from __future__ import annotations

import argparse
import json
import shlex
import subprocess
import sys
from pathlib import Path


def compiler_include_flags(cross: str) -> list[str]:
    """Implicit GCC/newlib include paths, needed by clangd/clang-tidy.

    The compile command records only explicit flags; GCC adds its own
    builtin include and the newlib include dir implicitly. Clang does not,
    so its `inttypes.h`/`stdint.h` wrappers fail with `include_next`.
    These flags are written to compile_commands.json only (tools), never
    used by the firmware build.
    """
    try:
        gcc_inc = subprocess.check_output(
            [f"{cross}gcc", "-print-file-name=include"], text=True
        ).strip()
        libc = subprocess.check_output(
            [f"{cross}gcc", "-print-file-name=libc.a"], text=True
        ).strip()
    except (OSError, subprocess.CalledProcessError):
        return []

    flags: list[str] = []
    for path in (Path(gcc_inc), Path(libc).parent.parent / "include"):
        if path.is_absolute() and path.is_dir():
            flags += ["-isystem", str(path)]
    return flags


def main() -> int:
    parser = argparse.ArgumentParser(description="generate compile_commands.json")
    parser.add_argument("--root", type=Path, default=Path("."), help="project root")
    parser.add_argument("--output", type=Path, default=None, help="output path")
    parser.add_argument("--cross", default="arm-none-eabi-", help="toolchain prefix")
    args = parser.parse_args()

    root = args.root.resolve()
    output = args.output or (root / "compile_commands.json")
    extra_flags = compiler_include_flags(args.cross)

    proc = subprocess.run(
        ["make", "-B", "-n", "V=1", "all"],
        cwd=root,
        capture_output=True,
        text=True,
        check=False,
    )
    if proc.returncode != 0:
        print("compile-commands: make -n failed", file=sys.stderr)
        print(proc.stdout + proc.stderr, file=sys.stderr)
        return 2

    entries: dict[str, dict[str, str]] = {}
    for line in proc.stdout.splitlines():
        if " -c " not in line or " -o " not in line:
            continue
        try:
            argv = shlex.split(line.strip())
            src = Path(argv[argv.index("-c") + 1])
        except (ValueError, IndexError):
            continue
        if src.suffix != ".c":
            continue
        src = src.resolve()
        command = line.strip()
        if extra_flags:
            command += " " + " ".join(extra_flags)
        entries.setdefault(
            str(src),
            {
                "directory": str(root),
                "file": str(src),
                "command": command,
            },
        )

    if not entries:
        print("compile-commands: no C compile commands found", file=sys.stderr)
        return 2

    output.write_text(
        json.dumps([entries[key] for key in sorted(entries)], indent=2) + "\n",
        encoding="utf-8",
    )
    print(f"compile-commands: {len(entries)} entries -> {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
