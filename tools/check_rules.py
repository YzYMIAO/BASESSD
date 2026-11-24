#!/usr/bin/env python3
"""CLAUDE.md / rules / skills 分层一致性检查（正向不变量）。

四层架构：
  CLAUDE.md              常驻判据（R/C/P）
  .claude/rules/*.md     路径作用域规则（H/S + 领域约束），必须带 paths:
  .claude/skills/*       按需 skill（统一 basessd- 前缀，ASCII kebab-case）
  docs/                  知识

检查项：
  1. CLAUDE.md 体积预算（硬上限 220 行 / 28,000 字节；目标 200 行 / 25 KB）；
  2. CLAUDE.md 行首定义 R1~R12 / C1~C12 / P0~P14 各恰好一次；
  3. .claude/rules/c-coding.md 行首定义 H1~H50 / S1~S6 各恰好一次；
  4. docs/guides/coding_rules.md 中 ### H1~H50 各恰好一次；
  5. .claude/rules/*.md 必须有 YAML paths:；
  6. skill 目录名 = frontmatter name，统一 basessd- 前缀，ASCII kebab-case，
     有 description、必需小节，< 500 行；
  7. 实际 skill / symlink 集合 == EXPECTED_SKILLS；skill-map 完整且无未知名；
  8. SKILL.md 的 docs/... / .claude/... 引用存在；
  9. 关键路径存在；
  10. --self-test 覆盖上述每条负例（C7）。

用法：
  python3 tools/check_rules.py --root . --check
  python3 tools/check_rules.py --root . --self-test

退出码：0 = 一致；1 = 违规；2 = 工具/输入错误。
"""

from __future__ import annotations

import argparse
import re
import sys
import tempfile
from pathlib import Path

ROOT_LINES_HARD = 220
ROOT_BYTES_HARD = 28_000
ROOT_LINES_TARGET = 200
ROOT_BYTES_TARGET = 25_000
SKILL_MAX_LINES = 500
SKILL_PREFIX = "basessd-"
# Claude Code / VS Code: skill name == directory name, lowercase/digits/hyphens only.
SKILL_NAME_RX = re.compile(r"^[a-z0-9-]+$")
REQUIRED_SKILL_SECTIONS = ("## 产物格式", "## 常见失败", "## 完成")
# Concrete docs/... and .claude/... references; skip globs/placeholders.
SKILL_REF_RX = re.compile(
    r"(?<![\w./-])((?:docs|\.claude)/[A-Za-z0-9_./-]*(?:\.[A-Za-z0-9]+|/))"
)

EXPECTED_ROOT = {
    "R": set(range(1, 13)),
    "C": set(range(1, 13)),
    "P": set(range(0, 15)),
}
EXPECTED_C_CODING = {
    "H": set(range(1, 51)),
    "S": set(range(1, 7)),
}
EXPECTED_SKILLS = {
    "basessd-project",
    "basessd-design",
    "basessd-verify",
    "basessd-maintain",
    "basessd-release",
    "basessd-design-arch",
    "basessd-verify-board",
    "basessd-develop-fc",
    "basessd-maintain-fault",
    "basessd-develop-ftl",
    "basessd-develop-hil",
    "basessd-develop-platform",
    "basessd-review",
}
EXPECTED_RULES = {"c-coding", "fc-nand", "ftl", "hil", "platform"}

KEY_PATHS = [
    "docs/design/known_open_items.md",
    "docs/design/known_closed_items.md",
    "docs/design/common/assertion_guidelines.md",
    ".claude/review/STATE.md",
    "docs/guides/coding_rules.md",
    "docs/guides/coding_rules_generic.md",
    ".claude/skill-collection/README.md",
    ".claude/skill-collection/docs/skill-map.md",
    ".claude/rules/c-coding.md",
    ".claude/rules/ftl.md",
    ".claude/rules/fc-nand.md",
    ".claude/rules/hil.md",
    ".claude/rules/platform.md",
]

