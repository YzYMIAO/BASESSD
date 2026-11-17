# 开源整理审计与执行记录（历史快照，2026-11-17）

> 从 `docs/guides/open_source_checklist.md` 移出。以下内容是 2026-11-17 的审计与执行记录，
> 不代表当前状态；当前状态以 checklist 的「执行状态」表和 git 历史为准。

## 8. 执行记录模板

每完成一步，在下面追加一行；`已完成` 必须附证据（R1）：

| ID | 状态 | 执行人 | 证据（命令输出 / commit / 日志） | 备注 |
|---|---|---|---|---|
| OS-00 | 已完成 | Claude Code | `~/.local/share/soft_ssd-backup/soft_ssd-before-open-source-20261117.bundle`（18 MB，`git bundle verify` = "complete history"）+ `.commit`（HEAD 记录） | 未建 `open-source-prep` 分支，仍在 `main` |
| OS-01 | 进行中 | Claude Code / 用户 | `f26c6a4` 删除 `docs/claude_code_usage.md`；工作树凭据扫描 0 命中；`3ee9ef9` 确认已在 `origin/main`；**旧 token 已由用户确认吊销** | **2026-11-17 用户裁决：暂不公开**；历史重写（方案 A：新仓 squash）推迟到公开前执行；`RISK-REL-01` 保持待后续 |
| OS-02 | 已完成 | Claude Code / 用户 | `e6414b7` 加根 `LICENSE` / `THIRD_PARTY_NOTICES.md` / μnit `COPYING`；本轮补 `third_party/stm32h743/LICENSE`（HAL BSD-3-Clause + CMSIS Device Apache-2.0 映射与原文），NOTICE 与 README 同步 | 许可文本取自上游仓库 LICENSE.md（2026-11-17）；改动在工作树，**未提交** |
| OS-03 | 已完成 | Claude Code / 用户 | `git rm` 7 PDF + 9 HTML；`git ls-files docs/reference` 只剩 `.md`；本地副本 `~/.local/share/soft_ssd-backup/reference-20261117/`；`docs/reference/README.md` 改链接清单；跨文档路径改 URL | 改动在工作树，**未提交**（R3）；2026-11-17 用户裁决「移出仓库」 |
| OS-04 | 已完成 | Claude Code / 用户 | `README.md` 重写：定位 / 架构 / 硬件 / 构建 / 烧写 / 状态表 / 已知限制 / 文档地图 / License / 反馈；失效路径已修 | 改动在工作树，**未提交**（R3）；状态表按 R1 标注已验证范围 |
| OS-05 | 已完成 | Claude Code | `f26c6a4`：删除 `src/common/{data_structure,algorithm,sort.c}`、`include/common/data_structure`、`config/controller_config.h`；`make all` / `make release` / `make tests UT=FTL` 零新告警 | 未上板（删死代码不改建链/位图/地址，R2 不需上板） |
| OS-06 | 已完成 | Claude Code | `f26c6a4`：删除遗留 `test/`、两份 `convert_encoding.py`、`sh/open-ubuntu.bat`、`.clang-format_array` 等；本轮按动作 3 把 `sh/debug_env.sh` 的 JLink 路径改为 `JLINK_EXE` 环境变量，`wsl_debug_environment.md` 的本机路径 / J-Link 序列号 / WSL 动态 IP 全部改为占位符，`testing_guide.md` 的 DiskSpd 路径改为占位符 | 改动在工作树，**未提交**；调试链路需用户本机设置 `JLINK_EXE` 后确认 |
| OS-07 | 已完成 | Claude Code | `f26c6a4`：`architecture.md` 重写；`ut_tree_restructure.md` 移入 `archive/`；`nand_model.md` 并入 `hardware_contract.md` 后与 `ftl_design.md` 桩一并删除；README/docs 链接同步 | 同上 |
| OS-08 | 已完成 | Claude Code | `f26c6a4`：负例框架入 `testing_guide.md §12`，未闭合台账入三层 UT 设计 §7.2；`docs/design/README.md` 新建；`docs/README.md` 导航同步 | 同上 |
| OS-09 | 已完成 | 用户 / Claude Code | 2026-11-17 用户裁决**全部保留**：`CLAUDE.md` + `.claude/skills/` + `.codex/skills/`；`settings.local.json` 已 ignore | 无需改动代码 |
| OS-10 | 进行中 | Claude Code | 已建 `.github/workflows/build.yml`（ubuntu-22.04 + arm-none-eabi 10.3 + clang-format-15；跑 `make all` / `make release` / `make tests UT=FTL`） | **未在 GitHub Actions 上跑过**（仓库未公开）；本地等价命令已通过 |
| OS-11 | 待办 |  |  | 用户裁决**暂不公开**；未打 tag、未发布 |
| OS-12 | 不适用 |  |  | 无可选体积目标时不执行 |
| OS-13 | 不适用 |  |  | 按 H49/C2 不按行数拆分 |

