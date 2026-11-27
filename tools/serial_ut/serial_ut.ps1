<#
.SYNOPSIS
  串口 UT 分层 runner：整个会话只打开一次串口；全量 → 父套件 → 剩余单例。

.DESCRIPTION
  串口：COM9 / 115200 / 8N1（本机 comtool 配置；GUI 必须先关闭）。
  失败后自动用 J-Link GDB server（localhost:2331）复位：
    wsl.exe -e gdb-multiarch ... monitor reset / go
  分层定位：
    -Mode full  先跑全量；失败后跑失败用例的父套件；父套件仍失败则跑该父套件中
                失败点之后的单例。
    -Mode suite 只跑一个父套件。
    -Mode after 从指定用例之后按 --list 顺序逐个跑。
    -Mode one   单跑一个用例。
    -Mode list  只列用例。

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File .\serial_ut.ps1 -Mode list
.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File .\serial_ut.ps1 -Mode full -Seed 0x81a7c024
.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File .\serial_ut.ps1 -Mode suite -Suite /ftl/negative/media/gc/
.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File .\serial_ut.ps1 -Mode names -NamesFile .\rerun.txt
#>
[CmdletBinding()]
param(
    [ValidateSet('full', 'suite', 'after', 'one', 'names', 'list')][string]$Mode = 'full',
    [string]$Suite = '',
    [string]$After = '',
    [string]$Test = '',
    [string[]]$Names = @(),
    [string]$NamesFile = '',
    [string]$Seed = '0x81a7c024',
    [string]$Port = 'COM9',
    [int]$Baud = 115200,
    [string]$LogDir = "$env:TEMP\basessd\ut",
    [int]$IdleMs = 800,
    [switch]$NoReset,
    [switch]$CleanTemp
)

$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force -Path $LogDir | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd_HHmmss'
$sessionPath = Join-Path $LogDir "session_$stamp.log"
$summaryPath = Join-Path $LogDir "summary_$stamp.txt"
$results = New-Object System.Collections.ArrayList

function Log([string]$msg) {
    $line = "[{0}] {1}" -f (Get-Date -Format 'HH:mm:ss'), $msg
    Add-Content -Path $sessionPath -Value $line -Encoding UTF8
    Write-Host $line
}

$p = New-Object System.IO.Ports.SerialPort
$p.PortName = $Port
$p.BaudRate = $Baud
$p.Parity = [System.IO.Ports.Parity]::None
$p.DataBits = 8
$p.StopBits = [System.IO.Ports.StopBits]::One
$p.ReadBufferSize = 1048576
$p.NewLine = "`r"

function Get-Timeout([string]$name) {
    if ($name -match '^/bench/speed/') { return 900 }
    if ($name -match '^/ftl/negative/media/') { return 600 }
    if ($name -match '^/ftl/media/') { return 240 }
    return 30
}

function Send-Read([string]$cmd, [int]$timeoutSec, [switch]$UntilIdle) {
    $p.DiscardInBuffer()
    $p.WriteLine($cmd)
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $all = ''
    $last = 0.0
    while ($sw.Elapsed.TotalSeconds -lt $timeoutSec) {
        $c = $p.ReadExisting()
        if ($c.Length -gt 0) {
            $all += $c
            $last = $sw.Elapsed.TotalSeconds
        }
        if ($all -match 'Total wall-clock') { break }
        if ($all -match '\[ERR\] abort\(\) called') { break }
        if ($all -match '\[ASSERT\] console active') { break }
        if ($UntilIdle -and ((($sw.Elapsed.TotalSeconds - $last) * 1000) -ge $IdleMs)) { break }
        Start-Sleep -Milliseconds 100
    }
    return $all
}

function Wait-Prompt([int]$timeoutSec = 8) {
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $all = ''
    while ($sw.Elapsed.TotalSeconds -lt $timeoutSec) {
        $c = $p.ReadExisting()
        if ($c.Length -gt 0) { $all += $c }
        if ($all -match 'ssd>') { return }
        Start-Sleep -Milliseconds 100
    }
}