DEF_RE = re.compile(r"^\*\*(R|C|H|P|S)(\d+) ")
SECTION_RE = re.compile(r"^### H(\d+) ")


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def frontmatter(text: str) -> dict[str, str] | None:
    """极简 YAML frontmatter：只抽 name/description，并记录是否有 paths。"""
    lines = text.splitlines()
    if not lines or lines[0].strip() != "---":
        return None
    try:
        end = next(i for i in range(1, len(lines)) if lines[i].strip() == "---")
    except StopIteration:
        return None

    data: dict[str, str] = {}
    for line in lines[1:end]:
        if line.startswith("name:"):
            data["name"] = line.split(":", 1)[1].strip()
        elif line.startswith("description:"):
            data["description"] = line.split(":", 1)[1].strip()
        elif line.startswith("paths:"):
            data["paths"] = "yes"
    return data


def definitions(text: str, kind: str) -> list[int]:
    result: list[int] = []
    for line in text.splitlines():
        match = DEF_RE.match(line)
        if match and match.group(1) == kind:
            result.append(int(match.group(2)))
    return result


def check_expected(
    text: str, kind: str, expected: set[int], where: str
) -> list[str]:
    found = definitions(text, kind)
    errors: list[str] = []
    missing = sorted(expected - set(found))
    extra = sorted(set(found) - expected)
    duplicates = sorted({value for value in found if found.count(value) > 1})
    if missing:
        errors.append(f"{where}: 缺少 {kind} 规则 {missing}")
    if extra:
        errors.append(f"{where}: 多出未定义编号 {kind}{extra}")
    if duplicates:
        errors.append(f"{where}: {kind} 规则重复 {duplicates}")
    return errors


def check_budget(text: str, where: str) -> list[str]:
    lines = len(text.splitlines())
    size = len(text.encode("utf-8"))
    errors: list[str] = []
    if lines > ROOT_LINES_HARD:
        errors.append(
            f"{where}: {lines} 行 > 硬上限 {ROOT_LINES_HARD}；细则移 rules/skill"
        )
    if size > ROOT_BYTES_HARD:
        errors.append(
            f"{where}: {size} 字节 > 硬上限 {ROOT_BYTES_HARD}；细则移 rules/skill"
        )
    return errors


def check_paths(root: Path) -> list[str]:
    return [f"缺少关键路径 {rel}" for rel in KEY_PATHS if not (root / rel).is_file()]


def check_skill_set(present: set[str], where: str = "skill 集合") -> list[str]:
    """实际 skill 集合必须与 EXPECTED_SKILLS 完全一致。"""
    errors: list[str] = []
    missing = sorted(EXPECTED_SKILLS - present)
    extra = sorted(present - EXPECTED_SKILLS)
    if missing:
        errors.append(f"{where}: 缺少 {missing}")
    if extra:
        errors.append(f"{where}: 多余 {extra}")
    return errors


def check_skill_links(root: Path) -> list[str]:
    """`.claude/skills/<name>` 必须是指向 collection 的 symlink，且目标有 SKILL.md。"""
    links_dir = root / ".claude" / "skills"
    if not links_dir.is_dir():
        return [".claude/skills/: 目录不存在"]
    present = {
        path.name
        for path in links_dir.iterdir()
        if not path.name.startswith(".")
    }
    errors = check_skill_set(present, ".claude/skills")
    for name in sorted(EXPECTED_SKILLS & present):
        link = links_dir / name
        if not link.is_symlink():
            errors.append(f".claude/skills/{name}: 应为 symlink")
        elif not (link / "SKILL.md").is_file():
            errors.append(f".claude/skills/{name}: symlink 目标缺 SKILL.md")
    return errors


def check_no_codex(root: Path) -> list[str]:
    """`.codex` 已合并进 `.claude`；活跃规则 / skill 不得再引用它。"""
    paths = [
        root / "CLAUDE.md",
        *sorted((root / ".claude" / "rules").glob("*.md")),
        *sorted((root / ".claude" / "skill-collection" / "skills").rglob("SKILL.md")),
    ]
    errors: list[str] = []
    for path in paths:
        if path.is_file() and ".codex" in read(path):
            errors.append(
                f"{path.relative_to(root)}: 仍引用 .codex（已合并到 .claude）"
            )
    return errors


