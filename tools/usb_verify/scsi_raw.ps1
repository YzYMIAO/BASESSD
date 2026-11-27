<#
.SYNOPSIS
    直接向 USB MSC 物理盘下发 SCSI CDB（BOT 层没有文件系统参与）。

.DESCRIPTION
    用途：验证固件里 Windows 自带工具打不到的 SCSI 路径：
      - sync10 / sync16 : SYNCHRONIZE CACHE(10) 0x35 / (16) 0x91
      - unmap           : UNMAP 0x42（支持多 range / NLB=0 / 高 32 位非零负例）
      - fua-write       : WRITE(10) 0x2A 带 FUA 位（byte1 bit3）
      - read            : READ(10) 0x28（用于 FUA 写后读回）
      - inquiry         : INQUIRY 0x12（确认盘符对应的物理盘）

    通过 IOCTL_SCSI_PASS_THROUGH_DIRECT 下发；需要管理员 PowerShell。
    UNMAP 参数表布局与固件 `usb_msc_unmap_parse` 一致：
      8B 头（BE16 总长 + BE16 描述符长） + N×16B（8B LBA + 4B NLB + 4B reserved）。

    ⚠ fua-write 会以原始扇区写入，绕过文件系统；只在测试盘上使用。
    ⚠ UNMAP / FUA 的 LBA 以设备 512B 扇区计，不是文件偏移。

.EXAMPLE
    # 查询物理盘号
    Get-Disk | Format-Table Number, FriendlyName, Size, BusType

.EXAMPLE
    powershell -NoProfile -ExecutionPolicy Bypass -File .\scsi_raw.ps1 -DiskNumber 2 -Op inquiry
    powershell -NoProfile -ExecutionPolicy Bypass -File .\scsi_raw.ps1 -DiskNumber 2 -Op sync10
    powershell -NoProfile -ExecutionPolicy Bypass -File .\scsi_raw.ps1 -DiskNumber 2 -Op sync16
    powershell -NoProfile -ExecutionPolicy Bypass -File .\scsi_raw.ps1 -DiskNumber 2 -Op unmap -Lba 1000 -Count 8

.EXAMPLE
    # UNMAP 负例：16 range、NLB=0、高 32 位非零
    powershell -NoProfile -ExecutionPolicy Bypass -File .\scsi_raw.ps1 -DiskNumber 2 -Op unmap -Ranges 16 -Variant normal
    powershell -NoProfile -ExecutionPolicy Bypass -File .\scsi_raw.ps1 -DiskNumber 2 -Op unmap -Ranges 2 -Variant nlb0
    powershell -NoProfile -ExecutionPolicy Bypass -File .\scsi_raw.ps1 -DiskNumber 2 -Op unmap -Ranges 1 -Variant high-lba

.EXAMPLE
    # FUA 写 8 个扇区后立即读回
    powershell -NoProfile -ExecutionPolicy Bypass -File .\scsi_raw.ps1 -DiskNumber 2 -Op fua-write -Lba 2000 -Count 8 -Pattern 165
    powershell -NoProfile -ExecutionPolicy Bypass -File .\scsi_raw.ps1 -DiskNumber 2 -Op read -Lba 2000 -Count 8 -Hex 32
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][int]$DiskNumber,
    [Parameter(Mandatory = $true)][ValidateSet('sync10', 'sync16', 'unmap', 'write', 'fua-write', 'write-sync', 'write-twice', 'read', 'inquiry', 'selftest')][string]$Op,
    [uint64]$Lba = 0,
    [uint32]$Count = 1,
    [int]$Ranges = 1,
    [ValidateSet('normal', 'nlb0', 'high-lba')][string]$Variant = 'normal',
    [byte]$Pattern = 0xA5,
    [int]$Iterations = 1,
    [int]$Hex = 32
)

$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;

public static class ScsiRaw
{
    [StructLayout(LayoutKind.Sequential)]
    public struct SCSI_PASS_THROUGH_DIRECT
    {
        public ushort Length;
        public byte ScsiStatus;
        public byte PathId;
        public byte TargetId;
        public byte Lun;
        public byte CdbLength;
        public byte SenseInfoLength;
        public byte DataIn;
        public uint DataTransferLength;
        public uint TimeOutValue;
        public IntPtr DataBuffer;
        public uint SenseInfoOffset;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 16)]
        public byte[] Cdb;
    }

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool DeviceIoControl(
        IntPtr hDevice, uint dwIoControlCode,
        IntPtr lpInBuffer, uint nInBufferSize,
        IntPtr lpOutBuffer, uint nOutBufferSize,
        out uint lpBytesReturned, IntPtr lpOverlapped);

    public const uint IOCTL_SCSI_PASS_THROUGH_DIRECT = 0x0004D014;
}
"@

$SptType = [type]'ScsiRaw+SCSI_PASS_THROUGH_DIRECT'
$SptSize = [Runtime.InteropServices.Marshal]::SizeOf([System.Type]$SptType)
$SenseLen = 32

