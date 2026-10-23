# 存储主控 / 软 SSD 测试指南（2026-10-22）

> 定位：从板端 NAND/FC 到主机 USB 的**分层测试流程、工具、性能方法、当前实测**。
> 配套：`docs/design/usb_msc_udisk.md`（USB 架构）、`docs/design/usb_msc_verification.md`
> （上板验证手册）、`docs/design/known_risks.md`（残余风险）、
> `docs/design/fc_nand_ut.md`（FC UT）。
>
> **报告原则**：任何性能数字必须同时写明**盘状态（空盘 / 近满盘 / GC）、镜像、测试文件大小、
> 块大小、QD、读写比、缓存设置**。不同状态的结果不能直接比较。

---

## 1. 结论先行

- **没有单一的“存储主控测试标准”**。它是分层的：NAND 看 ONFI/datasheet，主机接口看
  USB-IF，性能方法看 SNIA PTS，耐久性看 JEDEC；**FTL 正确性没有公开统一标准**，
  只能靠 directed / random / fault-injection / 掉电测试。
- 本文按这些标准**描述流程**，但当前只执行**简化子集**：不做正式 preconditioning /
  steady-state 判定、不做耐久/认证。**不得把本文数字说成 SNIA/JEDEC/USB-IF 合规。**
- 当前平台：STM32H743 + MT29F4G08，USB2_OTG_FS **Full Speed 12 Mbps**、BOT 单命令，
  主机可见容量 424 MiB。
- 已通过：H2testw 全盘 415/423 MB 无错误；写 **765 KB/s**、读 **884 KB/s**（近满盘/GC
  条件）；TRIM 后断电重启不复活；`bad=0`。⚠ 2026-10-23 dirty media 曾出现
  `[FTL] dangling ... st=CLOSED`，不要再把 `dng` 恒为 0 当默认既成事实（见 RISK-MIG-08）。
- 4K 随机（128 MB 文件、QD1、`-Suw`）：写 **532 KiB/s / 133 IOPS / 7.5 ms**，
  读 **768 KiB/s / 193 IOPS / 5.2 ms**。
- 仍缺：`[USB ] latency` 未在写阶段取到；FC UT 上板回归、掉电循环、严格 steady-state
  矩阵、耐久性都还未系统化。

---

## 2. 标准与出处

| 层 | 标准 / 出处 | 管什么 |
|---|---|---|
| NAND/PHY | **ONFI 规范** + MT29F4G08 datasheet | 命令、时序、参数页、ECC 要求 |
| 主机接口 | **USB-IF Mass Storage BOT** | 枚举、CBW/CSW、错误恢复 |
| 性能方法 | **SNIA SSS PTS** | preconditioning、steady-state、性能报告 |
| 耐久性 | **JEDEC JESD218 / JESD219** | SSD 耐久性要求、工作负载 |
| NVMe | NVMe 规范 | **当前不适用**（USB BOT，不是 NVMe） |
| FTL 正确性 | 无公开统一标准 | directed / random / fault-injection / 掉电 |

出处：

- SNIA SSS PTS：<https://www.snia.org/tech_activities/standards/curr_standards/pts>
- JEDEC JESD218：<https://www.jedec.org/standards-documents/docs/jesd218b>
- JEDEC JESD219：<https://www.jedec.org/standards-documents/docs/jesd219>
- ONFI：<https://www.onfi.org/specifications>
- USB-IF：<https://www.usb.org/compliance>

⚠ 本仓分层对照见 `docs/design/fc_nand_design.md` §26；它只讲 HIL/FTL/FCC/DRAM 与
队列·门铃·完成模型，**不定义 BIT 内部细节**。

### 2.1 标准要求 vs 当前能力

