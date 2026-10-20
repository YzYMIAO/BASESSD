<#
.SYNOPSIS
    soft_ssd USB MSC U 盘验证脚本（Windows 原生）。

.DESCRIPTION
    把「板子当 U 盘」拆成四个可独立判定的步骤，避免把枚举 / 容量 / 格式化 /
    数据完整性 / 拔插持久性混在一次操作里（混在一起时失败无法归因）：

      inspect  只读。列出 USB 磁盘、分区、卷、扇区大小。
               验证：枚举成功、容量 429 MiB、扇区 512B、分区/文件系统状态正常。

      format   破坏性，需管理员。删分区 → Clear-Disk → MBR → 新建分区 → FAT32。
               验证：主机写出的 MBR/分区表/FAT 引导扇区能被设备正确保存并读回。
               ⚠ 会清空整块 U 盘。

      write    写非均匀测试数据并 flush。普通窗口即可。
               写入：
                 test.bin    = C:\Windows\System32\notepad.exe 的副本（360448B）
                 pattern.bin = 16KB，每 2KB 块内容 = 块号（0..7）
               并计算 SHA256、执行 Write-VolumeCache（SYNCHRONIZE CACHE），
               把期望哈希写进 %TEMP%\softssd_usb_verify_state.json。

      check    重新读回并比对哈希 + 2KB 块指纹。应在**拔插 USB 之后**执行。
               验证：SYNCHRONIZE CACHE 生效、数据真的落盘、重枚举后读回一致。
               期望最后一行 RESULT=PASS。

    为什么 pattern.bin 必须每 2KB 不同：
      4KB 写入 = 2 plane × 2KB 页。历史缺陷 RISK-WB-06 中 plane1 复制了
      plane0 的数据，表现为「每 4KB 的后 2KB 重复前 2KB」。CLI 的 wr/rd pattern
      每 2KB 相同（0x55/0xAA），完全看不见这个错误；必须用互不相同的 2KB 块。

    为什么 write 后必须拔插再 check：
      Windows 可能从自己的页缓存返回刚写入的数据，立即读回一致不代表设备上
      真的正确。只有拔插（或重启）后重新枚举、重新读盘，才是设备侧证据。

.PARAMETER Step
    inspect | format | write | check

.PARAMETER DiskNumber
    可选。显式指定 Windows 磁盘号（Get-Disk 的 Number）。
    默认自动查找：BusType=USB、FriendlyName 含 SOFTSSD、Size=429 MiB。

.PARAMETER DriveLetter
    可选。显式指定盘符（如 F）。默认按卷标 SOFTSSD 查找；卷标为空时退回
    “目标磁盘上有盘符的分区”。

.PARAMETER PatternBlocks
    pattern.bin 的 2KB 块数，默认 8。每块内容 = 块号（0..254，必须互不相同）。

.NOTES
    安全：
      · 所有步骤先校验 FriendlyName / Size / BusType，不匹配立即中止；
        不会碰系统 NVMe。
      · format / usb_rebuild_mbr.ps1 是破坏性操作。
      · 主机操作期间不要同时敲板子 CLI（共用 host busy gate）。

    权限：
      · format / usb_rebuild_mbr.ps1 需要管理员 PowerShell；
      · inspect / write / check 普通窗口即可。Write-VolumeCache 若被拒绝，
        改用 Windows“安全删除硬件”后再拔插。

    运行方式：
      · 把 tools/usb_verify/ 复制到 Windows 本地后运行；
      · 直接从 WSL 的 UNC 路径（\\wsl.localhost\...）运行会被 Windows PowerShell
        的 RemoteSigned 策略拒绝（未数字签名）。
      · 本脚本是 UTF-8 with BOM：Windows PowerShell 5.1 读无 BOM 的 UTF-8
        中文注释可能吞换行、导致语法错误，编辑后请保持 BOM。

    日志与状态：
      · %TEMP%\softssd_usb_verify_<step>.log
      · %TEMP%\softssd_usb_verify_state.json（write 生成，check 读取）

    配套文档：
      · docs/design/usb_msc_verification.md（步骤/判据/故障排查）
      · docs/design/usb_msc_udisk.md（方案/取舍）
      · docs/design/known_risks.md（RISK-WB-06 / RISK-USB-01）

.EXAMPLE
    powershell -NoProfile -File .\usb_verify.ps1 -Step inspect

.EXAMPLE
    # 管理员窗口
    powershell -NoProfile -File .\usb_verify.ps1 -Step format

.EXAMPLE
    powershell -NoProfile -File .\usb_verify.ps1 -Step write
    # 拔掉 USB 线、重新插上
    powershell -NoProfile -File .\usb_verify.ps1 -Step check

