<#
.SYNOPSIS
    重建 soft_ssd USB 盘的 MBR（分区表损坏时的恢复脚本）。

.DESCRIPTION
    什么时候用：
      · usb_verify.ps1 -Step format 报 "Not enough available capacity"；
      · Get-Partition 显示 offset=0 / Type=Logical / MbrType 异常；
      · Windows 磁盘管理能看见盘，但无法删除/重建分区。

    为什么不能只靠 Clear-Disk：
      异常 MBR（例如分区项从 LBA 0 开始、或 extended/EBR 残留）会让 Windows
      的存储栈保留陈旧分区对象；本脚本直接对 \\.\PhysicalDriveN 的前 1MB 写零，
      连 MBR + 所有 EBR 一起清掉，再走正常的 Initialize-Disk / New-Partition。

    流程：
      1. 校验目标确实是 SOFTSSD / 512MiB / USB（防误伤 NVMe）；
      2. dump 当前 MBR（前 512B 的分区表项）；
      3. 删除缓存分区对象 + Clear-Disk；
      4. raw 写零前 1MB，并读回确认（这一步同时验证 host 写是否真的到达设备）；
      5. Initialize-Disk(MBR) → New-Partition → Format-Volume(FAT32)；
      6. dump 新 MBR。

    ⚠ 破坏性：会清空整块 U 盘。需要管理员权限。
    ⚠ 只用于 soft_ssd 的 U 盘；不要对系统盘执行。

.PARAMETER DiskNumber
    可选。显式指定 Windows 磁盘号。默认自动查找
    BusType=USB、FriendlyName 含 SOFTSSD、Size=512 MiB 的磁盘。

.NOTES
    运行方式：
      · 把 tools/usb_verify/ 复制到 Windows 本地后运行；
      · 直接从 WSL 的 UNC 路径运行会被 Windows PowerShell 的 RemoteSigned
        策略拒绝；
      · 本脚本是 UTF-8 with BOM（Windows PowerShell 5.1 需要）。

    日志：%TEMP%\softssd_usb_rebuild_mbr.log

    配套文档：docs/design/usb_msc_verification.md（USB-V03 / 故障排查）。

.EXAMPLE
    # 管理员 PowerShell
    powershell -NoProfile -File .\usb_rebuild_mbr.ps1

.EXAMPLE
    powershell -NoProfile -File .\usb_rebuild_mbr.ps1 -DiskNumber 2
#>

[CmdletBinding()]
param(
    [int]$DiskNumber = -1
)

$ErrorActionPreference = 'Stop'

# 与 usb_verify.ps1 保持一致：目标识别 + 防误操作校验。
$VolumeLabel  = 'SOFTSSD'
$ExpectedSize = 536870912
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
            ($_.Size -eq $ExpectedSize)
        } | Select-Object -First 1
    }

    if ($null -eq $disk) {
        throw 'SOFTSSD USB disk not found'
    }
    if (($disk.BusType -ne 'USB') -or
        ($disk.Size -ne $ExpectedSize) -or
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

try {
    if (-not (Test-IsElevated)) {
        throw 'usb_rebuild_mbr requires an elevated PowerShell (Run as administrator)'
    }

    $disk = Get-TargetDisk

    "Disk: $($disk.FriendlyName) Number=$($disk.Number) Size=$($disk.Size) Style=$($disk.PartitionStyle)"

    Show-Mbr -Buf (Read-Mbr -Number $disk.Number) -Title 'MBR before:'

    # 先让 Windows 忘掉缓存分区对象；某些异常 MBR 下只 Clear-Disk 不够。
    Get-Partition -DiskNumber $disk.Number -ErrorAction SilentlyContinue | ForEach-Object {
        "Removing cached partition $($_.PartitionNumber)"
        Remove-Partition -DiskNumber $disk.Number -PartitionNumber $_.PartitionNumber `
            -Confirm:$false -ErrorAction SilentlyContinue
    }

    Clear-Disk -Number $disk.Number -RemoveData -RemoveOEM -Confirm:$false -ErrorAction SilentlyContinue

    # 原始写零前 1MB：
    #   · 覆盖 MBR（LBA 0）与所有可能的 EBR / 对齐保留区；
    #   · 读回 signature=0x00 0x00 就是“host 写确实到达设备”的证据。
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

    # 从干净的 MBR 开始，Windows 才会按标准对齐创建主分区。
    Initialize-Disk -Number $disk.Number -PartitionStyle MBR -Confirm:$false -ErrorAction SilentlyContinue

    $partition = New-Partition -DiskNumber $disk.Number -UseMaximumSize -AssignDriveLetter

    Start-Sleep -Seconds 1
    Update-HostStorageCache

    $partition = Get-Partition -DiskNumber $disk.Number -PartitionNumber $partition.PartitionNumber
    "New partition: offset=$($partition.Offset) size=$($partition.Size) type=$($partition.Type) letter=$($partition.DriveLetter)"

    if ([string]::IsNullOrEmpty($partition.DriveLetter)) {
        throw 'new partition has no drive letter'
    }

    Show-Mbr -Buf (Read-Mbr -Number $disk.Number) -Title 'MBR after create:'

    # FAT32：与 usb_verify.ps1 -Step format 的口径一致。
    $volume = Format-Volume -DriveLetter $partition.DriveLetter -FileSystem FAT32 `
        -NewFileSystemLabel $VolumeLabel -Confirm:$false -Force

    "Formatted: $($volume.DriveLetter): $($volume.FileSystem) size=$($volume.Size)"
    'RESULT=PASS'
    "LOG=$LogFile"
}
catch {
    "ERROR: $($_.Exception.Message)"
    'RESULT=FAIL'
    "LOG=$LogFile"
    exit 1
}
finally {
    Stop-Transcript | Out-Null
}
