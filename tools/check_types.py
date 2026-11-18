#!/usr/bin/env python3
"""H50 composite-type definition checker.

Positive invariant, not an old-name grep:
  * every complete composite definition in a header uses
    `typedef struct TAG_S { ... } alias_s;`
  * a bare `struct/enum/union TAG { ... }` is allowed only in a .c file
    when TAG has a forward `typedef ... TAG alias;` (opaque implementation type)
  * tag/alias suffixes and kinds match
  * every forward typedef has a definition in the scanned project tree
  * anonymous top-level typedef definitions are rejected

Usage:
    python3 tools/check_types.py --root .
    python3 tools/check_types.py --root . --include-tests

Exit code 0 = clean, 1 = violations, 2 = tool/input error.

Known limitations (by design, regex/token based — no preprocessor):
  * mutually exclusive `#if` branches that both define the same tag are
    reported as duplicates;
  * pointer typedefs (`typedef struct X_S {...} *p;`) are not modelled and
    the `*` is ignored, so the alias-name check sees `p`;
  * braces produced by macros can confuse the depth tracking.
The checker is a positive invariant for the forms this repository uses; its
discrimination is covered by `tools/test_check_types.py` (P13/C7).
"""

from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path


@dataclass
class Token:
    value: str
    line: int


@dataclass
class Forward:
    tag: str
    alias: str
    kind: str
    file: str
    line: int


@dataclass
class Definition:
    tag: str | None
    alias: str | None
    kind: str
    file: str
    line: int
    typedef_form: bool
    anonymous: bool = False


@dataclass
class Report:
    definitions: list[Definition] = field(default_factory=list)
    forwards: list[Forward] = field(default_factory=list)
    errors: list[str] = field(default_factory=list)

    def error(self, path: Path, line: int, message: str) -> None:
        self.errors.append(f"{path}:{line}: {message}")


TOKEN_RE = re.compile(r"[A-Za-z_]\w*|[{}();,]")
COMPOSITE = {"struct", "enum", "union"}
KIND_SUFFIX = {"struct": "S", "enum": "E", "union": "U"}
ALIAS_SUFFIX = {"struct": "s", "enum": "e", "union": "u"}


def strip_comments_and_literals(text: str) -> str:
    """Blank comments/strings while preserving newlines and byte offsets."""
    out = list(text)
    i = 0
    n = len(text)
    state = "normal"

    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ""

        if state == "normal":
            if c == "/" and nxt == "/":
                out[i] = out[i + 1] = " "
                i += 2
                state = "line"
                continue
            if c == "/" and nxt == "*":
                out[i] = out[i + 1] = " "
                i += 2
                state = "block"
                continue
            if c == '"':
                out[i] = " "
                i += 1
                state = "string"
                continue
            if c == "'":
                out[i] = " "
                i += 1
                state = "char"
                continue
            i += 1
            continue

        if state == "line":
            if c == "\n":
                state = "normal"
            else:
                out[i] = " "
            i += 1
            continue

        if state == "block":
            if c == "*" and nxt == "/":
                out[i] = out[i + 1] = " "
                i += 2
                state = "normal"
                continue
            if c != "\n":
                out[i] = " "
            i += 1
            continue

        # string / char
        if c == "\\" and i + 1 < n:
            out[i] = " "
            if text[i + 1] != "\n":
                out[i + 1] = " "
            i += 2
            continue
        if (state == "string" and c == '"') or (state == "char" and c == "'"):
            out[i] = " "
            i += 1
            state = "normal"
            continue
        if c != "\n":
            out[i] = " "
        i += 1

    return "".join(out)


def tokenize(text: str) -> list[Token]:
    stripped = strip_comments_and_literals(text)
    line_starts = [0]
    for m in re.finditer("\n", stripped):
        line_starts.append(m.end())

    def line_at(pos: int) -> int:
        lo, hi = 0, len(line_starts) - 1
        while lo <= hi:
            mid = (lo + hi) // 2
            if line_starts[mid] <= pos:
                lo = mid + 1
            else:
                hi = mid - 1
        return hi + 1

    return [Token(m.group(0), line_at(m.start())) for m in TOKEN_RE.finditer(stripped)]


def matching_brace(tokens: list[Token], open_index: int) -> int | None:
    depth = 0
    for i in range(open_index, len(tokens)):
        if tokens[i].value == "{":
            depth += 1
        elif tokens[i].value == "}":
            depth -= 1
            if depth == 0:
                return i
    return None


def skip_attributes(tokens: list[Token], index: int) -> int:
    """Skip GCC-style attributes such as `__attribute__((packed))` after `}`."""
    while index < len(tokens) and tokens[index].value.startswith("__") and tokens[index].value != "__":
        name = tokens[index].value
        index += 1
        if index < len(tokens) and tokens[index].value == "(":
            depth = 0
            while index < len(tokens):
                if tokens[index].value == "(":
                    depth += 1
                elif tokens[index].value == ")":
                    depth -= 1
                    if depth == 0:
                        index += 1
                        break
                index += 1
        # `__attribute__` is the common case; any other `__foo` token is
        # conservatively treated as an attribute prefix.
        if name != "__attribute__":
            break
    return index