.EXAMPLE
    powershell -NoProfile -File .\usb_verify.ps1 -Step inspect -DiskNumber 2 -DriveLetter F
#>

[CmdletBinding()]
param(
    [ValidateSet('inspect', 'format', 'write', 'check')]
    [string]$Step = 'inspect',

    [int]$DiskNumber = -1,

    [string]$DriveLetter = '',

    [int]$PatternBlocks = 8
)

$ErrorActionPreference = 'Stop'

# ── 目标识别常量 ──────────────────────────────────────────────────────────
# 这三个常量同时用于“自动找盘”和“防误操作校验”；不要为了省事放宽。
$VolumeLabel      = 'SOFTSSD'       # format 后写入的卷标
$ExpectedSize     = 449839104       # 429 MiB = 878,592 × 512B（主机可见容量，见 ftl.h FTL_LBA_MAX）
# 2KB = 一个 NAND plane 页；pattern 按 2KB 分组，专门抓 plane 镜像错误。
$PatternBlockSize = 2048
$TestFileName     = 'test.bin'
$PatternFileName  = 'pattern.bin'

# write 与 check 之间的期望值通过该文件传递（C: 上，跨拔插保留）。
$StateFile = Join-Path $env:TEMP 'softssd_usb_verify_state.json'
$LogFile   = Join-Path $env:TEMP ("softssd_usb_verify_{0}.log" -f $Step)

Start-Transcript -Path $LogFile -Force | Out-Null

# 判断当前 PowerShell 是否被提权。format / rebuild 必须提权；其他步骤不需要。
function Test-IsElevated {
    $identity  = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)

    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

<#
    找到并校验目标 USB 磁盘。

    防误操作是硬约束：即使调用者显式传了 -DiskNumber，也要再校验
    FriendlyName / Size / BusType —— 防止把 0/1 号 NVMe 当成 U 盘格式化。
    校验失败直接 throw，后续不会执行任何写操作。
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
        throw ('SOFTSSD USB disk not found: need FriendlyName=*SOFTSSD*, ' +
               'Size=449839104, BusType=USB')
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
    找到目标卷。

    优先级：
      1. 显式 -DriveLetter；
      2. 卷标 SOFTSSD；
      3. 目标磁盘上有盘符的分区（用户手工格式化可能清空卷标）。

    只按“目标磁盘的盘符”回退，不会拿别的 removable 盘（例如另一个 U 盘）。
#>
function Get-TargetVolume {
    if ($DriveLetter) {
        $volume = Get-Volume -DriveLetter $DriveLetter -ErrorAction SilentlyContinue
    }
    else {
        $volume = Get-Volume | Where-Object { $_.FileSystemLabel -eq $VolumeLabel } |
            Select-Object -First 1

        if ($null -eq $volume) {
            $disk = Get-TargetDisk
            $part = Get-Partition -DiskNumber $disk.Number -ErrorAction SilentlyContinue |
                Where-Object { $_.DriveLetter } | Select-Object -First 1

            if ($null -ne $part) {
                $volume = Get-Volume -DriveLetter $part.DriveLetter -ErrorAction SilentlyContinue
            }
        }
    }

    if ($null -eq $volume) {
        throw ("Volume not found (label='{0}', letter='{1}')" -f $VolumeLabel, $DriveLetter)
    }

    return $volume
}

<#
    生成非均匀 2KB 块 pattern：
      块 0 全 0x00，块 1 全 0x01，……，块 7 全 0x07。
    这样任何“后 2KB 复制前 2KB / 块顺序错位”都会在 check 的块指纹里暴露。
#>
function Write-PatternFile {
    param([string]$Path, [int]$Blocks)

    if (($Blocks -le 0) -or ($Blocks -gt 255)) {
        throw 'PatternBlocks must be in [1,255] (each block is filled with its index)'
    }

    $bytes = New-Object byte[] ($Blocks * $PatternBlockSize)

    for ($b = 0; $b -lt $Blocks; $b++) {
        for ($j = 0; $j -lt $PatternBlockSize; $j++) {
            $bytes[($b * $PatternBlockSize) + $j] = [byte]$b
        }
    }

    [IO.File]::WriteAllBytes($Path, $bytes)
}

<#
    校验 pattern.bin 的每一块：
      块 b 的 2048 个字节必须全部等于 b。
    这是 RISK-WB-06（plane1 复制 plane0）的直接负例判据。
