# USB MSC U 盘上板验证手册

> 状态：**基础数据面已上板通过（rev27，2026-10-20）；掉电/性能/FTL 专项待测**。
> 目标平台：阿波罗 V2 核心板（STM32H743IIT6 + MT29F4G08 + Type-C USB）。
> 配套脚本：`tools/usb_verify/`（`usb_verify.ps1` / `usb_rebuild_mbr.ps1`）。
> 设计与取舍：`docs/design/usb_msc_udisk.md`。已知风险：`docs/design/known_risks.md`。

本文把「把板子当 U 盘用」拆成**可独立判定**的步骤。每一步都写清：验证什么、
执行什么、看什么、期望什么、证据是什么。不要把「枚举」「格式化」「数据完整性」
「掉电恢复」混在一次操作里——混在一起时，失败无法归因。

---

## 1. 范围

### 1.1 本手册覆盖

- USB 设备枚举（`USB2_OTG_FS`，全速 12 Mbps）
- SCSI 容量 / 分区 / FAT 格式化（host 侧）
- 主机多页、非均匀数据的写 → flush → 拔插 → 读回校验
- FTL 在 host 数据面上的基本正确性（L2P / wbuf / journal）
- 失败时的判据与恢复动作

### 1.2 本手册不覆盖（见 §8）

- 大文件/GC 压力、`dd` 吞吐
- 写过程中掉电恢复
- `RISK-MD-06`（回收旧日志块 vs Anchor 落盘）的专项复现
- `RISK-MIG-02`（predictive 迁移活锁）的修复验证
- UASP / TRIM / 多 LUN / HS 480 Mbps（v1 非目标）

---

## 2. 环境与前置

| 项 | 要求 |
|---|---|
| 板子 | STM32H743 + MT29F4G08，USB Type-C 接 **PA11/PA12（OTG_FS，AF10）** |
| 固件 | 含本手册对应功能的 `debug.elf`；本手册实测为 **rev27** |
| 主机 | Windows 10/11（原生），管理员窗口仅用于分区/格式化/raw MBR 恢复 |
| 串口 | USART1 115200，用于看板端日志（`[USB]` / `[FTL]` / `[L2P]` / `[MIG]`） |
| 脚本 | `tools/usb_verify/usb_verify.ps1`、`usb_rebuild_mbr.ps1` |

### 2.1 关于 WSL 与脚本运行

WSL 里的 `lsblk` **看不到** Windows 已枚举的 USB 大容量存储设备（`Virtual Disk`
是 WSL 虚拟盘）。两个选择：

1. **推荐**：验证在 Windows 原生做（本手册）；
2. 需要在 Linux 工具（`mkfs.vfat` / `fsck.vfat` / `dd` / `fio`）下做时，先用
   `usbipd-win` 把设备 attach 给 WSL；这会把它从 Windows 摘走，测完 `detach` 归还。

脚本运行注意：

- **把 `tools/usb_verify/` 复制到 Windows 本地再运行**。直接从 WSL 的 UNC 路径
  （`\\wsl.localhost\...`）运行会被 Windows PowerShell 的 `RemoteSigned` 拒绝；
- 脚本是 **UTF-8 with BOM**（Windows PowerShell 5.1 读无 BOM 的 UTF-8 中文注释会
  吞换行，导致语法错误）。编辑后保持 BOM。

### 2.2 烧写后的第一证据

串口横幅必须显示**当次**镜像：

```text
=========== soft_ssd rev=27 built ... ===========
```

⚠ 不看横幅就报「验证通过」不算证据（CLAUDE.md R1 / §3.3）：`.noinit` 崩溃记录、
Windows 磁盘缓存、旧镜像都会制造假象。

---

## 3. 安全规则

1. **脚本只认 `FriendlyName=*SOFTSSD*`、`Size=536870912`、`BusType=USB`**；
   任一不符立即中止。手工操作时也必须先用 `Get-Disk` 确认。
