<#
.SYNOPSIS
  Host-side crash-consistency loop for the Ext4Fsd driver (Hyper-V + WSL).

.DESCRIPTION
  Per iteration: start the VM, let the guest run crashgen for a random time,
  hard-reset it (Stop-VM -TurnOff), copy the guest's crashgen log by mounting
  the guest OS VHDX read-only, attach the ext4 test disk to WSL, run
  e2fsck -fn, verify every flushed file (verify.py, read-only noload mount),
  run e2fsck -fp, e2fsck -fn again, release the disk and append a row to
  ResultsDir\summary.csv.

  Requires: elevated PowerShell 5.1+, Hyper-V module, WSL2 with e2fsprogs and
  python3.  See README.md in this directory.

.PARAMETER VMName            Hyper-V VM name.
.PARAMETER GuestOsVhd        Path of the guest's OS VHDX (read on the host while the VM is off).
.PARAMETER TestDiskVhd       ext4 test disk as a VHDX (use this OR -TestDiskNumber).
.PARAMETER TestDiskNumber    ext4 test disk as a physical disk number (Get-Disk).
.PARAMETER GuestLogPath      Log path relative to the guest OS volume root (default crashlog\run.log).
.PARAMETER Iterations        Number of crash iterations.
.PARAMETER MinRunSec         Minimum seconds the guest runs after its heartbeat is OK.
.PARAMETER MaxRunSec         Maximum seconds the guest runs after its heartbeat is OK.
.PARAMETER BootTimeoutSec    Max seconds to wait for the heartbeat after Start-VM.
.PARAMETER ResultsDir        Output directory (default results\<timestamp> next to this script).
.PARAMETER Label             Free text recorded in summary.csv (e.g. "HEAD" or "journal-fix").
.PARAMETER Mnt               Mount point inside WSL (default /mnt/crashtest).
.PARAMETER VerifyScript      Path to verify.py (default: next to this script).
.PARAMETER WslDistro         Optional WSL distro name (wsl -d).
.PARAMETER ToolTimeoutSec    Timeout for each native tool invocation.
.PARAMETER DryRun            Print the commands instead of executing them.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$VMName,
    [Parameter(Mandatory = $true)][string]$GuestOsVhd,
    [string]$TestDiskVhd = '',
    [int]$TestDiskNumber = -1,
    [string]$GuestLogPath = 'crashlog\run.log',
    [int]$Iterations = 20,
    [int]$MinRunSec = 20,
    [int]$MaxRunSec = 120,
    [int]$BootTimeoutSec = 300,
    [string]$ResultsDir = '',
    [string]$Label = '',
    [string]$Mnt = '/mnt/crashtest',
    [string]$VerifyScript = '',
    [string]$WslDistro = '',
    [int]$ToolTimeoutSec = 3600,
    [switch]$DryRun
)

$ErrorActionPreference = 'Stop'

# ---------------------------------------------------------------- helpers

$script:LogFile = ''

function Write-Log {
    param([string]$Msg)
    $line = '[{0}] {1}' -f (Get-Date -Format 'HH:mm:ss'), $Msg
    Write-Host $line
    if ($script:LogFile) {
        Add-Content -Path $script:LogFile -Value $line -Encoding ASCII
    }
}

function Test-IsAdmin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    $pr = New-Object Security.Principal.WindowsPrincipal($id)
    return $pr.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

# Run a PowerShell scriptblock (cmdlet step), or just print it under -DryRun.
function Step {
    param([string]$Desc, [scriptblock]$Block)
    Write-Log ('> ' + $Desc)
    if ($DryRun) { return $null }
    return (& $Block)
}

# Run a native exe with output redirected to files; returns the exit code.
# (No 2>&1: stdout -> $OutFile, stderr -> $OutFile.err.)
function Invoke-Native {
    param(
        [string]$File,
        [string]$ArgString,
        [string]$OutFile,
        [string]$WorkDir = '',
        [string]$StdinFile = ''
    )
    $shown = $File + ' ' + $ArgString
    if ($StdinFile) { $shown = $shown + ' < ' + $StdinFile }
    if ($WorkDir) { $shown = '(cwd ' + $WorkDir + ') ' + $shown }
    Write-Log ('> ' + $shown)
    if ($DryRun) { return 0 }

    $sp = @{
        FilePath               = $File
        PassThru               = $true
        NoNewWindow            = $true
        RedirectStandardOutput = $OutFile
        RedirectStandardError  = ($OutFile + '.err')
    }
    if ($ArgString) { $sp['ArgumentList'] = $ArgString }
    if ($WorkDir) { $sp['WorkingDirectory'] = $WorkDir }
    if ($StdinFile) { $sp['RedirectStandardInput'] = $StdinFile }
    $p = Start-Process @sp
    $null = $p.Handle   # keep the handle so ExitCode is readable (PS 5.1 quirk)
    if (-not $p.WaitForExit($ToolTimeoutSec * 1000)) {
        try { $p.Kill() } catch { }
        Write-Log ('  TIMEOUT after ' + $ToolTimeoutSec + 's, killed')
        return -1
    }
    $p.WaitForExit()
    return [int]$p.ExitCode
}

