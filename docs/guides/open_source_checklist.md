# 开源整理与发布步骤

> **性质**：维护者操作清单——记录仓库发布前的整理步骤，不是项目使用指南。

> **用途**：把当前仓库整理到可公开状态。先处理**发布阻塞项**（凭据、许可证、第三方资料、
> README），再做代码与文档的结构性清理；最后用干净克隆复测。
>
> **边界**：本清单只做“发布整理”，不改变固件行为。任何删除都必须先有“零引用”证据，
> 并跑 `make all` / `make release` / `make tests UT=FTL`（R2）；提交与推送仍由用户发话（R3）。
>
> **审计基线**：2026-11-17。执行前用各步骤里的命令复测；代码与文档会变，数字只作起点。
>
> **关联登记**：`docs/design/known_open_items.md` 的 `RISK-REL-01` ~ `RISK-REL-05`。

---

## 目录

1. [使用方式与状态词](#1-使用方式与状态词)
2. [审计基线](#2-审计基线)
3. [P0：公开前必须完成](#3-p0公开前必须完成)
4. [P1：结构与可读性](#4-p1结构与可读性)
5. [P2：可选优化](#5-p2可选优化)
6. [发布前最终检查](#6-发布前最终检查)
7. [回滚与注意事项](#7-回滚与注意事项)
8. [执行状态](#8-执行状态)

---

## 1. 使用方式与状态词

- **按 ID 顺序执行**，不要跳 P0 直接做 P1。P0 未全部通过，不向公开远端推送。
- 每个步骤都有「动作」「验收」「回退」三栏。验收必须给**可判定证据**，不接受“应该没问题”（R1）。
- 状态词只有四个：`待办` / `进行中` / `已完成` / `不适用`。
  `已完成` 必须在记录表里附命令输出、commit 或串口证据；只改代码不跑验收 = `进行中`。
- 执行人：
  - **用户**：token 轮换、许可证选择、公开推送、仓库设置。
  - **我（Claude Code）**：只读审计、批量删除/改名、文档改写、构建复测。
  - 涉及删除 API、改介质格式、改语义的步骤不在本清单内；那些按 R7 / P10 单独确认。

### 1.1 建议执行顺序

```text
OS-00 备份与冻结
OS-01 凭据清理          ← 最高优先级
OS-02 LICENSE 与第三方声明
OS-03 论文/网页存档移出
OS-04 README 重写
OS-05 死代码清理
OS-06 遗留 test / 机器相关脚本
OS-07 过期文档与失效链接
OS-08 文档分层（P1）
OS-09 AI 工具配置范围（P1）
OS-10 CI（P1）
OS-11 发布（P1）
OS-12/13 可选优化
```

---

## 2. 审计基线

> 以下数字来自 2026-11-17 的只读审计；执行前先复测。**不要把数字写进 README**，
> README 只写当前事实与状态。

| 项 | 基线 | 复测命令 / 判据 |
|---|---|---|
| 本地目录 | 约 905 MB | `du -sh --exclude=.git .` |
| 其中 `build/` | 约 870 MB，已被 `.gitignore` | `du -sh build`；`git check-ignore build` |
| Git 跟踪总量 | 34.5 MB | `git ls-files -z \| xargs -0 du -cb \| tail -1` |
| `third_party` | 18.4 MB | `git ls-files -z third_party \| xargs -0 du -cb \| tail -1` |
| `docs` | 11.6 MB（含 7.3 MB PDF/HTML） | `git ls-files -z docs \| xargs -0 du -cb \| tail -1` |
| 源码 | 79,992 行（`src/` + `include/`） | `find src include -name '*.[ch]' -print0 \| xargs -0 cat \| wc -l` |
| 文档 | 55,930 行 Markdown | `find docs -name '*.md' -print0 \| xargs -0 cat \| wc -l` |
| 测试 | 25,570 行（`tests/`） | `find tests -type f -print0 \| xargs -0 cat \| wc -l` |
| 死代码 | 约 10,706 行，零外部引用 | 见 OS-05 的 grep 与 ELF 符号检查 |
| `docs/archive` | 105 个 md，23,527 行 | `find docs/archive -name '*.md' \| wc -l` |
| 公开阻塞 | 凭据、无 LICENSE、7 PDF + 9 HTML、README 290 B | 见 OS-01 ~ OS-04 |

**已有事实（不是猜测）**：

- 现有 `build/release/bin/release.elf` 中，`ds_list_create` / `twoSum` /
  `bubble_sort` 等 dead 符号**都不存在**；`--gc-sections` 已丢弃整块。删除死代码
  **不改固件行为**，只减少编译与阅读成本。
- `test/app` 是 **aarch64 ELF**，`test/test.s` 是 AArch64 汇编；目标芯片是 Cortex-M7，
  且构建系统对 `test/` 零引用。
- `README.md` 指向 `docs/design/wsl_debug_environment.md`，实际路径已是
  `docs/guides/wsl_debug_environment.md`。
- `docs/claude_code_usage.md` 含真实 token；该文件由提交 `3ee9ef9` 引入。
- 根目录没有 `LICENSE` / `COPYING`；`third_party/stm32h743` 也没有 LICENSE 文件，
  但 ST HAL 头文件声明“license terms in the LICENSE file”。
- `third_party/tinyusb/LICENSE` 存在（MIT）。
- `docs/reference/papers/` 7 个 PDF、`docs/reference/pages/` 9 个 HTML；再分发权未确认。

---

## 3. P0：公开前必须完成

### OS-00 备份与冻结

**目标**：把当前状态留一份可回退的完整备份；在独立分支上做整理。

**动作**：

```bash
git status --porcelain
git switch -c open-source-prep
git bundle create /tmp/soft_ssd-before-open-source.bundle --all
git rev-parse HEAD > /tmp/soft_ssd-before-open-source.commit
```

**验收**：`git bundle verify /tmp/soft_ssd-before-open-source.bundle` 通过；
工作树无未提交改动。

**回退**：从 bundle 克隆或 `git bundle unbundle`；不要在没有备份时跑 history rewrite。

**执行人**：我（可执行）；备份文件位置由用户确认。

---

### OS-01 凭据清理（最高优先级）

**目标**：让公开历史里**不存在**任何可用凭据。

**已知问题**：

- `docs/claude_code_usage.md` 含 `ANTHROPIC_AUTH_TOKEN` 形态的真实值。
- 引入提交：`3ee9ef9`（`docs: 新增 Claude Code 使用说明（重点：模型切换）`）。
- 只在新提交里删除文件**不够**：旧 commit 仍可 checkout 到该文件。

**动作（按顺序）**：

1. **用户**：立即到凭据提供方（火山方舟控制台）**吊销/轮换**该 token。
   无论它是否只是示例，都按“已泄露”处理。
2. 从工作树删除离题文档（**提交需用户发话**，R3）：

```bash
git rm docs/claude_code_usage.md
git commit -m "chore: remove off-topic credential-bearing doc"
```

3. **本仓采用的路径**：私有仓继续开发，**历史先不动**；迁移到新公开仓库时用下面二选一，
   确保新仓库的**历史里没有这个文件**。

   **A. squash 首提交（推荐，最简单）**：公开仓只有一个「当前树」提交，旧历史不随行。

```bash
# 在私有仓内新建无父提交的发布分支
git switch --orphan public-main
git add -A
git commit -m "Initial public release"
# 推到新建的空公开仓库
git remote add public <公开仓库 URL>
git push -u public public-main:main
```

   **B. 克隆副本上 filter-repo（想保留逐版本历史时）**：本机未装，需先 `pip install git-filter-repo`。

```bash
git clone --no-local . /tmp/soft_ssd-public
cd /tmp/soft_ssd-public
git filter-repo --path docs/claude_code_usage.md --invert-paths
git remote add origin <公开仓库 URL>
git push -u origin main
```

> 方案 B 会改写全部 commit hash（本仓约 389 个），文档里引用的旧 hash 在公开仓中失效。
> 无论 A/B，旧历史仍留在私有仓；`git clone --no-local` 得到的是独立副本，不影响原仓。

4. **禁止**：把现有私有仓直接改成 public，或把现有历史直接 `git push --all` 到新公开仓 ——
   两者都会把 `3ee9ef9` 一起带出去。

**验收**（全部为 0 或空）：

```bash
git log --all --oneline -- docs/claude_code_usage.md
git grep -nE '(ark-[0-9a-f]{8}-[0-9a-f-]{27}|sk-ant-[A-Za-z0-9_-]{20,}|sk-[A-Za-z0-9]{32,})'
git log --all -G 'ark-[0-9a-f]{8}-[0-9a-f-]{27}' --oneline
```

**回退**：从 `/tmp/soft_ssd-before-open-source.bundle` 恢复。
**注意**：如果仓库已经在公开远端存在，优先考虑“新建公开仓库 + 当前树 squash 首提交”而不是
在旧仓库上强推；两种方案都要先轮换 token。

**执行人**：用户（轮换 / 推送）；我（删除与改写命令）。

---

### OS-02 LICENSE 与第三方声明

**目标**：公开仓库有明确的项目许可证，且不误覆盖 vendor 代码。

**动作**：

1. **用户选择**项目许可证（这是法律决定，不由我代选）。建议三选一：

| 许可证 | 适用场景 | 备注 |
|---|---|---|
| `Apache-2.0`（推荐） | 希望保留专利授权、允许商用 | 与 TinyUSB MIT、ST BSD-3-Clause、CMSIS Apache-2.0 兼容 |
| `MIT` | 最简、最宽松 | 同样兼容现有依赖 |
| `GPL-3.0` | 要求衍生作品继续开源 | 会限制部分商用；需确认 vendor 许可兼容性 |

2. 根目录新增 `LICENSE`（许可证原文，不改写）。
3. 新增 `THIRD_PARTY_NOTICES.md`（或 `NOTICE`），逐项列出：

| 组件 | 位置 | 许可证 | 处理 |
|---|---|---|---|
| TinyUSB 0.21.0 | `third_party/tinyusb/` | MIT | 保留 `third_party/tinyusb/LICENSE`，在 NOTICE 引用 |
| STM32H743 HAL | `third_party/stm32h743/hal/` | ST 上游 LICENSE（BSD-3-Clause 或 SLA0044，以你获取的包为准） | 补 `third_party/stm32h743/LICENSE` 原文 |
| CMSIS Core / Device | `third_party/stm32h743/cmsis-*` | Apache-2.0（头文件 SPDX 已声明） | 保留 SPDX 头 + 在 NOTICE 引用 |
| munit | `tests/framework/munit.*` | 上游许可证 | 在 NOTICE 引用 |

4. `README.md` 增加 License 段，指向 `LICENSE` 与 `THIRD_PARTY_NOTICES.md`。

**验收**：

```bash
git ls-files | grep -iE 'LICENSE|NOTICE|COPYING'
head -20 LICENSE
git grep -n 'SPDX-License-Identifier' third_party/stm32h743/cmsis-core | head
```

**注意**：不要给 vendor 文件换头、改版权行；不要用一个根 LICENSE 声称覆盖 ST/CMSIS/TinyUSB。

**执行人**：用户（选许可证）；我（补 NOTICE、README 链接）。

---

### OS-03 论文与网页存档移出公开仓库

**目标**：不在公开仓库再分发版权状态不明的 PDF/HTML。

**状态**：2026-11-17 用户裁决**移出仓库**，已执行。`git rm` 7 PDF + 9 HTML；
本地副本在 `~/.local/share/soft_ssd-backup/reference-20261117/`（仓外，7.3 MB）；
`docs/reference/README.md` 已改为链接清单（题名 / 出处 / URL / 用途），
`docs/README.md`、`usb_msc.md`、`ftl_design.md`、`ftl_revision_history.md`、`known_open_items.md`
中的本地路径引用已改为 URL。同批移出 `docs/reference/README.md`
（Micron 数据手册原文摘录）。改动在工作树，**未提交**（R3）。

**动作**：

```bash
git rm docs/reference/papers/*.pdf
git rm docs/reference/pages/*.html
```

把 `docs/reference/README.md` 改成**链接清单**：题名、会议/期刊、arXiv/URL、用途。
本地副本保留在仓库外（或私人分支），不要进入公开历史。

**验收**：

```bash
git ls-files docs/reference | grep -E '\.(pdf|html)$'   # 应为空
git ls-files -z docs/reference | xargs -0 du -cb | tail -1  # 应只剩 md
```

**注意**：ACM / IEEE / USENIX 论文通常不允许随仓库再分发；即使站点当前可公开访问，
也不等于允许镜像。若你已取得书面再分发许可，在 `docs/reference/README.md` 注明来源与许可。

**执行人**：我（删除）；用户（确认是否有再分发许可）。

---

### OS-04 README 重写

**目标**：第一次打开仓库的人能在 5 分钟内知道“这是什么、怎么编译、验到什么程度、哪里有限制”。

**动作**：重写根 `README.md`，至少包含：

1. **一句话定位**：STM32H743 + MT29F4G08 的 DRAM-less 软 SSD 固件，HIL / FTL / FCC 分层。
2. **架构**：一页图或表，指向 `docs/architecture.md` / `docs/design/hil_design.md` /
   `docs/design/ftl_design.md` / `docs/design/fc_nand_design.md`。
3. **硬件假设**：MCU、NAND、FMC bank、USB、UART、调试器；指向 `docs/hardware_contract.md`。
4. **构建**：工具链、依赖、`make all` / `make release` / `make tests UT=FTL`。
5. **烧写与上板**：JLink 链路、串口 CLI 入口；指向 `docs/guides/wsl_debug_environment.md`。
6. **状态表**（按 R1 如实写）：

| 项目 | 状态 | 证据 / 说明 |
|---|---|---|
| debug / release 构建 | 已验证 / 未验证（按当前事实） | 构建命令与告警数 |
| UT 编译 | 已验证 / 未验证 | `make tests UT=...` |
| 板端回归 | 已验证项逐条列出 | 串口日志 / 测试记录 |
| `CONFIG_FC_RAID` | 关闭，未验证 | `include/fc/fc.h` |
| IWDG | 关闭，待上板决策 | `src/main.c` |
| read recovery `0x89` | 保留地址，空转，不处理 | `src/ftl/read_recovery.c` |

7. **Known limitations**：DRAM-less、USB MSC BOT（无 NVMe SQ/CQ/doorbell）、
   无原生多队列、RAID 关闭、看门狗关闭等。
8. **文档地图**：指向 `docs/README.md`，只列 6~8 个首要入口。
9. **License**：指向 `LICENSE` 与 `THIRD_PARTY_NOTICES.md`。
10. **安全/联系**：报告方式（issue 或邮箱，按用户选择）。

**验收**：

- 干净克隆后按 README 命令能完成构建三关。
- `README.md` 里不存在 `docs/design/wsl_debug_environment.md` 这类失效路径。
- 状态表不含“应该没问题 / 已完成”这类无证据措辞（R1）。

**执行人**：我（起草）；用户（确认状态与联系方式）。

---

### OS-05 死代码清理

**目标**：删掉与 SSD 固件无关、且零引用的 `src/common` 模块。

**删除对象**：

```bash
git rm -r src/common/data_structure src/common/algorithm
git rm -r include/common/data_structure
git rm src/common/sort.c
git rm config/controller_config.h
```

同时改 `src/common/makefile.mk`：移除 `COMMON_MODS = algorithm data_structure` 与对应的
`include $(foreach ...)` 行；`MODULE_SRCS += $(wildcard $(SRC_ROOT)/common/*.c)` 保留
（它继续收集 `assertion.c` / `log.c` / `crash.c` 等在用的公共模块）。

**依据（删除前先复测）**：

```bash
# 1) 模块外零引用
git grep -nE '\bds_(list|slist|deque|array|rbtree|pqueue|queue|stack|ring)_s\b' -- src include tests
git grep -nE '\b(bubble_sort|selection_sort|insertion_sort|shell_sort|quick_sort_optimize|merge_sort_iterative)\s*\(' -- src include tests
git grep -nE '\b(twoSum|threeSum|romanToInt|addBinary|reverseBits)\s*\(' -- src include tests

# 2) 最终 ELF 无符号（现有 release 产物已证）
arm-none-eabi-nm build/release/bin/release.elf | grep -E 'ds_list_create|twoSum|bubble_sort'
```

三项引用检查应为空；ELF 检查应为空。`src/common/algorithm/*.c` 是 LeetCode 式练习代码
（`twoSum` / `romanToInt` / `addBinary`），不属于主控固件。

**验收**：

```bash
test ! -d src/common/data_structure
test ! -d src/common/algorithm
test ! -e src/common/sort.c
make all
make release
make tests UT=FTL
```

要求零新告警（R2）；`make all` 内含 `types-check`，输出 `types-check: OK`。
删除只涉及零引用符号，不改建链 / 位图 / 地址；**不需要上板**。若构建或链接出现任何
非预期差异，停下查，不要继续删。

**执行人**：我；提交由用户发话。

---

### OS-06 遗留 `test/` 与机器相关脚本

**目标**：去掉与目标平台无关的残留，以及写死本机路径的脚本。

**动作**：

1. 删除遗留测试目录（真正的测试是 `tests/`）：

```bash
git rm -r test
```

2. 删除个人启动器：

```bash
git rm sh/open-ubuntu.bat
```

3. `sh/debug_env.sh` 二选一：
   - **推荐**：改成模板，JLink 路径由环境变量提供：
     `JLINK_EXE="${JLINK_EXE:-JLinkGDBServerCL.exe}"`，并在注释里给 Windows 示例；
   - 或删除脚本，保留 `docs/guides/wsl_debug_environment.md` 的手动步骤。
   同时更新该指南里写死的 `<JLink 安装目录>` 示例。

4. 删除未使用的格式配置：

```bash
git rm .clang-format_array
```

并删掉 `makefile` 中被注释的 `FORMAT_CONFIG := .clang-format_array` 行（H5）。

5. `.vscode/launch.json`：检查是否含本机绝对路径；有则改成 `${workspaceFolder}` 模板。
   它目前**已被跟踪但被 `.gitignore` 声明忽略**，二选一：保留通用模板并删除 ignore 行，
   或 `git rm --cached .vscode/launch.json` 并保留 ignore。

6. `sh/list-dir.bat` / `sh/extract_func_decl.sh` 是通用工具，可保留；
   `sh/flash.jlink` 已被忽略，保持本地即可。

**验收**：

```bash
git grep -nE '/mnt/|/home/|C:\\\\|D:\\\\|<user>' -- ':!docs/archive' ':!docs/guides'
find . -maxdepth 2 -name '*.bat' -o -name '*.jlink'
```

预期只剩通用示例或无命中；不能有个人目录、个人用户名、本机 JLink 安装路径。

**执行人**：我；脚本泛化后由用户确认调试链路仍可用。

---

### OS-07 过期文档与失效链接

**目标**：删除/重写“桩文档”和已过期叙述，避免公开后第一眼就是错信息。

**动作**：

1. `docs/nand_model.md`（5 行）：并入 `docs/hardware_contract.md` 或删除。
2. `docs/ftl_design.md`（8 行）：只是指向 `docs/design/ftl_design.md` 的桩，且正文写
   “已作废”；删除桩，更新所有引用。
3. `docs/architecture.md`（28 行）：重写为当前架构一页纸；删除“NVMe 主机层已于 v0.47 删除”
   这类变更史（H37），把 HIL 现状改为 USB MSC + UART CLI，并链接 `docs/design/hil_design.md`。
4. `docs/design/ut_tree_restructure.md`（116 行）：历史计划，移到 `docs/archive/`。
5. `README.md` 的 `docs/design/wsl_debug_environment.md` 改为
   `docs/guides/wsl_debug_environment.md`。
6. `docs/README.md`：删除 `claude_code_usage.md` 行；增加本清单入口；更新 `guides/` 描述。

**状态**：1~6 **全部已执行**。`architecture.md` 已重写；`ut_tree_restructure.md` 已 `git mv` 进
`docs/archive/`；README 与 `docs/README.md` 链接已更新；`nand_model.md` 内容并入
`hardware_contract.md` §NAND device model 后与 `ftl_design.md` 桩一并 `git rm`。

**验收**：

```bash
git grep -n 'docs/design/wsl_debug_environment.md' -- README.md docs/*.md docs/design/*.md docs/guides/*.md
git grep -n 'docs/ftl_design.md' -- README.md docs/*.md docs/design/*.md
```

第一条应为空（archive 历史记录可保留）；第二条应只指向 `docs/design/ftl_design.md` 自身。

**执行人**：我。

---

## 4. P1：结构与可读性

### OS-08 文档分层

**目标**：公开读者先看到“主干”，工程内部账本仍可查，但不淹没入口。

**动作（低链接成本方案）**：

1. 不移动 `known_open_items.md` / `known_open_items.md`：`CLAUDE.md` 与大量文档都按现路径引用，
   移动会产生大面积链接回扫。改为在 `docs/README.md` 明确标注：
   - **公开主干**：`architecture.md`、`hardware_contract.md`、`design/ftl_design.md`、
     `design/fc_nand_design.md`、`design/hil_design.md`、`design/usb_msc.md`、
     `design/storage_controller_design.md`。
   - **工程内部账本**：`known_open_items.md`、`known_open_items.md`、revision history、negative coverage、
     `docs/archive/`；保留路径，仅供维护者。
2. 负例测试三件套不合并成单文档，而按**层次**并入：
   - 通用框架（判据、注入 API、seam 选型、用例模板、验收）→
     `docs/guides/testing_guide.md` §12；
   - 未闭合风险的判定台账（M/N/K）→ 各层 UT 设计文档 §7.2
     （`ftl_ut_design.md` / `fc_nand_ut.md` / `hil_ut_design.md`）；
   - `negative_deferred_eval.md` 的 K 行可行性理由已并入各层台账，原文件不再保留。
   原三件套正文不入 `docs/archive/`：其中的判定结论已按当前状态重新落盘，
   历史执行记录见 `docs/archive/known_risks_batches.md`。
3. 给 `docs/design/` 增加一个 `README.md`（阅读顺序 + 哪些是内部账本），不复制设计内容。
   **已建立**：`docs/design/README.md`（阅读顺序表 + 内部账本清单）。
4. 更新所有被移动文档的链接；按 P2 全路径引用。

**验收**：

```bash
git grep -n 'negative_test_framework.md|negative_coverage.md|negative_deferred_eval.md' -- docs README.md
# 活跃文档中应只剩新文档/归档说明的引用
```

`docs/README.md` 的“按任务该读哪几个”表仍能覆盖 FTL / FC / HIL / UT / 调试 / 风险。

**执行人**：我；合并后由用户确认没有丢判定结论。

---

### OS-09 AI 工具配置范围

**目标**：决定 `.claude/`、`.codex/`、`CLAUDE.md` 是否随公开仓库发布。

**现状**：

- `CLAUDE.md` 618 行，是本仓的工程规则与判据，公开价值高，但含内部 risk ID 与工作流。
- `.claude/skills/` 5 个文件，是 Claude Code 的项目技能。
- `.codex/skills/c-coding-standard/SKILL.md` 是 C 规范来源，`CLAUDE.md` 多处引用。

**建议**：

- **保留** `CLAUDE.md` 在根目录，并在 README 里一句话说明“这是项目的工程规则/评审判据，
  不是用户手册”。
- `.claude/` 与 `.codex/` 若公开价值低，可移出公开仓库；但移出前必须回扫 `CLAUDE.md` 的
  引用，改成“内部参考”或不写路径。
- 不要为了“看起来干净”删掉 `CLAUDE.md` 的规则正文；它是本项目区别于普通 demo 的部分。

**裁决（2026-11-17）**：用户选择**全部保留**（`CLAUDE.md` + `.claude/skills/` + `.codex/skills/`）。
理由：三者互相引用，全部保留时引用自洽；`.claude/settings.local.json` 已在 `.gitignore` 中，
不入库。

**验收**：

```bash
git grep -n '\.codex/skills|\.claude/skills' -- CLAUDE.md docs
git grep -nE '/home/|/mnt/|C:\\\\|D:\\\\' -- .claude .codex CLAUDE.md
```

第二条应为空；第一条若保留目录则指向存在的文件。

**执行人**：用户决策；我执行移动/链接回扫。

---

### OS-10 CI

**目标**：让公开仓库能证明“干净克隆可以构建”，而不是只靠作者本机。

**动作**：新增 `.github/workflows/build.yml`，示例：

```yaml
name: build

on:
  push:
  pull_request:

jobs:
  build:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - name: Install toolchain
        run: |
          sudo apt-get update
          sudo apt-get install -y gcc-arm-none-eabi make clang-format python3
      - name: Debug build
        run: make all
      - name: Release build
        run: make release
      - name: FTL UT compile
        run: make tests UT=FTL
```

**注意**：

- `make all` 前置 `format`，CI 必须装 `clang-format`；`types-check` 需要 `python3`。
- `make tests` 只证链接，不证正确；CI 不跑板，README 里必须写清“UT 上板仍需硬件”。
- 如果仓库不使用 GitHub Actions，跳过本步并在 README 说明“本项目未接 CI”。

**验收**：workflow 在干净 clone 上通过；失败时先修构建，不要用 `--no-verify` 绕过。

**执行人**：我（起草 workflow）；用户（启用 Actions）。

---

### OS-11 发布

**目标**：打出可复现的公开版本，并留下发布记录。

**动作**：

1. 完成 OS-00 ~ OS-10 后，在 `main` 上打标签：

```bash
git tag -a vX.Y.Z -m "soft_ssd vX.Y.Z: <一句话范围>"
```

2. 写 release notes：范围、已知限制、上板验证项、构建命令、许可证。
3. 公开推送（用户发话）：

```bash
git push origin main
git push origin vX.Y.Z
```

4. 用干净克隆做一次最终构建（见 §6）。

**验收**：公开仓库的 README 与 release notes 一致；标签指向的提交能通过 CI。

**执行人**：用户（推送 / 发布）；我（准备标签与说明草稿）。

---

## 5. P2：可选优化

### OS-12 vendor 裁剪（可选）

**目标**：缩小 `third_party/stm32h743`，但不增加升级成本到不可维护。

**做法**：

- 以 `third_party/stm32h743_vendor.mk` 的 `STM32H743_SRCS` 为唯一编译清单；
- 只删除**未被清单引用**的 `hal/src/*.c`，保留全部 `hal/inc` 头与 CMSIS；
- 保留 vendor LICENSE 与升级说明；不要手改上游源文件。

**验收**：干净构建三关通过；vendor 许可文件仍在；README 记录“裁剪依据 = vendor.mk”。

**建议**：不是发布阻塞项。没有明确的仓库体积目标时，**不做**。

---

### OS-13 大文件拆分（默认不做）

`src/ftl/ftl.c`（4,035 行 / 79 个函数）、`src/hil/cli.c`（2,683 行 / 80 个函数）看起来大，
但没有证据表明是重复实现。按 H49 / C2：

- **行数不是拆分判据**；判据是第二个消费者、或封装住的不变量/边界。
- 公开仓库的读者体验可以通过 README / 设计文档 / 目录导航改善，不靠切碎模块。
- 只有在实际出现“同一职责被复制到第二个文件”时才拆。

---

## 6. 发布前最终检查

在**干净克隆**里执行，不要在当前工作树里“看起来通过”：

```bash
git clone <repo-url> /tmp/soft_ssd-public
cd /tmp/soft_ssd-public

# 1) 构建三关（零新告警）
make all
make release
make tests UT=FTL

# 2) 仓库卫生
git status --porcelain
git ls-files | grep -E '\.(o|elf|map|hex|bin|pdf|html)$'
git grep -nE '(ark-[0-9a-f]{8}-[0-9a-f-]{27}|sk-ant-[A-Za-z0-9_-]{20,}|sk-[A-Za-z0-9]{32,})'
git grep -nE '/mnt/|/home/|C:\\\\|D:\\\\|<user>'
git grep -nE 'TODO|FIXME|XXX|HACK'

# 3) 体积与对象
git count-objects -vH
git ls-files -z | xargs -0 du -cb | tail -1
```

判据：

- 构建三关通过，无新告警；`make all` 输出 `types-check: OK`。
- 第 2 组命令：构建产物/PDF/HTML 应为空；凭据与个人路径应为空（通用示例除外）；
  `TODO/FIXME` 应为 0（当前基线已是 0）。
- `LICENSE` / `THIRD_PARTY_NOTICES.md` 存在且内容完整。
- README 的状态表逐项有证据；未上板项明确写“未上板”（R1）。
- `docs/reference` 只剩链接清单，不含 PDF/HTML。
- 如果本次整理碰了建链 / 位图 / 地址，按 R2 上板；纯删除/文档整理不需要。

---

## 7. 回滚与注意事项

### 7.1 history rewrite 的代价

- `git filter-repo` 会改写全部受影响的 commit hash；协作者必须重新克隆。
- 改写前必须有 `git bundle` 备份；改写后不要立刻删除备份。
- 如果仓库已公开，强推不能撤回别人已拉取的副本；token 必须轮换，不能只靠删历史。

### 7.2 不要做的事

- **不要**只在新提交里删 `docs/claude_code_usage.md` 就认为 token 安全。
- **不要**在没有 LICENSE 的情况下公开；也不要用一个根 LICENSE 覆盖 vendor。
- **不要**把 `docs/reference` 的 PDF/HTML 一起公开，除非能出示再分发许可。
- **不要**为了“看起来干净”删 `docs/design/known_open_items.md` / `known_open_items.md` / 设计文档；
  它们是本项目的工程记录与差异化资产。
- **不要**按行数拆分 `ftl.c` / `cli.c`；没有第二消费者或封装不变量时，拆了只会增加阅读成本。
- **不要**在公开后随意 force-push；需要重写历史时先与用户确认并通知已克隆者。
- **不要**声称“全部验证通过”；README 只写有证据的上板项（R1）。

### 7.3 回退点

| 回退点 | 方式 |
|---|---|
| 整理前完整状态 | `/tmp/soft_ssd-before-open-source.bundle` |
| 单步删除 | `git restore --source <commit> -- <path>`（未提交时） |
| history rewrite | 从 bundle 重新克隆，丢弃改写分支 |
| 已公开后发现问题 | 新提交修复；不要在公开主线上强推，除非用户明确要求 |

---

## 8. 执行状态

> 详细执行证据、2026-11-17 审计快照和旧计数已移入
> [`../archive/open_source_audit_20261117.md`](../archive/open_source_audit_20261117.md)。
> 本节只保留当前结论；状态更新必须附 commit / 命令 / 日志证据（R1）。

| ID | 状态 | 当前结论 |
|---|---|---|
| OS-00 | 已完成 | bundle + `.commit` 已生成 |
| OS-01 | 进行中 | `docs/claude_code_usage.md` 已删除、旧 token 已吊销；**历史 squash 待公开前执行**（用户裁决暂不公开） |
| OS-02 | 已完成 | `e6414b7`（Apache-2.0 / NOTICE / μnit COPYING）+ `62c4b18`（ST 组件许可原文） |
| OS-03 | 已完成 | `62c4b18`：7 PDF + 9 HTML 已移出，`docs/reference` 只剩链接清单 |
| OS-04 | 已完成 | `62c4b18`：README 重写；失效路径已修 |
| OS-05 | 已完成 | `f26c6a4`：死代码/死配置已删除，构建三关通过 |
| OS-06 | 已完成 | `f26c6a4`：遗留 `test/`、转换脚本、机器相关文件已清理；本机路径改占位符 |
| OS-07 | 已完成 | `f26c6a4` + `921f0d1`：文档分层、死链与过期引用清理 |
| OS-08 | 已完成 | `f26c6a4` + `921f0d1`：负例框架并入各层 UT 设计，台账统一 |
| OS-09 | 已完成 | 用户裁决：`CLAUDE.md` / `.claude` / `.codex` 保留 |
| OS-10 | 进行中 | `.github/workflows/build.yml` 已建；**未在 GitHub Actions 实际运行**（仓库未公开） |
| OS-11 | 待办 | 用户裁决**暂不公开**；未打 tag、未发布 |
| OS-12 | 不适用 | 无可选体积目标 |
| OS-13 | 不适用 | 按 H49/C2 不按行数拆分目录 |

**发布结论：未完成。** OS-01 历史清理、OS-10 CI 实跑、OS-11 公开/tag 仍未完成。
