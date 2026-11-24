# ============================================================================
# 工具 / H 规则检查目标（root makefile 只做总控）
#
# 这里只放“脚本调用”：H41/H50/H51 统一 runner、文档 / 台账 / rules / 审查
# 标记检查、compile-commands / tidy。编译、链接、format 留在根 makefile。
# 加新检查 = 本文件加 target + 根 makefile 的 check 组加一行。
# ============================================================================
PYTHON ?= python3

.PHONY: check-format check-c check-docs check-meta
check-format: format-check
check-c: c-rules-check c-rules-test
check-docs: docs-check docs-test
check-meta: ledger-check ledger-test rules-check rules-test review-marks-check review-marks-test

# ============================================================================
# 编辑 / 静态分析辅助（不挂 check；依赖外部工具）
# ============================================================================
.PHONY: compile-commands tidy

# 从 `make -B -n V=1 all` 的编译命令行重建 compile_commands.json（.vscode /
# clangd 使用）。不依赖 bear，也不实际编译；工具本身在 tools/gen_compile_commands.py。
compile-commands:
	@command -v $(PYTHON) >/dev/null 2>&1 || { \
		echo "错误：未找到 $(PYTHON)"; \
		exit 1; \
	}
	@$(PYTHON) $(PROJECT_ROOT)/tools/gen_compile_commands.py --root $(PROJECT_ROOT)

# clang-tidy 与 .clang-format 同用 v15；依赖 compile_commands.json。
# 不挂 check：CI 不装 clang-tidy，版本漂移也不应阻塞构建。
TIDY_RUNNER ?= run-clang-tidy-15
tidy: compile-commands
	@command -v $(TIDY_RUNNER) >/dev/null 2>&1 || { \
		echo "错误：未找到 $(TIDY_RUNNER)"; \
		exit 1; \
	}
	@$(TIDY_RUNNER) -p $(PROJECT_ROOT) '^$(PROJECT_ROOT)/src/.*\.c$$'

# ============================================================================
# H 规则统一入口（H41 / H50 / H51）；旧目标是兼容别名。
# ============================================================================
C_RULES_CHECK := $(PROJECT_ROOT)/tools/check_c.py

# ============================================================================
# H50 复合类型定义形态检查
# ============================================================================

.PHONY: types-check
types-check:
	@command -v $(PYTHON) >/dev/null 2>&1 || { \
		echo "错误：未找到 $(PYTHON)"; \
		exit 1; \
	}
	@$(PYTHON) "$(C_RULES_CHECK)" --root $(PROJECT_ROOT) --rule H50

# 检查器自检（P13/C7）：正例必须通过，每条负例必须报错。
# 区分力证据在 tools/test_check_types.py，不依赖 pytest。
.PHONY: types-test
types-test:
	@command -v $(PYTHON) >/dev/null 2>&1 || { \
		echo "错误：未找到 $(PYTHON)"; \
		exit 1; \
	}
	@$(PYTHON) "$(C_RULES_CHECK)" --root $(PROJECT_ROOT) --rule H50 --self-test

# ============================================================================
# H51 文件内布局检查
#   - .c：include / 宏 / 类型 / 文件级变量必须在首个函数/原型之前；main 最后；
#   - .h：include / 宏 / 类型 / extern 变量必须在首个原型之前；只允许 static
#     inline 快路径；禁止 #pragma once；include guard 由 H12/headers 检查覆盖；
#   - 短函数在长函数之后、入口型原型在查询原型之前，报告为 warning。
# ============================================================================
.PHONY: layout-check layout-report layout-test
layout-check:
	@$(PYTHON) "$(C_RULES_CHECK)" --root $(PROJECT_ROOT) --rule H51

layout-report:
	@$(PYTHON) "$(C_RULES_CHECK)" --root $(PROJECT_ROOT) --rule H51 --report

layout-test:
	@$(PYTHON) "$(C_RULES_CHECK)" --root $(PROJECT_ROOT) --rule H51 --self-test

# H 规则统一入口（H41 / H50 / H51）；后续新规则只加 rulecheck/textual/ 模块。
.PHONY: c-rules-check c-rules-test
c-rules-check:
	@$(PYTHON) "$(C_RULES_CHECK)" --root $(PROJECT_ROOT) --all