def check_skill(dir_name: str, text: str) -> list[str]:
    errors: list[str] = []
    fm = frontmatter(text)
    if fm is None:
        return [f"skill {dir_name}: 缺少 frontmatter"]
    if fm.get("name") != dir_name:
        errors.append(
            f"skill {dir_name}: frontmatter name={fm.get('name')!r} 与目录名不一致"
        )
    if not dir_name.startswith(SKILL_PREFIX):
        errors.append(f"skill {dir_name}: 缺少统一前缀 {SKILL_PREFIX}")
    if not SKILL_NAME_RX.match(dir_name):
        errors.append(
            f"skill {dir_name}: 名称只能包含小写字母、数字、连字符"
            "（VS Code / Claude Code 校验）"
        )
    if not fm.get("description"):
        errors.append(f"skill {dir_name}: 缺少 description")
    for section in REQUIRED_SKILL_SECTIONS:
        if not re.search(rf"^{re.escape(section)}\s*$", text, re.MULTILINE):
            errors.append(f"skill {dir_name}: 缺少 {section}")
    lines = len(text.splitlines())
    if lines >= SKILL_MAX_LINES:
        errors.append(
            f"skill {dir_name}: {lines} 行 >= {SKILL_MAX_LINES}；拆 references"
        )
    return errors


def check_skill_refs(root: Path, dir_name: str, text: str) -> list[str]:
    """SKILL.md 里的 docs/... / .claude/... 具体路径必须存在。"""
    errors: list[str] = []
    seen: set[str] = set()
    for match in SKILL_REF_RX.finditer(text):
        path = match.group(1).rstrip(".,;:)")
        if path in seen:
            continue
        seen.add(path)
        if not (root / path).exists():
            errors.append(f"skill {dir_name}: 引用路径不存在 {path}")
    return errors


def check_skill_map(root: Path) -> list[str]:
    """skill-map.md 必须列出全部 skill，且不得出现未知名。"""
    path = root / ".claude" / "skill-collection" / "docs" / "skill-map.md"
    if not path.is_file():
        return ["缺少 .claude/skill-collection/docs/skill-map.md"]
    text = read(path)
    errors: list[str] = []
    missing = sorted(name for name in EXPECTED_SKILLS if name not in text)
    if missing:
        errors.append(f"skill-map.md: 缺少 {missing}")
    names = set(re.findall(r"\bbasessd-[a-z0-9-]+\b", text))
    unknown = sorted(names - EXPECTED_SKILLS)
    if unknown:
        errors.append(f"skill-map.md: 未知 skill {unknown}")
    return errors


def check_rule_file(path: Path) -> list[str]:
    text = read(path)
    fm = frontmatter(text)
    if fm is None:
        return [f"{path.relative_to(path.parent.parent.parent)}: 缺少 frontmatter"]
    if "paths" not in fm:
        return [f"{path.name}: 缺少 paths:（无 paths 的规则会在启动时常驻）"]
    body = text.split("---", 2)[-1].strip()
    if not body:
        return [f"{path.name}: 规则正文为空"]
    return []


def check_all(root: Path) -> list[str]:
    errors: list[str] = []

    claude_path = root / "CLAUDE.md"
    if not claude_path.is_file():
        return ["缺少 CLAUDE.md"]
    claude_text = read(claude_path)
    errors.extend(check_budget(claude_text, "CLAUDE.md"))
    for kind, expected in EXPECTED_ROOT.items():
        errors.extend(check_expected(claude_text, kind, expected, "CLAUDE.md"))

    c_coding_path = root / ".claude" / "rules" / "c-coding.md"
    if not c_coding_path.is_file():
        errors.append("缺少 .claude/rules/c-coding.md")
    else:
        c_coding_text = read(c_coding_path)
        for kind, expected in EXPECTED_C_CODING.items():
            errors.extend(
                check_expected(c_coding_text, kind, expected, "c-coding.md")
            )

    guide_path = root / "docs" / "guides" / "coding_rules.md"
    if not guide_path.is_file():
        errors.append("缺少 docs/guides/coding_rules.md")
    else:
        guide_text = read(guide_path)
        found = [
            int(match.group(1))
            for line in guide_text.splitlines()
            for match in [SECTION_RE.match(line)]
            if match
        ]
        missing = sorted(set(range(1, 51)) - set(found))
        duplicates = sorted({value for value in found if found.count(value) > 1})
        if missing:
            errors.append(f"coding_rules.md: 缺少 H 小节 {missing}")
        if duplicates:
            errors.append(f"coding_rules.md: H 小节重复 {duplicates}")

    rules_dir = root / ".claude" / "rules"
    if not rules_dir.is_dir():
        errors.append("缺少 .claude/rules/")
    else:
        present = {path.stem for path in rules_dir.glob("*.md")}
        missing = sorted(EXPECTED_RULES - present)
        if missing:
            errors.append(f".claude/rules/: 缺少 {missing}")
        for path in sorted(rules_dir.glob("*.md")):
            errors.extend(check_rule_file(path))

    skills_dir = root / ".claude" / "skill-collection" / "skills"
    if not skills_dir.is_dir():
        errors.append("缺少 .claude/skill-collection/skills/")
    else:
        present = set()
        for path in sorted(skills_dir.rglob("SKILL.md")):
            name = path.parent.name
            present.add(name)
            text = read(path)
            errors.extend(check_skill(name, text))
            errors.extend(check_skill_refs(root, name, text))
        errors.extend(
            check_skill_set(present, ".claude/skill-collection/skills")
        )

    errors.extend(check_skill_map(root))
    errors.extend(check_paths(root))
    errors.extend(check_skill_links(root))
    errors.extend(check_no_codex(root))
    return errors