2. **不要碰系统 NVMe**（Number 0/1）。`format` / `usb_rebuild_mbr.ps1` 都是破坏性的。
3. 主机操作期间**不要同时敲板子 CLI**：两者共用 host busy gate，会互相让路或报 busy。
4. 板端 CLI 的 `format`（FTL 格式化）完成后必须**断电重启**（命令会打印
   `POWER-CYCLE NOW`）；这与 Windows 侧格式化不是一回事。
5. 写测试文件期间不要拔板子电源（掉电测试单独做，见 §8）。

---

## 4. 验证矩阵

| ID | 验证什么 | 执行 | 期望 | 状态 |
|---|---|---|---|---|
| USB-V01 | USB 枚举与 MSC 挂载 | 上电，看串口 + `Get-Disk` | `[USB ] mounted`；Windows 出现 512MiB USB 磁盘 | ✅ rev27 |
| USB-V02 | 容量 / 扇区 / 总线 | `-Step inspect` 或 `Get-Disk`/`Get-Volume` | Size=536,870,912；512/512 字节扇区；BusType=USB | ✅ rev27 |
| USB-V03 | MBR 分区 + FAT 格式化 | 管理员 `-Step format`（或磁盘管理） | 新建主分区 + FAT，卷 Healthy/OK | ✅ rev27 |
| USB-V04 | 非均匀多页写 → 读回 | `-Step write` | `SRC_HASH == DST_HASH` | ✅ rev27 |
| USB-V05 | 4KB pair 镜像 / 2KB 块指纹 | `-Step check` | pattern 的 2KB 块 0..7 内容分别为 0..7 | ✅ rev27 |
| USB-V06 | SYNCHRONIZE CACHE + 拔插持久性 | `write` → `Write-VolumeCache` → 拔插 → `check` | `RESULT=PASS`，哈希不变 | ✅ rev27 |
| USB-V07 | 大文件 + GC/CP 压力 | 写 32–64MB 随机文件，flush，拔插，哈希 | 哈希一致；无 `[MIG]` 风暴 / 无 ASSERT | ⬜ 待测 |
| USB-V08 | 写过程中掉电 | 写临时文件时断板子电源，重启重挂载 | 盘能重新挂载；FTL journal/CP 恢复；损坏范围可解释 | ⬜ 待测 |
| USB-V09 | 吞吐 | 大文件复制 / `dd` 计时 | USB FS 预期 ~1 MB/s 量级；记录实测 | ⬜ 待测 |
| FTL-V01 | `RISK-MD-06` 回收前 Anchor | 填满一个元数据日志块（64 个 region）后冷启动 | 无 `[L2P] load failed`；`anchor_seq > cp_seq` | ⬜ 待测 |
| FTL-V02 | `RISK-MIG-02` predictive 活锁 | 观察串口 `[MIG] abort kind=2 ... victim read failed` | 不应反复重选同一 victim；坏块应被隔离 | ⬜ 待测 |

---

## 5. 详细步骤

### USB-V01 枚举

**验证什么**：PA11/PA12 接线、HSI48 时钟、VBUS sensing 关闭、描述符与
`SET_CONFIGURATION` 全部成立。

**执行**：

1. 上电，串口应出现：

```text
[INF] [USB ] device stack ready (USB2_OTG_FS, full speed)
[INF] [USB ] mounted (MSC BOT, 1048576 sectors x 512 bytes)
```

2. Windows：

```powershell
Get-Disk | Where-Object BusType -eq 'USB' | Format-Table Number,FriendlyName,Size,PartitionStyle,BusType
```

**期望**：

- 设备侧有 `[USB ] mounted`；
- 磁盘 `FriendlyName = SOFTSSD NAND U-DISK`、`Size = 536870912`、`BusType = USB`。

**证据**：串口原文 + `Get-Disk` 输出。⚠ 只有 `mounted` 不能证明数据面可用，继续 V02。

---

### USB-V02 容量与扇区