function Read-Text {
    param([string]$Path)
    if ($DryRun -or -not (Test-Path -LiteralPath $Path)) { return '' }
    $t = Get-Content -LiteralPath $Path -Raw
    if ($null -eq $t) { return '' }
    return ($t -replace "`0", '')   # wsl.exe's own messages are UTF-16
}

function Get-WslPrefix {
    $p = ''
    if ($WslDistro) { $p = '-d ' + $WslDistro + ' ' }
    return $p + '-u root '
}

# Run a Linux program in WSL (no shell, so no quoting issues); returns exit code.
function Invoke-Wsl {
    param([string]$Cmd, [string]$OutFile, [string]$WorkDir = '', [string]$StdinFile = '')
    return (Invoke-Native -File 'wsl.exe' -ArgString ((Get-WslPrefix) + '-e ' + $Cmd) `
            -OutFile $OutFile -WorkDir $WorkDir -StdinFile $StdinFile)
}

function Get-WslDiskNames {
    param([string]$OutFile)
    $rc = Invoke-Wsl -Cmd 'lsblk -dno NAME' -OutFile $OutFile
    if ($DryRun) { return @() }
    if ($rc -ne 0) { throw ('lsblk failed, exit ' + $rc) }
    $names = @()
    foreach ($l in ((Read-Text $OutFile) -split "`r?`n")) {
        $n = $l.Trim()
        if ($n) { $names += $n }
    }
    return $names
}

# Target strings for wsl --mount / --unmount.
function Get-WslMountArgs {
    if ($TestDiskVhd) { return ('--mount --vhd "' + $TestDiskVhd + '" --bare') }
    return ('--mount \\.\PHYSICALDRIVE' + $TestDiskNumber + ' --bare')
}
function Get-WslUnmountArgs {
    if ($TestDiskVhd) { return ('--unmount "' + $TestDiskVhd + '"') }
    return ('--unmount \\.\PHYSICALDRIVE' + $TestDiskNumber)
}

# Make sure the test disk is released by WSL, offline on the host (physical),
# and attached to the VM.  Safe to call repeatedly.
function Initialize-TestDisk {
    param([string]$OutFile)
    # Release from WSL (ignore failure: it is usually not mounted).
    $null = Invoke-Native -File 'wsl.exe' -ArgString (Get-WslUnmountArgs) -OutFile $OutFile
    if ($TestDiskVhd) {
        $found = Step ('Get-VMHardDiskDrive -VMName ' + $VMName + ' (is ' + $TestDiskVhd + ' attached?)') {
            Get-VMHardDiskDrive -VMName $VMName | Where-Object { $_.Path -ieq $TestDiskVhd }
        }
        if ($DryRun -or -not $found) {
            $null = Step ('Add-VMHardDiskDrive -VMName ' + $VMName + ' -ControllerType SCSI -Path ' + $TestDiskVhd + '   (only if not attached)') {
                Add-VMHardDiskDrive -VMName $VMName -ControllerType SCSI -Path $TestDiskVhd
            }
        }
    }
    else {
        $d = Step ('Get-Disk -Number ' + $TestDiskNumber) { Get-Disk -Number $TestDiskNumber }
        if ($DryRun -or -not $d.IsOffline) {
            $null = Step ('Set-Disk -Number ' + $TestDiskNumber + ' -IsOffline $true   (only if online)') {
                Set-Disk -Number $TestDiskNumber -IsOffline $true
            }
        }
        $found = Step ('Get-VMHardDiskDrive -VMName ' + $VMName + ' (is physical disk ' + $TestDiskNumber + ' attached?)') {
            Get-VMHardDiskDrive -VMName $VMName | Where-Object { $_.DiskNumber -eq $TestDiskNumber }
        }
        if ($DryRun -or -not $found) {
            $null = Step ('Add-VMHardDiskDrive -VMName ' + $VMName + ' -ControllerType SCSI -DiskNumber ' + $TestDiskNumber + '   (only if not attached)') {
                Add-VMHardDiskDrive -VMName $VMName -ControllerType SCSI -DiskNumber $TestDiskNumber
            }
        }
    }
}

function Wait-Heartbeat {
    param([int]$TimeoutSec)
    $sw = [Diagnostics.Stopwatch]::StartNew()
    while ($sw.Elapsed.TotalSeconds -lt $TimeoutSec) {
        $hb = Get-VMIntegrationService -VMName $VMName -Name 'Heartbeat'
        if ($hb.PrimaryStatusDescription -eq 'OK') { return $true }
        Start-Sleep -Seconds 2
    }
    return $false
}

# Mount the guest OS VHDX read-only, copy the crashgen log, dismount.
# Returns $true when the log was copied.
function Copy-GuestLog {
    param([string]$Dest)
    if ($DryRun) {
        Write-Log ('> Mount-VHD -Path ' + $GuestOsVhd + ' -ReadOnly -Passthru')
        Write-Log ('> (find NTFS partition drive letter containing \' + $GuestLogPath + ')')
        Write-Log ('> Copy-Item <drive>:\' + $GuestLogPath + ' ' + $Dest)
        Write-Log ('> Dismount-VHD -Path ' + $GuestOsVhd)
        return $true
    }
    $copied = $false
    Write-Log ('> Mount-VHD -Path ' + $GuestOsVhd + ' -ReadOnly')
    Mount-VHD -Path $GuestOsVhd -ReadOnly
    try {
        $dn = (Get-VHD -Path $GuestOsVhd).DiskNumber
        for ($pass = 1; $pass -le 2 -and -not $copied; $pass++) {
            if ($pass -eq 2) {
                # No letter had the log: try to give letterless Basic partitions one.
                foreach ($p in (Get-Partition -DiskNumber $dn)) {
                    if (-not $p.DriveLetter -and $p.Type -eq 'Basic') {
                        try { Add-PartitionAccessPath -DiskNumber $dn -PartitionNumber $p.PartitionNumber -AssignDriveLetter } catch { Write-Log ('  could not assign letter: ' + $_) }
                    }
                }
            }
            foreach ($p in (Get-Partition -DiskNumber $dn)) {
                if (-not $p.DriveLetter) { continue }
                $src = ([string]$p.DriveLetter) + ':\' + $GuestLogPath
                if (Test-Path -LiteralPath $src) {
                    Write-Log ('> Copy-Item ' + $src + ' ' + $Dest)
                    Copy-Item -LiteralPath $src -Destination $Dest -Force
                    $copied = $true
                    break
                }
            }
        }
        if (-not $copied) { Write-Log ('  guest log \' + $GuestLogPath + ' not found on any partition') }
    }
    finally {
        Write-Log ('> Dismount-VHD -Path ' + $GuestOsVhd)
        try { Dismount-VHD -Path $GuestOsVhd } catch { Write-Log ('  Dismount-VHD failed: ' + $_) }
    }
    return $copied
}

function Invoke-Cleanup {
    # Best effort: leave nothing mounted/attached/running after an error.
    if ($DryRun) { return }
    try { if ((Get-VHD -Path $GuestOsVhd).Attached) { Dismount-VHD -Path $GuestOsVhd } } catch { }
    try { $null = Invoke-Native -File 'wsl.exe' -ArgString (Get-WslUnmountArgs) -OutFile (Join-Path $ResultsDir 'cleanup_unmount.txt') } catch { }
    try { if ((Get-VM -Name $VMName).State -ne 'Off') { Stop-VM -Name $VMName -TurnOff -Force } } catch { }
}

function Add-SummaryRow {
    param($Row)
    $obj = [pscustomobject]$Row
    if ($DryRun) {
        Write-Log ('> Export-Csv -Append ' + $SummaryCsv + '   row: ' + (($Row.Values | ForEach-Object { [string]$_ }) -join ','))
        return
    }
    $obj | Export-Csv -Path $SummaryCsv -Append -NoTypeInformation -Encoding ASCII
}

# ---------------------------------------------------------------- one iteration

function Invoke-Iteration {
    param([int]$N)
    $tag = 'iter{0:D3}' -f $N
    $dir = Join-Path $ResultsDir $tag
    if (-not $DryRun) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
    $row = [ordered]@{
        iter = $N; label = $Label; run_sec = ''; fsck_n_exit = ''; fsck_p_exit = ''
        fsck_n2_exit = ''; verify_ok = ''; checked = ''; missing = ''; mismatched = ''; note = ''
    }
    $notes = New-Object System.Collections.ArrayList
    $script:IterResult = @{ Row = $row; Stop = $false }
    Write-Log ('=== ' + $tag + ' ===')

    # (a) disk released from WSL + attached to VM, then boot
    Initialize-TestDisk -OutFile (Join-Path $dir 'wsl_prestart.txt')
    $startUtc = (Get-Date).ToUniversalTime()
    $null = Step ('Start-VM -Name ' + $VMName) { Start-VM -Name $VMName }
    $booted = $true
    if ($DryRun) {
        Write-Log ('> (wait up to ' + $BootTimeoutSec + 's for Get-VMIntegrationService -Name Heartbeat PrimaryStatusDescription -eq OK)')
    }
    else {
        $booted = Wait-Heartbeat -TimeoutSec $BootTimeoutSec
    }
    $runSec = 0
    if ($booted) {
        $runSec = Get-Random -Minimum $MinRunSec -Maximum ($MaxRunSec + 1)
        Write-Log ('guest up; letting crashgen run for ' + $runSec + 's')
        $t0 = Get-Date
        if (-not $DryRun) {
            $sw = [Diagnostics.Stopwatch]::StartNew()
            while ($sw.Elapsed.TotalSeconds -lt $runSec) {
                if ((Get-VM -Name $VMName).State -ne 'Running') { [void]$notes.Add('vm-left-running-state'); break }
                Start-Sleep -Seconds 1
            }
            $runSec = [int][math]::Round(((Get-Date) - $t0).TotalSeconds)
            $hb = (Get-VMIntegrationService -VMName $VMName -Name 'Heartbeat').PrimaryStatusDescription
            if ($hb -ne 'OK') { [void]$notes.Add('heartbeat-' + $hb + '-at-stop(guest hang/BSOD?)') }
        }
    }
    else {
        [void]$notes.Add('boot-timeout')
        Write-Log ('no heartbeat within ' + $BootTimeoutSec + 's')
    }
    $row.run_sec = $runSec

    # (b) hard reset
    $null = Step ('Stop-VM -Name ' + $VMName + ' -TurnOff -Force') { Stop-VM -Name $VMName -TurnOff -Force }

    # (c) guest log
    $logCopy = Join-Path $dir 'run.log'
    $haveLog = Copy-GuestLog -Dest $logCopy
    if ($haveLog -and -not $DryRun) {
        $lines = @(Get-Content -LiteralPath $logCopy)
        $nFlushed = @($lines | Where-Object { $_ -like "FLUSHED`t*" }).Count
        Write-Log ('guest log: ' + $lines.Count + ' lines, ' + $nFlushed + ' FLUSHED')
        if ($lines.Count -eq 0 -or $lines[0] -notlike "HEADER`t*") {
            [void]$notes.Add('log-no-header')
        }
        else {
            $m = [regex]::Match($lines[0], 'start=(\S+?)(\t|$)')
            if ($m.Success) {
                $gs = [datetime]::Parse($m.Groups[1].Value, [Globalization.CultureInfo]::InvariantCulture, [Globalization.DateTimeStyles]::AdjustToUniversal)
                if ($gs -lt $startUtc.AddSeconds(-10)) {
                    [void]$notes.Add('log-older-than-this-boot(stale?)')
                    Write-Log 'WARNING: log header predates this VM start - crashgen may not have started (check the scheduled task / MinRunSec)'
                }
            }
        }
    }
    elseif (-not $haveLog) {
        [void]$notes.Add('log-copy-failed')
    }

    # (d) attach test disk to WSL, find the new device
    $dev = ''
    $wslMounted = $false
    $before = Get-WslDiskNames -OutFile (Join-Path $dir 'lsblk_before.txt')
    $rc = Invoke-Native -File 'wsl.exe' -ArgString (Get-WslMountArgs) -OutFile (Join-Path $dir 'wsl_mount.txt')
    if ($rc -ne 0) {
        [void]$notes.Add('wsl-mount-failed:' + $rc)
        Write-Log ('wsl --mount failed (' + $rc + '): ' + (Read-Text (Join-Path $dir 'wsl_mount.txt')))
    }
    else {
        $wslMounted = $true
        if ($DryRun) {
            $null = Get-WslDiskNames -OutFile (Join-Path $dir 'lsblk_after.txt')
            Write-Log '> (new device = lsblk after minus lsblk before; if the disk has a partition table the ext* partition from lsblk -lno NAME,FSTYPE is used)'
            $dev = '/dev/sdX'
        }
        else {
            $after = Get-WslDiskNames -OutFile (Join-Path $dir 'lsblk_after.txt')
            $new = @($after | Where-Object { $before -notcontains $_ })
            if ($new.Count -eq 0) { [void]$notes.Add('no-new-wsl-device') }
            else {
                if ($new.Count -gt 1) { [void]$notes.Add('multiple-new-wsl-devices') }
                $dev = '/dev/' + $new[$new.Count - 1]
                # If the disk carries a partition table, use the ext* partition.
                $null = Invoke-Wsl -Cmd ('lsblk -lno NAME,FSTYPE ' + $dev) -OutFile (Join-Path $dir 'lsblk_fs.txt')
                foreach ($l in ((Read-Text (Join-Path $dir 'lsblk_fs.txt')) -split "`r?`n")) {
                    $parts = $l.Trim() -split '\s+'
                    if ($parts.Count -ge 2 -and $parts[1] -match '^ext[234]$') { $dev = '/dev/' + $parts[0]; break }
                }
            }
        }
    }

    if ($dev) {
        Write-Log ('test filesystem device: ' + $dev)

        # (e) read-only fsck of the crashed image
        $rc = Invoke-Wsl -Cmd ('e2fsck -fn ' + $dev) -OutFile (Join-Path $dir 'fsck_n.txt')
        $row.fsck_n_exit = $rc

        # (f) read-only mount without journal replay, verify flushed files
        $null = Invoke-Wsl -Cmd ('mkdir -p ' + $Mnt) -OutFile (Join-Path $dir 'mkdir.txt')
        $rc = Invoke-Wsl -Cmd ('mount -o ro,noload ' + $dev + ' ' + $Mnt) -OutFile (Join-Path $dir 'mount.txt')
        if ($rc -ne 0) {
            $row.verify_ok = 'mount-failed'
            [void]$notes.Add('ro-mount-failed:' + $rc)
        }
        elseif (-not $haveLog) {
            $row.verify_ok = 'no-log'
            $null = Invoke-Wsl -Cmd ('umount ' + $Mnt) -OutFile (Join-Path $dir 'umount.txt')
        }
        else {
            $vout = Join-Path $dir 'verify.json'
            $vrc = Invoke-Wsl -Cmd ('python3 - --log run.log --mnt ' + $Mnt) -OutFile $vout -WorkDir $dir -StdinFile $VerifyScript
            if ($DryRun) {
                $row.verify_ok = 'DRYRUN'
            }
            else {
                try {
                    $vj = (Read-Text $vout) | ConvertFrom-Json
                    $row.checked = $vj.checked
                    $row.missing = $vj.missing
                    $row.mismatched = $vj.mismatched
                    if ($vrc -eq 0) { $row.verify_ok = 'true' } else { $row.verify_ok = 'false' }
                    foreach ($w in @($vj.warnings)) { if ($w) { [void]$notes.Add('verify-warning:' + $w) } }
                }
                catch {
                    $row.verify_ok = 'error'
                    [void]$notes.Add('verify-no-json(exit ' + $vrc + ')')
                }
            }
            $null = Invoke-Wsl -Cmd ('umount ' + $Mnt) -OutFile (Join-Path $dir 'umount.txt')
        }

        # (g) repair, then re-check; 0/1 = repaired, >=4 = not repaired
        $rc = Invoke-Wsl -Cmd ('e2fsck -fp ' + $dev) -OutFile (Join-Path $dir 'fsck_p.txt')
        $row.fsck_p_exit = $rc
        $rc = Invoke-Wsl -Cmd ('e2fsck -fn ' + $dev) -OutFile (Join-Path $dir 'fsck_n2.txt')
        $row.fsck_n2_exit = $rc
        if (-not $DryRun -and $rc -ne 0) {
            [void]$notes.Add('UNRECOVERABLE:fsck-n2-exit-' + $rc)
            $script:IterResult.Stop = $true
        }
    }

    # (h) release the disk from WSL; re-offline / re-attach physical disks
    if ($wslMounted) {
        $null = Invoke-Native -File 'wsl.exe' -ArgString (Get-WslUnmountArgs) -OutFile (Join-Path $dir 'wsl_unmount.txt')
    }
    Initialize-TestDisk -OutFile (Join-Path $dir 'wsl_poststop.txt')

    # (i) summary row
    $row.note = ($notes -join ';')
    Add-SummaryRow -Row $row
    $line = 'RESULT ' + $tag + ': fsck_n=' + $row.fsck_n_exit + ' fsck_p=' + $row.fsck_p_exit +
        ' fsck_n2=' + $row.fsck_n2_exit + ' verify_ok=' + $row.verify_ok + ' checked=' + $row.checked +
        ' missing=' + $row.missing + ' mismatched=' + $row.mismatched
    if ($row.note) { $line = $line + ' note=' + $row.note }
    Write-Log $line
}