def self_test() -> int:
    """正例必须通过；每条负例必须报错（C7）。"""
    failures: list[str] = []
    cases = 0

    def require(name: str, ok: bool, detail: str = "") -> None:
        nonlocal cases
        cases += 1
        if not ok:
            suffix = f": {detail}" if detail else ""
            failures.append(f"{name}{suffix}")

    def root_text(
        r_ids: set[int] | None = None,
        c_ids: set[int] | None = None,
        p_ids: set[int] | None = None,
    ) -> str:
        r_ids = set(range(1, 13)) if r_ids is None else r_ids
        c_ids = set(range(1, 13)) if c_ids is None else c_ids
        p_ids = set(range(0, 15)) if p_ids is None else p_ids
        lines = ["# test"]
        lines.extend(f"**R{i} rule.**" for i in sorted(r_ids))
        lines.extend(f"**C{i} rule.**" for i in sorted(c_ids))
        lines.extend(f"**P{i} rule.**" for i in sorted(p_ids))
        return "\n".join(lines) + "\n"

    def c_coding_text(h_ids: set[int] | None = None) -> str:
        h_ids = set(range(1, 51)) if h_ids is None else h_ids
        lines = ["---", "paths:", '  - "src/**"', "---", ""]
        lines.extend(f"**H{i} rule.**" for i in sorted(h_ids))
        lines.extend(f"**S{i} rule.**" for i in range(1, 7))
        return "\n".join(lines) + "\n"

    def skill_text(name: str, h_ids: set[int] | None = None) -> str:
        h_ids = set(range(1, 51)) if h_ids is None else h_ids
        lines = ["---", f"name: {name}", "description: test", "---", ""]
        lines.extend(f"### H{i} rule" for i in sorted(h_ids))
        lines.extend(
            ["## 产物格式", "x", "## 常见失败", "x", "## 完成", "- [ ] x"]
        )
        return "\n".join(lines) + "\n"

    ok_root = root_text()
    require("正例 root", not check_budget(ok_root, "root"))
    for kind, expected in EXPECTED_ROOT.items():
        require(f"正例 root {kind}", not check_expected(ok_root, kind, expected, "root"))
    require("正例 c-coding", not check_expected(c_coding_text(), "H", EXPECTED_C_CODING["H"], "cc"))
    require("正例 skill", not check_skill("basessd-review", skill_text("basessd-review")))
    require(
        "skill 名非法字符",
        bool(check_skill("basessd-review(审查)", skill_text("basessd-review(审查)"))),
    )
    require(
        "skill 缺完成",
        bool(
            check_skill(
                "basessd-review",
                skill_text("basessd-review").replace("## 完成\n- [ ] x\n", ""),
            )
        ),
    )
    require(
        "skill 集合缺",
        bool(check_skill_set(set(EXPECTED_SKILLS) - {"basessd-review"})),
    )
    require(
        "skill 集合多",
        bool(check_skill_set(set(EXPECTED_SKILLS) | {"basessd-extra"})),
    )

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        (root / "docs/guides").mkdir(parents=True)
        (root / "docs/guides/ok.md").write_text("x", encoding="utf-8")
        require(
            "skill 引用正例",
            not check_skill_refs(root, "basessd-review", "见 docs/guides/ok.md"),
        )
        require(
            "skill 引用缺失",
            bool(
                check_skill_refs(
                    root, "basessd-review", "见 docs/guides/missing.md"
                )
            ),
        )

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        map_path = root / ".claude/skill-collection/docs/skill-map.md"
        map_path.parent.mkdir(parents=True)
        map_path.write_text("\n".join(sorted(EXPECTED_SKILLS)), encoding="utf-8")
        require("skill-map 正例", not check_skill_map(root))
        map_path.write_text(
            "\n".join(sorted(EXPECTED_SKILLS - {"basessd-review"})),
            encoding="utf-8",
        )
        require("skill-map 缺项", bool(check_skill_map(root)))
        map_path.write_text(
            "\n".join(sorted(EXPECTED_SKILLS | {"basessd-extra"})),
            encoding="utf-8",
        )
        require("skill-map 未知名", bool(check_skill_map(root)))

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        links = root / ".claude/skills"
        links.mkdir(parents=True)
        (links / "basessd-extra").symlink_to(".")
        require(
            "多余 skill symlink",
            any("多余" in error for error in check_skill_links(root)),
        )

    big = ok_root + "\n".join("x" for _ in range(ROOT_LINES_HARD)) + "\n"
    require("root 超行数", bool(check_budget(big, "root")))

    require(
        "root 缺 R12",
        bool(check_expected(root_text(r_ids=set(range(1, 12))), "R", EXPECTED_ROOT["R"], "root")),
    )
    require(
        "c-coding 缺 H50",
        bool(
            check_expected(
                c_coding_text(h_ids=set(range(1, 50))),
                "H",
                EXPECTED_C_CODING["H"],
                "cc",
            )
        ),
    )

    skill_missing = skill_text("basessd-review", h_ids=set(range(1, 50)))
    found = [
        int(m.group(1))
        for line in skill_missing.splitlines()
        for m in [SECTION_RE.match(line)]
        if m
    ]
    require("skill 缺 H50", 50 not in found)

    require(
        "skill 前缀错",
        bool(check_skill("c-rules", skill_text("c-rules"))),
    )
    require(
        "规则文件缺 paths",
        bool(check_rule_file_path_text("---\nname: x\n---\n# body\n")),
    )

    with tempfile.TemporaryDirectory() as tmp:
        empty = Path(tmp)
        require("关键路径缺失", bool(check_paths(empty)))

    with tempfile.TemporaryDirectory() as tmp:
        require("缺 skill symlink", bool(check_skill_links(Path(tmp))))

    with tempfile.TemporaryDirectory() as tmp:
        stale = Path(tmp) / "CLAUDE.md"
        stale.write_text("见 .codex/skills/...", encoding="utf-8")
        require("残留 .codex 引用", bool(check_no_codex(Path(tmp))))

    if failures:
        print("rules-test: FAIL")
        for failure in failures:
            print(f"  - {failure}")
        return 1

    print(f"rules-test: OK ({cases} cases)")
    return 0