---

## 附：全仓逐目录发布范围审计（2026-11-17）

> 对象：`git ls-files` 的 662 个跟踪文件 + 本机未跟踪文件。判据：**是否含本机信息 /
> 凭据 / 不可再分发的第三方原文 / 构建产物**。跟踪数会随改动变化，本表只记判定与例外。

| 目录 | 跟踪文件 | 判定 | 说明 |
|---|---:|---|---|
| 根目录 | 12 | 保留 | 构建入口 / 规则 / 许可证；无本机路径 |
| `.github/` | 1 | 保留 | CI 工作流（构建三关，不上板） |
| `.vscode/` | 3 | 保留 | 全 `${workspaceFolder}` / 环境变量模板，无个人序列号 |
| `.claude/` | 5 | 保留 | 用户裁决（OS-09）；`settings.local.json` 与评审草稿已 ignore |
| `.codex/` | 1 | 保留 | 用户裁决（OS-09）；C 规范为**改写摘要**，非原文 |
| `linker/` | 2 | 保留 | 产品 / UT 链接脚本 |
| `sh/` | 4 | 保留 | `debug_env.sh` 已改为 `JLINK_EXE` 环境变量 / `.debug_env.local`；`flash.jlink` 本地忽略 |
| `tools/` | 8 | 保留 | UT 文档检查器 + Windows USB 工具；无个人路径 |
| `tests/` | 23 | 保留 | UT 源 + μnit（含上游 COPYING） |
| `src/` | 83 | 保留 | 固件源码，无本机路径 |
| `include/` | 80 | 保留 | 头文件，无本机路径 |
| `docs/design/` | 19 | 保留 | 主设计 + 内部账本（known_open_items / known_open_items） |
| `docs/guides/` | 2 | 保留 | 本机 JLink 路径 / 序列号 / WSL 动态 IP 已改占位符 |
| `docs/reference/` | 1 | 保留 | 仅 `README.md`（链接清单）；datasheet 原文摘录已移出，见 OS-03 |
| `docs/archive/` | 106 | 保留 | 历史记录；仅一处 RFC1918 WSL 私网 IP，无凭据 / 个人身份 |
| `third_party/` | 306 | 保留 | 裁剪后的 TinyUSB + ST HAL/CMSIS，含各组件许可 |
| `build/`、`compile_commands.json` | 0 | 忽略 | 构建产物 / 含绝对路径 |
| 本机文件 | — | 忽略 | `sh/flash.jlink`、`.claude/settings.local.json`、`.claude/io_review_backlog.md`、`.debug_env.local`、`*.svd`、Python 缓存、编辑器临时文件 |

**已裁决（2026-11-17）**：`docs/reference/README.md`（Micron 数据手册
Read Cache / Two-Plane Read Cache 两节原文摘录）**移出仓库**，本地副本在
`~/.local/share/soft_ssd-backup/reference-20261117/`；所有活跃文档引用改为
「datasheet 节名 + `docs/reference/README.md` 链接」，与 OS-03 口径一致。

---

## 附：与本清单对应的风险登记

`docs/design/known_open_items.md` §2AM 已登记：

- `RISK-REL-01` 凭据进入 git 历史
- `RISK-REL-02` 项目 / vendor 许可证缺失
- `RISK-REL-03` 论文与网页存档再分发权未确认
- `RISK-REL-04` README 不可用且链接失效
- `RISK-REL-05` `src/common` 零引用死代码

完成对应步骤后按 R11 更新状态与证据，不删除历史行。
