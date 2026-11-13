<#
.SYNOPSIS
    重建 soft_ssd USB 盘的 MBR（分区表损坏时的恢复脚本）。

.DESCRIPTION
    什么时候用：
      · usb_verify.ps1 -Step format 报 "Not enough available capacity"；
      · Get-Partition 显示 offset=0 / Type=Logical / MbrType 异常；
      · Windows 磁盘管理能看见盘，但无法删除/重建分区。

    为什么不能只靠 Clear-Disk / New-Partition：
      异常 MBR（例如分区项从 LBA 0 开始、或 extended/EBR 残留）会让 Windows
      的存储栈保留陈旧分区对象；PowerShell 的 New-Partition 会报
      “Not enough available capacity”。diskpart clean 能清掉缓存布局，是实测
      可用的路径；若仍失败，再对 \\.\PhysicalDriveN 前 1MB 写零（覆盖 MBR+EBR）。

    流程：
      1. 校验目标确实是 SOFTSSD / 429MiB / USB（防误伤 NVMe）；
      2. dump 当前 MBR（前 512B 的分区表项）；
      3. diskpart：clean → create partition primary → format → assign
         （默认 NTFS；FAT32/exFAT 在部分 Windows 构建上 0% 失败）；
      4. 若 diskpart 失败：raw 写零前 1MB → 再走一次 diskpart；
      5. dump 新 MBR 并回读卷信息。

    ⚠ 破坏性：会清空整块 U 盘。需要管理员权限。
    ⚠ 只用于 soft_ssd 的 U 盘；不要对系统盘执行。

.PARAMETER DiskNumber
    可选。显式指定 Windows 磁盘号。默认自动查找
    BusType=USB、FriendlyName 含 SOFTSSD、Size=429 MiB 的磁盘。

.NOTES
    运行方式：
      · 把 tools/usb_verify/ 复制到 Windows 本地后运行；
      · 直接从 WSL 的 UNC 路径运行会被 Windows PowerShell 的 RemoteSigned
        策略拒绝；
      · 本脚本是 UTF-8 with BOM（Windows PowerShell 5.1 需要）。

    日志：%TEMP%\softssd_usb_rebuild_mbr.log

    配套文档：docs/design/usb_msc.md Part B（USB-V03 / 故障排查）。

.EXAMPLE
    # 管理员 PowerShell
    powershell -NoProfile -File .\usb_rebuild_mbr.ps1

.EXAMPLE
    powershell -NoProfile -File .\usb_rebuild_mbr.ps1 -DiskNumber 2
#>

[CmdletBinding()]
param(
    [int]$DiskNumber = -1,

    # 默认 NTFS：FAT32/exFAT 在 Windows 10 25H2 build 26200 上实测在 0% 失败
    # （ERROR_NOT_SUPPORTED / ERROR_INSUFFICIENT_BUFFER），NTFS 可用。
    [ValidateSet('NTFS', 'FAT32', 'exFAT')]
    [string]$FileSystem = 'NTFS'
)

$ErrorActionPreference = 'Stop'

# 与 usb_verify.ps1 保持一致：目标识别 + 防误操作校验。
$VolumeLabel  = 'SOFTSSD'
$ExpectedSize = 444596224       # 424 MiB = 868,352 × 512B（介质 v12，0 坏块；容量按坏块动态扣减）
$SizeTolerance = 67108864       # ±64 MiB ≈ 512 个坏块（RISK-CAP-03）
$LogFile      = Join-Path $env:TEMP 'softssd_usb_rebuild_mbr.log'

Start-Transcript -Path $LogFile -Force | Out-Null

function Test-IsElevated {
    $identity  = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)

    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

<#
    目标磁盘校验。
    ⚠ 显式 -DiskNumber 也必须过 FriendlyName / Size / BusType 三关，
      防止把 NVMe 系统盘当成 U 盘。
