# USB MSC U 盘验证脚本（Windows 原生）

配套文档：`docs/design/usb_msc_verification.md`。

## 文件

| 文件 | 用途 |
|---|---|
| `usb_verify.ps1` | 主脚本：`inspect` / `format` / `write` / `check` |
| `usb_rebuild_mbr.ps1` | 分区表损坏时的恢复：raw 清零前 1MB → 重建 MBR → FAT32 |

两个脚本都带完整的 comment-based help，可直接查看：

```powershell
Get-Help .\usb_verify.ps1 -Full
Get-Help .\usb_rebuild_mbr.ps1 -Full
```

## 运行方式

脚本在 WSL 仓库里。**推荐把 `tools/usb_verify/` 复制到 Windows 本地后再运行**：

```powershell
# 只读检查（普通 PowerShell 即可）
powershell -NoProfile -File .\usb_verify.ps1 -Step inspect
```

⚠ 直接从 WSL 的 UNC 路径（`\\wsl.localhost\<distro>\...`）运行会被 Windows PowerShell
的 `RemoteSigned` 策略拒绝（“无法加载文件…未数字签名”）。要么复制到本地，要么在
明确接受的前提下使用 `-ExecutionPolicy Bypass`。

⚠ 脚本是 **UTF-8 with BOM**：Windows PowerShell 5.1 读无 BOM 的 UTF-8 中文注释时
可能把换行吞掉，导致语法错误。编辑脚本后请保持 BOM。

## 步骤

### 1. 检查（只读）

```powershell
powershell -NoProfile -File .\usb_verify.ps1 -Step inspect
```

确认输出里有：

- `FriendlyName = SOFTSSD NAND U-DISK`
- `Size ≈ 444596224`（424 MiB，0 坏块时；运行时按实际坏块扣减，脚本按 ±64 MiB 窗口匹配）
- `BusType = USB`

### 2. 格式化（破坏性，需管理员）

管理员 PowerShell：

```powershell
powershell -NoProfile -File .\usb_verify.ps1 -Step format
```

默认自动找 `SOFTSSD / 429MiB / USB` 盘；也可以显式指定 `-DiskNumber 2`。

### 3. 写入测试数据

普通窗口即可：

```powershell
powershell -NoProfile -File .\usb_verify.ps1 -Step write
```

会写入：

- `test.bin`：`C:\Windows\System32\notepad.exe` 的副本（非均匀数据）
- `pattern.bin`：16KB，每 2KB 块内容 = 块号（0..7）

并执行 `Write-VolumeCache`（主机侧 SYNCHRONIZE CACHE）。哈希写入
`%TEMP%\softssd_usb_verify_state.json`。

### 4. 拔插后校验

**拔掉 USB 线、重新插上**，然后：

```powershell
powershell -NoProfile -File .\usb_verify.ps1 -Step check
```

期望最后一行：

```text
RESULT=PASS
```

失败时返回码为 1，并打印 `RESULT=FAIL`。

## 恢复分区表

仅在 `-Step format` 报 “Not enough available capacity” / 出现异常 Logical 分区时使用：

```powershell
# 管理员
powershell -NoProfile -File .\usb_rebuild_mbr.ps1
```

它会先打印 MBR，写零前 1MB，再重建 MBR + FAT32。

## 日志与状态

- 每步日志：`%TEMP%\softssd_usb_verify_<step>.log`
- 写入状态：`%TEMP%\softssd_usb_verify_state.json`
- MBR 恢复日志：`%TEMP%\softssd_usb_rebuild_mbr.log`

## 安全约束

- 所有脚本都会先校验 `FriendlyName=*SOFTSSD*`、`Size`（424 MiB ±64 MiB）、`BusType=USB`，
  不匹配直接中止。
- **format / rebuild 会清空这块 U 盘**；不要对 NVMe 系统盘执行。
- 主机操作期间不要同时敲板子 CLI（共用 host busy gate）。
- `format` / `rebuild` 需要管理员权限；`inspect` / `write` / `check` 普通权限即可
  （`Write-VolumeCache` 若被拒绝，改用 Windows“安全删除硬件”）。
