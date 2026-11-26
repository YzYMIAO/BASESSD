"""文档类型策略：哪类文档启用哪些检查。

规则只读这张表，不把路径判断散落在各规则里。
"""

from __future__ import annotations

import fnmatch

POLICY: dict[str, dict[str, object]] = {
    "design": {
        "paths": [
            "docs/design/ftl_design.md",
            "docs/design/hil_design.md",
            "docs/design/fc_nand_design.md",
            "docs/architecture.md",
        ],
        "checks": ["platform", "code", "temporal", "size", "ledger"],
    },
    "architecture": {
        "paths": ["docs/design/controller_target_features.md"],
        "checks": [],
    },
    "platform": {
        "paths": [
            "docs/hardware_contract.md",
            "docs/boot_flow.md",
            "docs/design/platform_design.md",
            "docs/design/usb_msc.md",
            "docs/design/common/fault_capture.md",
            "docs/design/common/timer.md",
            "docs/design/common/cli.md",
            "docs/design/common/logging.md",
        ],
        # Platform docs are expected to be implementation-specific.
        "checks": [],
    },
    "verification": {
        "paths": [
            "docs/guides/testing_guide.md",
            "docs/design/ftl_ut_design.md",
            "docs/design/hil_ut_design.md",
            "docs/design/fc_nand_ut.md",
            "docs/design/common/munit_adapter.md",
        ],
        # Verification docs are expected to carry dates/results/risk IDs.
        "checks": [],
    },
    "ledger": {
        "paths": [
            "docs/design/known_open_items.md",
            "docs/design/known_closed_items.md",
        ],
        "checks": [],
    },
    "revision": {
        "paths": ["docs/design/revision_history/*.md"],
        "checks": [],
    },
    "archive": {
        "paths": ["docs/archive/**"],
        "checks": [],
    },
}

CHECK_LABEL = {
    "platform": "平台/内存域",
    "code": "实现坐标",
    "temporal": "日期/版本/测试快照",
    "size": "内存占用大小",
    "ledger": "风险/归档引用",
}

CHECK_ORDER = ("platform", "code", "temporal", "size", "ledger")


def category_of(path: str) -> str | None:
    for category, cfg in POLICY.items():
        for pattern in cfg["paths"]:  # type: ignore[index]
            if fnmatch.fnmatch(path, pattern):
                return category
    return None


def checks_for(path: str) -> set[str]:
    category = category_of(path)
    if category is None:
        return set()
    return set(POLICY[category]["checks"])  # type: ignore[index]
