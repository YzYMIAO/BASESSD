"""H41 头文件路径 / 裸名规则。

H41 的两条可机械判据：
  * 项目头 include 必须是裸文件名，不能带目录前缀；
  * include / src 下不得有同名私有头（同一裸名两套语义）。
"""

from __future__ import annotations

import re
import tempfile
from collections import defaultdict
from pathlib import Path

from rulecheck.registry import Finding, Rule

_INCLUDE_RE = re.compile(r'^\s*#\s*include\s*"([^"]*/[^"]*)"')


def _default_files(root: Path) -> list[Path]:
    files: list[Path] = []
    for name in ("src", "include", "tests"):
        base = root / name
        if base.is_dir():
            files.extend(sorted(base.rglob("*.c")))
            files.extend(sorted(base.rglob("*.h")))
    return files


def check_files(files: list[Path]) -> list[Finding]:
    findings: list[Finding] = []
    by_name: dict[str, list[Path]] = defaultdict(list)

    for path in files:
        if path.suffix == ".h":
            by_name[path.name].append(path)
        try:
            lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
        except OSError as error:
            findings.append(Finding(path=path, line=1, message=f"读取失败：{error}"))
            continue
        for line_no, line in enumerate(lines, start=1):
            match = _INCLUDE_RE.match(line)
            if match is not None:
                findings.append(
                    Finding(
                        path=path,
                        line=line_no,
                        message=f"项目头 include 必须是裸文件名（H41）：{match.group(1)}",
                    )
                )

    for name, paths in sorted(by_name.items()):
        if len(paths) > 1:
            joined = "、".join(str(path) for path in paths)
            findings.append(
                Finding(path=paths[1], line=1, message=f"同名私有头 `{name}`：{joined}（H41）")
            )
    return findings


def check(root: Path, targets: list[Path] | None) -> list[Finding]:
    return check_files(targets if targets is not None else _default_files(root))


def selftest() -> tuple[bool, str]:
    with tempfile.TemporaryDirectory(prefix="h41_") as tmp:
        root = Path(tmp)
        (root / "src").mkdir()
        (root / "include").mkdir()
        good = root / "include" / "a.h"
        good.write_text('#include "b.h"\n', encoding="utf-8")
        clean = check_files([good])
        if clean:
            return (False, "good case failed")

        bad = root / "src" / "bad.c"
        bad.write_text('#include "sub/a.h"\n', encoding="utf-8")
        bad_findings = check_files([bad])
        if not any("裸文件名" in item.message for item in bad_findings):
            return (False, "directory-prefixed include not detected")

        (root / "src" / "a.h").write_text("", encoding="utf-8")
        duplicate = check_files([good, root / "src" / "a.h"])
        if not any("同名私有头" in item.message for item in duplicate):
            return (False, "duplicate basename not detected")
    return (True, "3 cases")


RULE = Rule(
    rule_id="H41",
    title="头文件裸名与 basename 唯一",
    kind="textual",
    check=check,
    selftest=selftest,
    roots=("src", "include", "tests"),
)
