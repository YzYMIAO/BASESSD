"""H51 文件内布局规则。

词法与条目模型在 `rulecheck.model`；本文件只保留 H51 的段序 / 注册表
判据、兼容 `check_file()` 与旧 CLI 自检。
"""

from __future__ import annotations

import re
import sys
import tempfile
import textwrap
from pathlib import Path

from rulecheck.model import (
    COMPOSITE,
    ENTRY_SUFFIXES,
    POLICY,
    REGISTRY_NAME_RE,
    SCAN_DIRS,
    Document,
    Item,
    Result,
    load_document,
    parse_items,
    strip_comments_and_literals,
)
from rulecheck.registry import Finding, Rule

def is_static_inline(item: Item) -> bool:
    words = set(re.findall(r"[A-Za-z_]\w*", item.text))
    return "static" in words and ("inline" in words or "__inline__" in words)


def is_static_function(item: Item) -> bool:
    return "static" in set(re.findall(r"[A-Za-z_]\w*", item.text))



def declared_var_name(text: str) -> str:
    """取文件级变量声明 / 定义的名字（启发式）。"""
    match = re.search(r"([A-Za-z_]\w*)\s*(?:\[[^\]]*\]\s*)?=\s*\{", text)
    if match:
        return match.group(1)
    match = re.search(r"([A-Za-z_]\w*)\s*\[[^\]]*\]\s*$", text)
    return match.group(1) if match else ""


def registry_table(item: Item, items: list[Item]) -> tuple[str, list[Item]] | None:
    """返回（表名，初始化项引用的更早本文件函数）；不是注册表则返回 None。

    注册表的判据：文件级 const 数组 / 结构体，初始化项直接引用本文件更早
    定义的函数（dispatch / command / task / handler / device descriptor）。
    """
    if item.kind != "var" or "const" not in item.text or "{" not in item.text:
        return None
    name = declared_var_name(item.text)
    if not name or REGISTRY_NAME_RE.search(name) is None:
        return None
    body = item.text[item.text.find("{") :]
    funcs = [
        f
        for f in items
        if f.kind == "func" and f.line < item.line and f.name and re.search(rf"\b{re.escape(f.name)}\b", body)
    ]
    return (name, funcs) if funcs else None


def is_entry_function(item: Item) -> bool:
    # 注册表定位时，入口组 = 模块 public API；static 的 *_init/_step 只是
    # 实现细节，不应把表挡在入口组之前。
    return item.name == "main" or not is_static_function(item)


def registry_placement_ok(item: Item, registry: tuple[str, list[Item]], items: list[Item], result: Result, source: str) -> bool:
    name, referenced = registry
    funcs = [f for f in items if f.kind == "func"]
    referenced_names = {f.name for f in referenced}
    non_entry = [f for f in funcs if not is_entry_function(f) or f.name in referenced_names]
    entries = [f for f in funcs if is_entry_function(f) and f.name not in referenced_names]

    after_impl = not non_entry or item.line > max(f.line for f in non_entry)
    before_entries = not entries or item.line < min(f.line for f in entries)
    at_end = not any(f.line > item.line for f in funcs)
    if not (after_impl and (before_entries or at_end)):
        result.violation(
            item.line,
            f"registry table '{name}' must follow implementations and precede the entry group "
            f"(or be the last function-side data; H51)",
        )
        return True

    used_before = any(
        f.line < item.line and name in source[f.start_off : f.end_off] for f in funcs
    )
    if used_before:
        has_forward = any(
            v.kind in ("var", "extern_var")
            and v.line < item.line
            and declared_var_name(v.text) == name
            and "{" not in v.text
            for v in items
        )
        if not has_forward:
            result.violation(
                item.line,
                f"registry table '{name}' is used before its definition; add one file-scope "
                f"forward declaration (H51)",
            )
            return True
    return True


def prototype_is_entry(item: Item) -> bool:
    return any(item.name.endswith(suffix) for suffix in ENTRY_SUFFIXES)