def parse_file(path: Path, root: Path, report: Report) -> None:
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError as exc:
        report.errors.append(f"{path}: cannot read: {exc}")
        return

    tokens = tokenize(text)
    rel = str(path.relative_to(root))
    i = 0
    depth = 0

    while i < len(tokens):
        tok = tokens[i].value

        if tok == "{":
            # `extern "C" {` is a linkage wrapper, not a composite body.
            if i > 0 and tokens[i - 1].value == "extern":
                i += 1
                continue
            depth += 1
            i += 1
            continue
        if tok == "}":
            if depth > 0:
                depth -= 1
            i += 1
            continue
        if depth != 0:
            i += 1
            continue

        if tok == "typedef":
            j = i + 1
            if j >= len(tokens) or tokens[j].value not in COMPOSITE:
                i += 1
                continue
            kind = tokens[j].value
            k = j + 1
            if k >= len(tokens):
                i += 1
                continue

            if tokens[k].value == "{":
                # typedef struct { ... } alias; -> anonymous tag
                close = matching_brace(tokens, k)
                if close is None:
                    report.error(path, tokens[i].line, "unterminated anonymous composite definition")
                    return
                alias_index = skip_attributes(tokens, close + 1)
                alias = tokens[alias_index].value if alias_index < len(tokens) else None
                report.definitions.append(
                    Definition(None, alias, kind, rel, tokens[i].line, True, anonymous=True)
                )
                i = close + 1
                continue

            tag = tokens[k].value
            if k + 1 < len(tokens) and tokens[k + 1].value == "{":
                close = matching_brace(tokens, k + 1)
                if close is None:
                    report.error(path, tokens[i].line, f"unterminated definition of {tag}")
                    return
                alias_index = skip_attributes(tokens, close + 1)
                alias = tokens[alias_index].value if alias_index < len(tokens) else None
                report.definitions.append(
                    Definition(tag, alias, kind, rel, tokens[i].line, True)
                )
                i = close + 1
                continue

            # typedef struct TAG alias;  (forward declaration)
            if k + 2 < len(tokens) and tokens[k + 2].value == ";":
                report.forwards.append(Forward(tag, tokens[k + 1].value, kind, rel, tokens[i].line))
                i = k + 2
                continue

            i += 1
            continue

        if tok in COMPOSITE:
            kind = tok
            j = i + 1
            if j >= len(tokens):
                i += 1
                continue

            if tokens[j].value == "{":
                close = matching_brace(tokens, j)
                if close is None:
                    report.error(path, tokens[i].line, "unterminated anonymous composite definition")
                    return
                report.definitions.append(
                    Definition(None, None, kind, rel, tokens[i].line, False, anonymous=True)
                )
                i = close + 1
                continue

            tag = tokens[j].value
            if j + 1 < len(tokens) and tokens[j + 1].value == "{":
                close = matching_brace(tokens, j + 1)
                if close is None:
                    report.error(path, tokens[i].line, f"unterminated definition of {tag}")
                    return
                after = skip_attributes(tokens, close + 1)
                alias = None
                if after < len(tokens) and tokens[after].value != ";":
                    # `struct TAG {...} name;` is a variable declaration, not a typedef.
                    alias = tokens[after].value
                report.definitions.append(
                    Definition(tag, alias, kind, rel, tokens[i].line, False)
                )
                i = close + 1
                continue

            # bare forward declaration `struct TAG;` is not the H50 form
            if j + 1 < len(tokens) and tokens[j + 1].value == ";":
                report.error(path, tokens[i].line, f"bare forward declaration `{kind} {tag};`; use `typedef {kind} {tag} alias;`")
                i = j + 1
                continue

            i += 1
            continue

        i += 1


