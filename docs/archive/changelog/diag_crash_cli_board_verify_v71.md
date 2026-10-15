# v0.71 — `crash` 上板验证通过 + 一条未闭合的 HardFault 记录

**日期**：2026-10-14
**范围**：`docs/design/fault_capture.md`（补 FPU 帧排布的出处）。**无代码改动。**

## v0.70 上板验证：通过

```
crash clear
crash record cleared
→ 复位
=========== soft_ssd rev=24 built Oct 14 2026 13:51:19 ===========
[INF] [FAULT] reset reason: PIN (rsr=0x00460000)
（无 previous crash / previous assert 行）
```

清除前有记录、清除后复位不再出现 ⇒ **`crash clear` 按设计工作**，
且再次确认 `.noinit` 记录确实跨软复位保留。

## ⚠ 但记录内容不是断言，是 HardFault

清除前那一行是：

```
[ERR] [FAULT] previous crash: HardFault PC=0x2400038c LR=0x24000b09 CFSR=0x00000001 HFSR=0x40000000
```

**与 v0.69/v0.70 讨论的 `bst.h` 断言是两个不同的签名**。记录覆盖式写入（后写的赢），
所以在那条 assert 之后板子**真的又崩过一次**——这次是 CPU 故障。

| 字段 | 值 | 含义 |
|---|---|---|
| `CFSR` | `0x00000001` | `MMFSR.IACCVIOL` —— 取指访问违例 |
| `HFSR` | `0x40000000` | `FORCED` —— MemManage 升级成 HardFault |
| `PC` | `0x2400038c` | 落在 `.bss`（AXI_RAM）内 |
| `LR` | `0x24000b09` | 同样落在 `.bss` 内 |

⇒ CPU **试图从数据区取指**。两个独立寄存器同时指向数据区，典型是
**函数指针 / 返回地址被写坏**（控制流被破坏，不是数据被破坏）。
按当前镜像符号表，两个地址都落在 `g_wp_buf`（`0x240002a0`–`0x240012a0`，4 KB，
`wp` 命令的写源缓冲）范围内。

## 先决核实：故障帧取法是对的

若 FPU 扩展帧排布反过来，`frame[6]` 读到的是 `S6` 而不是 PC —— PC/LR 就是浮点寄存器
垃圾，`.bss` 之类的推论全部作废。

**已对照 Zephyr `include/zephyr/arch/arm/cortex_m/exception.h` 的 `struct arch_esf` 核实**：
`struct __basic_sf {r0,r1,r2,r3,r12,lr,pc,xpsr}` 在 `struct __fpu_sf {s[16],fpscr,undefined}`
**之前**，故障路径直接取 `esf->basic.pc` ⇒ **基本帧在低地址、SP 指向 R0**，FPU 帧在高地址。
`fault_port_entry_c` 的 `frame[0..7]` 取法正确，`fault_capture.md` §5.1 原说法成立
（本次补上了出处）。⇒ **记录里的 PC/LR 是真值**。

## 未闭合（登记在案，不再往下查）

- **不知道何时/如何触发**：用户不知复现步骤；`cp_seq` 106→177 说明空转过十来分钟，
  崩在空转还是崩在负载里**未定**。
- **当场 dump 没留下**：HardFault 时固件会立刻打一整块 `=== CPU FAULT ===`
  （PC/LR/SP/xPSR/R0-R3/CFSR 位名/栈扫描候选返回地址），那块才是定位关键。
- **操作顺序反了**：先 `crash clear` 才复位 ⇒ 详细记录（`expr`/`file` + 完整故障帧）
  被抹掉，启动横幅只剩 4 个字段。

## 下次复现时按顺序做

1. **先 `crash`**（看详细），**再** `crash clear` —— 顺序不能反。
2. 保留**当场**的 `=== CPU FAULT ===` 整块日志（含栈扫描）。
3. 用 `map <lba>` 与 `[FTL] dangling` 交叉对照：两者指向同一个 `g` ⇒ 与 v0.69 的
   plane 掩码同一条线；不相干 ⇒ 独立问题。

## 可选加固（未做）

启动横幅故障行只打 4 个字段，逼人依赖"当场 dump"。可复用现成的 `fault_decode_cfsr` /
`fault_decode_hfsr`，把 `SP/xPSR/MMFAR/BFAR` + CFSR/HFSR 的**位名解码**打进开机日志，
让单条启动日志自足。**未排期。**