# ============================================================================
# 第 3 层：H51 策略检查
#   每条规则一个函数；新增规则只在这里加函数，不改词法层。
# ============================================================================
def _check_c_definition_order(items: list[Item], result: Result, source: str) -> None:
    # C 要求文件级初始化器先见到函数声明；这类前置声明可以出现在变量表之前，
    # 对 H51 而言它不算“函数区”的起点。
    def is_var_forward(item: Item) -> bool:
        if item.kind != "proto" or not item.name:
            return False
        return any(
            var.kind == "var" and var.start_off >= item.start_off and item.name in var.text
            for var in items
        )

    first_api = None
    for item in items:
        if item.kind in ("proto", "func") and not is_var_forward(item):
            first_api = item
            break

    if first_api is None:
        return
    for item in items:
        if item.line <= first_api.line:
            continue
        if item.kind not in ("include", "macro", "type", "var", "extern_var"):
            continue
        registry = registry_table(item, items)
        if registry is not None:
            registry_placement_ok(item, registry, items, result, source)
            continue
        result.violation(
            item.line,
            f"{item.kind} appears after the first function/prototype "
            f"at line {first_api.line} (H51)",
        )


def _check_c_main_last(items: list[Item], result: Result) -> None:
    main_item = next((item for item in items if item.kind == "func" and item.name == "main"), None)
    if main_item is None:
        return
    for item in items:
        if item.kind == "func" and item.line > main_item.line:
            result.violation(item.line, f"function '{item.name}' defined after main (H51)")


def check_c(path: Path, items: list[Item], result: Result, source: str) -> None:
    """`.c` 检查入口：规则彼此独立；新增规则只加一个检查函数。"""
    _check_c_definition_order(items, result, source)
    _check_c_main_last(items, result)

def _check_h_pragma_once(items: list[Item], result: Result) -> None:
    pragma = next((item for item in items if item.kind == "pragma_once"), None)
    if pragma is not None:
        result.violation(pragma.line, "#pragma once 禁止使用；改用 include guard（H12）")


def _check_h_definition_order(items: list[Item], result: Result) -> None:
    first_proto = next((item for item in items if item.kind == "proto"), None)
    if first_proto is None:
        return
    for item in items:
        if item.line <= first_proto.line:
            continue
        if item.kind in ("include", "macro", "type", "extern_var"):
            result.violation(
                item.line,
                f"{item.kind} appears after the first prototype at line "
                f"{first_proto.line} (H51)",
            )


def _check_h_inline_functions(items: list[Item], result: Result) -> None:
    first_proto = next((item for item in items if item.kind == "proto"), None)
    for item in items:
        if item.kind != "func":
            continue
        if not is_static_inline(item):
            result.violation(
                item.line,
                f"function definition '{item.name}' in a header; only static inline "
                f"fast paths are allowed (H12)",
            )
        elif first_proto is not None and item.line > first_proto.line:
            # 调用后文原型的 static inline 必须排在原型之后；这里只报告不失败，
            # 纯 fast path 仍然放在原型区之前。
            result.warning(
                item.line,
                f"static inline '{item.name}' appears after a prototype at line "
                f"{first_proto.line} (H51; allowed only for prototype-dependent wrappers)",
            )


def _check_h_prototype_order(items: list[Item], result: Result) -> None:
    entry_seen = None
    for item in items:
        if item.kind != "proto":
            continue
        if prototype_is_entry(item):
            entry_seen = item
        elif entry_seen is not None:
            result.warning(
                item.line,
                f"query prototype '{item.name}' appears after entry prototype "
                f"'{entry_seen.name}' at line {entry_seen.line} (H51)",
            )


def check_h(path: Path, items: list[Item], result: Result) -> None:
    """`.h` 检查入口：各条规则独立。"""
    _check_h_pragma_once(items, result)
    _check_h_definition_order(items, result)
    _check_h_inline_functions(items, result)
    _check_h_prototype_order(items, result)