# ---------------------------------------------------------------- main

if ($TestDiskVhd -and $TestDiskNumber -ge 0) { throw 'Specify -TestDiskVhd OR -TestDiskNumber, not both.' }
if (-not $TestDiskVhd -and $TestDiskNumber -lt 0) { throw 'Specify -TestDiskVhd or -TestDiskNumber.' }
if ($MaxRunSec -lt $MinRunSec) { throw '-MaxRunSec must be >= -MinRunSec.' }
if (-not $VerifyScript) { $VerifyScript = Join-Path $PSScriptRoot 'verify.py' }
if (-not $ResultsDir) {
    $ResultsDir = Join-Path (Join-Path $PSScriptRoot 'results') (Get-Date -Format 'yyyyMMdd-HHmmss')
}

if (-not $DryRun) {
    if (-not (Test-IsAdmin)) { throw 'This script must run from an elevated (Administrator) PowerShell.' }
    Import-Module Hyper-V -ErrorAction Stop
    if (-not (Get-Command wsl.exe -ErrorAction SilentlyContinue)) { throw 'wsl.exe not found.' }
    if (-not (Test-Path -LiteralPath $VerifyScript)) { throw ('verify script not found: ' + $VerifyScript) }
    if (-not (Test-Path -LiteralPath $GuestOsVhd)) { throw ('guest OS VHD not found: ' + $GuestOsVhd) }
    if ($TestDiskVhd -and -not (Test-Path -LiteralPath $TestDiskVhd)) { throw ('test disk VHD not found: ' + $TestDiskVhd) }
    $null = Get-VM -Name $VMName
    New-Item -ItemType Directory -Force -Path $ResultsDir | Out-Null
    $script:LogFile = Join-Path $ResultsDir 'crash-loop.log'
}
$SummaryCsv = Join-Path $ResultsDir 'summary.csv'