#>
function Get-TargetDisk {
    if ($DiskNumber -ge 0) {
        $disk = Get-Disk -Number $DiskNumber
    }
    else {
        $disk = Get-Disk | Where-Object {
            ($_.BusType -eq 'USB') -and
            ($_.FriendlyName -like '*SOFTSSD*') -and
            ([math]::Abs($_.Size - $ExpectedSize) -le $SizeTolerance)
        } | Select-Object -First 1
    }

    if ($null -eq $disk) {
        throw 'SOFTSSD USB disk not found'
    }
    if (($disk.BusType -ne 'USB') -or
        ([math]::Abs($disk.Size - $ExpectedSize) -gt $SizeTolerance) -or
        ($disk.FriendlyName -notlike '*SOFTSSD*')) {
        throw ("Disk {0} is not the expected SOFTSSD USB disk: name='{1}' size={2} bus={3}" -f `
            $disk.Number, $disk.FriendlyName, $disk.Size, $disk.BusType)
    }

    return $disk
}

<#
    读物理盘前 512B（MBR）。
    使用 \\.\PhysicalDriveN 绕过文件系统缓存；需要管理员权限。
#>
function Read-Mbr {
    param([int]$Number)

    $path = "\\.\PhysicalDrive$Number"
    $fs   = [IO.File]::Open($path, [IO.FileMode]::Open,
                            [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
    $buf  = New-Object byte[] 512
    [void]$fs.Read($buf, 0, 512)
    $fs.Close()

    return $buf
}

<#
    打印 4 个 MBR 分区表项（offset 446 起，每项 16B）：
      status / type / startLBA / sectors + 0x55AA 签名。
    用于记录“写零前 / 写零后 / 新建分区后”的差异。
#>
function Show-Mbr {
    param([byte[]]$Buf, [string]$Title)

    $Title
    for ($i = 0; $i -lt 4; $i++) {
        $offset = 446 + (16 * $i)
        $type   = $Buf[$offset + 4]
        $start  = [BitConverter]::ToUInt32($Buf, $offset + 8)
        $count  = [BitConverter]::ToUInt32($Buf, $offset + 12)

        "  entry$i status=0x$($Buf[$offset].ToString('X2')) type=0x$($type.ToString('X2')) startLBA=$start sectors=$count"
    }
    "  signature=0x$($Buf[510].ToString('X2')) 0x$($Buf[511].ToString('X2'))"
}

<#
    用 diskpart 走已验证可用的恢复路径：
      clean → create partition primary → format fs=<FileSystem> quick → assign。
    PowerShell 的 Clear-Disk / New-Partition 在异常 MBR（offset 0 / Logical）下
    会报“Not enough available capacity”，而 diskpart clean 能清掉 Windows 的
    缓存布局。格式化默认 NTFS（FAT32/exFAT 在部分 Windows 构建上 0% 失败）。
#>
function Invoke-DiskpartRepair {
    param([int]$Number, [string]$Fs)

    $script = Join-Path $env:TEMP ("softssd_diskpart_{0}.txt" -f [Guid]::NewGuid().ToString('N'))

    @(
        "select disk $Number",
        "clean",
        "create partition primary",
        "format fs=$($Fs.ToLowerInvariant()) quick label=$VolumeLabel",
        "assign",
        "exit"
    ) | Set-Content -Path $script -Encoding ASCII

    try {
        $output = & diskpart /s $script 2>&1
    }
    finally {
        Remove-Item $script -Force -ErrorAction SilentlyContinue
    }

    $output | ForEach-Object { "  $_" }

    if ($LASTEXITCODE -ne 0) {
        throw "diskpart failed (exit $LASTEXITCODE)"
    }
}

try {
    if (-not (Test-IsElevated)) {
        throw 'usb_rebuild_mbr requires an elevated PowerShell (Run as administrator)'
    }

    $disk = Get-TargetDisk

    "Disk: $($disk.FriendlyName) Number=$($disk.Number) Size=$($disk.Size) Style=$($disk.PartitionStyle)"

    Show-Mbr -Buf (Read-Mbr -Number $disk.Number) -Title 'MBR before:'

    # 先走 diskpart 路径：clean 清掉 Windows 缓存的幽灵分区，再建分区/格式化。
    # 若仍失败，raw 写零前 1MB（覆盖 MBR/EBR）后重试一次。
    try {
        Invoke-DiskpartRepair -Number $disk.Number -Fs $FileSystem
    }
    catch {
        "diskpart path failed: $($_.Exception.Message)"
        "Falling back to raw zero + retry..."

        $path = "\\.\PhysicalDrive$($disk.Number)"
        $fs   = [IO.File]::Open($path, [IO.FileMode]::Open,
                                [IO.FileAccess]::ReadWrite, [IO.FileShare]::ReadWrite)
        $zero = New-Object byte[] (1024 * 1024)
        $fs.Write($zero, 0, $zero.Length)
        $fs.Flush($true)
        $fs.Close()

        Start-Sleep -Seconds 1
        Update-HostStorageCache

        Show-Mbr -Buf (Read-Mbr -Number $disk.Number) -Title 'MBR after zero:'

        Invoke-DiskpartRepair -Number $disk.Number -Fs $FileSystem
    }

    Start-Sleep -Seconds 1
    Update-HostStorageCache

    $partition = Get-Partition -DiskNumber $disk.Number -ErrorAction SilentlyContinue | Select-Object -First 1
    if (($null -eq $partition) -or [string]::IsNullOrEmpty($partition.DriveLetter)) {
        throw 'formatted disk has no drive letter'
    }

    Show-Mbr -Buf (Read-Mbr -Number $disk.Number) -Title 'MBR after create:'

    $volume = Get-Volume -DriveLetter $partition.DriveLetter
    if (($null -eq $volume) -or ($volume.FileSystem -ne $FileSystem)) {
        throw "format did not produce $FileSystem (got '$($volume.FileSystem)')"
    }

    "Formatted: $($volume.DriveLetter): $($volume.FileSystem) size=$($volume.Size)"
    'RESULT=PASS'
    "LOG=$LogFile"
}
catch {
    "ERROR: $($_.Exception.Message)"
    'RESULT=FAIL'
    "LOG=$LogFile"
    throw
}
finally {
    Stop-Transcript | Out-Null
}
