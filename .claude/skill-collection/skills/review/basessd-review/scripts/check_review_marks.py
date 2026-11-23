#!/usr/bin/env python3
"""审查标记（#优化# / #决策# / ...）报告与校验。

标记是设计文档行首或 C 注释体第一段内容的普通文本，故意不包 HTML 注释：
人工审查时要能看见，C 里也只能写在 // 或 /* */ 里。

用法：
    python3 check_review_marks.py --root . --report
    python3 check_review_marks.py --root . --check
    python3 check_review_marks.py --root . --strict
    python3 check_review_marks.py --self-test

退出码：0 = 无错误；1 = 有格式/引用错误（--strict 还含非保留标记）；
        2 = 工具/输入错误。
"""

from __future__ import annotations

import argparse
import io
import re
import sys
import tempfile
from contextlib import redirect_stderr, redirect_stdout
from dataclasses import dataclass, field
from pathlib import Path

@dataclass(frozen=True)
class TagSpec:
    """本仓策略：一个标签属于哪个报告分组。"""

    group: str
    defect: bool = False


# 加标签只改这张表；TAGS / TAG_SET / RETAIN_TAG / DEFECT_TAG 都从它派生。
TAG_SPECS: dict[str, TagSpec] = {
    "#优化#": TagSpec("todo"),
    "#决策#": TagSpec("action"),
    "#讨论#": TagSpec("action"),
    "#重构#": TagSpec("todo"),
    "#未验#": TagSpec("unverified"),
    "#缺陷#": TagSpec("todo", defect=True),
    "#标识保留#": TagSpec("retained"),
}
TAGS: tuple[str, ...] = tuple(TAG_SPECS)
TAG_SET = frozenset(TAG_SPECS)
RETAIN_TAG = next(tag for tag, spec in TAG_SPECS.items() if spec.group == "retained")
DEFECT_TAG = next(tag for tag, spec in TAG_SPECS.items() if spec.defect)

LEDGER_TOKEN_RX = re.compile(r"\b(?:RISK|ISS)-[A-Z][A-Z0-9]*-\d+\b")
CJK_RX = re.compile(r"[一-鿿]")
FENCE_RX = re.compile(r"^\s*(?:```|~~~)")
HEADING_RX = re.compile(r"^(#{1,6})\s+(.*)$")

GROUP_ORDER = ("retained", "action", "unverified", "todo")
GROUP_TITLES = {
    "retained": "保留",
    "action": "需要拍板 / 讨论",
    "unverified": "待验证",
    "todo": "待做",
}


@dataclass
class Parsed:
    tags: list[str]
    desc: str
    error: str | None


@dataclass
class Mark:
    path: str
    line: int
    tags: list[str]
    desc: str
    context: str
    kind: str


@dataclass
class Issue:
    path: str
    line: int
    message: str
    severity: str


@dataclass
class ScanResult:
    marks: list[Mark] = field(default_factory=list)
    issues: list[Issue] = field(default_factory=list)
    files: int = 0


def _looks_like_unknown_tag(token: str) -> bool:
    """Catch Chinese-looking #标签 / #标签# typos without flagging #1 or #include."""
    return (
        token.startswith("#")
        and len(token) > 2
        and CJK_RX.search(token) is not None
    )


def parse_marker_text(text: str) -> Parsed:
    """Parse a Markdown line or C comment body. Return empty tags when not a marker."""
    text = text.strip()
    if not text.startswith("#"):
        return Parsed([], "", None)
    if text.startswith("# ") or text.startswith("#\t") or text.startswith("##"):
        return Parsed([], "", None)

    parts = text.split()
    if not parts:
        return Parsed([], "", None)
    if parts[0] not in TAG_SET:
        if _looks_like_unknown_tag(parts[0]):
            return Parsed([], "", f"未知标签 {parts[0]}（已知：{' '.join(TAGS)}）")
        return Parsed([], "", None)

    tags = [parts[0]]
    i = 1
    while i < len(parts) and parts[i] in TAG_SET:
        tags.append(parts[i])
        i += 1
    if i < len(parts) and _looks_like_unknown_tag(parts[i]):
        return Parsed(tags, "", f"未知标签 {parts[i]}（已知：{' '.join(TAGS)}）")

    desc = " ".join(parts[i:]).strip()
    tags = list(dict.fromkeys(tags))
    if not desc:
        return Parsed(tags, "", "标记缺少问题描述")
    return Parsed(tags, desc, None)