function Set-BeBytes([byte[]]$Dst, [int]$Offset, [uint64]$Value, [int]$Width) {
    for ($i = 0; $i -lt $Width; $i++) {
        $Dst[$Offset + $Width - 1 - $i] = [byte](($Value -shr (8 * $i)) -band 0xFF)
    }
}

function Copy-Bytes([byte[]]$Src, [byte[]]$Dst, [int]$Count) {
    for ($i = 0; $i -lt $Count; $i++) { $Dst[$i] = $Src[$i] }
}

function Invoke-Write10([bool]$Fua) {
    $bytes = [uint64]$Count * 512
    $data = New-Object byte[] $bytes
    for ($i = 0; $i -lt $data.Length; $i++) { $data[$i] = $Pattern }
    $cdb = New-Object byte[] 16
    $cdb[0] = 0x2A
    if ($Fua) { $cdb[1] = 0x08 }
    Set-BeBytes $cdb 2 $Lba 4
    Set-BeBytes $cdb 7 $Count 2
    return Invoke-Scsi $cdb 0 $data
}

function Invoke-Sync10 {
    $cdb = New-Object byte[] 16
    $cdb[0] = 0x35
    Set-BeBytes $cdb 2 $Lba 4
    Set-BeBytes $cdb 7 $Count 2
    return Invoke-Scsi $cdb 0 $null
}

function Invoke-Scsi([byte[]]$Cdb, [int]$DataIn, [byte[]]$Data) {
    $spt = [Activator]::CreateInstance($SptType)
    $spt.Length = [uint16]$SptSize
    $spt.CdbLength = [byte]16
    $spt.SenseInfoLength = [byte]$SenseLen
    $spt.DataIn = [byte]$DataIn
    $spt.DataTransferLength = if ($null -ne $Data) { [uint32]$Data.Length } else { [uint32]0 }
    $spt.TimeOutValue = [uint32]30
    $spt.SenseInfoOffset = [uint32]$SptSize
    $spt.Cdb = New-Object byte[] 16
    Copy-Bytes $Cdb $spt.Cdb ([Math]::Min(16, $Cdb.Length))

    $structBuf = [Runtime.InteropServices.Marshal]::AllocHGlobal($SptSize + $SenseLen)
    $dataPtr = [IntPtr]::Zero
    try {
        [Runtime.InteropServices.Marshal]::StructureToPtr($spt, $structBuf, $false)
        if ($null -ne $Data) {
            $dataPtr = [Runtime.InteropServices.Marshal]::AllocHGlobal($Data.Length)
            [Runtime.InteropServices.Marshal]::Copy($Data, 0, $dataPtr, $Data.Length)
        }
        # 重写结构体，带上 DataBuffer 指针
        $spt.DataBuffer = $dataPtr
        [Runtime.InteropServices.Marshal]::StructureToPtr($spt, $structBuf, $false)

        $returned = [uint32]0
        $ok = [ScsiRaw]::DeviceIoControl(
            $script:hDevice,
            [ScsiRaw]::IOCTL_SCSI_PASS_THROUGH_DIRECT,
            $structBuf, [uint32]($SptSize + $SenseLen),
            $structBuf, [uint32]($SptSize + $SenseLen),
            [ref]$returned, [IntPtr]::Zero)
        if (-not $ok) {
            $err = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
            throw ("DeviceIoControl failed: Win32 error {0}" -f $err)
        }

        $outSpt = [Runtime.InteropServices.Marshal]::PtrToStructure($structBuf, [System.Type]$SptType)
        $sense = New-Object byte[] $SenseLen
        [Runtime.InteropServices.Marshal]::Copy([IntPtr]::Add($structBuf, $SptSize), $sense, 0, $SenseLen)

        if (($DataIn -ne 0) -and ($null -ne $Data)) {
            [Runtime.InteropServices.Marshal]::Copy($dataPtr, $Data, 0, [Math]::Min($Data.Length, [int]$outSpt.DataTransferLength))
        }

        return [pscustomobject]@{
            ScsiStatus = $outSpt.ScsiStatus
            Transferred = $outSpt.DataTransferLength
            SenseKey = [byte]($sense[2] -band 0x0F)
            Asc = [byte]$sense[12]
            Ascq = [byte]$sense[13]
        }
    }
    finally {
        if ($dataPtr -ne [IntPtr]::Zero) { [Runtime.InteropServices.Marshal]::FreeHGlobal($dataPtr) }
        [Runtime.InteropServices.Marshal]::FreeHGlobal($structBuf)
    }
}

function Show-Result([string]$Name, $Result) {
    if ($Result.ScsiStatus -eq 0) {
        Write-Host ("{0}: OK  transferred={1}" -f $Name, $Result.Transferred) -ForegroundColor Green
    }
    else {
        Write-Host ("{0}: SCSI STATUS=0x{1:X2}  sense=0x{2:X2}/0x{3:X2}/0x{4:X2}" -f $Name, $Result.ScsiStatus, $Result.SenseKey, $Result.Asc, $Result.Ascq) -ForegroundColor Red
    }
}

if ($Op -eq 'selftest') {
    $cdb = New-Object byte[] 16
    $cdb[0] = 0x91
    Set-BeBytes $cdb 2 $Lba 8
    Set-BeBytes $cdb 10 $Count 4
    'selftest cdb=' + (($cdb | ForEach-Object { '{0:X2}' -f $_ }) -join ' ')
    return
}

