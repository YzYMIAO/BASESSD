# testing_guide 实测数据历史归档

> 从 `docs/design/testing_guide.md` §7.1 / §7.2 / §7.5 / §9.1~§9.3 移出；正文不改写。
> 当前结论仍在主文件对应小节，便于快速判断；需要逐表/逐轮数据时查这里。

### 7.1 全盘 H2testw（近满盘、GC 参与）

| 配置 | 写 | 读 | cache 状态 | 条件 |
|---|---:|---:|---|---|
| release（2026-10-22 18:51，近满盘/GC） | **765 KB/s** | **884 KB/s** | program **关**；read **未生效** | 415/423 MB 无错误；断电重启 `restored`、`bad=0`（当时 `dng` 为 0）、`map/rd` 抽查通过 |
| release -O2（2026-10-25，从空盘写满） | **843 KB/s** | **889 KB/s** | program **开**（`cw=24906/99624/7945/0`）；read **开**（`cr=25931/102794/681/0`） | 415/423 MB 无错误；`cr` 平均 **3.96 pair/batch**、`fallbacks=0`；`cw` 每批 4 页、`aborts=0`；结束时 `free=61`、`bad=3` |

**收口结论**（同一 release 档，非严格 A/B：盘况/GC 不同）：

- **cache program 使顺序写 765 → 843 KB/s（+10%）**；`cw` 覆盖约 389 MiB / 415.6 MiB。
- **cache read 修复后已生效**（`cr batches=25931`，修复前恒 0），但**读速 884 → 889 KB/s
  在噪声内**，与 `/speed/ftl_rw` 的结论一致（RISK-PERF-03：瓶颈在 CPU 数据面，不在命令开销）。
- `cr declines=681`（约 2.6% 尝试）来自 region/块边界处不足 2 pair 的窗口；`cw declines=7945`
  多数是 `batch_active` 时 drain 重试，不是数据回退（回退字节只有约 26 MiB）。

---

### 7.2 4K 随机（Windows diskspd，128 MB 文件）

| 项目 | 吞吐 | IOPS | AvgLat | p99 | max |
|---|---:|---:|---:|---:|---:|
| 4K 随机写 | 532 KiB/s | 133.4 | 7.50 ms | 12.56 ms | 20.61 ms |
| 4K 随机读 | 768 KiB/s | 193.2 | 5.18 ms | 6.24 ms | 17.59 ms |
| 4K 70/30 | 584 KiB/s | 145.7 | 6.86 ms | 12.19 ms | 27.02 ms |

条件：QD1、1 thread、`-Suw`、每项 60 s、Test Size 128 MB。写 532 KiB/s 约为顺序写
765 KB/s 的 70%；读 768 KiB/s 接近顺序读 884 KB/s。对 USB FS BOT 是正常量级。

⚠ 这一组 4K 随机测试**没有同步记录板端 `free`/镜像 banner**，因此只能作为量级参考；
不能把它当成“空盘 baseline”或“GC 稳态”的正式对比数据。正式报告请按 §6.7 补齐条件。

---

### 7.5 速度分层对照（2026-10-25，release -O2）

器件 MT29F4G08ABADAWP（3.3 V，ONFI Mode 5，x16）：tWC/tRC = 20 ns ⇒ 总线 **100 MB/s**
（1.8 V/25 ns 为 80 MB/s）；tR=25 µs、tPROG=200 µs（典型）、tERASE=700 µs（典型）。
2-plane pair = 4096 B 主区（总线 4224 B ≈ 42.2 µs @100 MB/s）⇒ 理论上限：

| 层级 | 读 | 写 |
|---|---:|---:|
| NAND 阵列 + 总线（2-plane） | 61 MB/s（普通）/ 97 MB/s（cache 流水） | 20.5 MB/s（cache program 稳态） |
| FC raw（`/speed/fc_nand_rw`，cache 档） | 3.68 MB/s（阵列 **6%**） | 4.42 MB/s（**22%**） |
| FTL host API（`/speed/ftl_rw`，cache 档） | 3.45 MB/s（**6%**） | 2.75 MB/s（**13%**） |
| USB MSC（H2testw） | 0.889 MB/s（FS 总线 59%；FTL 内部 26%） | 0.843 MB/s（56%；31%） |

按吞吐折算每个 4 KiB pair 的平均耗时：FC 读 1.11 ms / 写 0.93 ms；FTL 读 1.19 ms /
写 1.49 ms，而器件单 pair 本身只需 67 µs（读）/ 242 µs（写）⇒ 当前内部瓶颈是 **CPU
数据面**（PIO 逐字搬运 + randomizer/CRC + BCH），不是 NAND，也不是总线；USB FS
12 Mbit/s 的 1.5 MB/s 是 host 侧硬顶。优化顺序与判据见 `known_risks.md` RISK-PERF-04。