def group_of(tags: list[str]) -> str:
    groups = {TAG_SPECS[tag].group for tag in tags if tag in TAG_SPECS}
    for group in GROUP_ORDER:
        if group in groups:
            return group
    return "todo"


def _kind_of(path: str) -> str:
    if path.startswith("docs/design/") and path.endswith("_design.md"):
        return "doc"
    if path.startswith("src/") or path.startswith("include/"):
        return "code"
    if path.startswith("tests/"):
        return "test"
    return "other"


def _heading_context(text: str) -> str:
    text = text.strip()
    if text and text[0].isdigit():
        return "§" + text
    return text


def _fenced_flags(lines: list[str]) -> list[bool]:
    inside = False
    flags: list[bool] = []
    for line in lines:
        if FENCE_RX.match(line):
            flags.append(True)
            inside = not inside
            continue
        flags.append(inside)
    return flags


def _skip_quoted(line: str, i: int) -> int:
    quote = line[i]
    i += 1
    while i < len(line):
        if line[i] == "\\":
            i += 2
            continue
        if line[i] == quote:
            return i + 1
        i += 1
    return i


def _iter_c_comments(text: str):
    """Yield (line number, comment body) for // and /* */ comments, ignoring strings."""
    in_block = False
    for lineno, line in enumerate(text.splitlines(), 1):
        i = 0
        while i < len(line):
            if in_block:
                end = line.find("*/", i)
                if end == -1:
                    yield lineno, line[i:]
                    i = len(line)
                else:
                    yield lineno, line[i:end]
                    in_block = False
                    i = end + 2
                continue

            ch = line[i]
            if ch in "\"'":
                i = _skip_quoted(line, i)
                continue
            if line.startswith("//", i):
                yield lineno, line[i + 2 :]
                i = len(line)
                continue
            if line.startswith("/*", i):
                in_block = True
                i += 2
                continue
            i += 1


def _strip_comment_prefix(body: str) -> str:
    body = body.strip()
    if body.startswith("*"):
        body = body.lstrip("*").strip()
    return body


def scan_md_file(path: str, text: str, kind: str) -> tuple[list[Mark], list[Issue]]:
    lines = text.splitlines()
    fenced = _fenced_flags(lines)
    heading = ""
    marks: list[Mark] = []
    issues: list[Issue] = []

    for lineno, line in enumerate(lines, 1):
        if fenced[lineno - 1]:
            continue
        stripped = line.strip()
        match = HEADING_RX.match(stripped)
        if match:
            heading = _heading_context(match.group(2))
            continue

        parsed = parse_marker_text(stripped)
        if parsed.error:
            issues.append(Issue(path, lineno, parsed.error, "error"))
        elif parsed.tags:
            marks.append(Mark(path, lineno, parsed.tags, parsed.desc, heading, kind))

    return marks, issues


def scan_c_file(path: str, text: str, kind: str) -> tuple[list[Mark], list[Issue]]:
    marks: list[Mark] = []
    issues: list[Issue] = []
    for lineno, body in _iter_c_comments(text):
        body = _strip_comment_prefix(body)
        if not body:
            continue
        parsed = parse_marker_text(body)
        if parsed.error:
            issues.append(Issue(path, lineno, parsed.error, "error"))
        elif parsed.tags:
            marks.append(Mark(path, lineno, parsed.tags, parsed.desc, "", kind))
    return marks, issues


