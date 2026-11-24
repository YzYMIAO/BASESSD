"""H 规则注册表：规则是数据，CLI 只负责遍历和输出。

规则分三类：
  * textual  —— 共享 Document / Item 模型，可做词法 / 条目级检查；
  * semantic —— 需要 clang AST / 编译数据库，后续单独接入；
  * manual   —— 人审判据，不做机械检查。

本模块不依赖具体 H 规则。
"""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable

Severity = str  # "error" / "warning"


@dataclass
class Finding:
    path: Path
    line: int
    message: str
    severity: Severity = "error"
    rule_id: str = ""


@dataclass
class Rule:
    rule_id: str
    title: str
    kind: str  # textual / semantic / manual
    check: Callable[[Path, list[Path] | None], list[Finding]]
    selftest: Callable[[], tuple[bool, str]] | None = None
    roots: tuple[str, ...] = ("src", "include")
    suffixes: tuple[str, ...] = (".c", ".h")


@dataclass
class RunResult:
    findings: list[Finding] = field(default_factory=list)

    @property
    def errors(self) -> list[Finding]:
        return [item for item in self.findings if item.severity == "error"]

    @property
    def warnings(self) -> list[Finding]:
        return [item for item in self.findings if item.severity == "warning"]


def expand_targets(root: Path, targets: list[str] | None) -> list[Path] | None:
    """展开 --path；None 表示规则使用自己的默认扫描范围。"""
    if not targets:
        return None
    files: list[Path] = []
    for raw in targets:
        path = Path(raw)
        if not path.is_absolute():
            path = root / path
        if path.is_dir():
            files.extend(sorted(path.rglob("*.c")))
            files.extend(sorted(path.rglob("*.h")))
        elif path.is_file():
            files.append(path)
        else:
            raise FileNotFoundError(path)
    return sorted(set(files))


def collect_default(root: Path, rule: Rule) -> list[Path]:
    files: list[Path] = []
    for name in rule.roots:
        base = root / name
        if not base.is_dir():
            continue
        for suffix in rule.suffixes:
            files.extend(sorted(base.rglob(f"*{suffix}")))
    return sorted(set(files))


def run_rules(root: Path, rules: list[Rule], targets: list[Path] | None, report: bool) -> RunResult:
    result = RunResult()
    for rule in rules:
        rule_targets = targets if targets is not None else collect_default(root, rule)
        findings = rule.check(root, rule_targets)
        for finding in findings:
            finding.rule_id = rule.rule_id
        result.findings.extend(findings)
        if report:
            print(f"[{rule.rule_id}] {rule.title}: {len(findings)} finding(s)")
    return result


def print_findings(findings: list[Finding], report: bool) -> None:
    ordered = sorted(findings, key=lambda item: (str(item.path), item.line, item.severity))
    for item in ordered:
        if item.severity == "warning" and not report:
            continue
        label = "warning" if item.severity == "warning" else "error"
        prefix = f"[{item.rule_id}] " if item.rule_id else ""
        print(f"{item.path}:{item.line}: [{label}] {prefix}{item.message}")


def run_selftests(rules: list[Rule]) -> int:
    failed = 0
    for rule in rules:
        if rule.selftest is None:
            print(f"{rule.rule_id}: no self-test")
            continue
        ok, message = rule.selftest()
        print(f"{rule.rule_id}-test: {'OK' if ok else 'FAIL'}{(' (' + message + ')') if message else ''}")
        if not ok:
            failed += 1
    return 0 if failed == 0 else 1
