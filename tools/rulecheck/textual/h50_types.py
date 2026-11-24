"""H50 复合类型命名 / 定义形态规则。

复合类型扫描在 `rulecheck.model`；本文件保留 H50 的命名 / 形态判据，
并把结果转换成统一 Finding。
"""

from __future__ import annotations

import re
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path

from rulecheck.model import CompositeDefinition, CompositeForward, scan_composites
from rulecheck.registry import Finding, Rule

KIND_SUFFIX = {"struct": "S", "enum": "E", "union": "U"}
ALIAS_SUFFIX = {"struct": "s", "enum": "e", "union": "u"}


@dataclass
class Report:
    definitions: list[CompositeDefinition] = field(default_factory=list)
    forwards: list[CompositeForward] = field(default_factory=list)
    errors: list[tuple[Path, int, str]] = field(default_factory=list)


def scan_files(root: Path, files: list[Path]) -> Report:
    """扫描一批文件；词法与复合类型解析全部走共享 model。"""
    report = Report()
    for path in files:
        if path.suffix not in {".c", ".h"}:
            continue
        scan = scan_composites(path, root)
        report.definitions.extend(scan.definitions)
        report.forwards.extend(scan.forwards)
        report.errors.extend((path, line, message) for line, message in scan.errors)
    return report


def validate(report: Report) -> None:
    """检查 H50 命名 / typedef 形态 / 跨文件前置声明一致性。"""
    defined_tags = {d.tag for d in report.definitions if d.tag}
    forward_tags = {f.tag for f in report.forwards}
    tag_kind = {d.tag: d.kind for d in report.definitions if d.tag}

    alias_owner: dict[str, tuple[str, str, int]] = {}
    tag_owner: dict[str, tuple[str, str, int]] = {}

    for definition in report.definitions:
        where = f"{definition.file}:{definition.line}"
        if definition.anonymous:
            report.errors.append(
                (
                    Path(definition.file),
                    definition.line,
                    f"anonymous {definition.kind} typedef has no tag (H50: add TAG_{KIND_SUFFIX[definition.kind]})",
                )
            )
            continue
        assert definition.tag is not None
        expected_tag = "_" + KIND_SUFFIX[definition.kind]
        if not re.fullmatch(r"[A-Z][A-Z0-9_]*_[SEU]", definition.tag) or not definition.tag.endswith(expected_tag):
            report.errors.append(
                (Path(definition.file), definition.line, f"tag `{definition.tag}` must be uppercase and end with `{expected_tag}`")
            )
        if definition.typedef_form:
            if definition.alias is None:
                report.errors.append(
                    (Path(definition.file), definition.line, f"{definition.kind} `{definition.tag}` definition has no typedef alias")
                )
            else:
                expected_alias = "_" + ALIAS_SUFFIX[definition.kind]
                if not re.fullmatch(r"[a-z][a-z0-9_]*_[seu]", definition.alias) or not definition.alias.endswith(expected_alias):
                    report.errors.append(
                        (
                            Path(definition.file),
                            definition.line,
                            f"alias `{definition.alias}` must be lowercase and end with `{expected_alias}`",
                        )
                    )
                if definition.alias == definition.tag:
                    report.errors.append(
                        (Path(definition.file), definition.line, f"tag and alias are identical (`{definition.tag}`)")
                    )
                if definition.alias in alias_owner and alias_owner[definition.alias][0] != definition.tag:
                    prev = alias_owner[definition.alias]
                    report.errors.append(
                        (
                            Path(definition.file),
                            definition.line,
                            f"alias `{definition.alias}` already denotes `{prev[0]}` at {prev[1]}:{prev[2]}",
                        )
                    )
                alias_owner[definition.alias] = (definition.tag, definition.file, definition.line)
        else:
            if definition.alias is not None:
                report.errors.append(
                    (
                        Path(definition.file),
                        definition.line,
                        f"bare `{definition.kind} {definition.tag} {{...}} {definition.alias};` is a variable declaration; use the typedef form",
                    )
                )
            is_c = definition.file.endswith(".c")
            if not is_c:
                report.errors.append(
                    (
                        Path(definition.file),
                        definition.line,
                        f"bare `{definition.kind} {definition.tag} {{...}}` in a header; use `typedef {definition.kind} {definition.tag} {{...}} alias;`",
                    )
                )
            elif definition.tag not in forward_tags:
                report.errors.append(
                    (
                        Path(definition.file),
                        definition.line,
                        f"bare `{definition.kind} {definition.tag} {{...}}` in .c without a forward typedef (not an opaque type)",
                    )
                )

        if definition.tag in tag_owner:
            prev = tag_owner[definition.tag]
            report.errors.append(
                (Path(definition.file), definition.line, f"tag `{definition.tag}` defined more than once (first at {prev[1]}:{prev[2]})")
            )
        tag_owner[definition.tag] = (definition.tag, definition.file, definition.line)

    forward_alias_owner: dict[str, str] = {}
    for forward in report.forwards:
        if forward.tag not in defined_tags:
            report.errors.append(
                (Path(forward.file), forward.line, f"forward typedef `{forward.tag}` has no definition in the scanned tree")
            )
        expected_tag = "_" + KIND_SUFFIX[forward.kind]
        expected_alias = "_" + ALIAS_SUFFIX[forward.kind]
        if not re.fullmatch(r"[A-Z][A-Z0-9_]*_[SEU]", forward.tag) or not forward.tag.endswith(expected_tag):
            report.errors.append(
                (Path(forward.file), forward.line, f"forward tag `{forward.tag}` must be uppercase and end with `{expected_tag}`")
            )
        if not re.fullmatch(r"[a-z][a-z0-9_]*_[seu]", forward.alias) or not forward.alias.endswith(expected_alias):
            report.errors.append(
                (Path(forward.file), forward.line, f"forward alias `{forward.alias}` must be lowercase and end with `{expected_alias}`")
            )
        if forward.tag in tag_kind and tag_kind[forward.tag] != forward.kind:
            report.errors.append(
                (
                    Path(forward.file),
                    forward.line,
                    f"forward `{forward.kind} {forward.tag}` conflicts with its definition kind `{tag_kind[forward.tag]}`",
                )
            )
        owner = alias_owner.get(forward.alias)
        if owner is not None and owner[0] != forward.tag:
            report.errors.append(
                (
                    Path(forward.file),
                    forward.line,
                    f"forward alias `{forward.alias}` already denotes `{owner[0]}` at {owner[1]}:{owner[2]}",
                )
            )
        prev = forward_alias_owner.get(forward.alias)
        if prev is not None and prev != forward.tag:
            report.errors.append(
                (Path(forward.file), forward.line, f"forward alias `{forward.alias}` already denotes `{prev}`")
            )
        forward_alias_owner[forward.alias] = forward.tag


def check(root: Path, targets: list[Path] | None) -> list[Finding]:
    report = scan_files(root, targets or [])
    validate(report)
    return [
        Finding(path=path, line=line, message=message, severity="error")
        for path, line, message in report.errors
    ]


def selftest() -> tuple[bool, str]:
    script = Path(__file__).resolve().parents[2] / "test_check_types.py"
    proc = subprocess.run([sys.executable, str(script)], capture_output=True, text=True, check=False)
    return (proc.returncode == 0, proc.stdout.strip().splitlines()[-1] if proc.stdout.strip() else "no output")


RULE = Rule(
    rule_id="H50",
    title="复合类型 / 类型别名命名与定义形态",
    kind="textual",
    check=check,
    selftest=selftest,
    roots=("src", "include"),
)