⚠ `/speed/` 读数是**吞吐**（含测试图案填充/校验 CPU），不是单命令延迟；munit 的
`[ OK ] [ 耗时 ]` 现在只用平台 `time_us`（产品侧为 SysTick 64 位 µs；DWT 已删除，RISK-UT-05 闭合）；历史日志里 10.7 s 附近的取模值仍按旧口径解读。速度结论始终只看行内 `*_ms`。

---

### 9.1 实测

| 配置 | 写 | 读 | `latency avg`（16 KB/请求） | 备注 |
|---|---:|---:|---:|---|
| debug + 4 KB EP | 261 KB/s | 297 KB/s | — | 旧 baseline |
| debug + 16 KB EP | 295~322 KB/s | — | 27.5 ms | `-O0` 主因 |
| **release + 16 KB EP（空盘）** | **673 KB/s** | **758 KB/s** | **6.6 ms** | H2testw 无错误 |
| **release + 16 KB EP + 1/4/5 批处理（近满盘/GC）** | **765 KB/s** | **884 KB/s** | 未取（写后断电） | H2testw 无错误 |
| **release + 16 KB EP + cache program + T5 移除（2026-10-23）** | **843 KB/s** | **889 KB/s** | **4.1 ms** | H2testw 415/423 MB；`cw=24906/99624/0 abort`；无 T5/无 GC |

结论：`-O0` 是设备侧主因；16 KB EP 有效；GC/近满盘不是主因；USB FS BOT
仍是主瓶颈，本板 release + cache program 实测 **843/889 KB/s**（avg latency 4.1 ms）。

### 9.2 已做改动

- `CFG_TUD_MSC_EP_BUFSIZE` 4096 → 16384；`[USB ] latency` SysTick µs 统计；
- stack guard release 启动崩溃修复（`RISK-PLAT-04`）；
- LFSR 8-step 查表、FC 读路径多步骤合并、FTL 确认批量化；
- 写确认/TRIM 全异步、Journal 背压、CLI 上报错位修复、MIG 空块日志抑制。

### 9.3 后续方案

| 方案 | 收益 | 风险/成本 | 判据 |
|---|---|---|---|
| EP 16 → 32 KB | 5–10%（估计） | RAM +32 KB（EP + bounce）；直方图/静态断言同步。**2026-10-23 决定暂不实施**：H2testw 受 USB FS 约束，预期收益小 | 分块出现 `<=32K`；无错误；若未来实施，按 32K→64K 的边际收益再评估 |
| FC/NAND 子段 SysTick µs | 定位剩余 6.6 ms | 只加观测 | 子段之和解释 6.6 ms |
| BCH 表驱动/展开 | 编解码下降 | 需 UT 负例 | 纠错结果不变 |
| PIO 按字 / `CONFIG_NAND_DMA` | 有限 | **FMC NAND 无专用 DMA 请求**，`nand_dma.c` 是 DMA2 mem2mem，不是外设 DMA | 与 PIO 对比；时序不退化 |
| USB 内部 DMA | 仅 `GHWCFG2.arch==internal DMA` | 当前板 `arch=0`（slave-only） | 先看启动日志，不列优化项 |
| USB HS 硬件 | 突破 FS 1.2 MB/s | 换板/PHY | 不在当前范围 |

> **主机 FAT 分配单元 8K→32K 不是设备吞吐旋钮。** TinyUSB 仍按
> `CFG_TUD_MSC_EP_BUFSIZE=16384` 分块（`include/tusb_config.h:39`），
> 2026-10-23 的 `[USB ] wr chunk` 平均约 16 KB/call 已说明数据相粒度由 EP 缓冲决定；
> 32K 簇只会减少主机 FAT 元数据，不减少设备侧 16 KB 异步完成往返。
> 预期收益在噪声内；若未来要测吞吐，再 A/B `EP 16→32 KB`，而不是簇大小。本轮决定不实施 EP 扩展。
>
> **EP 不是越大越好：32 KB 是首选，64 KB 只在 32 KB A/B 有明确收益后再试。**
> 每翻倍一次只再摊薄一次 FTL submit/complete 固定开销，32→64 KB 的增量约为
> 16→32 KB 的一半；代价是 TinyUSB EP + bounce 共再 +64 KB AXI_RAM。主机簇大小
> 64 KB 不需要 EP=64 KB 才正确——32 KB 会把它拆成两个数据相，语义不变。
