<#
.SYNOPSIS
    Creates the Hyper-V test VM for Ext4Fsd (run from an elevated PowerShell on the host).

.DESCRIPTION
    Step 1 (before Windows is installed):
        .\tools\vm\new-testvm.ps1 -IsoPath D:\Software\Win11.iso
    Creates a Gen2 VM with vTPM and Secure Boot (Windows 11 setup wants both), an
    OS disk, an empty ext4 test disk (dynamic VHDX, formatted later from WSL by
    format-testdisk.ps1) and boots it from the ISO. Install Windows in the VM window.

    Step 2 (after Windows is installed and shut down):
        .\tools\vm\new-testvm.ps1 -AfterInstall
    Ejects the ISO and turns Secure Boot off; test-signed drivers do not load with
    Secure Boot on, even with testsigning enabled.

    Automatic checkpoints are disabled: the crash loop hard-resets the VM and must
    never roll the disks back.
#>
param(
    [string] $VMName = 'ext4test',
    [string] $IsoPath = '',
    [string] $VMDir = 'D:\VMs\ext4test',
    [int] $MemoryGB = 8,
    [int] $Cpus = 4,
    [int] $OsDiskGB = 100,
    [int] $TestDiskGB = 2048,
    [switch] $AfterInstall
)

$ErrorActionPreference = 'Stop'

$id = [Security.Principal.WindowsIdentity]::GetCurrent()
if (-not (New-Object Security.Principal.WindowsPrincipal($id)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Run this from an elevated (Administrator) PowerShell.'
}

if ($AfterInstall) {
    $vm = Get-VM -Name $VMName
    if ($vm.State -ne 'Off') { throw "Shut down $VMName first (Start > Shut down inside the VM)." }
    Get-VMDvdDrive -VMName $VMName | Set-VMDvdDrive -Path $null
    Set-VMFirmware -VMName $VMName -EnableSecureBoot Off
    Set-VMFirmware -VMName $VMName -FirstBootDevice (Get-VMHardDiskDrive -VMName $VMName -ControllerLocation 0)
    Write-Host "Secure Boot off and ISO ejected on $VMName." -ForegroundColor Green
    return
}

if (-not $IsoPath -or -not (Test-Path $IsoPath)) { throw "Windows 11 ISO not found: '$IsoPath'" }
if (Get-VM -Name $VMName -ErrorAction SilentlyContinue) { throw "VM $VMName already exists." }

New-Item -ItemType Directory -Force $VMDir | Out-Null
$osVhd = Join-Path $VMDir "$VMName-os.vhdx"
$testVhd = Join-Path $VMDir "$VMName-ext4.vhdx"

New-VM -Name $VMName -Generation 2 -MemoryStartupBytes ([int64]$MemoryGB * 1GB) `
       -NewVHDPath $osVhd -NewVHDSizeBytes ([int64]$OsDiskGB * 1GB) -Path $VMDir | Out-Null
Set-VM -Name $VMName -ProcessorCount $Cpus -StaticMemory -AutomaticCheckpointsEnabled $false `
       -CheckpointType Disabled -AutomaticStopAction TurnOff
Set-VMKeyProtector -VMName $VMName -NewLocalKeyProtector
Enable-VMTPM -VMName $VMName
Set-VMFirmware -VMName $VMName -EnableSecureBoot On -SecureBootTemplate MicrosoftWindows

# Internet access during setup is optional; attach the default switch if it exists.
$sw = Get-VMSwitch -Name 'Default Switch' -ErrorAction SilentlyContinue
if ($sw) { Connect-VMNetworkAdapter -VMName $VMName -SwitchName $sw.Name }

New-VHD -Path $testVhd -SizeBytes ([int64]$TestDiskGB * 1GB) -Dynamic | Out-Null
Add-VMHardDiskDrive -VMName $VMName -ControllerType SCSI -Path $testVhd

$dvd = Add-VMDvdDrive -VMName $VMName -Path $IsoPath -Passthru
Set-VMFirmware -VMName $VMName -FirstBootDevice $dvd

Write-Host "Created $VMName" -ForegroundColor Green
Write-Host "  OS disk   : $osVhd"
Write-Host "  ext4 disk : $testVhd ($TestDiskGB GB dynamic, unformatted)"
Write-Host 'Next: vmconnect localhost ext4test, start it, press a key to boot the ISO, install Windows.'
Write-Host '      Then shut the VM down and run this script again with -AfterInstall.'