def _iter_targets(root: Path):
    docs = root / "docs" / "design"
    if docs.is_dir():
        for path in sorted(docs.glob("*_design.md")):
            if path.is_file():
                yield path

    for base in ("src", "include", "tests"):
        directory = root / base
        if not directory.is_dir():
            continue
        for path in sorted(directory.rglob("*")):
            if not path.is_file() or path.suffix not in (".c", ".h"):
                continue
            parts = set(path.relative_to(root).parts)
            if "third_party" in parts or "Drivers" in parts:
                continue
            if "CMSIS" in parts or "cmsis" in parts:
                continue
            yield path


def _load_ledger(root: Path) -> str:
    text = ""
    for rel in (
        "docs/design/known_open_items.md",
        "docs/design/known_closed_items.md",
    ):
        path = root / rel
        if path.is_file():
            text += path.read_text(encoding="utf-8", errors="ignore")
    return text


def _check_ledger(marks: list[Mark], ledger: str) -> list[Issue]:
    issues: list[Issue] = []
    for mark in marks:
        tokens = LEDGER_TOKEN_RX.findall(mark.desc)
        if mark.kind == "doc":
            if DEFECT_TAG in mark.tags and not tokens:
                issues.append(
                    Issue(
                        mark.path,
                        mark.line,
                        "文档中的 #缺陷# 缺 RISK-*/ISS-* 台账编号",
                        "error",
                    )
                )
            for token in tokens:
                if token not in ledger:
                    issues.append(
                        Issue(mark.path, mark.line, f"台账里找不到 {token}", "error")
                    )
        elif tokens:
            issues.append(
                Issue(
                    mark.path,
                    mark.line,
                    f"代码/测试注释里出现 {', '.join(tokens)}；风险映射只在台账",
                    "warning",
                )
            )
    return issues


def scan_root(root: Path) -> ScanResult:
    result = ScanResult()
    for path in _iter_targets(root):
        rel = path.relative_to(root).as_posix()
        kind = _kind_of(rel)
        try:
            text = path.read_text(encoding="utf-8", errors="ignore")
        except OSError as exc:
            result.issues.append(Issue(rel, 0, f"读取失败：{exc}", "error"))
            continue
        result.files += 1
        if kind == "doc":
            marks, issues = scan_md_file(rel, text, kind)
        else:
            marks, issues = scan_c_file(rel, text, kind)
        result.marks.extend(marks)
        result.issues.extend(issues)

    result.issues.extend(_check_ledger(result.marks, _load_ledger(root)))
    return result


def render_report(result: ScanResult) -> None:
    marks = sorted(result.marks, key=lambda m: (m.path, m.line))
    groups: dict[str, list[Mark]] = {key: [] for key in GROUP_ORDER}
    for mark in marks:
        groups[group_of(mark.tags)].append(mark)

    retained = len(groups["retained"])
    open_count = len(marks) - retained
    print(
        f"review-marks: {len(marks)} marks in {result.files} files "
        f"({retained} retained, {open_count} open)"
    )

    for key in GROUP_ORDER:
        group = groups[key]
        if not group:
            continue
        print()
        print(f"{GROUP_TITLES[key]} ({len(group)}):")
        for mark in group:
            context = f"  {mark.context}" if mark.context else ""
            print(f"  {mark.path}:{mark.line}{context}")
            print(f"    {' '.join(mark.tags)} {mark.desc}")

    counts = {tag: 0 for tag in TAGS}
    for mark in marks:
        for tag in mark.tags:
            if tag in counts:
                counts[tag] += 1
    print()
    print("by tag: " + " ".join(f"{tag}={counts[tag]}" for tag in TAGS))


