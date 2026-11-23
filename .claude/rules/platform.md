---
paths:
  - "src/platform/**"
  - "include/platform/**"
  - "src/main.c"
---

# 平台规则（常驻于平台代码路径）

> 平台映射见 `docs/hardware_contract.md` / `docs/design/platform_design.md`；
> 上板见 `basessd-verify-board`；故障见 `basessd-maintain-fault`。

## 缓存与 DMA

- DMA 缓冲区必须 Clean / Invalidate，地址与长度按 32 字节对齐（H17）。
- 缓存维护不免费：不要对同一缓冲区反复做。
- SWD 读**可缓存内存**绕过 D-Cache，可能读到陈旧值；不能当证据。

## 中断与并发

- 临界区用**可嵌套的 PRIMASK 保存 / 恢复**（H18）。
- ISR 短、不阻塞、可重入；禁止 `LOG*` / `ASSERT` / 动态分配 / 等待 / FTL·NAND 事务（H21）。
- `volatile` 只用于硬件寄存器 / ISR 共享 / DMA 可见缓冲；顺序敏感处配 `__DMB()`（H16）。

## 工具链陷阱

- 动 HAL / 库前先读它的源码：`HAL_RCCEx_GetPeriphCLKFreq()` 对 FMC / QSPI / USART 恒返 0。
- 本仓链接**完整 newlib**（`config.mk` 明确不用 `-specs=nano.specs`），`hh/ll/j/z/t` 均可用；不要照搬 newlib-nano 的 `h/l/L` 限制。
- CMSIS 头的 `optimize("Os")` 会 **TU 级泄漏**；`.bin` 逐字相同在动 include 图时不成立。

## 看门狗与生命周期

- IWDG 当前所有构建档关闭；启用与否是**待上板决策**（`RISK-CLI-01`）。
- 长同步操作走 `storage_wdg_kick()`；喂狗点不能被慢路径饿死；不在 ISR 喂狗。
- 栈预算 2 KiB；`inline` 不必然省栈。
