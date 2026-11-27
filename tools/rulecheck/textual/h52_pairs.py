"""H52 模块文件成对检查（FTL / FC / NAND / PLATFORM 四层）。

判据：
  · 每个模块 .c 必须有同名 .h（同 basename，可在本层 src/ 或 include/）；
  · 子目录模块按同相对子路径配对（如 src/nand/devices/x.c ↔ include/nand/devices/x.h）；
  · 一个内部 .h 声明的符号只能由一个 .c 实现（公共 include/ 契约头豁免）；
  · 例外必须在文件头标注，机器可检：
      header-only: config | contract | inline-access   （无 .c 的头）
      implements <header.h>                            （端口适配 / 接口实现 .c）
      internal-only                                    （无跨 TU 导出的内部拆分 .c）
      multi-impl: <reason>                             （注册/分发表等一表多头）

标记只认文件头前 40 行，避免正文注释误命中。
"""

from __future__ import annotations

import re
import tempfile
from pathlib import Path

from rulecheck.model import load_document
from rulecheck.registry import Finding, Rule

# (本层 src 目录, 公共 include 目录)；公共 include 头允许多个 .c 实现（H52）。
LAYERS = (
    (Path("src/ftl"), Path("include/ftl")),
    (Path("src/fc"), Path("include/fc")),
    (Path("src/nand"), Path("include/nand")),
    (Path("src/platform"), Path("include/platform")),
    (Path("src/hil"), Path("include/hil")),
)
HEADER_ONLY_KINDS = {"config", "contract", "inline-access"}
HEAD_LINES = 40


def _head_text(path: Path) -> str:
    try:
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError:
        return ""
    return "\n".join(lines[:HEAD_LINES])


def _markers(path: Path) -> dict[str, str]:
    text = _head_text(path)
    result: dict[str, str] = {}
    match = re.search(r"header-only\s*:\s*([a-z-]+)", text)
    if match:
        result["header_only"] = match.group(1)
    match = re.search(r"implements\s+([A-Za-z0-9_./-]+\.h)", text)
    if match:
        result["implements"] = match.group(1)
    match = re.search(r"multi-impl\s*:\s*([^\n*]+)", text)
    if match:
        result["multi_impl"] = match.group(1).strip()
    if re.search(r"\binternal-only\b", text):
        result["internal_only"] = "1"
    return result


def _find_header(root: Path, scope: Path, public: Path, source: Path) -> Path | None:
    """按 H52 查找顺序定位 .c 的同名头（同目录 → 本层根 → 公共根 → 同相对子路径）。"""
    name = source.with_suffix(".h").name
    try:
        rel = source.relative_to(root / scope)
    except ValueError:
        return None
    candidates = (
        source.with_suffix(".h"),
        root / scope / name,
        root / public / name,
        root / scope / rel.with_suffix(".h"),
        root / public / rel.with_suffix(".h"),
    )
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    return None


def _lookup_header_name(root: Path, scope: Path, public: Path, name: str) -> Path | None:
    """按纯文件名查找头（`implements <header.h>` 标记用）。"""
    for candidate in (root / scope / name, root / public / name):
        if candidate.is_file():
            return candidate
    return None


def _target_files(root: Path, scope: Path, targets: list[Path] | None) -> tuple[list[Path], list[Path]]:
    base = root / scope
    c_files = sorted(base.rglob("*.c")) if base.is_dir() else []
    h_files = sorted(base.rglob("*.h")) if base.is_dir() else []
    if targets is not None:
        wanted = {path.resolve() for path in targets}
        c_files = [path for path in c_files if path.resolve() in wanted]
        h_files = [path for path in h_files if path.resolve() in wanted]
    return c_files, h_files


def _definitions(root: Path, scope: Path) -> dict[str, set[Path]]:
    """函数名 → 定义它的 .c 集合（只扫本层 src 目录）。"""
    base = root / scope
    table: dict[str, set[Path]] = {}
    if not base.is_dir():
        return table
    for path in sorted(base.rglob("*.c")):
        try:
            document = load_document(path)
        except (OSError, ValueError):
            continue
        for item in document.functions():
            if item.name:
                table.setdefault(item.name, set()).add(path)
    return table


def _check_forward(
    root: Path, scope: Path, public: Path, c_files: list[Path], h_files: list[Path]
) -> list[Finding]:
    findings: list[Finding] = []
    for path in c_files:
        markers = _markers(path)
        if "implements" in markers:
            target = _lookup_header_name(root, scope, public, Path(markers["implements"]).name)
            if target is None:
                findings.append(Finding(path, 1, f"implements 指向的头不存在：{markers['implements']}"))
            continue
        if "internal_only" in markers:
            continue
        if _find_header(root, scope, public, path) is None:
            findings.append(Finding(path, 1, "模块 .c 缺少同名 .h（或补 implements / internal-only 标记）"))
    for path in h_files:
        if (path.parent / path.with_suffix(".c").name).is_file():
            continue
        markers = _markers(path)
        kind = markers.get("header_only")
        if kind in HEADER_ONLY_KINDS:
            continue
        findings.append(
            Finding(path, 1, "头文件没有同名 .c；补 header-only: config|contract|inline-access 标记")
        )
    return findings


