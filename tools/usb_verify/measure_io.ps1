# measure_io.ps1 — 在 Windows 侧采样 U 盘每个 IO 的大小与延迟
#
# 目的：不改固件，先区分 ~300 KB/s 的限制来自
#   · host 按 512 B/命令下发（Avg Bytes/Transfer ≈ 512）
#   · host 按 4 KB/命令下发、但每命令延迟很高（≈4096 且 Sec/Transfer ≈ 0.013）
#   · host 发大块（≈64KB+），瓶颈在设备完成/回执
#
# 用法（普通权限即可；H2testw 写入阶段同时运行）：
#   powershell -NoProfile -File .\measure_io.ps1 -Seconds 30
#   powershell -NoProfile -File .\measure_io.ps1 -Seconds 30 -DiskNumber 2
#
# 输出：每秒一行原始值 + 末尾汇总与判读。写入阶段和校验阶段建议各跑一次。

[CmdletBinding()]
param(
    [int]$Seconds = 20,
    [int]$DiskNumber = -1
)

$ErrorActionPreference = 'Stop'

function Get-SoftSsdDisk {
    param([int]$Number)
    if ($Number -ge 0) {
        return Get-PhysicalDisk -DeviceNumber $Number
    }
    return Get-PhysicalDisk |
        Where-Object { ($_.BusType -eq 'USB') -and ($_.FriendlyName -like '*SOFTSSD*') } |
        Select-Object -First 1
}

$disk = Get-SoftSsdDisk -Number $DiskNumber
if ($null -eq $disk) {
    throw 'SOFTSSD USB disk not found: 插上 U 盘，或用 -DiskNumber 指定。'
}

Write-Host ("Target: disk {0} '{1}' BusType={2} Size={3}" -f `
    $disk.DeviceId, $disk.FriendlyName, $disk.BusType, $disk.Size)

# PhysicalDisk 的性能实例名形如 "2 F:"；按磁盘号前缀匹配（无盘符时也可能只是 "2"）。
$sample = Get-CimInstance Win32_PerfFormattedData_PerfDisk_PhysicalDisk |
    Where-Object { $_.Name -match ('^' + [regex]::Escape([string]$disk.DeviceId) + '(\s|$)') } |
    Select-Object -First 1
if ($null -eq $sample) {
    throw (("PhysicalDisk performance instance for disk {0} not found. " +
            '请确认 U 盘已挂载；或用 Get-CimInstance Win32_PerfFormattedData_PerfDisk_PhysicalDisk | Format-Table Name 查看实例名。') -f $disk.DeviceId)
}

Write-Host ("Instance: {0}" -f $sample.Name)
Write-Host ("Sampling {0} s (run H2testw now)..." -f $Seconds)
Write-Host '  t  Bytes/s   Bytes/Transfer  Sec/Transfer  Transfers/s  Queue'
$rows = @()
for ($i = 0; $i -lt $Seconds; $i++) {
    $s = Get-CimInstance Win32_PerfFormattedData_PerfDisk_PhysicalDisk |
        Where-Object Name -EQ $sample.Name | Select-Object -First 1
    if ($null -ne $s) {
        $row = [pscustomobject]@{
            T               = $i
            BytesPerSec     = [uint64]$s.DiskBytesPerSec
            BytesPerTransfer= [double]$s.AvgDiskBytesPerTransfer
            SecPerTransfer  = [double]$s.AvgDiskSecPerTransfer
            TransfersPerSec = [uint64]$s.DiskTransfersPerSec
            Queue           = [uint64]$s.CurrentDiskQueueLength
        }
        $rows += $row
        Write-Host ("{0,3} {1,10} {2,15:N0} {3,13:N6} {4,12} {5,6}" -f `
            $row.T, $row.BytesPerSec, $row.BytesPerTransfer, `
            $row.SecPerTransfer, $row.TransfersPerSec, $row.Queue)
    }
    Start-Sleep -Seconds 1
}

$active = @($rows | Where-Object { $_.TransfersPerSec -gt 0 })
if ($active.Count -eq 0) {
    Write-Host 'No active IO during the sample window: 采样期间没有读写（H2testw 是否在跑？）。'
    exit 0
}

$avgBytes = ($active | Measure-Object BytesPerTransfer -Average).Average
$avgSec   = ($active | Measure-Object SecPerTransfer -Average).Average
$avgTps   = ($active | Measure-Object TransfersPerSec -Average).Average
$avgQ     = ($active | Measure-Object Queue -Average).Average
$sumBytes = ($active | Measure-Object BytesPerSec -Sum).Sum
$avgRate  = $sumBytes / $active.Count

Write-Host ''
Write-Host '=== Summary (active samples only) ==='
Write-Host ("Avg Bytes/Transfer : {0:N0}" -f $avgBytes)
Write-Host ("Avg Sec/Transfer   : {0:N6}  ({1:N3} ms)" -f $avgSec, ($avgSec * 1000))
Write-Host ("Avg Transfers/sec  : {0:N0}" -f $avgTps)
Write-Host ("Avg Disk Bytes/sec : {0:N0}  ({1:N0} KB/s)" -f $avgRate, ($avgRate / 1024))
Write-Host ("Avg Queue Length   : {0:N2}" -f $avgQ)
Write-Host ''

# 判读：注意这是**文件系统/存储栈**的 IO 粒度，不是 USB CBW 的粒度。
# USB 侧实际分块（4KB/16KB/512B）看板端 `info` 的 `[USB] wr chunk` 直方图。
if ($avgBytes -lt 1024) {
    Write-Host '=> 文件系统 IO ≈512 B：应用/文件系统在发小块（仍需板端直方图确认 USB 分块）。'
} elseif ($avgBytes -lt 8192) {
    Write-Host '=> 文件系统 IO ≈4 KB：看 Sec/Transfer；USB 分块以板端 info 为准。'
} elseif ($avgBytes -lt 131072) {
    Write-Host '=> 文件系统 IO 64 KB 级：host 应用层在发大块；USB CBW 仍可能被 USBSTOR 上限拆分。'
} else {
    Write-Host '=> 文件系统/应用 IO 为大块（>128 KB）：这不能证明 USB CBW 也是大块。'
    Write-Host '   USB 侧分块请以板端 `info` 的 `[USB] wr chunk <=512/<=4K/<=16K/>16K` 为准。'
}
if ($avgQ -lt 1.5) {
    Write-Host '=> Queue ≈ 1：BOT 单命令串行/应用同步等待，没有流水。'
} else {
    Write-Host '=> Queue > 1：主机在排队（可能被设备 NAK 或完成延迟压住）。'
}
Write-Host '=> Sec/Transfer 是端到端每笔文件系统 IO 的耗时（含缓存/存储栈/USB/设备），不是单条 USB 命令耗时。'