**验证什么**：SCSI READ CAPACITY / 扇区大小与 FTL 坐标一致（1 LBA = 512B，
`FTL_LBA_MAX = 1,048,576`）。

**执行**：

```powershell
powershell -NoProfile -File .\usb_verify.ps1 -Step inspect
```

或手工：

```powershell
Get-Disk | Where-Object BusType -eq 'USB' | Format-List Number,FriendlyName,Size,LogicalSectorSize,PhysicalSectorSize,PartitionStyle
Get-Volume | Where-Object DriveType -eq 'Removable' | Format-Table DriveLetter,FileSystemLabel,FileSystem,HealthStatus,Size
```

**期望**：

- `Size = 536870912`（512 MiB）；
- `LogicalSectorSize = PhysicalSectorSize = 512`；
- 格式化后的卷 `Size ≈ 536,584,192`（扣除分区表/对齐）。

---

### USB-V03 分区 + FAT 格式化

**验证什么**：主机写出的 MBR/分区表/FAT 引导扇区能被设备正确保存并读回；
也就是 host 多页、非均匀写路径正确。

**执行（管理员）**：

```powershell
powershell -NoProfile -File .\usb_verify.ps1 -Step format
```

或 Windows 磁盘管理：初始化 MBR → 新建简单卷 → FAT32 → 分配盘符。

**期望**：

- `New-Partition` / `Format-Volume` 成功；
- `Get-Volume`：`FileSystem = FAT/FAT32`、`HealthStatus = Healthy`、
  `OperationalStatus = OK`；
- 资源管理器能打开盘符、能新建文件夹。

**失败时先看串口**：

- `[USB ] unsupported SCSI opcode=0xXX` → 主机用了未实现的 SCSI 命令（记录 opcode）；
- `[MIG] abort kind=2 ... victim read failed` 反复出现 → 见 `RISK-MIG-02`，
  先 CLI `format` + 断电重启；
- 哈希/格式化在**重插后**才失败 → 见 USB-V05 的 pair 镜像判据。

**分区表异常**（offset 0 / Logical 分区 / `Not enough available capacity`）：

```powershell
# 管理员
powershell -NoProfile -File .\usb_rebuild_mbr.ps1
```

它会对 `\\.\PhysicalDriveN` 前 1MB 写零、重建 MBR、再格式化为 FAT32。

---

### USB-V04 非均匀文件写 + 读回

**验证什么**：host 多页写、wbuf 页打包、L2P 映射、读回路径在**数据内容不均匀**时正确。
这是对 `RISK-WB-06`（plane-major pair 镜像被单页函数填装）的回归。

**执行**：

```powershell
powershell -NoProfile -File .\usb_verify.ps1 -Step write
```

脚本会：

- 复制 `C:\Windows\System32\notepad.exe` → `F:\test.bin`（360448 B，内容非均匀）；
- 生成 `F:\pattern.bin`：16KB，每 2KB 块内容 = 块号 0..7；
- 计算源/盘上 SHA256；
- `Write-VolumeCache`（SYNCHRONIZE CACHE）；
- 状态写入 `%TEMP%\softssd_usb_verify_state.json`。

**期望**：

```text
SRC_HASH == DST_HASH
FLUSH_OK
```

⚠ 立即读回一致**还不够**：Windows 可能从缓存返回刚写的数据。必须做 V06 的拔插。

---

### USB-V05 2KB 块指纹

**验证什么**：4KB 写入（= 2 plane × 2KB）中 plane1 的数据没有被 plane0 覆盖。
这是 `RISK-WB-06` 的直接判据。

**执行**：在 V04 之后、拔插之后运行 `-Step check`（脚本会自动做块指纹）。

或手工：

```powershell
$a = [IO.File]::ReadAllBytes('F:\pattern.bin')
for ($i=0; $i -lt 8; $i++) {
    $ok = $true
    for ($j=0; $j -lt 2048; $j++) {
        if ($a[$i*2048+$j] -ne [byte]$i) { $ok=$false; break }
    }
    "block=$i all_match=$ok"
}
```

