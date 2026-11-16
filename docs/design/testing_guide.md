# 存储主控 / 软 SSD 测试指南（2026-10-22）

> 定位：从板端 NAND/FC 到主机 USB 的**分层测试流程、工具、性能方法、当前实测**。
> 配套：`docs/design/usb_msc.md` Part A（USB 架构）、`docs/design/usb_msc.md` Part B
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
- 掉电：写到一半 / TRIM 后断电，重启 `map`/`rd`；**已验证一轮（2026-10-31）**：format → H2testw 写 130 MB 时断设备电 → 上电 `boot type=2`、`journal_entries=8704`、`closed=1050 free=2335 anomalies=0`，挂载后 H2testw verify 117 MB **0 错误**；尾差（130→117 MB）主要是主机写回缓存/文件大小元数据，设备窗口未分离（RISK-CACHE-18）；第二轮（覆盖写）2026-10-31：写 245 MB 掉电（`closed=1962 anomalies=0`）→ 删文件覆盖写 140 MB 掉电（`journal_entries=7688`、`closed=3086+2 anomalies`、`map 1000` VERDICT ok）；内容校验未做。**旧数据内容的低成本校验（建议）**：保留一个旧 `.h2w` 不删、删其余 → H2testw 再写新的（占用被释放的簇）→ 写到一半掉电 → 重启后 H2testw verify：它会校验所有现存 `.h2w`，旧文件过校验即证「未被覆盖的旧数据完好」。不要用 CLI `wr` 造 pattern（单次 ≤32 扇区、裸 LBA 会与主机 FS 冲突）。**可选（暂缓，2026-10-31 用户决定）**：设备 VWC 窗口量化——预分配文件（`SetLength` 后只 flush 一次）+ `FileOptions.WriteThrough` 逐块写已知 pattern，写到一半断设备电，重启后按块比对，丢失量应 ≤ ~1 MB。⚠ 不能用 CLI `wr`（同步确认，不经过提前回执窗口）；用 H2testw 也无法分离主机 FS 元数据。
**2026-10-31 flush 屏障已验证**：写 168 MB → 关闭 H2testw → 安全弹出 → 断设备电 → 上电 `boot type=1`/`journal_entries=0`、`closed=1342 free=2043 anomalies=0`、verify **167 MB 0 错误、无尺寸缺口** ⇒ flush 是真正的持久性屏障；未 flush 时的尺寸缺口来自主机写回/FS 元数据。
**2026-10-31 补充观察**：写 152 MB 后空闲 ~20 s 再断设备电，上电 `boot type=1`（干净启动）、`journal_entries=0`——空闲期 CP 追平后覆盖了 Journal 尾部，无需回放；`closed=1222 free=2163 anomalies=0` 对账一致，verify **137 MB 0 错误**（152→137 的差仍是主机侧文件大小元数据未落盘，文件未 close）。
**判读坑（2026-10-31 实测）**：不 flush 拔电后 H2testw verify 可能报 `Error reading file … Code 1392（文件或目录损坏）`——这是**主机文件系统元数据不一致**（文件大小已落盘、簇链没落盘），不是设备数据错；数据错会报 `Data error at offset …`。先 `chkdsk /f` 再判（2026-10-31 闭环：chkdsk 修正文件大小 138→127 MB 后，H2testw verify **127 MB 0 错误**、`dng=0`）。要测设备掉电窗口必须让主机先 flush/关写缓存，否则测到的是 FAT/NTFS 的崩溃一致性。TRIM 后掉电、GC 压力下掉电未做；
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
  cr=<batches>/<pairs>/<starts>/<declines>/<fallbacks>` 与
  `l2p[ch][ce][lun] cw=<batches>/<pages>/<declines>/<aborts> sw=<starts>/<page_ends>
  pc=<pad_closes> jf=<journal_page_writes> mb=<jobs>/<pages>/<aborts>`；
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

> 当前基线（release -O2，2026-10-25）：写 **843 KB/s** / 读 **889 KB/s**，H2testw
> 415/423 MB 无错误；cache program 开（`cw=24906/99624/7945/0`），cache read 开
> （`cr=25931/102794/681/0`）。完整两轮对比表见
> `docs/archive/testing_guide_results_history.md`。

### 7.2 4K 随机（Windows diskspd，128 MB 文件）

> 量级参考（QD1 / 1 thread / `-Suw`）：4K 随机写 **532 KiB/s**、读 **768 KiB/s**、
> 70/30 **584 KiB/s**。该组未同步记录板端 `free`/镜像 banner，只作量级参考；
> 完整表见 `docs/archive/testing_guide_results_history.md`。

### 7.3 4K 和 CW 的关系

- 1 CW = 512 B；一个 4K = **8 CW = 一个完整 page pair**（2 plane × 4 CW）；
- L2P region = 512 条 × 512 B = **256 KB**；
- 128 MB 覆盖 **512 个 region**，而 L2P cache 只有 64 行左右 ⇒ 随机 4K 会持续
  miss/驱逐，能压到 L2P 缓存、Journal、GC。
- 因此 4K 随机不是“比 CW 还小”的测试，而是这个 FTL 最自然的随机 I/O 单位；
  真正更碎的是 512 B 随机，可作为极端用例，但不是日常必测。

### 7.5 速度分层对照（2026-10-25，release -O2）

> 关键结论：NAND 阵列 + 总线理论上限约 **61 MB/s 读 / 20.5 MB/s 写**（2-plane）；
> FC raw **3.68 / 4.42 MB/s**；FTL host **3.45 / 2.75 MB/s**；USB MSC **0.889 / 0.843 MB/s**。
> 当前内部瓶颈是 **CPU 数据面**（PIO + randomizer/CRC + BCH），不是 NAND/总线。
> 完整分层表与耗时折算见 `docs/archive/testing_guide_results_history.md`。

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

### 8.7 测试运行产物（`build/test-results/`）

原始串口日志与运行元数据放 `build/test-results/`（`build/` 已被 `.gitignore` 整目录忽略），**不放 `tests/`**：`tests/` 只放测试源码（R4），生成物混入会让 `git status` 变脏。

每轮一个目录：

```text
build/test-results/<YYYYMMDD-HHMMSS>_<build>_<target>/
  run.log      # 串口终端工具的 log（原始输出）
  meta.txt     # commit / 镜像横幅时间戳 / 命令 / 结果 / 备注