def _print_issues(issues: list[Issue]) -> bool:
    errors = sorted(
        (issue for issue in issues if issue.severity == "error"),
        key=lambda issue: (issue.path, issue.line),
    )
    warnings = sorted(
        (issue for issue in issues if issue.severity == "warning"),
        key=lambda issue: (issue.path, issue.line),
    )

    if errors:
        print("review-marks: FAIL", file=sys.stderr)
        for issue in errors:
            print(
                f"  - {issue.path}:{issue.line}: {issue.message}",
                file=sys.stderr,
            )
    if warnings:
        print("review-marks: WARN", file=sys.stderr)
        for issue in warnings:
            print(
                f"  - {issue.path}:{issue.line}: {issue.message}",
                file=sys.stderr,
            )
    return bool(errors)


def _scan_fixture(files: dict[str, str]) -> ScanResult:
    with tempfile.TemporaryDirectory() as td:
        root = Path(td)
        for rel, text in files.items():
            path = root / rel
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text, encoding="utf-8")
        return scan_root(root)


def self_test() -> int:
    failures: list[str] = []
    cases = 0

    def check(name: str, ok: bool, detail: str = "") -> None:
        nonlocal cases
        cases += 1
        if not ok:
            suffix = f": {detail}" if detail else ""
            failures.append(f"{name}{suffix}")

    def errors(result: ScanResult) -> list[Issue]:
        return [issue for issue in result.issues if issue.severity == "error"]

    def warnings(result: ScanResult) -> list[Issue]:
        return [issue for issue in result.issues if issue.severity == "warning"]

    result = _scan_fixture(
        {
            "docs/design/ftl_design.md": (
                "### 3.2 分配器 / BST\n\n"
                "#优化# #讨论# 低 free 时成对分配失败是否应该回滚\n"
            )
        }
    )
    check(
        "Markdown 正例",
        len(result.marks) == 1
        and result.marks[0].tags == ["#优化#", "#讨论#"]
        and not errors(result),
    )
    check(
        "Markdown 标题上下文",
        bool(result.marks) and "3.2 分配器" in result.marks[0].context,
    )

    result = _scan_fixture({"src/a.c": "// #优化# 这里可以提前退出\n"})
    check("C 行注释", len(result.marks) == 1 and not errors(result))

    result = _scan_fixture({"src/a.c": "/*\n * #重构# 收敛坐标\n */\n"})
    check("C 块注释", len(result.marks) == 1 and not errors(result))

    result = _scan_fixture({"src/a.c": "foo(); // #优化# 重算\n"})
    check("C 行尾注释", len(result.marks) == 1 and not errors(result))

    result = _scan_fixture({"src/a.c": "/** #优化# 文档注释里的标记 */\n"})
    check("C /** 注释", len(result.marks) == 1 and not errors(result))

    result = _scan_fixture({"docs/design/ftl_design.md": "#优化 缺少闭合\n"})
    check("未知标签", bool(errors(result)))

    result = _scan_fixture({"docs/design/ftl_design.md": "#优化#\n"})
    check("空描述", bool(errors(result)))

    result = _scan_fixture(
        {"docs/design/ftl_design.md": "```\n#优化# 示例\n```\n"}
    )
    check("围栏代码块跳过", not result.marks and not errors(result))

    result = _scan_fixture(
        {"src/a.c": 'const char *s = "#优化# not a comment";\n'}
    )
    check("字符串跳过", not result.marks and not errors(result))

    result = _scan_fixture({"docs/design/ftl_design.md": "#缺陷# 问题\n"})
    check("文档缺陷缺台账", bool(errors(result)))

    result = _scan_fixture(
        {
            "docs/design/ftl_design.md": "#缺陷# RISK-FTL-01 问题\n",
            "docs/design/known_open_items.md": "| RISK-FTL-01 | 风险 | 信息 | x | y |\n",
        }
    )
    check("文档缺陷有台账", not errors(result))

    result = _scan_fixture({"src/a.c": "// #缺陷# 问题\n"})
    check(
        "代码缺陷不要求台账",
        not errors(result) and not warnings(result),
    )

    result = _scan_fixture({"src/a.c": "// #缺陷# RISK-FTL-01 问题\n"})
    check(
        "代码缺陷带 RISK 告警",
        bool(warnings(result)) and not errors(result),
    )

    result = _scan_fixture(
        {"docs/design/ftl_design.md": "#优化# #标识保留# 等下一版\n"}
    )
    check(
        "保留分组",
        len(result.marks) == 1 and group_of(result.marks[0].tags) == "retained",
    )
    check("strict 待做", group_of(["#优化#"]) == "todo")
    check("strict 保留", group_of(["#优化#", "#标识保留#"]) == "retained")
    check("group action", group_of(["#优化#", "#讨论#"]) == "action")
    check("group unverified", group_of(["#优化#", "#未验#"]) == "unverified")
    check(
        "group priority",
        group_of(["#优化#", "#未验#", "#讨论#", "#标识保留#"]) == "retained",
    )

    with tempfile.TemporaryDirectory() as td:
        root = Path(td)
        (root / "docs/design").mkdir(parents=True)
        (root / "docs/design/ftl_design.md").write_text(
            "#优化# 待做\n", encoding="utf-8"
        )
        with redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
            code = main(["--root", str(root), "--strict"])
        check("strict 非保留退出码", code == 1)

        (root / "docs/design/ftl_design.md").write_text(
            "#优化# #标识保留# 暂缓\n", encoding="utf-8"
        )
        with redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
            code = main(["--root", str(root), "--strict"])
        check("strict 保留退出码", code == 0)

    result = _scan_fixture({"docs/design/ftl_design.md": "### 3.2 分配器\n"})
    check("标题不是标记", not result.marks and not errors(result))

    result = _scan_fixture(
        {"docs/design/ftl_design.md": "#优化# #讨论 描述\n"}
    )
    check("后续未知标签", bool(errors(result)))

    result = _scan_fixture(
        {
            "docs/design/ftl_design.md": "#缺陷# RISK-FTL-99 问题\n",
            "docs/design/known_open_items.md": "| RISK-FTL-01 | 风险 | 信息 | x | y |\n",
        }
    )
    check("台账找不到 ID", bool(errors(result)))

    result = _scan_fixture(
        {"docs/design/ftl_design.md": "#优化# #优化# 描述\n"}
    )
    check(
        "重复标签去重",
        len(result.marks) == 1 and result.marks[0].tags == ["#优化#"],
    )

    if failures:
        print("review-marks-test: FAIL", file=sys.stderr)
        for failure in failures:
            print(f"  - {failure}", file=sys.stderr)
        return 1

    print(f"review-marks-test: OK ({cases} cases)")
    return 0


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="审查标记报告/校验")
    parser.add_argument("--root", default=".", help="仓库根目录")
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--report", action="store_true", help="打印分组报告")
    group.add_argument("--check", action="store_true", help="只校验格式与引用")
    group.add_argument("--strict", action="store_true", help="非保留标记即失败")
    group.add_argument("--self-test", action="store_true", help="检查器自检")
    args = parser.parse_args(argv)

    if args.self_test:
        return self_test()

    root = Path(args.root).resolve()
    try:
        result = scan_root(root)
    except OSError as exc:
        print(f"review-marks: ERROR: {exc}", file=sys.stderr)
        return 2

    if args.report:
        render_report(result)
        return 1 if _print_issues(result.issues) else 0

    if args.check:
        failed = _print_issues(result.issues)
        if not result.issues:
            print(
                f"review-marks: OK ({result.files} files, "
                f"{len(result.marks)} marks)"
            )
        return 1 if failed else 0

    render_report(result)
    failed = _print_issues(result.issues)
    if failed:
        return 1

    non_retained = [mark for mark in result.marks if RETAIN_TAG not in mark.tags]
    if non_retained:
        print(
            f"review-marks: FAIL ({len(non_retained)} non-retained)",
            file=sys.stderr,
        )
        return 1

    retained = len(result.marks) - len(non_retained)
    print(f"review-marks: OK ({retained} retained)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