c-rules-test:
	@$(PYTHON) "$(C_RULES_CHECK)" --root $(PROJECT_ROOT) --all --self-test

# ============================================================================
# 文档分层检查（doc policy）
#   - design 文档不得携带平台实现、代码坐标、日期/测试快照、风险/归档引用；
#   - 例外用 <!-- doc-policy: allow ... --> 显式标注；
#   - 规则见 docs/README.md「文档分层规则」。
# 已挂进 make check；不挂 make all（构建高频，文档检查放在聚合检查里）。
# ============================================================================
DOCS_CHECK := $(PROJECT_ROOT)/tools/check_docs.py

.PHONY: docs-check
docs-check:
	@$(PYTHON) $(DOCS_CHECK) --root $(PROJECT_ROOT)

.PHONY: docs-report
docs-report:
	@$(PYTHON) $(DOCS_CHECK) --root $(PROJECT_ROOT) --report

.PHONY: docs-test
docs-test:
	@$(PYTHON) $(DOCS_CHECK) --self-test

# ============================================================================
# 开放式 / 已闭合台账一致性检查
#   - 0.1 状态统计 / 0.6 按模块统计 / 0. 概览由脚本生成，禁止手改；
#   - 开放式台账的模块索引不得出现「已闭合 / 不实现」（附录 A.5）；
#   - 同一 ID 在两个台账合起来不得重复登记。
# 已挂 make check；不挂 make all（构建高频）。
# ============================================================================
LEDGER_CHECK := $(PROJECT_ROOT)/tools/check_ledger.py

.PHONY: ledger-check
ledger-check:
	@$(PYTHON) $(LEDGER_CHECK) --root $(PROJECT_ROOT) --check

.PHONY: ledger-write
ledger-write:
	@$(PYTHON) $(LEDGER_CHECK) --root $(PROJECT_ROOT) --write

.PHONY: ledger-test
ledger-test:
	@$(PYTHON) $(LEDGER_CHECK) --self-test

# ============================================================================
# CLAUDE.md / rules / skills 分层检查
#   - CLAUDE.md 体积预算与 R/C/P 覆盖；
#   - .claude/rules/*.md 必须有 paths:，H/S 覆盖在 c-coding.md；
#   - skill 统一 basessd- 前缀、frontmatter 一致、< 500 行；
#   - 关键路径存在。
# 已挂 make check；不挂 make all（构建高频）。
# ============================================================================
RULES_CHECK := $(PROJECT_ROOT)/tools/check_rules.py

.PHONY: rules-check
rules-check:
	@$(PYTHON) $(RULES_CHECK) --root $(PROJECT_ROOT) --check

.PHONY: rules-test
rules-test:
	@$(PYTHON) $(RULES_CHECK) --root $(PROJECT_ROOT) --self-test

# ============================================================================
# 审查标记检查（#优化# / #决策# / #讨论# / #重构# / #未验# / #缺陷# / #标识保留#）
#   - 设计文档行首 / C 注释体第一段；
#   - 文档中的 #缺陷# 必须带台账 ID；代码 / 测试不带；
#   - 已挂 make check；不挂 make all（构建高频）。
# ============================================================================
REVIEW_MARKS := $(PROJECT_ROOT)/.claude/skill-collection/skills/review/basessd-review/scripts/check_review_marks.py

.PHONY: review-marks review-marks-check review-marks-strict review-marks-test
review-marks:
	@$(PYTHON) "$(REVIEW_MARKS)" --root $(PROJECT_ROOT) --report

review-marks-check:
	@$(PYTHON) "$(REVIEW_MARKS)" --root $(PROJECT_ROOT) --check

review-marks-strict:
	@$(PYTHON) "$(REVIEW_MARKS)" --root $(PROJECT_ROOT) --strict

review-marks-test:
	@$(PYTHON) "$(REVIEW_MARKS)" --self-test

# ============================================================================
# H41 头文件检查
#   - 项目头一律裸名：目录前缀 include 会因 -I 顺序产生静默误解析；
#   - include/ 与 src/ 的私有头不得同名（同一裸名两套语义）。
# 第三方位（third_party/）按 H41 豁免，不扫描。
# ============================================================================
.PHONY: headers-check
headers-check:
	@$(PYTHON) "$(C_RULES_CHECK)" --root $(PROJECT_ROOT) --rule H41