```

用 `sh/test_run_dir.sh [debug|release] [debug_ut|release_ut]` 创建目录并生成 `meta.txt`；脚本打印路径，把串口终端工具的日志目录指到它即可。

`meta.txt` 的四件套是证据判据（P6）：`commit` = 代码版本，`image` 的横幅时间戳 = 烧的是哪个镜像，`command` / `result` = 跑了什么、结果如何。报告引用结论时抄这四行；原始日志不进版本库，需要长期留证的结论写本文件 §7 或 `docs/design/known_risks.md`。

---

## 9. 吞吐优化记录（USB MSC）

### 9.1 当前结论

> `-O0` 是设备侧主因；16 KB EP 有效；cache program 使顺序写达到
> **843 / 889 KB/s**（avg latency 4.1 ms）；USB FS BOT 仍是主瓶颈。逐轮实测、
> 已做改动与后续方案见 `docs/archive/testing_guide_results_history.md`。

### 9.4 ITCM / DTCM（当前结论）

- `.ssd_code_high`：release 61,344 B / debug 57,168 B / 64 KB；debug 余量 8,368 B（2026-10-30）。
- `.dtcm_data`：release 67,832 B / 128 KB；预留 20 KB stack。
- `g_ftl_md` 放 SRAM4；BCH 静态表放 DTCM；**页缓冲必须在 AXI_NOCACHE，不得放 DTCM**。
- **不能用 H2testw 判断 ITCM 收益**（USB FS / NAND tPROG 约束）；用 CPU-bound 微基准或
  填盘后随机覆盖触发 GC 再 A/B。
- 详细 A/B 记录见 `docs/archive/testing_guide_itcm_ab.md`。

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

---

## 11. 负例执行清单（可执行）

> 来源：原独立文档 `negative_test_plan.md`，已归档到 `docs/archive/negative_test_plan.md`。
> 目的：把 `docs/design/known_risks.md` 里「待上板」的负例判据整理成可逐条执行的清单。
> **未上板前不得勾选**（R1）。

> 目的：把 `docs/design/known_risks.md` 里「待上板」的负例判据整理成可逐条执行的清单。
> 每条都给出：风险 ID、触发方式、期望、证据。**未上板前不得勾选**（R1）。
>
> 当前硬件：无程控电源 / USB 继电器 ⇒ 掉电与拔线只能手动，无法自动化与大批量。
> 已有能力：测试镜像的固件注入钩子（`FC_NAND_TEST_ENABLE`）+ 串口 CLI + H2testw/`/bench/speed`/UT。

---

### 11.0 前置条件

| 项 | 要求 |
|---|---|
| 测试镜像 | `make tests UT=FTL`（或 `UT=FC` / `UT=HIL`）——只有 `make tests` 定义 `FC_NAND_TEST_ENABLE` |
| 负例档 | `/fc-nand/negative/` 的 13 条 release-only 用例在 debug 下 `MUNIT_SKIP`；验证拒绝语义用 `make tests BUILD_TYPE=release UT=FC`（同为测试镜像，`ASSERT_REJECT` 才返回错误码） |
| 注入钩子 | `fault_inject.c/h` 与 `bbt fault` **只在测试镜像存在**；产品 debug/release 均不编入 |
| 串口 | USART1 115200，`log level debug`（按需） |
| 主机 | H2testw（全盘写读）、`/bench/speed`（FTL/FC 压测） |
| 判据取证 | 串口原始日志 + `info` / `ftl_ctx` 输出 + H2testw 报告 |

> ⚠ 注入类用例必须在**测试镜像**上跑；产品镜像跑不出这些路径（钩子不存在）。
> ⚠ `bbt fault` 会永久标坏块（写 BBT），跑完建议 `format --verify` 恢复介质。

---

### 11.1 NAND 写失败 / program fail

- [ ] **RISK-HOST-06** 写重试
  - 触发：`bbt fault <plane> <block>` 标坏即将写入的 data 块，再 `wr` 同一区域。
  - 期望：串口出现 `[FTL] write retry`，host 写仍成功；第二次失败如实 `IO_ERROR`。
  - 证据：串口行 + `info` 的 `bad` 增长。
- [ ] **RISK-CACHE-30/31/32** cache program 失败/撤销
  - 触发：`fi_cache_fail_arm <lba>`（测试镜像），写该 LBA 后读回。
  - 期望：失败页及其后页不确认映射、之前页已确认；无 `pad: stray page`；`cache_failc_abort` 类 UT 同步通过。
  - 证据：串口 + `/ftl/media/` 结果。
- [ ] **RISK-IF-08** NAND 读超时/FAIL 归因
  - 触发：注入单页读失败（`fi_*` 或 `bbt fault` 后读）。
  - 期望：命令状态带 `TIMEOUT`/`FAIL_CACHE`，FTL 不进成功路径。

### 11.2 Journal / Checkpoint 失败

- [ ] **RISK-JN-01/04/07/08/10** Journal 擦/写失败与撕裂页
  - 触发：`fi_journal_fail_arm <pages>`，写满一个环形块后复跑。
  - 期望：`[JRN] erase kick failed` 有界；`resume: skip torn page`；不二次编程；回放不完整时回退 full rebuild。
  - 证据：串口 + 重启后 `boot type=` / `journal_entries=`。
- [ ] **RISK-CP-04/05/07** CP 提交饥饿 / abort 幂等 / flush 屏障
  - 触发：持续 H2testw 写 + 观察 `[CP] submit stalled`；`fl` 后立即断电（见 §11.3）。
  - 期望：`abort at phase=1`（不是 12）、1s 限流；断电重启 `find latest` + restore，非 full rebuild。
- [ ] **RISK-HOST-05/16** confirm park timeout
  - 触发：满盘 + GC 压力（H2testw 写至 free 低位），开 `--log-project info`。
  - 期望：无 `confirm park timeout`；若有，`parked=`/`fc=` 能定位。

### 11.3 掉电（手动，当前无硬件）

> 步骤：H2testw 写到 `free` 低位（或指定 MB）→ **直接断板子供电**（不是拔 USB 线）→ 上电 → H2testw verify / `rd` 读回。

- [ ] **RISK-MD-06** 恢复路径丢映射（最高优先）
  - 步骤：`format` → 全量 `test`（制造多代写）→ `/bench/speed/ftl_rw` 写 25 MiB → 第 4 次重启前断电。
  - 期望：无 `region … degraded` / `mapping lost` / `fallback`；`info` 的 `closed` 对账一致。
  - 证据：开机完整日志 + `info`。
- [ ] **RISK-BOOT-15/18/22/23** 启动恢复/格式化中断
  - 步骤：格式化过程中（BBT save 后 / erase 中 / initial CP 前）断电；再上电。
  - 期望：能继续格式化或 `restored`；不出现 `refuse to format`。
- [ ] **RISK-CP-07** SYNCHRONIZE CACHE 屏障
  - 步骤：H2testw 写完后安全弹出（触发 fsync）→ 断电 → 上电 verify。
  - 期望：0 错误、无尺寸缺口。
- [ ] **RISK-HOST-14** 拔线/中止后的门控环形等待
  - 步骤：写到接近满盘 → 拔 USB（不断电）→ 重插 → 读回/校验。
  - 期望：`drain stalled` 快速触发并自愈；无 `dirty=64` 持续刷屏。

### 11.4 拔线 / 总线复位（手动或 `devcon`）

- [ ] **RISK-USBC-01/03、RISK-MSC-02** USB 中止/异步完成
  - 触发：读写过程中手动拔线；或 Windows `devcon disable/enable "USB\VID_0483&PID_5720*"`（逻辑断开）。
  - 期望：无死命令、无残留 CSW；重插后正常挂载；`pending_io` 不永久为真。
- [ ] **RISK-USB-10/13** 同 LBA 连写 / 两写槽时读
  - 触发：连续两次写同一 LBA（间隔 < FTL 完成）；两 chunk 在飞时立刻读。
  - 期望：两条命令都完成；读不报 I/O error。

### 11.5 坏块 / 读失败 / 超时

- [ ] **RISK-BBT-01/11/15/17/19/20** BBT 幂等/持久化/擦失败
  - 触发：`bbt fault` + 重启；`format --verify`；写飞行中再标坏。
  - 期望：`bad_count`/`spare_count`/BBT 位一致；`old_bad/recovered/new_bad` 合理；无 livelock。
- [ ] **RISK-GC-01/07/10、RISK-MIG-02/07** victim 读失败与熔断
  - 触发：`bbt fault` 标坏 victim plane 后触发 GC；H2testw 写到 2/3。
  - 期望：3 次失败后换候选；连续 8 个 victim 失败熔断；不成批标坏、不活锁。
- [ ] **RISK-RR-02/04** RR 恢复/预测
  - 触发：坏页读（`fi_*` 或 `bbt fault`）+ 可恢复/不可恢复两种。
  - 期望：可恢复返回成功；单 plane 不可恢复整体 IO_ERROR；含 BAD plane 的 SB 被跳过。

### 11.6 压力 / 背压 / 超时

- [ ] **RISK-GC-06/14、RISK-WL-07、RISK-CP-04** 满盘 GC/WL/CP
  - 触发：H2testw 写满 + `/bench/speed`；观察 `[GC]`/`[WL]`/`[CP]`。
  - 期望：写速稳定、无 `submit stalled` 刷屏、无 `confirm park timeout`；WL 不在 host 写期间占引擎。
- [ ] **RISK-IF-09/10/12/13** 流式序列/门控
  - 触发：大顺序读写 + 中途中止；观察 `d4=`/`mig=`/`rrab=`。
  - 期望：放弃序列不饿死后台；链尾不占 `chain_owner`。

---

### 11.7 目前缺硬件导致的覆盖缺口

| 场景 | 现状 | 自动化方案（后续） |
|---|---|---|
| 真实掉电 | 手动断板子供电，次数有限 | USB 继电器 / YKUSH / 程控电源 + 脚本 |
| 真实拔线 | 手动拔插 | 同上（切 VBUS） |
| 大批量掉电组合 | 无法批量 | 继电器 + H2testw/`/bench/speed` 脚本循环 |
| 总线复位 | `devcon` 逻辑复位可部分替代 | USB 协议分析仪（Beagle/LUNA） |

> 主机侧 `dm-flakey` / `dm-dust` / `scsi_debug` 只测**主机块层**，不是本设备的 FTL，
> 不能作为上板证据（R4）。

---

### 11.8 记录模板

每条执行后回填：

```text
RISK-XXX-NN | 镜像横幅（__DATE__ __TIME__）| 触发步骤 | 期望/实际 | 结论（通过/失败/未复现）| 证据（日志行）
```

结果写回 `known_risks.md` 对应条目的「历史/验证」行；通过的按 `docs/design/known_risks.md` §0.4 判定是否 `已闭合`。
