"""文档规则集：与 C 规则共用 `rulecheck.registry`。"""

from __future__ import annotations

from rulecheck.docs import selftest
from rulecheck.docs.rules.layering import RULE as LAYERING
from rulecheck.docs.rules.glossary import RULE as GLOSSARY
from rulecheck.docs.rules.links import RULE as LINKS
from rulecheck.docs.rules.refs import RULE as REFS
from rulecheck.docs.rules.symbols import RULE as SYMBOLS
from rulecheck.docs.rules.template import MODULE_RULE as MODULE
from rulecheck.docs.rules.template import RULE as TEMPLATE
from rulecheck.docs.rules.terms import RULE as TERMS

LAYERING.selftest = selftest.selftest

RULES = [LAYERING, LINKS, REFS, SYMBOLS, TEMPLATE, MODULE, TERMS, GLOSSARY]
