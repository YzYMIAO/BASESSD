"""文档分层检查：平台 / 代码坐标 / 日期 / 尺寸 / 台账引用。

规则只消费 `docs.model.DocLine` 与 `docs.policy`；路径分类不在这里重写。
"""

from __future__ import annotations

import re
from pathlib import Path

from rulecheck.docs import policy
from rulecheck.docs.model import parse_document
from rulecheck.registry import Finding, Rule

# Public contract headers are allowed in design docs: they are the API, not an
# implementation coordinate.
CODE_ALLOW_RX = re.compile(r"\bsizeof\s*\(")
PUBLIC_HEADER_RX = re.compile(
    r"include/(?:ftl/ftl_interface|fc/fc_interface|nand/nand_interface|hil/controller)\.h"
)

PLATFORM_RX = re.compile(
    r"\b(?:STM32H743|Cortex-M7|ITCM|DTCM|AXI_RAM|AXI_NOCACHE|SRAM[0-9]|"
    r"FMC|HAL_[A-Za-z0-9_]+|GPIO[A-Z]?|USART[0-9]?|NOLOAD|\.noinit|\.bss|"
    r"\.dtcm_data|\.sram[0-9]_data|DTCM_DATA|SRAM[0-9]_DATA)\b"
    r"|链接脚本|内存域|linker script",
    re.IGNORECASE,
)

CODE_RX = re.compile(
    r"\b(?:src|include)/[A-Za-z0-9_./-]+"
    r"|`[a-z_][a-z0-9_]{2,}\s*\("
    r"|\b[a-z_][a-z0-9_]{2,}\(\)"
    r"|\b[gs]_[a-z0-9_]{2,}\b"
    r"|\.(?:c|h):\d+"
)

TEMPORAL_RX = re.compile(
    r"20\d{2}-\d{2}-\d{2}"
    r"|\bv\d+\.\d+\b"
    r"|built\s+(?:Jan|Feb|Mar|Apr|May|Jun|Jul|Aug|Sep|Oct|Nov|Dec)"
    r"|KB/s|MB/s|MiB/s"
    r"|\bseed\b"
    r"|\d+/\d+\s*(?:passed|通过|tests successful)"
    r"|H2testw|CrystalDiskMark"
    r"|\d{2}:\d{2}.*镜像|镜像.*\d{2}:\d{2}"
)

SIZE_RX = re.compile(r"\b\d[\d,]*\s*(?:KB|KiB|MB|MiB)\b")
SIZE_CONTEXT_RX = re.compile(r"内存|占用|对象|结构体|workspace|实测|合计")
LEDGER_RX = re.compile(r"RISK-[A-Z]+-\d+|ISS-[A-Z]+-\d+|docs/archive|revision_history")


def _line_matches(category: str, line: str) -> bool:
    if category == "platform":
        return PLATFORM_RX.search(line) is not None
    if category == "code":
        code_line = PUBLIC_HEADER_RX.sub("", line)
        code_line = CODE_ALLOW_RX.sub("", code_line)
        return CODE_RX.search(code_line) is not None
    if category == "temporal":
        return TEMPORAL_RX.search(line) is not None
    if category == "size":
        return SIZE_RX.search(line) is not None and SIZE_CONTEXT_RX.search(line) is not None
    if category == "ledger":
        return LEDGER_RX.search(line) is not None
    return False


def scan_file(root: Path, path: Path) -> list[Finding]:
    """扫描单个文档；path 可以是绝对路径或相对 root 的路径。"""
    full = path if path.is_absolute() else root / path
    if not full.is_file():
        return []
    try:
        rel = str(full.relative_to(root))
    except ValueError:
        rel = str(full)
    checks = policy.checks_for(rel)
    if not checks:
        return []

    text = full.read_text(encoding="utf-8", errors="ignore")
    findings: list[Finding] = []
    for doc_line in parse_document(text):
        if doc_line.skip:
            continue
        active = checks - set(doc_line.allowed)
        for category in policy.CHECK_ORDER:
            if category in active and _line_matches(category, doc_line.text):
                findings.append(
                    Finding(
                        path=full,
                        line=doc_line.number,
                        message=doc_line.text.strip(),
                        category=category,
                    )
                )
                break
    return findings


def collect_docs(root: Path) -> list[Path]:
    docs = root / "docs"
    if not docs.is_dir():
        return []
    return sorted(path for path in docs.rglob("*.md") if path.is_file())


def check(root: Path, targets: list[Path] | None) -> list[Finding]:
    paths = targets if targets is not None else collect_docs(root)
    findings: list[Finding] = []
    for path in paths:
        findings.extend(scan_file(root, path))
    return findings


RULE = Rule(
    rule_id="DOC-LAYER",
    title="文档分层（平台 / 代码坐标 / 日期 / 尺寸 / 台账）",
    kind="doc",
    check=check,
    roots=("docs",),
    suffixes=(".md",),
)
