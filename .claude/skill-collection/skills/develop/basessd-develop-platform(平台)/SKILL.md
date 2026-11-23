---
name: basessd-develop-platform(平台)
description: 修改平台 / 移植层（时钟、定时器、看门狗、日志、故障捕获、缓存、DMA、USART）的流程与验收。用户说「改平台」「移植」「调时钟」「DMA/缓存」时使用。仅限平台；不用于 FTL、FC/NAND 或 HIL 业务逻辑。
---

# 平台 / 移植改动

## Quick Start
用 basessd-develop-platform 改 <平台服务 / 新 SoC / 新器件>：先读 `platform_design.md` / `hardware_contract.md` 与 `rules/platform.md`。

## 入口
- 目标在 `src/platform/`、`include/platform/`、`src/main.c`。
- 已读 `docs/design/platform_design.md`、`docs/hardware_contract.md`、`.claude/rules/platform.md`。
- 业务层语义 → 转对应层 skill。

## 工作流
1. 定服务：timer / watchdog / logging / fault capture / cache / DMA / USART。
2. 分边界：抽象接口零 HAL 类型 → 共享逻辑零 SoC 头 → `*_port.c` 只碰 HAL/MMIO。
3. 缓存/DMA：Clean / Invalidate、32B 对齐；ISR：PRIMASK、短、不阻塞。
4. 时钟/时序：查 datasheet / HAL 源码，不凭记忆；失败回滚。
5. 构建：`make all` / `make release`。
6. 时序/缓存/中断类改动 → 上板（`basessd-verify-board`）。
7. 证据与台账。

## 产物格式
- 改动说明：平台服务、边界、时钟/缓存/中断影响。
- 上板证据；未上板标「未上板」。
- `hardware_contract.md` / `platform_design.md` 如契约变化同步更新。

## 边界
- FTL → `basessd-develop-ftl(FTL改动)`。
- FC/NAND → `basessd-develop-fc(FC-NAND)`。
- HIL → `basessd-develop-hil(HIL)`。
- 代码审查 → `basessd-review(审查)`。

## 命令
`make all` / `make release`；上板见 `basessd-verify-board(上板)`；命令细节查 `make help`。

## 按需知识
- `.claude/rules/platform.md`
- `docs/design/platform_design.md`
- `docs/hardware_contract.md`
- `docs/design/common/{timer,logging,fault_capture}.md`
- `docs/design/known_open_items.md`

## 修订
改平台契约回扫所有 port；同步 `hardware_contract.md` / `platform_design.md`；重跑 rules-check。

## 常见失败

- 业务层直接碰 HAL / MMIO。
- DMA 漏 Clean / Invalidate 或对齐。
- 临界区用裸关中断。
- ISR 里做长事务 / 日志。

## 完成
三关零新告警；有上板证据；契约文档已同步；未提交未推送（R3）。

- [ ] 服务与边界写清
- [ ] 缓存 / 中断 / 时钟影响写清
- [ ] 上板证据