| 领域 | 标准要求 | 当前能做到 | 差距 / 报告口径 |
|---|---|---|---|
| 性能方法 | SNIA SSS PTS：preconditioning + steady-state + 报告条件 | H2testw 预填 + CDM/diskspd 简化矩阵 | 未做完整 SNIA preconditioning / steady-state 判定；结果只能标“本仓简化”，**不得称 PTS-compliant** |
| 耐久性 | JEDEC JESD218/219：耐久要求与工作负载 | 只做掉电循环、短时随机写 | 未做 P/E 循环、retention、read disturb；**不得声称通过 JEDEC 耐久** |
| NAND/PHY | ONFI + datasheet：命令/时序/参数页 | FC UT、`rdpaa --raw`、ONFI 参数页 | 无 ONFI 一致性测试仪/时序分析仪；**不得称 ONFI 认证** |
| 主机接口 | USB-IF Mass Storage BOT | Windows/Linux 枚举、H2testw、热插拔 | 无 USB 协议分析仪/认证；**不得称 USB-IF 合规** |
| NVMe | NVMe 规范：队列/门铃/完成 | 不适用（USB BOT） | 换 NVMe 平台前不做 |
| FTL 正确性 | 无公开统一标准 | directed + random + 掉电 + 故障注入 | 覆盖有限；结果按本仓用例集报告 |

**总口径**：本文所有数字只声称“**在本仓硬件/镜像/盘状态下测得**”，不声称符合
SNIA/JEDEC/ONFI/USB-IF 的正式认证。若引用“SNIA 式 / JEDEC 式”，必须同时写明
本仓的简化点。

---

## 3. 分层测试模型

| 层 | 测什么 | 工具 | 通过判据 |
|---|---|---|---|
| **L0 NAND/PHY** | ID、ONFI 参数页、时序模式、读/写/擦、坏块标记、ECC | 板端 `test /fc-nand/...`、`rdpaa --raw`、`bbt` | 无 ID/时序错误；ECC 可纠；坏块表一致 |
| **L1 FC** | desc 链、CRC、加扰、BCH、multi-plane、CACHE、错误位 | `make tests UT=FC` + 上板 `test` | 读写逐字节一致；负例能报错 |
| **L2 FTL 功能** | L2P、GC、WL、Journal、Checkpoint、TRIM、坏块、掉电恢复 | CLI `wr/rd c/map/tr/fl`、`info`、`bbt fault`、断电循环 | 数据不丢/不复活；无 `dangling`；重启 `restored` |
| **L3 主机接口** | USB 枚举、BOT、格式化、热插拔、文件系统 | Windows/Linux、H2testw、diskspd | 枚举正常；全盘读写无错误 |
| **L4 性能** | 顺序/随机、QD、混合、延迟 | fio / diskspd / CrystalDiskMark | 同条件可重复；报告 IOPS/MB/s/p99 |
| **L5 可靠性** | 掉电、retention、read disturb、P/E | 断电循环、烘烤、长时间写 | 无静默损坏；恢复符合预期 |
| **L6 合规/兼容** | USB-IF、OS、EMC | USB 分析仪、认证实验室 | 正式认证；当前不具备 |

当前能力：**L0~L3 可执行；L4 只能做简化性能矩阵；L5 只做掉电循环和小样本；
L6 不做。** 见表 2.1 的差距。

---

## 4. 推荐执行流程

### Phase A：板端基础

```text
make all
make release
make tests UT=FC
```

上板：

```text
test /fc-nand/...
info
bbt
format          # 需要干净介质时
```

判据：NAND ID/ONFI、坏块、FC 用例通过；`bad=0` 或已知坏块稳定。

### Phase B：FTL 功能

```text
wr 100 1
map 100
rd 100 1
tr 100
map 100
fl
```

再补：

- GC：写满/近满，观察 `[MIG ]`、`free`；
- WL：长时间 idle，观察是否异常搬迁；
- 掉电：写到一半 / TRIM 后断电，重启 `map`/`rd`；
- 坏块：`bbt fault` 注入，确认写重试/报错。

### Phase C：主机接口

- H2testw 全盘写 + 校验：`Test finished without errors`；
- 热插拔、Windows 格式化、重新挂载；
- diskspd：4K 随机读写、16K 顺序。

