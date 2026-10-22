# USB MSC 吞吐优化记录（2026-10-22）

> 配套：`usb_msc_udisk.md`（架构）、`usb_msc_verification.md`（验证手册）、
> `known_risks.md` `RISK-NAND-04` / `RISK-PLAT-04`。
> 本文只记录**测速方法、实测结果、已做改动、后续方案**；架构与命令语义以上述文档为准。

## 1. 目标与边界

- 平台：STM32H743 + MT29F4G08，USB2_OTG_FS **Full Speed 12 Mbps**、BOT 单命令。
- USB FS bulk 理论上限约 **1.2 MB/s**；BOT 实测常见 **0.8–1.0 MB/s**。
- 起点：v12 + debug 镜像 H2testw 写 **261 KB/s** / 读 **297 KB/s**。
- 目标：先到 **≥600 KB/s**，并保持 H2testw 全量无错误、掉电语义不变。

## 2. 测速方法

### 2.1 板端

1. `info` 的 `[USB] wr chunk calls=... bytes=... <=512=... <=4K=... <=16K=... >16K=...`
   —— TinyUSB 每次数据相回调的长度分布。平均 `bytes/calls` 就是实际分块大小。
2. `info` 的 `[USB] latency n=... avg=...us max=...us last=...us total=...ms`
   —— **submit → complete 的设备侧墙钟**（DWT；含 FTL/NAND/确认，不含主机发送数据
   的 USB 总线时间）。16 KB/请求时可直接和 13.5 ms 的 USB 总线时间相加。
3. `[STAT]` 的 `wr`、`free/op/mig` —— 判断页放大、GC 是否参与。

### 2.2 Windows（不改固件）

- `tools/usb_verify/measure_io.ps1`：采样
  `Win32_PerfFormattedData_PerfDisk_PhysicalDisk` 的
  `Bytes/Transfer`、`Sec/Transfer`、`Transfers/s`、`Queue`。
  ⚠ 这是**文件系统/存储栈**粒度（H2testw 常见 1 MB/笔），**不是 USB CBW 粒度**；
  USB 分块以板端 `info` 为准。
- 判读：`Queue≈1` 表示应用/BOT 串行；`Sec/Transfer` 是端到端每笔文件系统 IO 的耗时。

### 2.3 H2testw

- 写满 + 校验，记录写/读 KB/s 与 `Test finished without errors`。
- `415/423 MB` 是 H2testw 自带的约 2% 安全余量，**不是**容量/FAT 开销（8 KB 簇格式化后
  仍是 415）；容量以 `[CAP]`/`Get-Disk Size` 为准。

## 3. 实测结果

| 配置 | 写 | 读 | `latency avg`（16 KB/请求） | 备注 |
|---|---:|---:|---:|---|
| debug + 4 KB EP | 261 KB/s | 297 KB/s | — | 旧 baseline；近满盘、GC 参与 |
| debug + 16 KB EP（近满盘） | 295 KB/s | — | 27.5 ms（max 46.5） | 16 KB 分块已生效（≈96% `<=16K`） |
| debug + 16 KB EP（FTL format 后空盘） | 322 KB/s | — | 27.5 ms | `free=3218`、`mig=0/0`，排除 GC |
| **release + 16 KB EP（空盘）** | **673 KB/s** | **758 KB/s** | **6.6 ms（max 13.6）** | **全盘 H2testw 415/423 MB 无错误** |

结论：

1. **16 KB 端点缓冲有效**：平均回调长度 ≈16 KB，`>16K=0`。
2. **GC/近满盘不是主因**：空盘无 GC 时 debug 仍 322 KB/s。
3. **`-O0` 是设备侧主因**：同一份代码 release 把 `latency` 从 27.5 ms 降到 6.6 ms，
   写从 322 → 673 KB/s（读 758）。
4. 剩余预算：**USB FS 总线约 13.5 ms/16 KB + 设备 6.6 ms ≈ 20 ms** ⇒ 实用上限约
   **800 KB/s**；672 已接近。
5. USB DMA（DWC2 slave/PIO）和 NAND DMA 关闭都不是 13 ms 级瓶颈（见 §5）。

## 4. 已做改动

- `include/tusb_config.h`：`CFG_TUD_MSC_EP_BUFSIZE` **4096 → 16384**；
  `s_msc_bounce` 随宏放大；直方图改为 `<=512 / <=4K / <=16K / >16K`。
- `src/controller/usb_msc.c`：新增 `[USB] latency`（submit→complete DWT）；
  `src/platform/delay.c` / `include/platform/delay.h`：`delay_dwt_elapsed_cycles_us()`。
- **release-only 启动崩溃修复**：`src/common/stack_guard.c` 的
  `stack_guard_init` 原用入口 SP 作 `paint_hi`，`-O2` 下覆盖自己 `push` 的保存 LR，
  返回跳进 `STACK_GUARD_PATTERN`（PC=0x5354414A，MemManage IACCVIOL）。
  改为 `paint_hi = 入口SP - STACK_GUARD_INIT_FRAME_MARGIN(64B)`。
  登记 `RISK-PLAT-04`，release `14:05:16` 上板正常。
- 烧录路径：`.vscode/launch.json` 与 `sh/flash.jlink` 当前指向 release ELF
  （测速期配置；切回 debug 时同步改回）。

## 5. 后续方案（按收益/风险排序）

| 方案 | 预期收益 | 风险 / 成本 | 判据 |
|---|---|---|---|
| **EP 16 KB → 32 KB A/B** | 5–10%（摊薄每块固定开销） | RAM +16 KB；`uint16_t nbytes` 仍可容纳；直方图标签/静态断言同步 | H2testw 写/读提升；`<=32K` 分块出现；无错误 |
| FC/NAND 子段 DWT（encode/decode、WAIT_READY、PIO、confirm） | 定位剩余 6.6 ms 的可优化部分 | 只加观测，不改行为；debug 构建 | 子段之和能解释 6.6 ms；据此选下一项 |
| word-wise LFSR（保持同一序列） | 软件加扰成本下降 | 必须逐字节序列一致，不能改随机化算法（介质兼容） | 读回数据一致；encode 时间下降 |
| BCH 表驱动/内层展开 | 每 CW 编解码下降 | 需 UT 负例覆盖纠错路径 | 纠错结果不变；时间下降 |
| PIO 按字搬运 / `CONFIG_NAND_DMA=1` | PIO 阶段下降 | 地址/缓存一致性类改动，必须上板；DMA 失败可回退 PIO | 4 KB 读写正确；`bad=0`；时序不退化 |
| USB HS 硬件 | 突破 FS 1.2 MB/s 上限 | 需要换板/换 PHY，不在当前硬件范围 | — |

**不建议**为了跑分提前回 CSW 或跨 BOT 命令合并写：那会破坏掉电语义。

## 6. 复现命令

```powershell
# Windows：写入/校验阶段各一次
powershell -NoProfile -File .\measure_io.ps1 -Seconds 30
```

```text
# 板端 CLI（测试后）
info
```

关注：

```
[USB ] wr chunk calls=... bytes=... <=512=... <=4K=... <=16K=... >16K=...
[USB ] latency n=... avg=...us max=...us last=...us total=...ms
```