def validate(report: Report) -> None:
    defined_tags = {d.tag for d in report.definitions if d.tag}
    forward_tags = {f.tag for f in report.forwards}
    tag_kind = {d.tag: d.kind for d in report.definitions if d.tag}

    alias_owner: dict[str, tuple[str, str, int]] = {}
    tag_owner: dict[str, tuple[str, str, int]] = {}

    for d in report.definitions:
        where = f"{d.file}:{d.line}"
        if d.anonymous:
            report.errors.append(f"{where}: anonymous {d.kind} typedef has no tag (H50: add TAG_{KIND_SUFFIX[d.kind]})")
            continue
        assert d.tag is not None
        expected_tag = "_" + KIND_SUFFIX[d.kind]
        if not re.fullmatch(r"[A-Z][A-Z0-9_]*_[SEU]", d.tag) or not d.tag.endswith(expected_tag):
            report.errors.append(f"{where}: tag `{d.tag}` must be uppercase and end with `{expected_tag}`")
        if d.typedef_form:
            if d.alias is None:
                report.errors.append(f"{where}: {d.kind} `{d.tag}` definition has no typedef alias")
            else:
                expected_alias = "_" + ALIAS_SUFFIX[d.kind]
                if not re.fullmatch(r"[a-z][a-z0-9_]*_[seu]", d.alias) or not d.alias.endswith(expected_alias):
                    report.errors.append(f"{where}: alias `{d.alias}` must be lowercase and end with `{expected_alias}`")
                if d.alias == d.tag:
                    report.errors.append(f"{where}: tag and alias are identical (`{d.tag}`)")
                if d.alias in alias_owner and alias_owner[d.alias][0] != d.tag:
                    prev = alias_owner[d.alias]
                    report.errors.append(f"{where}: alias `{d.alias}` already denotes `{prev[0]}` at {prev[1]}:{prev[2]}")
                alias_owner[d.alias] = (d.tag, d.file, d.line)
        else:
            if d.alias is not None:
                report.errors.append(
                    f"{where}: bare `{d.kind} {d.tag} {{...}} {d.alias};` is a variable declaration; use the typedef form"
                )
            is_c = d.file.endswith(".c")
            if not is_c:
                report.errors.append(
                    f"{where}: bare `{d.kind} {d.tag} {{...}}` in a header; use `typedef {d.kind} {d.tag} {{...}} alias;`"
                )
            elif d.tag not in forward_tags:
                report.errors.append(
                    f"{where}: bare `{d.kind} {d.tag} {{...}}` in .c without a forward typedef (not an opaque type)"
                )

        if d.tag in tag_owner:
            prev = tag_owner[d.tag]
            report.errors.append(f"{where}: tag `{d.tag}` defined more than once (first at {prev[1]}:{prev[2]})")
        tag_owner[d.tag] = (d.tag, d.file, d.line)

    forward_alias_owner: dict[str, str] = {}

    for f in report.forwards:
        if f.tag not in defined_tags:
            report.errors.append(
                f"{f.file}:{f.line}: forward typedef `{f.tag}` has no definition in the scanned tree"
            )
        expected_tag = "_" + KIND_SUFFIX[f.kind]
        expected_alias = "_" + ALIAS_SUFFIX[f.kind]
        if not re.fullmatch(r"[A-Z][A-Z0-9_]*_[SEU]", f.tag) or not f.tag.endswith(expected_tag):
            report.errors.append(
                f"{f.file}:{f.line}: forward tag `{f.tag}` must be uppercase and end with `{expected_tag}`"
            )
        if not re.fullmatch(r"[a-z][a-z0-9_]*_[seu]", f.alias) or not f.alias.endswith(expected_alias):
            report.errors.append(
                f"{f.file}:{f.line}: forward alias `{f.alias}` must be lowercase and end with `{expected_alias}`"
            )
        if f.tag in tag_kind and tag_kind[f.tag] != f.kind:
            report.errors.append(
                f"{f.file}:{f.line}: forward `{f.kind} {f.tag}` conflicts with its definition kind `{tag_kind[f.tag]}`"
            )
        owner = alias_owner.get(f.alias)
        if owner is not None and owner[0] != f.tag:
            report.errors.append(
                f"{f.file}:{f.line}: forward alias `{f.alias}` already denotes `{owner[0]}` at {owner[1]}:{owner[2]}"
            )
        prev = forward_alias_owner.get(f.alias)
        if prev is not None and prev != f.tag:
            report.errors.append(
                f"{f.file}:{f.line}: forward alias `{f.alias}` already denotes `{prev}`"
            )
        forward_alias_owner[f.alias] = f.tag


def collect_files(root: Path, include_tests: bool) -> list[Path]:
    roots = [root / "include", root / "src"]
    if include_tests:
        roots.append(root / "tests")
    files: list[Path] = []
    for base in roots:
        if not base.exists():
            continue
        for path in base.rglob("*"):
            if not path.is_file() or path.suffix not in {".h", ".c"}:
                continue
            rel = path.relative_to(root)
            if any(part in {"third_party", "build", "cmsis", "hal"} for part in rel.parts):
                continue
            files.append(path)
    return sorted(files)


def main() -> int:
    parser = argparse.ArgumentParser(description="H50 composite-type definition checker")
    parser.add_argument("--root", type=Path, default=Path("."), help="project root")
    parser.add_argument("--include-tests", action="store_true", help="also scan tests/ (frozen; off by default)")
    args = parser.parse_args()
    root = args.root.resolve()

    report = Report()
    files = collect_files(root, args.include_tests)
    if not files:
        print("types-check: no source files found", file=sys.stderr)
        return 2

    for path in files:
        parse_file(path, root, report)
    validate(report)

    typedef_defs = sum(1 for d in report.definitions if d.typedef_form)
    opaque_defs = len(report.definitions) - typedef_defs
    print(
        f"types-check: files={len(files)} definitions={len(report.definitions)} "
        f"typedef_form={typedef_defs} bare_or_opaque={opaque_defs} forwards={len(report.forwards)}"
    )

    if report.errors:
        for err in report.errors:
            print(err, file=sys.stderr)
        print(f"types-check: FAILED ({len(report.errors)} violation(s))", file=sys.stderr)
        return 1

    print("types-check: OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