$path = "\\.\PhysicalDrive$DiskNumber"
$fs = [IO.File]::Open($path, [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite, [IO.FileShare]::ReadWrite)
$script:hDevice = $fs.SafeFileHandle.DangerousGetHandle()

try {
    switch ($Op) {
        'inquiry' {
            $cdb = New-Object byte[] 16
            $cdb[0] = 0x12
            $cdb[4] = 36
            $data = New-Object byte[] 36
            $r = Invoke-Scsi $cdb 1 $data
            Show-Result 'INQUIRY' $r
            if ($r.ScsiStatus -eq 0) {
                $vendor = [Text.Encoding]::ASCII.GetString($data, 8, 8).Trim()
                $product = [Text.Encoding]::ASCII.GetString($data, 16, 16).Trim()
                Write-Host ("  vendor='{0}' product='{1}'" -f $vendor, $product)
            }
        }
        'sync10' {
            $cdb = New-Object byte[] 16
            $cdb[0] = 0x35
            Set-BeBytes $cdb 2 $Lba 4
            Set-BeBytes $cdb 7 $Count 2
            Show-Result 'SYNCHRONIZE CACHE(10)' (Invoke-Scsi $cdb 0 $null)
        }
        'sync16' {
            $cdb = New-Object byte[] 16
            $cdb[0] = 0x91
            Set-BeBytes $cdb 2 $Lba 8
            Set-BeBytes $cdb 10 $Count 4
            Show-Result 'SYNCHRONIZE CACHE(16)' (Invoke-Scsi $cdb 0 $null)
        }
        'unmap' {
            if (($Ranges -lt 1) -or ($Ranges -gt 16)) { throw '-Ranges must be 1..16' }
            $descBytes = 16 * $Ranges
            $total = 8 + $descBytes
            $data = New-Object byte[] $total
            Set-BeBytes $data 0 $total 2
            Set-BeBytes $data 2 $descBytes 2
            for ($i = 0; $i -lt $Ranges; $i++) {
                $desc = 8 + (16 * $i)
                $thisLba = $Lba + ([uint64]$i * $Count)
                if (($i -eq 0) -and ($Variant -eq 'high-lba')) {
                    $thisLba = $Lba + 0x100000000
                }
                $thisNlb = [uint64]$Count
                if (($i -eq 0) -and ($Variant -eq 'nlb0')) { $thisNlb = 0 }
                Set-BeBytes $data $desc $thisLba 8
                Set-BeBytes $data ($desc + 8) $thisNlb 4
            }
            $cdb = New-Object byte[] 16
            $cdb[0] = 0x42
            Set-BeBytes $cdb 7 $total 4
            Show-Result ("UNMAP ranges={0} variant={1}" -f $Ranges, $Variant) (Invoke-Scsi $cdb 0 $data)
        }
        'write' {
            Show-Result ("WRITE(10) lba={0} count={1}" -f $Lba, $Count) (Invoke-Write10 $false)
        }
        'fua-write' {
            Show-Result ("WRITE(10) FUA lba={0} count={1}" -f $Lba, $Count) (Invoke-Write10 $true)
        }
        'write-sync' {
            $fail = 0
            for ($i = 0; $i -lt $Iterations; $i++) {
                $w = Invoke-Write10 $false
                if ($w.ScsiStatus -ne 0) { $fail++; Show-Result ("write #{0}" -f $i) $w }
                $s = Invoke-Sync10
                if ($s.ScsiStatus -ne 0) { $fail++; Show-Result ("sync #{0}" -f $i) $s }
            }
            Write-Host ("write-sync iterations={0} failures={1}" -f $Iterations, $fail)
        }
        'write-twice' {
            $fail = 0
            for ($i = 0; $i -lt $Iterations; $i++) {
                $a = Invoke-Write10 $false
                if ($a.ScsiStatus -ne 0) { $fail++; Show-Result ("write-a #{0}" -f $i) $a }
                $b = Invoke-Write10 $false
                if ($b.ScsiStatus -ne 0) { $fail++; Show-Result ("write-b #{0}" -f $i) $b }
            }
            Write-Host ("write-twice iterations={0} failures={1}" -f $Iterations, $fail)
        }
        'read' {
            $bytes = [uint64]$Count * 512
            $data = New-Object byte[] $bytes
            $cdb = New-Object byte[] 16
            $cdb[0] = 0x28
            Set-BeBytes $cdb 2 $Lba 4
            Set-BeBytes $cdb 7 $Count 2
            $r = Invoke-Scsi $cdb 1 $data
            Show-Result ("READ(10) lba={0} count={1}" -f $Lba, $Count) $r
            if ($r.ScsiStatus -eq 0) {
                $n = [Math]::Min($Hex, $data.Length)
                $line = ($data[0..($n - 1)] | ForEach-Object { '{0:X2}' -f $_ }) -join ' '
                Write-Host ("  first {0} bytes: {1}" -f $n, $line)
            }
        }
    }
}
finally {
    $fs.Dispose()
}