function Reset-Target {
    if ($NoReset) {
        Log 'RESET skipped (-NoReset); reset the board manually'
        Start-Sleep -Seconds 3
        return
    }
    $saved_eap = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        & wsl.exe -e gdb-multiarch -batch -ex 'set pagination off' `
            -ex 'target extended-remote localhost:2331' `
            -ex 'monitor reset' -ex 'monitor go' -ex 'detach' *> $null
        if ($LASTEXITCODE -ne 0) {
            Log ("RESET command exit={0}; reset the board manually" -f $LASTEXITCODE)
        }
        else {
            Log 'RESET via J-Link GDB server'
        }
    }
    catch {
        Log ("RESET failed: {0}" -f $_.Exception.Message)
    }
    finally {
        $ErrorActionPreference = $saved_eap
    }
    Start-Sleep -Milliseconds 2500
    Wait-Prompt 8
}

function Classify([string]$out) {
    if ($out -match '\[ERR\] abort\(\) called') { return 'ABORT' }
    if ($out -match '\[ASSERT\] console active') { return 'ABORT' }
    if ($out -match 'Error:') { return 'FAIL' }
    if ($out -match 'Total wall-clock') {
        if ($out -match '(\d+) of \d+ \([^)]*\) tests successful,\s*(\d+) \([^)]*\) test skipped') {
            if ([int]$Matches[1] -gt 0) { return 'OK' }
            if ([int]$Matches[2] -gt 0) { return 'SKIP' }
        }
        return 'FAIL'
    }
    return 'TIMEOUT'
}

function Save-TestLog([string]$name, [string]$out) {
    $safe = ($name -replace '[^A-Za-z0-9._-]', '_')
    $path = Join-Path $LogDir ("{0}.log" -f $safe)
    Set-Content -Path $path -Value $out -Encoding UTF8
    return $path
}

function Run-Test([string]$name, [int]$timeoutSec = 0) {
    if ($timeoutSec -le 0) { $timeoutSec = Get-Timeout $name }
    Log ("TEST {0} (timeout {1}s)" -f $name, $timeoutSec)
    $out = Send-Read ("test " + $name) $timeoutSec
    $status = Classify $out
    $path = Save-TestLog $name $out
    $null = $results.Add([pscustomobject]@{ Name = $name; Status = $status; Log = $path })
    Log ("RESULT {0} {1}" -f $status, $name)
    if (($status -eq 'ABORT') -or ($status -eq 'TIMEOUT')) { Reset-Target }
    return $status
}

function Get-TestList {
    $out = Send-Read 'test --list' 30 -UntilIdle
    return @($out -split "`r?`n" | Where-Object { $_ -match '^/[A-Za-z0-9_/-]+' } | ForEach-Object { $_.Trim() })
}

function Get-ParentSuite([string]$name) {
    $trim = $name.TrimEnd('/')
    $idx = $trim.LastIndexOf('/')
    if ($idx -le 0) { return '/' }
    return $trim.Substring(0, $idx + 1)
}

function Find-FailingTest([string]$out) {
    $lines = @($out -split "`r?`n")
    $errors = @($lines | Where-Object { $_ -match '^/[A-Za-z0-9_/-]+.*:\s+Error:' })
    if ($errors.Count -gt 0) {
        return ($errors[$errors.Count - 1] -replace ':.*$', '')
    }
    $tests = @($lines | Where-Object { $_ -match '^/[A-Za-z0-9_/-]+:' })
    if ($tests.Count -eq 0) { return '' }
    return ($tests[$tests.Count - 1] -replace ':.*$', '')
}