### Phase D：性能（本仓简化，不声称 SNIA 合规）

1. **空盘 best-case（本仓自拟）**：设备 `format` → 断电 → Windows 快速格式化；
   CDM 256 MiB / Count 1，跑 `SEQ1M Q1T1`、`RND4K Q1T1`。
   这是**best-case**，不是 SNIA steady-state。
2. **简化 preconditioning / GC 稳态（本仓自拟）**：H2testw 写满 → 删 `*.h2w`
   （**不要** device format）→ `info` 确认 `free` 低 → 用相同参数再跑 2~3 轮。
3. **完整 SNIA SSS PTS**：需要按标准规定做 preconditioning、steady-state 判定和
   报告。本仓当前**不做完整流程**；若要做，必须单独记录标准版本、负载、判定阈值，
   不能把第 2 步称为“SNIA 合规”。
   - 常见行业做法是顺序写满容量（常见 2× 容量）再跑若干轮；**这是实践描述，
     不是本仓已验证的 PTS 结论**。

### Phase E：可靠性 / soak

- 掉电循环 10~50 次：写/TRIM/FLUSH 后随机断电，重启校验；
- 长时间随机写/读；
- 高温烘烤 + retention 读回（有条件再做）；
- P/E 循环（学习阶段可只做小样本）。

### Phase F：回归

- 每次改 FTL/FC 后重跑 Phase A~C 的最小集；
- 性能只和**同状态**结果比。

---

## 5. 工具选择

### 5.1 主机侧

| 工具 | 测什么 | 适用性 |
|---|---|---|
| **H2testw** | 全盘容量 + 写后校验 | 完整性基线；`415/423 MB` 是约 2% 余量 |
| **CrystalDiskMark（标准版）** | GUI 顺序/随机、QD1/QD32 | 快速 baseline；USB 重点看 `RND4K Q1T1` |
| **diskspd**（Windows） | 自定义块大小、QD、读写比、时长、延迟 | 本机最实用的受控负载；脚本见 §8 |
| **fio** | 行业标准负载生成器：verify、延迟分位数、steady-state | 需要 Linux 块设备；WSL 需 usbipd 透传 |
| **ATTO / AS SSD** | GUI 柱状图/绿色条报告 | ATTO 可调块大小；AS SSD 默认 1 GB，424 MB 盘不适合 |
| **USBPcap + Wireshark** | USB BOT CBW/CSW、分块、往返 | 无硬件分析仪时的协议层观测 |
| **measure_io.ps1** | Windows 文件系统 IO 粒度 | `tools/usb_verify/measure_io.ps1`；不是 USB CBW 粒度 |

### 5.2 板端

- `test /fc-nand/...`：FC ECC/CRC/加扰/建链/多 plane/压力；
- `info` / `diag`：`free/op/bad/dng/mig/cp/jrn`；新增
  两行：`l2p[ch][ce][lun] seq=<run>/<max> pf=<calls>/<started>/<skipped>/<resident>/<hits>
  cr=<batches>/<pairs>/<declines>/<fallbacks>` 与
  `l2p[ch][ce][lun] cw=<batches>/<pages>/<declines>/<aborts> mb=<jobs>/<pages>/<aborts>`；
- **顺序读预取 / cache read**：先 `wr` 一段连续数据，再连续执行
  `rd <lba> 32`（每次 16 KiB，跨 512-LBA region）。`info` 的 `seq` 应增长、
  `pf started/hits` 增长；`cr batches/pairs` 增长说明 cache read 批次真的生效；
  随机/非对齐读应只让 `cr declines` 增长而数据正确；`wr`/`fl` 后 `seq` 归零。
- **host cache program**：先写满一个块的前一页（让 hot SB 的 page0 成功），再
  `wr <8 对齐 lba> 32` → `rd <同 lba> 32`；`cw batches/pages` 应增长且数据一致。
  非对齐/小请求/wbuf 非空时只应 `cw declines` 增长，写仍成功。batch 在飞时
  `fl` 必须等它结算后才返回。