Write-Log ('crash-loop: VM=' + $VMName + ' iterations=' + $Iterations + ' run=' + $MinRunSec + '..' + $MaxRunSec + 's label="' + $Label + '"' + $(if ($DryRun) { ' [DRY RUN]' } else { '' }))
Write-Log ('results: ' + $ResultsDir)

$consecInfraFail = 0
$exitCode = 0
$last = $Iterations
if ($DryRun) { $last = 1; Write-Log '(dry run: printing the command sequence for one iteration only)' }
for ($i = 1; $i -le $last; $i++) {
    $script:IterResult = $null
    try {
        Invoke-Iteration -N $i
        $consecInfraFail = 0
    }
    catch {
        $consecInfraFail++
        Write-Log ('ERROR in iteration ' + $i + ': ' + $_)
        Invoke-Cleanup
        Add-SummaryRow -Row ([ordered]@{
                iter = $i; label = $Label; run_sec = ''; fsck_n_exit = ''; fsck_p_exit = ''
                fsck_n2_exit = ''; verify_ok = 'error'; checked = ''; missing = ''; mismatched = ''
                note = ('script-error: ' + ($_.Exception.Message -replace '[\r\n,]+', ' '))
            })
        if ($consecInfraFail -ge 3) {
            Write-Log 'three consecutive infrastructure errors; stopping.'
            $exitCode = 2
            break
        }
        continue
    }
    if ($script:IterResult -and $script:IterResult.Stop) {
        Write-Log 'STOP: the test disk is not clean after e2fsck -fp (fsck_n2 != 0). Disk left unmounted for inspection; not continuing.'
        $exitCode = 3
        break
    }
}
Write-Log ('done. summary: ' + $SummaryCsv)
exit $exitCode
