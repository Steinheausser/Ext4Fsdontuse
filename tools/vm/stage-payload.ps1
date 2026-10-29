<#
.SYNOPSIS
    Fills the VM's PAYLOAD disk (a small NTFS VHDX) with a driver build, the test
    tools and a mode file, or collects results from it. Host side, elevated, VM off.

.DESCRIPTION
    The guest never needs to be signed into: at every boot a SYSTEM startup task
    (guest-autorun.ps1) reads PAYLOAD:\mode.txt, installs the staged driver if it
    differs from the installed one (then reboots), and runs the requested mode.

    Modes:
        none                 do nothing (normal desktop use)
        bench [durable]      run the B1-B6 suite once, write results.csv, shut down.
                             durable = lockdismount (HEAD baseline) | flushvol | none
        crash                start crashgen (use with tools\crash\crash-loop.ps1)
    Add the word 'nodefender' to skip the Defender exclusion for the ext4 volume.

.EXAMPLE
    .\tools\vm\stage-payload.ps1 -EwdkRoot F:\ -Build tools\build\out\18bd208-baseline\Release -Mode 'bench lockdismount'
    .\tools\vm\stage-payload.ps1 -Collect
#>
param(
    [string] $VMName = 'ext4test',
    [string] $VMDir = 'D:\VMs\ext4test',
    [string] $Build = '',
    [string] $EwdkRoot = '',
    [string] $Mode = '',
    [string] $Label = '',
    [switch] $Collect,
    [string] $ResultsDir = ''
)

$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path

$id = [Security.Principal.WindowsIdentity]::GetCurrent()
if (-not (New-Object Security.Principal.WindowsPrincipal($id)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Run this from an elevated (Administrator) PowerShell.'
}
$vm = Get-VM -Name $VMName
if ($vm.State -ne 'Off') { throw "$VMName must be off (it is $($vm.State))." }

$payloadVhd = Join-Path $VMDir "$VMName-payload.vhdx"

function Mount-Payload {
    $disk = Mount-VHD -Path $payloadVhd -Passthru | Get-Disk
    $part = Get-Partition -DiskNumber $disk.Number | Where-Object { $_.Type -eq 'Basic' } | Select-Object -First 1
    if (-not $part.DriveLetter) {
        $part | Add-PartitionAccessPath -AssignDriveLetter
        $part = Get-Partition -DiskNumber $disk.Number -PartitionNumber $part.PartitionNumber
    }
    return "$($part.DriveLetter):\"
}

# --- create the payload disk on first use -------------------------------------
if (-not (Test-Path $payloadVhd)) {
    if ($Collect) { throw "No payload disk yet: $payloadVhd" }
    New-VHD -Path $payloadVhd -SizeBytes 4GB -Dynamic | Out-Null
    $disk = Mount-VHD -Path $payloadVhd -Passthru | Get-Disk
    Initialize-Disk -Number $disk.Number -PartitionStyle GPT
    New-Partition -DiskNumber $disk.Number -UseMaximumSize -AssignDriveLetter |
        Format-Volume -FileSystem NTFS -NewFileSystemLabel PAYLOAD -Confirm:$false | Out-Null
    Dismount-VHD -Path $payloadVhd
    Add-VMHardDiskDrive -VMName $VMName -ControllerType SCSI -Path $payloadVhd
    Write-Host "Created and attached $payloadVhd"
}

$root = Mount-Payload
try {
    if ($Collect) {
        if (-not $ResultsDir) { $ResultsDir = Join-Path $repo ("tools\bench\results\" + (Get-Date -Format 'yyyyMMdd-HHmmss')) }
        New-Item -ItemType Directory -Force $ResultsDir | Out-Null
        foreach ($f in @('results.csv', 'bench-log.txt', 'autorun-log.txt', 'driver-installed.txt')) {
            $p = Join-Path $root $f
            if (Test-Path $p) { Copy-Item $p $ResultsDir -Force }
        }
        Write-Host "Collected into $ResultsDir" -ForegroundColor Green
        return
    }

    # --- tools and guest scripts ----------------------------------------------
    $tools = Join-Path $root 'tools'
    New-Item -ItemType Directory -Force $tools | Out-Null
    foreach ($exe in @('tools\bench\ext4bench.exe', 'tools\crash\crashgen.exe')) {
        $p = Join-Path $repo $exe
        if (-not (Test-Path $p)) { throw "$p missing; run tools\bench\build.cmd and tools\crash\build.cmd" }
        Copy-Item $p $tools -Force
    }
    foreach ($s in @('guest-setup.ps1', 'guest-autorun.ps1')) {
        Copy-Item (Join-Path $PSScriptRoot $s) $tools -Force
    }

    # --- driver: test-sign a copy with a local self-signed certificate ---------
    if ($Build) {
        $sys = Join-Path $Build 'Ext2Fsd.sys'
        if (-not (Test-Path $sys)) { throw "$sys not found" }
        if (-not $EwdkRoot) { throw '-EwdkRoot is needed to sign the driver (signtool comes from the EWDK).' }
        $signtool = Get-ChildItem (Join-Path $EwdkRoot 'Program Files\Windows Kits\10\bin') -Recurse -Filter signtool.exe |
                    Where-Object { $_.FullName -match '\\x64\\' } | Select-Object -First 1
        if (-not $signtool) { throw "signtool.exe not found under $EwdkRoot" }

        $cert = Get-ChildItem Cert:\CurrentUser\My | Where-Object { $_.Subject -eq 'CN=Ext4Fsd Test Signing' } | Select-Object -First 1
        if (-not $cert) {
            $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject 'CN=Ext4Fsd Test Signing' `
                        -CertStoreLocation Cert:\CurrentUser\My -NotAfter (Get-Date).AddYears(5)
        }

        $drv = Join-Path $root 'driver'
        New-Item -ItemType Directory -Force $drv | Out-Null
        Copy-Item $sys $drv -Force
        $pdb = Join-Path $Build 'Ext2Fsd.pdb'
        if (Test-Path $pdb) { Copy-Item $pdb $drv -Force }
        Export-Certificate -Cert $cert -FilePath (Join-Path $drv 'ext4fsd-test.cer') | Out-Null

        & $signtool.FullName sign /fd sha256 /sha1 $cert.Thumbprint (Join-Path $drv 'Ext2Fsd.sys')
        if ($LASTEXITCODE -ne 0) { throw "signtool failed ($LASTEXITCODE)" }

        if (-not $Label) { $Label = (Split-Path (Split-Path $Build -Parent) -Leaf) + '-' + (Split-Path $Build -Leaf) }
        Set-Content -Path (Join-Path $drv 'label.txt') -Value $Label -Encoding ASCII
        Write-Host "Staged driver $Label"
    }

    if ($Mode) {
        Set-Content -Path (Join-Path $root 'mode.txt') -Value $Mode -Encoding ASCII
        Write-Host "Mode: $Mode"
    }
} finally {
    Dismount-VHD -Path $payloadVhd
}