- **GC/WL cache batch**：写满触发 GC，`mb jobs/pages` 应增长；H2testw 全盘写+校验
  通过，`free/bad/dangling` 与既有回归一致。静态 WL high-wear 路径不批（`mb` 不增长
  是预期）。
- `map <lba>`：L2P 映射 + BST 状态；
- `rdpaa <paa> --raw`：原始页字节，验证加扰/ECC/布局；
- `wp`：页打包写缓冲合并；
- `bbt` / `bbt fault`：坏块表与故障注入；
- `tr` / `fl` / `crash`：TRIM、FLUSH、故障记录；
- `format`：设备侧全片格式化（破坏性，执行后必须断电重启）。

### 5.3 工具和标准的关系

CDM/diskspd 只是**负载生成器**，不是标准；SNIA PTS 规定的是**方法**：
preconditioning、steady-state 判定、报告条件。不要拿“某个软件跑出的数字”直接称为
“行业标准性能”。

---

## 6. 如何设计性能测试（重点）

### 6.1 先定义问题

- **空盘 best-case（本仓自拟）**：看主控/固件上限；不是 SNIA steady-state；
- **近满盘/GC 稳态（本仓简化 preconditioning）**：看真实使用后的性能；
- **A/B 优化对比**：同一状态、同一参数，只改一个变量；
- **耐久/掉电**：不是性能测试，另有流程。

> 行业分岔：消费级跑分常在空盘/新建测试文件上做；企业级 SNIA PTS 反而要求
> preconditioning + steady-state。**两者都合理，但不能混比，也不能把空盘数字
> 称为“行业标准性能”。**

### 6.2 固定变量

| 变量 | 建议 |
|---|---|
| 镜像 | debug/release 必须写清；banner 时间戳确认 |
| 盘状态 | 空盘 / 近满盘 / GC 稳态 |
| 测试文件 | 64/128/256 MiB；不能超过剩余空间 |
| 块大小 | 4K（随机）、16K/1M（顺序） |
| QD / 线程 | USB BOT 用 **QD1、1 thread** |
| 读写比 | 100/0、0/100、70/30 |
| 缓存 | `-Suw` 关闭软件缓存 + write-through |
| 时长/次数 | 每项 60 s 或 Count 1~3；至少 2~3 轮取中位数 |

### 6.3 空盘 best-case（本仓自拟）

1. 板端 `format`（debug 镜像，IWDG 关闭）；
2. 彻底断电重启；
3. Windows 新建卷/快速格式化；
4. CDM 256 MiB / Count 1；`SEQ1M Q1T1` + `RND4K Q1T1`；
5. 记录 `info` 的 `free`（应高位）与结果。

⚠ 这是 best-case，不是 SNIA steady-state；报告标题不要写“SNIA 性能”。

### 6.4 简化 preconditioning / GC 稳态（本仓自拟）

1. **不要** device format；
2. H2testw 写满（触发 FTL 分配/GC）；
3. 删除 `*.h2w`，让 Windows 有空间建 CDM 文件；
4. `info` 确认 FTL `free` 仍低（几十块）；
5. 用与空盘**完全相同**的 CDM 参数再跑；
6. 跑 2~3 轮，比较 IOPS/延迟是否稳定。

⚠ 这只是“把 FTL 带到近满/GC 状态”的简化方法，**不等于 SNIA PTS 的正式
preconditioning**；若报告引用 PTS，必须按标准补做 preconditioning/steady-state
判定并写明版本与参数。

### 6.5 为什么 256 MB 在空盘上不够触发 GC

- 424 MiB ≈ 3392 个用户块 ⇒ 每块约 **128 KiB**；
- 256 MiB ≈ **2048 块**；
- 空盘 `free≈3300`，写 256 MB 后仍剩 ≈1250 块；
- GC 触发线是 `free < 32` 块（约 4 MiB）⇒ 256 MiB 仍够不到；
- 近满盘（H2testw 写满后 `free≈64`）时，再写少量新数据就会触发 GC。
  **CDM 的“已用 0%”只是 Windows 文件系统视角，不等于 FTL free 高。**