def _check_reverse(root: Path, scope: Path, h_files: list[Path]) -> list[Finding]:
    findings: list[Finding] = []
    definitions = _definitions(root, scope)
    for header in h_files:
        pair = header.parent / header.with_suffix(".c").name
        if not pair.is_file():
            continue
        markers = _markers(header)
        if "multi_impl" in markers:
            continue
        try:
            document = load_document(header)
        except (OSError, ValueError):
            continue
        protos = [item for item in document.items if item.kind == "proto" and item.name]
        if not protos:
            continue
        owners: dict[Path, int] = {}
        for item in protos:
            for owner in definitions.get(item.name, set()):
                owners.setdefault(owner, item.line)
        if len(owners) > 1:
            listed = ", ".join(sorted(path.name for path in owners))
            line = min(owners.values())
            findings.append(
                Finding(
                    header,
                    line,
                    f"一个头由多个 .c 实现（{listed}）；拆成一头一对，或标 multi-impl: <理由>",
                )
            )
        elif owners and pair not in owners:
            findings.append(
                Finding(header, 1, f"同名实现文件 {pair.name} 未实现本头声明的符号")
            )
    return findings


def check(root: Path, targets: list[Path] | None) -> list[Finding]:
    findings: list[Finding] = []
    for scope, public in LAYERS:
        c_files, h_files = _target_files(root, scope, targets)
        findings.extend(_check_forward(root, scope, public, c_files, h_files))
        findings.extend(_check_reverse(root, scope, h_files))
    return findings


def _run_case(name: str, files: dict[str, str], want_errors: int) -> str | None:
    with tempfile.TemporaryDirectory(prefix=f"pairs_{name}_") as tmp:
        root = Path(tmp)
        for rel, text in files.items():
            path = root / rel
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text, encoding="utf-8")
        errors = [item for item in check(root, None) if item.severity == "error"]
        if len(errors) != want_errors:
            detail = "; ".join(f"{item.path.name}:{item.line} {item.message}" for item in errors)
            return f"{name}: 期望 {want_errors} 个错误，实得 {len(errors)}（{detail}）"
    return None


def selftest() -> tuple[bool, str]:
    cases: list[tuple[str, dict[str, str], int]] = [
        (
            "valid_pair",
            {
                "src/ftl/good.c": "#include \"good.h\"\nint good_fn(void) { return 0; }\n",
                "src/ftl/good.h": "int good_fn(void);\n",
            },
            0,
        ),
        (
            "missing_header",
            {"src/ftl/nohead.c": "int lone(void) { return 0; }\n"},
            1,
        ),
        (
            "header_only_ok",
            {"src/ftl/cfg.h": "/* header-only: config */\n#define CFG 1\n"},
            0,
        ),
        (
            "header_only_missing_marker",
            {"src/ftl/only.h": "#define CFG 1\n"},
            1,
        ),
        (
            "multi_impl",
            {
                "src/ftl/split.h": "void split_fn(void);\n",
                "src/ftl/split.c": "void split_fn(void) { }\n",
                "src/ftl/impl2.c": "void split_fn(void) { }\n",
                "src/ftl/impl2.h": "/* header-only: contract */\n",
            },
            1,
        ),
        (
            "multi_impl_allowed",
            {
                "src/ftl/op.h": "/* multi-impl: dispatch-table */\nvoid op_a(void);\n",
                "src/ftl/op.c": "void op_a(void) { }\n",
                "src/ftl/op2.c": "void op_a(void) { }\n",
                "src/ftl/op2.h": "/* header-only: contract */\n",
            },
            0,
        ),
        (
            "implements_ok",
            {
                "src/ftl/port.c": "/* implements delay.h */\nvoid port(void) { }\n",
                "include/ftl/delay.h": "/* header-only: contract */\n",
            },
            0,
        ),
        (
            "implements_missing",
            {"src/ftl/port.c": "/* implements missing.h */\nvoid port(void) { }\n"},
            1,
        ),
        (
            "subdir_pair",
            {
                "src/nand/devices/dev.c": "int dev_query(void) { return 0; }\n",
                "include/nand/devices/dev.h": "int dev_query(void);\n",
            },
            0,
        ),
        (
            "implements_public_header",
            {
                "src/fc/foo.c": "/* implements fc_interface.h */\nvoid foo_entry(void) { }\n",
                "include/fc/fc_interface.h": "/* header-only: contract */\nvoid foo_entry(void);\n",
            },
            0,
        ),
        (
            "platform_port_implements",
            {
                "src/platform/soc/x/delay_port.c": "/* implements delay.h */\nvoid port(void) { }\n",
                "include/platform/delay.h": "/* header-only: contract */\nvoid port(void);\n",
            },
            0,
        ),
        (
            "hil_public_pair",
            {
                "src/hil/foo.c": "int foo_entry(void) { return 0; }\n",
                "include/hil/foo.h": "/* header-only: contract */\nint foo_entry(void);\n",
            },
            0,
        ),
        (
            "hil_missing_header",
            {"src/hil/nohead.c": "int lone(void) { return 0; }\n"},
            1,
        ),
        (
            "platform_subdir_pair",
            {
                "src/platform/soc/x/dma_port.c": "const int dma_handle = 0;\n",
                "include/platform/soc/x/dma_port.h": "/* header-only: contract */\nextern const int dma_handle;\n",
            },
            0,
        ),
    ]
    failures = [message for name, files, want in cases if (message := _run_case(name, files, want))]
    if failures:
        return False, "; ".join(failures)
    return True, f"{len(cases)} cases"


RULE = Rule(
    rule_id="H52",
    title="模块文件成对（.c/.h，1:1 或显式例外）",
    kind="textual",
    check=check,
    selftest=selftest,
    roots=("src/ftl", "src/fc", "src/nand", "src/platform", "src/hil"),
)