def check_file(path: Path) -> Result:
    result = Result(path=path)
    document = load_document(path)
    result.items = document.items

    if path.suffix == ".c":
        check_c(path, document.items, result, document.source)
    else:
        check_h(path, document.items, result)
    return result


def source_files(root: Path) -> list[Path]:
    files: list[Path] = []
    for name in SCAN_DIRS:
        base = root / name
        if not base.is_dir():
            continue
        files.extend(sorted(base.rglob("*.c")))
        files.extend(sorted(base.rglob("*.h")))
    return files


def print_result(result: Result, report: bool) -> None:
    rel = result.path
    if result.violations:
        print(f"{rel}:")
        for line, message in result.violations:
            print(f"  {line}: {message}")
    if report:
        if not result.violations:
            print(f"{rel}: OK")
        for line, message in result.warnings:
            print(f"  {line}: warning: {message}")


def run_self_test() -> int:
    cases: list[tuple[str, str, str, int, str]] = []

    cases.append(
        (
            "valid_c",
            """\
            #include "a.h"
            #define FOO 1
            typedef int foo_t;
            static int s_state;
            static void short_q(void);
            static void short_q(void) { s_state++; }
            static void long_work(void)
            {
                s_state++;
                s_state++;
                s_state++;
                s_state++;
            }
            int main(void) { return 0; }
            """,
            ".c",
            0,
            "OK",
        )
    )
    cases.append(
        (
            "registry_after_handlers",
            """\
            #include "a.h"
            typedef void (*fn_t)(void);
            static void h1(void) { }
            static void h2(void) { }
            static const fn_t table[] = { h1, h2 };
            void entry(void) { }
            """,
            ".c",
            0,
            "OK",
        )
    )
    cases.append(
        (
            "registry_at_end",
            """\
            #include "a.h"
            typedef void (*fn_t)(void);
            static void h1(void) { }
            int main(void) { return 0; }
            static const fn_t table[] = { h1 };
            """,
            ".c",
            0,
            "OK",
        )
    )
    cases.append(
        (
            "registry_before_handlers",
            """\
            #include "a.h"
            typedef void (*fn_t)(void);
            static void h1(void) { }
            static const fn_t table[] = { h2 };
            static void h2(void) { }
            """,
            ".c",
            1,
            "var appears after",
        )
    )
    cases.append(
        (
            "registry_used_before_no_forward",
            """\
            #include "a.h"
            typedef void (*fn_t)(void);
            static void use(void) { table[0](); }
            static void h1(void) { }
            static const fn_t table[] = { h1 };
            void entry(void) { }
            """,
            ".c",
            1,
            "forward declaration",
        )
    )
    cases.append(
        (
            "registry_used_before_with_forward",
            """\
            #include "a.h"
            typedef void (*fn_t)(void);
            static const fn_t table[];
            static void use(void) { table[0](); }
            static void h1(void) { }
            static const fn_t table[] = { h1 };
            void entry(void) { }
            """,
            ".c",
            0,
            "OK",
        )
    )
    cases.append(
        (
            "var_init_forward_proto",
            """\
            #include "a.h"
            typedef void (*fn_t)(void);
            static void late(void);
            static const fn_t table[] = { late };
            static void late(void) { }
            int main(void) { return 0; }
            """,
            ".c",
            0,
            "OK",
        )
    )
    cases.append(
        (
            "macro_after_func",
            """\
            #include "a.h"
            static void f(void) { }
            #define LATE 1
            """,
            ".c",
            1,
            "macro appears after",
        )
    )
    cases.append(
        (
            "type_after_func",
            """\
            #include "a.h"
            static void f(void) { }
            typedef int late_t;
            """,
            ".c",
            1,
            "type appears after",
        )
    )
    cases.append(
        (
            "var_after_func",
            """\
            #include "a.h"
            static void f(void) { }
            static int s_late;
            """,
            ".c",
            1,
            "var appears after",
        )
    )
    cases.append(
        (
            "main_not_last",
            """\
            #include "a.h"
            int main(void) { return 0; }
            static void late(void) { }
            """,
            ".c",
            1,
            "defined after main",
        )
    )
    cases.append(
        (
            "valid_h",
            """\
            #ifndef A_H
            #define A_H
            #include "b.h"
            #define FOO 1
            typedef int foo_t;
            extern int g_state;
            static inline int foo_get(void) { return g_state; }
            int foo_query(void);
            void foo_init(void);
            #endif
            """,
            ".h",
            0,
            "OK",
        )
    )
    cases.append(
        (
            "h_extern_after_proto",
            """\
            #ifndef A_H
            #define A_H
            int foo_query(void);
            extern int g_state;
            #endif
            """,
            ".h",
            1,
            "extern_var appears after",
        )
    )
    cases.append(
        (
            "h_macro_after_proto",
            """\
            #ifndef A_H
            #define A_H
            int foo_query(void);
            #define LATE 1
            #endif
            """,
            ".h",
            1,
            "macro appears after",
        )
    )
    cases.append(
        (
            "h_non_inline_func",
            """\
            #ifndef A_H
            #define A_H
            static int foo_get(void) { return 0; }
            #endif
            """,
            ".h",
            1,
            "only static inline",
        )
    )
    cases.append(
        (
            "h_inline_after_proto",
            """\
            #ifndef A_H
            #define A_H
            int foo_query(void);
            static inline int foo_get(void) { return 0; }
            #endif
            """,
            ".h",
            0,
            "OK",
        )
    )
    cases.append(
        (
            "h_pragma_once",
            """\
            #pragma once
            int foo_query(void);
            """,
            ".h",
            1,
            "#pragma once",
        )
    )
    cases.append(
        (
            "comment_ignore",
            """\
            #include "a.h"
            /* typedef int fake_t; */
            // #define FAKE 1
            static void f(void) { }
            """,
            ".c",
            0,
            "OK",
        )
    )

    failures: list[str] = []
    for name, text, suffix, want_rc, needle in cases:
        import tempfile

        with tempfile.TemporaryDirectory(prefix=f"layout_{name}_") as tmp:
            root = Path(tmp)
            path = root / ("a" + suffix)
            path.write_text(textwrap.dedent(text), encoding="utf-8")
            result = check_file(path)
            rc = 1 if result.violations else 0
            output = "\n".join(
                f"{line}: {message}" for line, message in result.violations
            )
            if rc != want_rc:
                failures.append(f"{name}: expected exit {want_rc}, got {rc}\n{output}")
            if needle not in output and not (want_rc == 0 and needle == "OK"):
                failures.append(f"{name}: expected output to contain {needle!r}\n{output}")

    if failures:
        print("layout-test: FAIL")
        for failure in failures:
            print(failure)
        return 1
    print(f"layout-test: OK ({len(cases)} cases)")
    return 0




def _files(root: Path, targets: list[Path] | None) -> list[Path]:
    if targets is not None:
        return [path for path in targets if path.suffix in {".c", ".h"}]
    return source_files(root)


def check(root: Path, targets: list[Path] | None) -> list[Finding]:
    findings: list[Finding] = []
    for path in _files(root, targets):
        result = check_file(path)
        findings.extend(
            Finding(path=path, line=line, message=message, severity="error")
            for line, message in result.violations
        )
        findings.extend(
            Finding(path=path, line=line, message=message, severity="warning")
            for line, message in result.warnings
        )
    return findings


def selftest() -> tuple[bool, str]:
    return (run_self_test() == 0, "18 cases")


RULE = Rule(
    rule_id="H51",
    title="文件内布局（.c / .h / 注册表）",
    kind="textual",
    check=check,
    selftest=selftest,
    roots=("src",),
)