### 6.6 推荐负载矩阵

| 目的 | 负载 | 工具/参数 |
|---|---|---|
| 全盘完整性 | 顺序写满 + 读回校验 | H2testw |
| 随机 4K | `RND4K Q1T1` | CDM 256 MiB / Count 1 |
| 顺序大块 | `SEQ1M Q1T1` | CDM；或 H2testw |
| 混合 | 4K 70% 写 / 30% 读 | diskspd `-b4K -r -w70 -o1 -t1 -d60 -L -Suw` |
| GC 持续压力 | 4K 随机写 60 s | diskspd `-b4K -r -w100 -o1 -t1 -d60 -L -Suw` |

### 6.7 报告模板

| 镜像 | 盘状态 | preconditioning | 块 | QD | 读写比 | MB/s | IOPS | AvgLat | p99 | free/mig |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|---|
| release 18:51:42 | 近满盘/GC | 本仓简化：H2testw 写满 + 删 .h2w（非 PTS） | 4K | 1 | 100W | 532 KiB/s | 133 | 7.50 ms | 12.56 ms | free=54 |

### 6.8 常见坑

- **Windows 读缓存**：读速度可能偏高；用 `-Suw` / `FILE_FLAG_NO_BUFFERING`。
- **CDM 测试文件复用**：`SEQ1M` 和 `RND4K` 共用同一个 Test Size 文件，不会把
  free 再降一半；空盘跑 2+4 两次也够不到 GC 线。
- **AS SSD 默认 1 GB**：424 MB 盘不适合。
- **CDM 1 GiB Test Size**：超过盘容量；用 128/256 MiB。
- **单次波动**：±10~20% 正常；同条件 2~3 次取中位数。
- **H2testw 415/423 MB**：约 2% 安全余量，不是失败。
- **BOT 单命令**：QD>1 主要测主机排队，不代表设备并行。
- **不要把 CDM/diskspd 结果写成“SNIA/JEDEC 标准性能”**：它们只是负载生成器；
  标准管的是 preconditioning、steady-state 与报告条件。
- **preconditioning 方法必须写清**：空盘 best-case、H2testw 写满（本仓简化）、
  还是按 SNIA PTS 的正式流程；三种结果不可比。

---

## 7. 当前实测结果

### 7.1 全盘 H2testw（近满盘、GC 参与）

| 配置 | 写 | 读 | 条件 |
|---|---:|---:|---|
| release 18:51:42 | **765 KB/s** | **884 KB/s** | 415/423 MB 无错误；断电重启 `restored`、`bad=0`（当时 `dng` 为 0）、`map/rd` 抽查通过 |

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

### 7.3 4K 和 CW 的关系

- 1 CW = 512 B；一个 4K = **8 CW = 一个完整 page pair**（2 plane × 4 CW）；
- L2P region = 512 条 × 512 B = **256 KB**；
- 128 MB 覆盖 **512 个 region**，而 L2P cache 只有 64 行左右 ⇒ 随机 4K 会持续
  miss/驱逐，能压到 L2P 缓存、Journal、GC。
- 因此 4K 随机不是“比 CW 还小”的测试，而是这个 FTL 最自然的随机 I/O 单位；
  真正更碎的是 512 B 随机，可作为极端用例，但不是日常必测。

### 7.4 解释口径

- **IOPS**：每秒完成多少次 4K；4K × IOPS ≈ 吞吐。
- **AvgLat**：平均一次 I/O 的 submit→complete；包含 USB 往返 + FTL + NAND。
- **p99 / max**：尾部延迟；`max 20~27 ms` 常来自 GC/CP/Journal 刷写，不等同错误。
- 与 H2testw 的 16 KB/大块顺序数字不能直接比；块大小和队列深度不同。

---

## 8. 复现命令

### 8.1 构建与板端

```bash
make all
make release
make tests UT=FC
```

