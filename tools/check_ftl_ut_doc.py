#!/usr/bin/env python3
"""正向不变量检查：docs/design/ftl_ut_design.md 的模块化结构仍成立。

文档按被测模块组织，用例清单归测试源、风险状态归 known_open_items / known_closed_items、覆盖判定归各层 UT 设计文档 §7.2。本 checker 只守本文的结构契约：

  1. 必需的章标题与 §2.1~§2.17 模块小节都在；
  2. 正文引用的每个 RISK-* 都在 docs/design/known_open_items.md 或 known_closed_items.md 里有登记；
  3. 没有已废弃口径（旧快照数、旧用例表说法）；
  4. 没有手工维护的全局用例表（首列直接是用例名的表格行）；
  5. 没有 §9 运行快照；
  6. 负例套件路径齐全（pure / media 的 gc、l2p、meta、boot、if）；
  7. 测试源注册的每个 FTL 用例名在文档里至少出现一次（模块表 / 场景表 / 负例列）。

判据是正向不变量：从两个台账重建合法 RISK 集合、从测试源重建用例名集合，
再与文档比对，不是「旧词 grep = 0」。`--self-test` 先构造必然失败的输入，确认每条
检查都有区分力（C7），再宣称通过。
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

DOC = ROOT / "docs" / "design" / "ftl_ut_design.md"
KNOWN_RISKS = [
    ROOT / "docs" / "design" / "known_open_items.md",
    ROOT / "docs" / "design" / "known_closed_items.md",
]

REQUIRED_HEADINGS = [
    "## 0. 文档定位与单一事实源",
    "## 1. 测试项与测试分层",
    "## 2. 按模块的测试设计",
    "## 3. 跨模块 / 系统级场景",
    "## 4. 测试技术、夹具与 seam",
    "## 5. 构建、运行与证据",
    "## 6. 不覆盖与判据盲区",
    "## 7. 风险追溯（副轴）",
    "## 8. 维护规则",
]

MODULE_HEADINGS = [f"### 2.{i}" for i in range(1, 18)]

REQUIRED_SUITES = [
    "/ftl/negative/pure/",
    "/ftl/negative/media/gc/",
    "/ftl/negative/media/l2p/",
    "/ftl/negative/media/meta/",
    "/ftl/negative/media/boot/",
    "/ftl/negative/media/if/",
]

STALE_PHRASES = [
    "95/95",
    "BOARD/OUT",
    "用例名与参数以 §4 表为准",
    "§4.1~§4.6",
    "2026-11-11",
    "最近验收快照",
]

TEST_FILES = [
    ROOT / "tests" / "unit" / "test_ftl_pure.c",
    ROOT / "tests" / "unit" / "test_ftl_media.c",
    ROOT / "tests" / "unit" / "test_negative_pure.c",
    ROOT / "tests" / "unit" / "test_negative_media.c",
    ROOT / "tests" / "unit" / "test_speed.c",
]

RISK_RE = re.compile(r"RISK-[A-Z0-9]+-\d+")
CASE_ROW_RE = re.compile(r"^\|\s*`([^`]+)`\s*\|")


def read_text(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8")
    except OSError as exc:
        print(f"ERROR: 无法读取 {path}: {exc}", file=sys.stderr)
        return ""


def parse_registered_names(paths: list[Path]) -> set[str]:
    """从各 `MunitTest xxx[] = { ... };` 注册数组提取用例名。"""
    names: set[str] = set()
    for path in paths:
        text = read_text(path)
        for match in re.finditer(r"MunitTest\s+\w+\s*\[\s*\]\s*=\s*\{", text):
            end = text.find("};", match.end())
            if end == -1:
                continue
            for line in text[match.end() : end].splitlines():
                stripped = line.strip()
                if not stripped.startswith("{") or re.match(r"\{\s*NULL", stripped):
                    continue
                literal = re.search(r'"((?:[^"\\]|\\.)*)"', stripped)
                if literal is not None:
                    names.add(literal.group(1))
    return names


def check_headings(text: str) -> list[str]:
    errors: list[str] = []
    for heading in REQUIRED_HEADINGS:
        if not any(line.startswith(heading) for line in text.splitlines()):
            errors.append(f"缺少必需章节「{heading}」")
    for heading in MODULE_HEADINGS:
        if not any(line.startswith(heading) for line in text.splitlines()):
            errors.append(f"缺少模块小节「{heading}」")
    return errors


def check_risks(text: str, known_text: str) -> list[str]:
    known = set(RISK_RE.findall(known_text))
    if not known:
        return ["台账（known_open_items / known_closed_items）里找不到任何 RISK-* 登记"]

    errors: list[str] = []
    for lineno, line in enumerate(text.splitlines(), start=1):
        for risk_id in RISK_RE.findall(line):
            if risk_id not in known:
                errors.append(f"第 {lineno} 行引用未登记风险 {risk_id}")
    return errors


def check_stale(text: str) -> list[str]:
    errors: list[str] = []
    for lineno, line in enumerate(text.splitlines(), start=1):
        for phrase in STALE_PHRASES:
            if phrase in line:
                errors.append(f"第 {lineno} 行含废弃口径「{phrase}」")
    return errors


def check_no_case_tables(text: str, registered: set[str]) -> list[str]:
    errors: list[str] = []
    for lineno, line in enumerate(text.splitlines(), start=1):
        match = CASE_ROW_RE.match(line)
        if match is None:
            continue
        cell = match.group(1)
        if cell.startswith("RISK-"):
            continue
        if cell in registered:
            errors.append(f"第 {lineno} 行手工维护全局用例表「{cell}」")
    return errors


def check_no_snapshot(text: str) -> list[str]:
    errors: list[str] = []
    for lineno, line in enumerate(text.splitlines(), start=1):
        if line.startswith("## 9."):
            errors.append(f"第 {lineno} 行仍有 §9 运行快照")
    return errors


def check_negative_suites(text: str) -> list[str]:
    errors: list[str] = []
    for suite in REQUIRED_SUITES:
        if suite not in text:
            errors.append(f"缺少负例套件路径「{suite}」")
    return errors


def check_case_coverage(text: str, registered: set[str]) -> list[str]:
    errors: list[str] = []
    for name in sorted(registered):
        if name not in text:
            errors.append(f"已注册用例「{name}」未在文档里出现")
    return errors


def run_checks(text: str, known_text: str, registered: set[str]) -> list[str]:
    errors: list[str] = []
    errors.extend(check_headings(text))
    errors.extend(check_risks(text, known_text))
    errors.extend(check_stale(text))
    errors.extend(check_no_case_tables(text, registered))
    errors.extend(check_no_snapshot(text))
    errors.extend(check_negative_suites(text))
    errors.extend(check_case_coverage(text, registered))
    return errors


def self_test() -> int:
    skeleton = "\n".join(REQUIRED_HEADINGS + MODULE_HEADINGS) + "\n"
    skeleton += "\n".join(REQUIRED_SUITES) + "\n"
    known = "RISK-UT-42\n"
    registered = {"alloc_free_pool_invariants"}
    skeleton += "代表用例 `alloc_free_pool_invariants`。\n"

    cases = [
        ("缺章节", skeleton.replace(REQUIRED_HEADINGS[0] + "\n", "", 1), known),
        ("缺模块小节", skeleton.replace("### 2.17\n", "", 1), known),
        ("未登记风险", skeleton + "引用 RISK-ZZ-99\n", known),
        ("废弃口径", skeleton + "旧快照 95/95\n", known),
        ("全局用例表", skeleton + "| `alloc_free_pool_invariants` | x |\n", known),
        ("运行快照", skeleton + "## 9. 最近验收快照\n", known),
        ("缺负例套件", skeleton.replace("/ftl/negative/media/if/\n", "", 1), known),
        ("用例漏覆盖", skeleton.replace("代表用例 `alloc_free_pool_invariants`。\n", "", 1), known),
    ]

    failed = False
    for label, text, known_text in cases:
        errors = run_checks(text, known_text, registered)
        if not errors:
            print(f"self-test FAIL: {label} 未被检出", file=sys.stderr)
            failed = True

    valid = skeleton + "| `RISK-UT-42` | `alloc_free_pool_invariants` | x |\n"
    if run_checks(valid, known, registered):
        print("self-test FAIL: 合法骨架被误报", file=sys.stderr)
        failed = True

    if failed:
        return 1
    print("ftl_ut_doc: self-test OK")
    return 0


def main() -> int:
    if len(sys.argv) > 1:
        if sys.argv[1] == "--self-test":
            return self_test()
        print(f"用法: {sys.argv[0]} [--self-test]", file=sys.stderr)
        return 2

    text = read_text(DOC)
    known_text = "\n".join(read_text(path) for path in KNOWN_RISKS)
    registered = parse_registered_names(TEST_FILES)
    errors = run_checks(text, known_text, registered)

    if errors:
        for err in errors:
            print(f"ERROR: {err}", file=sys.stderr)
        print(f"ftl_ut_doc: FAIL ({len(errors)} 项)", file=sys.stderr)
        return 1

    print(f"ftl_ut_doc: OK ({len(registered)} 个注册用例)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