def check_rule_file_path_text(text: str) -> list[str]:
    """自检用：对规则文件文本做与 check_rule_file 相同的检查。"""
    fm = frontmatter(text)
    if fm is None:
        return ["缺少 frontmatter"]
    if "paths" not in fm:
        return ["缺少 paths:"]
    if not text.split("---", 2)[-1].strip():
        return ["正文为空"]
    return []


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="CLAUDE.md / rules / skills 检查")
    parser.add_argument("--root", default=".", help="仓库根目录")
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--check", action="store_true", help="校验分层与覆盖")
    group.add_argument("--self-test", action="store_true", help="检查器自检")
    args = parser.parse_args(argv)

    if args.self_test:
        return self_test()

    root = Path(args.root)
    try:
        errors = check_all(root)
    except OSError as exc:
        print(f"rules-check: ERROR: {exc}", file=sys.stderr)
        return 2

    if errors:
        print("rules-check: FAIL")
        for error in errors:
            print(f"  - {error}")
        return 1

    claude_text = read(root / "CLAUDE.md")
    lines = len(claude_text.splitlines())
    size = len(claude_text.encode("utf-8"))
    target = ""
    if lines > ROOT_LINES_TARGET or size > ROOT_BYTES_TARGET:
        target = f"（目标 {ROOT_LINES_TARGET} 行 / {ROOT_BYTES_TARGET} B）"
    print(
        f"rules-check: OK (CLAUDE.md {lines} 行 / {size} B{target}; "
        f"R12 C12 P15 H50 S6; {len(EXPECTED_SKILLS)} skills; "
        f"{len(EXPECTED_RULES)} rules)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