```text
# 板端 CLI
format                  # 需要空盘时
info
wr 100 1
map 100
rd 100 1
tr 100
map 100
fl
```

### 8.2 H2testw

- 写满 + 校验；记录写/读 KB/s 与 `Test finished without errors`；
- 415/423 MB 是正常余量。

### 8.3 CrystalDiskMark

- 选 F: 424MiB；
- Test Size **256 MiB**（或 128）；
- Test Count **1~2**；
- 重点跑 `SEQ1M Q1T1`、`RND4K Q1T1`；Q8/Q32 参考价值低。

### 8.4 diskspd（Windows）

本地脚本（用户机器，仓库不收录）：

```text
D:\StudyTool\TuBaTool\DiskSpd\amd64\run_diskspd_tests.ps1
```

等价命令：

```powershell
.\diskspd.exe -c128M -b4K -o1 -t1 -r -w100 -d60 -L -Suw F:\diskspd-test\test.dat
.\diskspd.exe -c128M -b4K -o1 -t1 -r -w0   -d60 -L -Suw F:\diskspd-test\test.dat
.\diskspd.exe -c128M -b4K -o1 -t1 -r -w70  -d60 -L -Suw F:\diskspd-test\test.dat
.\diskspd.exe -c128M -b16K -o1 -t1 -s -w100 -d60 -L -Suw F:\diskspd-test\test.dat
```

- 所有测试共用同一个文件，避免 4×128 MB 超过 424 MB；
- `-Suw` 关闭软件缓存 + write-through；
- `-o1 -t1` 对应 USB BOT 的真实串行。
- 非管理员会看到 `SeManageVolumePrivilege` 警告并使用较慢的文件预分配；建议以管理员运行。

### 8.5 板端观测

```text
info
```

关注：

```text
[USB ] wr chunk calls=... bytes=... <=512=... <=4K=... <=16K=... >16K=...
[USB ] latency n=... avg=...us max=...us last=...us total=...ms
[STAT] ch=0 t=...s free=... bad=... mig=.../... mv=... nop=...
```

- `[USB ] latency` 是 submit→complete 设备侧墙钟，和 16 KB 总线时间可相加；
- `free` 是 FTL NAND 空闲块，不是 Windows 剩余空间；
- `[STAT]` 已从 5 s 改为 **10 s**，只留 `ch/t/free/bad/mig/mv/nop`；`op/spare/cp/jrn/wr/dng/rrab/mode`
  用 `info`/`diag` 按需查；
- `mig` 是 **active/paused**，不是 GC 次数；`diag` 的 `mv` 是累计搬移 CW，
  `nop` 是累计空块回收数；`rec` 是成功回收 SB 数（含空块）。
- GC 事件看：

```text
[GC  ] start reason=periodic trig=0x10 mode=off free=...
[GC  ] progress jobs=... moved=... rec=... noop=... abort=... free=...
[GC  ] end jobs=... moved=... rec=... noop=... abort=... elapsed=...ms
```

- 默认 INFO 只打上面三种；逐 victim 的
  `[MIG ] move k=gc src0=... dst0=... v=... mv=... t=...ms` 只在 `log level debug` 输出；
- `vcw=0` 的空块回收不打任何 `[MIG ] move`，只让 `[GC  ] end noop=` /
  `diag rec=` 增长；
- `[GC  ] trig` 只在 `log level debug` 里每 5 s 一行；
  地址细节用 `map <lba>` / `addr pba|paa|g` 反查；
- `free=32` 是 GC 目标线，触发条件是 **`free < 32`**；32 本身不触发；
- BUSY 档跳过 WL/predict 与普通周期评估，但 **`free < FREE_POOL_TARGET` 时仍做
  水位 GC**；写路径分配失败的 urgent GC 也在 BUSY 执行（`RISK-GC-15`），
  否则会等到 free≈1、已无目的页可分配，只能 no-gain；
- GC 时 `free` 应在目标线附近波动，不应持续跌破 8。

### 8.6 地址反查