#>
function Test-PatternFile {
    param([string]$Path, [int]$Blocks)

    if (-not (Test-Path $Path)) {
        return $false
    }

    $bytes = [IO.File]::ReadAllBytes($Path)

    if ($bytes.Length -ne ($Blocks * $PatternBlockSize)) {
        return $false
    }

    for ($b = 0; $b -lt $Blocks; $b++) {
        for ($j = 0; $j -lt $PatternBlockSize; $j++) {
            if ($bytes[($b * $PatternBlockSize) + $j] -ne [byte]$b) {
                return $false
            }
        }
    }

    return $true
}

<#
    只读打印磁盘 / 分区 / 卷状态。

    用 Write-Host 直接输出，避免被调用者的 `$disk = Show-DiskState` 捕获并丢弃；
    函数只通过 return 返回磁盘对象。
#>
function Show-DiskState {
    $disk = Get-TargetDisk

    Write-Host 'Disk:'
    $disk | Format-List Number, FriendlyName, Size, LogicalSectorSize, PhysicalSectorSize,
        PartitionStyle, OperationalStatus, IsReadOnly, IsOffline | Out-String | Write-Host

    Write-Host 'Partitions:'
    Get-Partition -DiskNumber $disk.Number -ErrorAction SilentlyContinue |
        Format-Table PartitionNumber, DriveLetter, Offset, Size, Type, MbrType -AutoSize |
        Out-String | Write-Host

    Write-Host 'Volumes:'
    Get-Volume -ErrorAction SilentlyContinue | Where-Object { $_.DriveType -eq 'Removable' } |
        Format-Table DriveLetter, FileSystemLabel, FileSystem, HealthStatus, OperationalStatus,
        Size, SizeRemaining -AutoSize | Out-String | Write-Host

    return $disk
}

