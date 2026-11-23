---
name: basessd-develop-hil
description: 修改 HIL / USB MSC / CLI 的流程与验收。用户说「改 USB」「改 CLI」「请求拆分」「完成/超时」时使用。仅限 HIL；不用于 FTL、FC/NAND 或平台改动。
---

# HIL / USB / CLI 改动

## Quick Start
用 basessd-develop-hil 改 <请求拆分 / USB 路径 / CLI 命令>：先读 `hil_design.md` / `usb_msc.md` 与 `rules/hil.md`。

## 入口
- 目标在 `src/hil/`、`include/hil/`。
- 已读 `docs/design/hil_design.md`、`docs/design/usb_msc.md`、`.claude/rules/hil.md`。
- 只是 FTL/FC 内部逻辑 → 转对应层 skill。

## 工作流
1. 定前端：USB MSC / CLI / controller 哪条入口。
2. 请求拆分：一个 host 请求 → 一个完成闸门；子请求完成前不回成功。
3. 完成/超时/abort：tombstone、reclaimable、迟到完成不覆盖 TIMEOUT。
4. USB：BOT 单命令、deferred error、mount/umount 会话代次。
5. CLI：长命令不阻塞主循环；参数按外部输入校验。
6. 构建：`make all` / `make release` / `make tests UT=HIL`。
7. USB/CLI 行为变化 → 上板端到端（`basessd-verify-board`）。

## 产物格式
- 改动说明：入口、拆分/完成路径、超时/session 变化。
- HIL UT 证据；USB 端到端结果；未上板标「未上板」。
- 台账按 R11 更新。

## 边界
- FTL 逻辑 → `basessd-develop-ftl`。
- FC/NAND → `basessd-develop-fc`。
- 平台/移植 → `basessd-develop-platform`。
- 代码审查 → `basessd-review`。

## 命令
`make tests UT=HIL`；上板见 `basessd-verify-board`；命令细节查 `make help`。

## 按需知识
- `.claude/rules/hil.md`
- `docs/design/hil_design.md`
- `docs/design/usb_msc.md`
- `docs/design/common/cli.md`
- `docs/design/known_open_items.md`

## 修订
改拆分/完成/超时语义回扫全部入口（C10）；同步设计文档与台账。

## 常见失败

- 子请求未完成就回 host 成功。
- 迟到完成覆盖 TIMEOUT。
- 完成 / abort 不配对。
- 长命令阻塞主循环。

## 完成
三关零新告警；有 HIL UT 证据；USB/CLI 端到端已上板；未提交未推送（R3）。

- [ ] 拆分 / 完成 / 超时路径写清
- [ ] HIL UT 有证据
- [ ] USB / CLI 端到端已上板