日志只给 `lba` / `pba` / `gblock` 身份；要展开字段时：

```text
map <lba>                         # LBA → pba/paa + ch/ce/lun/plane/block/page/cw/g
addr pba <pba>                    # PBA → paa + 全部字段
addr paa <paa> [ch ce]            # PAA → PBA + 全部字段（默认 ch=ce=0）
addr g <g> [ch ce lun]            # 实例内全局块号 → plane/block + BST 摘要
```

判据：`map` 得到的 `pba/paa/g/plane/blk/pg/cw` 必须与 `addr` 反查一致；
三者不一致说明日志/命令中的地址口径分叉，按缺陷登记（C9）。

---

## 9. 吞吐优化记录（USB MSC）

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

- `CFG_TUD_MSC_EP_BUFSIZE` 4096 → 16384；`[USB ] latency` DWT 统计；
- stack guard release 启动崩溃修复（`RISK-PLAT-04`）；
- LFSR 8-step 查表、FC 多 Pass 合并、FTL 确认批量化；
- 写确认/TRIM 全异步、Journal 背压、CLI 上报错位修复、MIG 空块日志抑制。

### 9.3 后续方案

| 方案 | 收益 | 风险/成本 | 判据 |
|---|---|---|---|
| EP 16 → 32 KB | 5–10%（估计） | RAM +32 KB（EP + bounce）；直方图/静态断言同步。**2026-10-23 决定暂不实施**：H2testw 受 USB FS 约束，预期收益小 | 分块出现 `<=32K`；无错误；若未来实施，按 32K→64K 的边际收益再评估 |
| FC/NAND 子段 DWT | 定位剩余 6.6 ms | 只加观测 | 子段之和解释 6.6 ms |
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

### 9.4 ITCM / DTCM（2026-10-23，正常路径已上板；压力/掉电未覆盖）

| 项 | 当前值 | 判据 |
|---|---|---|
| `.ssd_code_high`（ITCM） | release 61,632 B / debug 62,080 B / 64 KB | VMA `0x00000000`；启动复制后 USB/NAND 正常；release 余量 3,904 B |
| `.dtcm_data` | release 67,832 B / 128 KB | 预留 20 KB stack；交换版：BCH 进 DTCM、`g_ftl_md` 回 SRAM4 |
| DTCM 内容 | 整对象放置：`ftl.o`（`g_ftl`/`g_ftl_wbuf`/`g_ftl_journal`）、`fc.o`（命令池+描述符池+SQ/HSQ/CQ+簿记）、`nand_dispatch.o`（dispatch queues）、`fc_bch_engine.o`（BCH 静态表） | 均为 CPU-only；**NAND 页缓冲不放 DTCM** |
| SRAM4 内容 | `metadata.o` 的 `g_ftl_md`（33,752 B，NOLOAD，init 时 memset） | 回到 TCM 之前的基线域；基线 843/889 已验证 |
| ITCM 内容 | 公共集（debug/release）：`ftl.o`、`l2p_cache.o`、`wbuf.o`、`pending.o`、`cache_batch.o`、`fc.o`、`fc_data_path.o`、`fc_bch_engine.o`、`nand_dispatch.o`、`nand_fmc.o`。release 额外集：`allocator.o`、`journal.o`、`metadata.o`、`metadata_job.o`、`migrate.o`、`gc.o`、`wear_leveling.o`、`bg_scheduler.o`、`ftl_op.o`、`fc_interface.o`、`fc_queue.o`、`fc_command.o`、`nand_engine.o` | 用对象路径 `*release/obj/...` 做 build-type 选择，不是 C 预处理器；debug 对象在 `debug/obj/`，不匹配额外集。超过 64 KB 或出现 relocation truncated 时按目标文件收窄 |
| 板端 A/B | 基线 843/889 KB/s；TCM 版（release ITCM 29,728 B）840/885 KB/s；当前 release ITCM 61,632 B（板端结果为重构前 61,384 B 镜像）+ BCH↔md 交换版 **843/887 KB/s**，`info avg=4171us`、`cw=24906/99624/7891/0`、`bad=0`、`mig=0/0`、`gcm=0`，H2testw 415/423 MB 无错误 | 与 840/885 差 +3/+2 KB/s，属测量抖动；本次未触发 GC/迁移，额外 ITCM 集收益未覆盖 |
| BCH/堆 A/B | 旧实现 BCH 需 41 KB 表 + 32 KB roots 的 DTCM 堆；TCM 版只剩约 67 KB 堆，疑似初始化失败并静默跳过 ECC | 启动日志应出现 `[FC ] BCH ready: static tables, no heap`；写速恢复 ≥843 KB/s |
| BCH↔md 交换（正常路径已上板） | BCH 42,656 B → DTCM；`g_ftl_md` 33,752 B → SRAM4；DTCM 67,832 B，free≈63 KB | 2026-10-23 boot `BCH ready` + 恢复 + H2testw 415/423 MB 0 错误（843/887 KB/s）通过；压力/掉电仍按 RISK-TCM-04 |
| 页缓冲域 A/B | 页缓冲放 DTCM：multi-plane handler 数据正确但 NAND 读回全 FF，`format` 全候选失败；放 AXI_NOCACHE：`format` 成功且 H2testw 415/423 MB 通过（840/885 KB/s） | 页缓冲必须 AXI_NOCACHE；不得再条件放 DTCM |
| ITCM 收益判据 | H2testw 是 USB FS（12 Mbit/s 链路，实测 843/887 KB/s 已接近可用带宽）+ NAND tPROG 约束下的顺序负载；本次 `mig=0/0`、`gcm=0`，额外 ITCM 模块基本未执行 | **不能用 H2testw 判断 ITCM 收益**；用 BCH/metadata/GC CPU-bound 微基准，或填盘后随机覆盖触发 GC 再 A/B |