**期望**：8 个块全部 `all_match=True`，且块号与内容一致。

**为什么不能用 CLI `wr 0 32 c` 代替**：CLI pattern 每 2KB 相同（0x55/0xAA），
plane1 复制 plane0 时**完全看不出来**。必须用每 2KB 互不相同的数据。

---

### USB-V06 flush + 拔插持久性

**验证什么**：host `SYNCHRONIZE CACHE` 路径、FTL journal/L2P 持久化、重新枚举后读回一致。

**执行**：

1. V04 已完成（`Write-VolumeCache` OK）；
2. **拔掉 USB 线、重新插上**；
3. 运行：

```powershell
powershell -NoProfile -File .\usb_verify.ps1 -Step check
```

**期望**：

```text
DST_HASH == EXPECTED_DST_HASH
PAT_HASH == EXPECTED_PAT_HASH
PATTERN_BLOCKS_OK=True
RESULT=PASS
```

失败时返回码 1。

---

## 6. 脚本速查

先把 `tools/usb_verify/` 复制到 Windows 本地（UNC 直接运行会被执行策略拒绝），然后：

```powershell
# 1) 只读
powershell -NoProfile -File .\usb_verify.ps1 -Step inspect

# 2) 格式化（管理员）
powershell -NoProfile -File .\usb_verify.ps1 -Step format

# 3) 写测试数据
powershell -NoProfile -File .\usb_verify.ps1 -Step write

# 4) 拔插后校验
powershell -NoProfile -File .\usb_verify.ps1 -Step check

# 分区表损坏时（管理员）
powershell -NoProfile -File .\usb_rebuild_mbr.ps1
```

显式指定盘：

```powershell
powershell -NoProfile -File .\usb_verify.ps1 -Step inspect -DiskNumber 2 -DriveLetter F
```

脚本细节、日志位置、UTF-8 BOM 与执行策略说明见 `tools/usb_verify/README.md`；
每个脚本都带 comment-based help，可 `Get-Help .\usb_verify.ps1 -Full` 查看。

---

## 7. 故障排查

| 症状 | 先查什么 | 动作 |
|---|---|---|
| Windows 完全看不到盘 | 串口是否有 `[USB ] mounted`；设备管理器有无未知设备 | 无 mounted：查 PA11/PA12、HSI48、VBUS sensing；有 mounted 无磁盘：查 SCSI 容量/inquiry |
| 能看到盘，格式化失败 | 串口 `[USB ] unsupported SCSI opcode=0xXX` | 记录 opcode；补对应 SCSI 命令（`0x91` = SYNCHRONIZE CACHE(16)、`0x2F` = VERIFY(10)、`0x42` = UNMAP） |
| 格式化失败且串口有 `[MIG] ... victim read failed` 风暴 | `RISK-MIG-02` | 先 CLI `format` + 断电；仍复现则修 predictive 失败计数/黑名单 |
| 重插后哈希不一致 | V05 的 2KB 块指纹 | 若块 1=块 0：`RISK-WB-06`（pair 镜像填装）；确认 rev 已含 `ftl_cmd_write_data_pair_cw` |
| 分区表 offset 异常 / Logical 分区 / Not enough capacity | `Get-Partition` 的 Offset/MbrType | 跑 `usb_rebuild_mbr.ps1` |
| `Write-VolumeCache` 拒绝访问 | 是否普通窗口 | 改用 Windows“安全删除硬件”后再拔插 |
| WSL `lsblk` 看不到 | 是否在 WSL 里 | 用 Windows 原生，或 `usbipd-win` attach |
| 串口反复 `[MIG] kind=1` | 是否 WL 在收敛 | 观察是否自行停止；长时间不停按 WL 活锁单独排查 |

---

## 8. 本轮实测记录（2026-10-20）

