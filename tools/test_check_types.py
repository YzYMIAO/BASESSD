#!/usr/bin/env python3
"""Self-test for tools/check_types.py (P13 / C7).

Discrimination proof: the positive fixture must pass, and every negative
fixture must exit 1 with the expected diagnostic. A checker that only ever
prints OK is indistinguishable from a broken checker; this script is the
negative example that proves the checks can fail.

Run directly or via `make types-test`. Exit 0 = all cases behaved as expected.
"""

from __future__ import annotations

import subprocess
import sys
import tempfile
import textwrap
from pathlib import Path

CHECKER = Path(__file__).with_name("check_types.py")


def run_case(name: str, header: str, source: str, want_rc: int, needle: str) -> list[str]:
    failures: list[str] = []
    with tempfile.TemporaryDirectory(prefix=f"check_types_{name}_") as tmp:
        root = Path(tmp)
        (root / "include").mkdir()
        (root / "src").mkdir()
        (root / "include" / "a.h").write_text(textwrap.dedent(header), encoding="utf-8")
        if source:
            (root / "src" / "a.c").write_text(textwrap.dedent(source), encoding="utf-8")

        proc = subprocess.run(
            [sys.executable, str(CHECKER), "--root", str(root)],
            capture_output=True,
            text=True,
            check=False,
        )
        output = proc.stdout + proc.stderr
        if proc.returncode != want_rc:
            failures.append(f"{name}: expected exit {want_rc}, got {proc.returncode}\n{output}")
        if needle not in output:
            failures.append(f"{name}: expected output to contain {needle!r}\n{output}")
    return failures


VALID_HEADER = """\
    #ifndef A_H
    #define A_H
    typedef struct GOOD_S
    {
        int a[4];
        unsigned b : 2;
        void (*cb)(void);
        struct OTHER_S *other;
    } good_s;
    typedef enum GOOD_KIND_E
    {
        GOOD_A = 1,
        GOOD_B,
    } good_kind_e;
    typedef union GOOD_VALUE_U
    {
        int i;
        float f;
    } good_value_u;
    typedef struct OTHER_S other_s;
    #endif
"""

VALID_SOURCE = """\
    #include "a.h"
    struct OTHER_S
    {
        int x;
    };
"""


def main() -> int:
    cases = [
        # name, header, source, expected rc, expected message
        ("valid_full", VALID_HEADER, VALID_SOURCE, 0, "types-check: OK"),
        (
            "valid_attr_nested",
            """\
            typedef struct WRAP_S
            {
                struct { int x; };
                int y;
            } wrap_s __attribute__((packed));
            """,
            "",
            0,
            "types-check: OK",
        ),
        (
            "bare_header",
            "struct BAD_S { int q; };\n",
            "",
            1,
            "in a header",
        ),
        (
            "tag_lower",
            "typedef struct foo_s { int a; } foo_s;\n",
            "",
            1,
            "must be uppercase",
        ),
        (
            "alias_bad_suffix",
            "typedef struct FOO_S { int a; } foo_x;\n",
            "",
            1,
            "must be lowercase",
        ),
        (
            "anonymous",
            "typedef struct { int a; } foo_s;\n",
            "",
            1,
            "anonymous struct typedef has no tag",
        ),
        (
            "bare_forward",
            "struct FOO_S;\n",
            "",
            1,
            "bare forward declaration",
        ),
        (
            "forward_undefined",
            "typedef struct FOO_S foo_s;\n",
            "",
            1,
            "has no definition",
        ),
        (
            "duplicate_tag",
            """\
            typedef struct FOO_S { int a; } foo_s;
            typedef struct FOO_S { int b; } foo2_s;
            """,
            "",
            1,
            "defined more than once",
        ),
        (
            "alias_collision",
            """\
            typedef struct FOO_S { int a; } foo_s;
            typedef struct BAR_S { int b; } foo_s;
            """,
            "",
            1,
            "already denotes",
        ),
        (
            "opaque_without_forward",
            "",
            "struct FOO_S { int a; };\n",
            1,
            "without a forward typedef",
        ),
        (
            "opaque_ok",
            "typedef struct FOO_S foo_s;\n",
            "struct FOO_S { int a; };\n",
            0,
            "types-check: OK",
        ),
        (
            "forward_alias_kind",
            "typedef struct FOO_S foo_e;\n",
            "struct FOO_S { int a; };\n",
            1,
            "forward alias `foo_e`",
        ),
        (
            "forward_tag_kind",
            "typedef enum FOO_S foo_s;\n",
            "struct FOO_S { int a; };\n",
            1,
            "forward tag `FOO_S`",
        ),
        (
            "forward_alias_collision",
            """\
            typedef struct A_S a_s;
            typedef struct B_S a_s;
            """,
            "struct A_S { int a; };\nstruct B_S { int b; };\n",
            1,
            "forward alias `a_s` already denotes",
        ),
    ]

    failures: list[str] = []
    for name, header, source, want_rc, needle in cases:
        failures.extend(run_case(name, header, source, want_rc, needle))

    if failures:
        for failure in failures:
            print(f"types-test: FAIL\n{failure}", file=sys.stderr)
        print(f"types-test: FAILED ({len(failures)} case(s))", file=sys.stderr)
        return 1

    print(f"types-test: OK ({len(cases)} cases)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