⚠ `DTCM_DATA` / `.dtcm_data` 是 NOLOAD：`g_ftl` 必须在 `ftl_init` 入口全量 memset；
其余候选 handle 已有显式 memset。H743 startup 原先不复制 `.ssd_code_high`，本版已补
`__ssd_code_high_load_start/start/end` 复制循环。

⚠ DMA/NAND 页缓冲：`g_fc_buffer_pool` / `g_fc_return_staging` / `g_nand_channel_page_buffer`
恒在 `.noncacheable_buffer`（`AXI_NOCACHE`，`0x24060000..0x24080000`）。
**页缓冲放 DTCM 会静默破坏 multi-plane program**（NAND 读回全 FF），已由板端 A/B 证实；
不得再按 `CONFIG_NAND_DMA` 条件把它放回 DTCM。

**不建议**为了跑分提前回 CSW 或跨 BOT 命令合并写：会破坏掉电语义。

---

## 10. 当前限制与未闭合

**标准符合性**

- 本文**不声称**符合 SNIA PTS / JEDEC JESD218/219 / ONFI / USB-IF；
- 性能结果只声称“本仓硬件/镜像/盘状态下测得”；
- “SNIA 式 / 简化 preconditioning”必须按 §6.4 注明是本仓简化。

**能力边界**

- `[USB ] latency` 未在 765/884 那轮写阶段取到（写后已断电）；
- 4K 随机那组**未同步记录 banner/free**，只能当量级参考；
- `make tests UT=FTL` 仍因冻结测试源编不过；FC UT 需上板；
- 未做 USB-IF 认证、USB 协议分析仪、NAND ATE；
- 未做完整 JEDEC 耐久性 / retention / read disturb / P/E 循环；
- USB BOT 单命令，不能测主控多队列/门铃；
- 4K 随机结果受 Windows/USB 调度影响；单次 ±10~20% 正常；
- 未做 512 B 随机（真正小于 CW 的碎片）与 partial-page 专项；
- `RISK-HOST-07/08/09/10`、`RISK-L2P-06`、`RISK-NAND-04` 见
  `docs/design/known_risks.md`。