try {
    switch ($Step) {
        # ── inspect：只读 ──────────────────────────────────────────────────
        # 验证：枚举成功、429 MiB、512/512 扇区、分区/卷状态。
        # 不改任何数据；失败时也只打印，不尝试修复。
        'inspect' {
            $null = Show-DiskState
            "LOG=$LogFile"
        }

        # ── format：破坏性，需管理员 ──────────────────────────────────────
        # 验证：host 写出的 MBR / 分区表 / FAT 引导扇区能被设备保存并读回。
        # 步骤与手工验证过的顺序一致：
        #   删分区 → Clear-Disk → Initialize MBR → New-Partition → Format FAT32。
        # 若 New-Partition 报 “Not enough available capacity” 或分区表异常
        # （offset 0 / Logical），改用 usb_rebuild_mbr.ps1（raw 清零前 1MB）。
        'format' {
            if (-not (Test-IsElevated)) {
                throw 'format requires an elevated PowerShell (Run as administrator)'
            }

            $disk = Show-DiskState

            # 先删掉缓存中的分区对象；某些异常 MBR 下 Clear-Disk 单独不够。
            'Removing existing partitions...'
            Get-Partition -DiskNumber $disk.Number -ErrorAction SilentlyContinue | ForEach-Object {
                "  remove partition $($_.PartitionNumber) offset=$($_.Offset) size=$($_.Size)"
                Remove-Partition -DiskNumber $disk.Number -PartitionNumber $_.PartitionNumber `
                    -Confirm:$false -ErrorAction SilentlyContinue
            }

            # Clear-Disk 失败不中止：后续 raw MBR 重建是更强的手段（见配套脚本）。
            Clear-Disk -Number $disk.Number -RemoveData -RemoveOEM -Confirm:$false -ErrorAction SilentlyContinue
            Start-Sleep -Seconds 1
            Update-HostStorageCache

            Initialize-Disk -Number $disk.Number -PartitionStyle MBR -Confirm:$false -ErrorAction SilentlyContinue

            # -UseMaximumSize 会按 Windows 的对齐策略放置分区；AssignDriveLetter
            # 让后续 Format-Volume 有明确目标。
            $partition = New-Partition -DiskNumber $disk.Number -UseMaximumSize -AssignDriveLetter

            Start-Sleep -Seconds 1
            Update-HostStorageCache

            $partition = Get-Partition -DiskNumber $disk.Number -PartitionNumber $partition.PartitionNumber

            if ([string]::IsNullOrEmpty($partition.DriveLetter)) {
                throw 'new partition has no drive letter (try tools/usb_verify/usb_rebuild_mbr.ps1)'
            }

            # FAT32：429 MiB 下 Windows 也可以选 FAT16/FAT；这里固定 FAT32，
            # 便于与 Linux mkfs.vfat 的测试口径对齐。
            $volume = Format-Volume -DriveLetter $partition.DriveLetter -FileSystem FAT32 `
                -NewFileSystemLabel $VolumeLabel -Confirm:$false -Force

            "Formatted: $($volume.DriveLetter): $($volume.FileSystem) size=$($volume.Size)"
            "LOG=$LogFile"
        }

        # ── write：写非均匀数据 + flush ───────────────────────────────────
        # 验证：host 多页写、wbuf 页打包、L2P 映射在**内容不均匀**时正确。
        # 这是 RISK-WB-06 的回归入口：pattern 每 2KB 不同，plane 镜像错误会暴露。
        #
        # 立即读回一致只说明 Windows 缓存一致；真正的设备侧判据是拔插后的 check。
        'write' {
            $volume = Get-TargetVolume
            $dir    = "$($volume.DriveLetter):"

            if (-not (Test-Path "$dir\")) {
                throw "drive $dir is not accessible"
            }

            $src = 'C:\Windows\System32\notepad.exe'

            # test.bin 选一个系统自带的、非均匀的中等文件；不要用全零文件。
            Copy-Item $src "$dir\$TestFileName" -Force
            Write-PatternFile -Path "$dir\$PatternFileName" -Blocks $PatternBlocks

            $srcHash = (Get-FileHash $src -Algorithm SHA256).Hash
            $dstHash = (Get-FileHash "$dir\$TestFileName" -Algorithm SHA256).Hash
            $patHash = (Get-FileHash "$dir\$PatternFileName" -Algorithm SHA256).Hash

            "SRC_HASH=$srcHash"
            "DST_HASH=$dstHash"
            "PAT_HASH=$patHash"

            if ($srcHash -ne $dstHash) {
                throw 'immediate readback mismatch (host cache vs device?)'
            }

            # SYNCHRONIZE CACHE：让设备把 write-back 数据落盘。
            # 失败不是致命错误（可能是权限），但必须提示用户改用安全弹出。
            try {
                Write-VolumeCache -DriveLetter $volume.DriveLetter -ErrorAction Stop
                'FLUSH_OK'
            }
            catch {
                "FLUSH_WARN: $($_.Exception.Message)"
                'Use Windows "Safely Remove Hardware" before unplugging.'
            }

            # 保存期望值供 check 使用；C: 上的文件跨拔插保留。
            @{
                drive          = $volume.DriveLetter
                src_hash       = $srcHash
                pattern_hash   = $patHash
                pattern_blocks = $PatternBlocks
                written_at     = (Get-Date).ToString('s')
            } | ConvertTo-Json | Set-Content -Path $StateFile -Encoding ascii

            "STATE=$StateFile"
            'Now unplug and replug the USB cable, then run: -Step check'
            "LOG=$LogFile"
        }

        # ── check：拔插后读回校验 ────────────────────────────────────────
        # 验证：
        #   1. test.bin 的 SHA256 与源一致；
        #   2. pattern.bin 的 SHA256 与 write 时一致；
        #   3. 每个 2KB 块内容 = 块号（plane 镜像 / 块错位的直接判据）。
        # 失败返回码 1，便于脚本/CI 判定。
        'check' {
            if (-not (Test-Path $StateFile)) {
                throw "state file not found: $StateFile (run -Step write first)"
            }

            $state  = Get-Content $StateFile -Raw | ConvertFrom-Json
            $volume = Get-TargetVolume
            $dir    = "$($volume.DriveLetter):"

            $patHash = (Get-FileHash "$dir\$PatternFileName" -Algorithm SHA256).Hash
            $dstHash = (Get-FileHash "$dir\$TestFileName" -Algorithm SHA256).Hash
            $patOk   = Test-PatternFile -Path "$dir\$PatternFileName" -Blocks $state.pattern_blocks

            "DST_HASH=$dstHash"
            "EXPECTED_DST_HASH=$($state.src_hash)"
            "PAT_HASH=$patHash"
            "EXPECTED_PAT_HASH=$($state.pattern_hash)"
            "PATTERN_BLOCKS_OK=$patOk"

            if (($dstHash -eq $state.src_hash) -and
                ($patHash -eq $state.pattern_hash) -and
                ($patOk -eq $true)) {
                'RESULT=PASS'
                "LOG=$LogFile"
            }
            else {
                'RESULT=FAIL'
                "LOG=$LogFile"
                exit 1
            }
        }

        default {
            throw "unknown step: $Step"
        }
    }
}
catch {
    # 统一错误出口：打印错误与日志路径，返回非零退出码（供调用者判定）。
    "ERROR: $($_.Exception.Message)"
    "LOG=$LogFile"
    exit 1
}
finally {
    Stop-Transcript | Out-Null
}
