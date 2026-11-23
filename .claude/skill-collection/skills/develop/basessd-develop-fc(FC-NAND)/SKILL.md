---
name: basessd-develop-fc(FC-NAND)
description: 修改 FC/NAND 层代码的流程与验收。用户说「改 FC」「改 NAND」「调 timing」「动描述符链」时使用。仅限 FC/NAND；不用于 FTL、平台或纯文档改动。
---

# FC / NAND 改动

## Quick Start
用 basessd-develop-fc 改 <desc 链 / ECC / timing>：先读 `fc_nand_design.md` 与 `rules/fc-nand.md`，再 `make tests UT=FC`。

## 入口
- 目标在 `src/fc/`、`src/nand/`、`include/fc/`、`include/nand/`。
- 已读 `fc_nand_design.md` 对应章节与 `.claude/rules/fc-nand.md`。
- 不确定该放哪层 → 先转 `basessd-design-arch(架构)`。

## 工作流
1. 定坐标：flat 绝对 CW 号、`plane_paa[plane]`、PBA↔PAA 单一漏斗。
2. 读设计文档与 `rules/fc-nand.md`；需要协议依据时查 `include/nand/onfi_1_0.h` / datasheet。
3. grep 已有 UOP / handler / helper，不另造第二套坐标。
4. 改代码：desc 链上界、池配对、ECC/CRC、状态读取、timing 回滚。
5. 构建：`make all` / `make release` / `make tests UT=FC`。
6. 碰建链 / 位图 / 地址 → 上板（`basessd-verify-board(上板)`）。
7. 证据与台账（R1 / R11）。

## 产物格式
- 改动说明：文件、坐标/链/时序变化、验证证据。
- 新增的失败坐标 / 状态读取检查必须有负例（C7）。
- 台账按 R11 更新；未上板标「未上板」。

## 边界
- FTL 改动 → `basessd-develop-ftl(FTL改动)`。
- 平台/移植 → `basessd-develop-platform(平台)`。
- 上板执行 → `basessd-verify-board(上板)`。
- 代码审查 → `basessd-review(审查)`。

## 命令
`make all` / `make release` / `make tests UT=FC`；命令细节查 `make help`，上板见 `basessd-verify-board`。

## 按需知识
- `.claude/rules/fc-nand.md`
- `docs/design/fc_nand_design.md`
- `include/nand/onfi_1_0.h`（命令/状态定义）
- `docs/design/known_open_items.md`

## 修订
改坐标 / 位图 / 链语义回扫全部调用点（C10）；同步设计文档与台账。

## 常见失败

- 出现第二套坐标 / 手写位操作。
- desc 链无上界 / 环检测。
- 状态字节 / FAIL 被静默丢弃。
- timing 失败不回滚。

## 完成
三关零新告警；有生效证据；建链/位图/地址已上板；未提交未推送（R3）。

- [ ] 坐标 / 链 / 时序变化写清
- [ ] 新检查有负例
- [ ] 三关零新告警
- [ ] 建链 / 位图 / 地址已上板
