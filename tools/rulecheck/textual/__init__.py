"""可机械检查的文本 / 结构规则。"""

from __future__ import annotations

from rulecheck.registry import Rule
from rulecheck.textual.h41_headers import RULE as H41
from rulecheck.textual.h50_types import RULE as H50
from rulecheck.textual.h51_layout import RULE as H51
from rulecheck.textual.h52_pairs import RULE as H52

RULES: list[Rule] = [H41, H50, H51, H52]