| 固件/步骤 | 结果 | 说明 |
|---|---|---|
| rev24 枚举 | ✅ | 串口 `[USB ] mounted`；确认 PA11/PA12 可用 |
| rev24/25 host 格式化 | ❌ | Windows 报“无法完成格式化 / track 0 bad”；串口出现 `[L2P] region 0 load failed`、`[MIG]` 风暴；期间定位了 `RISK-L2P-04` / `RISK-MD-06` / `RISK-MIG-02` |
| rev26 MBR 重建 + 文件测试 | ❌ | 能分区/挂载，但 `notepad.exe` 拔插后哈希不一致；2KB 块指纹显示块 1=源块 0、块 3=源块 2 ⇒ 定位 `RISK-WB-06` |
| rev27 pair 镜像修复 | ✅ | FAT 格式化成功；`notepad.exe` 源/盘哈希一致；16KB pattern 的 2KB 块 0..7 全匹配；`Write-VolumeCache` OK；拔插后 `RESULT=PASS` |

**脚本形态的验证**：

| 脚本步骤 | 状态 | 说明 |
|---|---|---|
| `usb_verify.ps1 -Step inspect` | ✅ | 自动识别 SOFTSSD/512MiB/USB，输出磁盘/分区/卷 |
| `usb_verify.ps1 -Step write` | ✅ | 写 `test.bin` + `pattern.bin`，`SRC_HASH==DST_HASH`，`FLUSH_OK` |
| `usb_verify.ps1 -Step check` | ✅（逻辑） | 哈希 + 2KB 块指纹 `RESULT=PASS`；真实拔插判据用同一套检查手工做过 |
| `usb_verify.ps1 -Step format` | ⚠ 未在最终脚本形态重跑 | 其步骤与手工验证过的“删分区→Clear-Disk→MBR→FAT32”一致；当前盘已被手工格式化，未再破坏性重跑 |
| `usb_rebuild_mbr.ps1` | ⚠ 未在最终脚本形态重跑 | 手工版（raw 清零前 1MB → 重建 MBR）已执行成功；脚本是其整理版 |

**已上板验证的结论**：

- USB MSC 枚举、容量、MBR 分区、FAT 格式化、非均匀多页写、读回、SYNCHRONIZE CACHE、
  拔插持久性均通过（rev27）。
- `RISK-WB-06` 已修并上板验证（基础数据面）。

**仍未验证**：

- 大文件/GC/CP 压力、`dd` 吞吐、写过程中掉电（USB-V07/V08/V09）。
- `RISK-MD-06` 的回收前 Anchor 专项（FTL-V01）。
- `RISK-MIG-02` 的 predictive 活锁（FTL-V02）。

---

## 9. 下一步

1. **USB-V07**：写 32–64 MB 随机文件（`fsutil`/PowerShell 生成）→ flush → 拔插 → 哈希。
2. **USB-V08**：写临时文件时断板子电源 → 重启重挂载 → 检查 FTL 恢复日志
   （`boot type` / `journal_entries` / `[L2P]`）。
3. **USB-V09**：记录大文件复制耗时，换算 MB/s。
4. **FTL-V01**：按 `RISK-MD-06` 的 64-region 方法验证回收前 Anchor。
5. **FTL-V02**：若 `victim read failed` 风暴复现，修 `RISK-MIG-02`。

---

## 10. 相关文档

- 方案/取舍/BOT vs UASP/与主控差距：`docs/design/usb_msc_udisk.md`
- 已知风险与登记：`docs/design/known_risks.md`
  （`RISK-WB-06` / `RISK-MIG-02` / `RISK-MD-06` / `RISK-L2P-04` / `RISK-USB-01`）
- 脚本用法：`tools/usb_verify/README.md`
- 分层判据：`docs/design/fc_nand_design.md §26`
- 协议事实：USB Mass Storage Class BOT 1.0、SCSI transparent command set
  （本仓未存档原文，按协议事实引用，R12）