try {
    $p.Open()
    Start-Sleep -Milliseconds 300
    Wait-Prompt 5
    Log ("OPEN {0}@{1} logdir={2}" -f $Port, $Baud, $LogDir)

    switch ($Mode) {
        'list' {
            $names = Get-TestList
            $names | ForEach-Object { Write-Output $_ }
            Log ("LIST count={0}" -f $names.Count)
        }
        'one' {
            if (-not $Test) { throw '-Mode one requires -Test' }
            $null = Run-Test $Test
        }
        'suite' {
            if (-not $Suite) { throw '-Mode suite requires -Suite' }
            $null = Run-Test $Suite 900
        }
        'after' {
            if (-not $After) { throw '-Mode after requires -After' }
            $names = Get-TestList
            $idx = [Array]::IndexOf($names, $After)
            if ($idx -lt 0) { throw "test not found: $After" }
            for ($i = $idx + 1; $i -lt $names.Count; $i++) {
                $null = Run-Test $names[$i]
            }
        }
        'names' {
            $list = @()
            if ($NamesFile) {
                $list = @(Get-Content -Path $NamesFile | Where-Object { $_.Trim() } | ForEach-Object { $_.Trim() })
            }
            if ($Names.Count -gt 0) { $list += $Names }
            if ($list.Count -eq 0) { throw '-Mode names requires -Names or -NamesFile' }
            foreach ($n in $list) { $null = Run-Test $n }
        }
        'full' {
            $out = Send-Read ("test --seed " + $Seed) 1800
            $status = Classify $out
            $null = Save-TestLog ('__full_' + $Seed) $out
            Log ("FULL {0}" -f $status)
            if ($status -ne 'OK') {
                $failed = Find-FailingTest $out
                if ($failed) {
                    $parent = Get-ParentSuite $failed
                    Log ("FULL failed at {0}; retry parent suite {1}" -f $failed, $parent)
                    $pst = Run-Test $parent 900
                    if ($pst -ne 'OK') {
                        Log 'PARENT still failing; run remaining tests in this parent individually'
                        $names = Get-TestList
                        $fidx = [Array]::IndexOf($names, $failed)
                        for ($i = $fidx + 1; $i -lt $names.Count; $i++) {
                            if (-not $names[$i].StartsWith($parent)) { break }
                            $null = Run-Test $names[$i]
                        }
                    }
                    else {
                        Log 'PARENT passed; failure is full-suite state/order related'
                    }
                }
                if (($status -eq 'ABORT') -or ($status -eq 'TIMEOUT')) { Reset-Target }
            }
        }
    }
}
finally {
    $counts = @{}
    foreach ($r in $results) {
        if (-not $counts.ContainsKey($r.Status)) { $counts[$r.Status] = 0 }
        $counts[$r.Status]++
    }
    $summary = @()
    $summary += ("session: {0}" -f $sessionPath)
    $summary += ("results: {0}" -f $results.Count)
    foreach ($k in ($counts.Keys | Sort-Object)) { $summary += ("  {0}: {1}" -f $k, $counts[$k]) }
    $failed = @($results | Where-Object { $_.Status -notin @('OK', 'SKIP') })
    $summary += 'non-OK:'
    foreach ($r in $failed) { $summary += ("  {0} {1} -> {2}" -f $r.Status, $r.Name, $r.Log) }
    $summary | Set-Content -Path $summaryPath -Encoding UTF8
    Write-Host ("SUMMARY {0}" -f $summaryPath)
    $summary | ForEach-Object { Write-Host $_ }
    if ($CleanTemp) {
        $keep = @($failed | ForEach-Object { [IO.Path]::GetFileName($_.Log) })
        Get-ChildItem -Path $LogDir -Filter '*.log' | Where-Object {
            ($_.FullName -ne $sessionPath) -and ($keep -notcontains $_.Name)
        } | Remove-Item -Force
        Write-Host 'CLEANED per-test logs (kept session/summary/failing)'
    }
    if ($p.IsOpen) { $p.Close() }
}
