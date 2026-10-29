<#
.SYNOPSIS
    One-time setup inside the test VM. Run from an elevated PowerShell in the guest:
        powershell -ExecutionPolicy Bypass -File P:\tools\guest-setup.ps1
    (P: = whatever letter the PAYLOAD disk got.) Then reboot the VM.

.DESCRIPTION
    - testsigning on; boot-failure recovery prompts off (the crash loop hard-resets
      the VM many times and must never stop at "Automatic Repair").
    - Fast Startup / hibernation off.
    - Trusts the payload's test-signing certificate.
    - Installs the staged Ext2Fsd.sys as the Ext2Fsd file-system service with the
      INF's default parameters (Ext4Fsd\Ext2Fsd.inf).
    - Registers the startup task "Ext4Autorun" (SYSTEM) that runs
      PAYLOAD:\tools\guest-autorun.ps1 at every boot, so nobody has to sign in.
#>
$ErrorActionPreference = 'Stop'

$vol = Get-Volume -FileSystemLabel PAYLOAD -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $vol -or -not $vol.DriveLetter) { throw 'PAYLOAD volume not found (is the payload disk attached and online?).' }
$P = "$($vol.DriveLetter):"
$drv = "$P\driver"
if (-not (Test-Path "$drv\Ext2Fsd.sys")) { throw "No staged driver in $drv; run stage-payload.ps1 -Build ... on the host." }

bcdedit /set testsigning on | Out-Null
bcdedit /set '{current}' recoveryenabled No | Out-Null
bcdedit /set '{current}' bootstatuspolicy IgnoreAllFailures | Out-Null
powercfg /h off

foreach ($store in @('Root', 'TrustedPublisher')) {
    Import-Certificate -FilePath "$drv\ext4fsd-test.cer" -CertStoreLocation "Cert:\LocalMachine\$store" | Out-Null
}

$target = "$env:SystemRoot\System32\drivers\Ext2Fsd.sys"
# On a re-run the driver may be loaded: its image can be renamed but not overwritten.
if (Test-Path $target) { Move-Item $target "$target.old" -Force }
Copy-Item "$drv\Ext2Fsd.sys" $target -Force

$svc = 'HKLM:\SYSTEM\CurrentControlSet\Services\Ext2Fsd'
if (-not (Get-Service Ext2Fsd -ErrorAction SilentlyContinue)) {
    sc.exe create Ext2Fsd type= filesys start= system error= normal `
        binPath= 'System32\drivers\Ext2Fsd.sys' DisplayName= 'Ext2,Ext3,Ext4 filesystem driver' | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "sc create failed ($LASTEXITCODE)" }
}
New-Item -Path "$svc\Parameters" -Force | Out-Null
Set-ItemProperty "$svc\Parameters" -Name AutoMount -Value 1 -Type DWord
Set-ItemProperty "$svc\Parameters" -Name CheckingBitmap -Value 0 -Type DWord
Set-ItemProperty "$svc\Parameters" -Name Ext3ForceWriting -Value 1 -Type DWord
Set-ItemProperty "$svc\Parameters" -Name WritingSupport -Value 1 -Type DWord
Set-ItemProperty "$svc\Parameters" -Name CodePage -Value 'utf8' -Type String

$hash = (Get-FileHash $target -Algorithm SHA256).Hash
Set-Content "$P\driver-installed.txt" -Value ("{0} {1} {2}" -f (Get-Date -Format s), (Get-Content "$drv\label.txt"), $hash)

$cmd = '$v = Get-Volume -FileSystemLabel PAYLOAD | Select-Object -First 1; ' +
       '& ($v.DriveLetter + '':\tools\guest-autorun.ps1'')'
$action = New-ScheduledTaskAction -Execute 'powershell.exe' `
            -Argument ('-NoProfile -ExecutionPolicy Bypass -Command "' + $cmd + '"')
$trigger = New-ScheduledTaskTrigger -AtStartup
$settings = New-ScheduledTaskSettingsSet -ExecutionTimeLimit ([TimeSpan]::Zero) -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries
Register-ScheduledTask -TaskName 'Ext4Autorun' -Action $action -Trigger $trigger -Settings $settings `
    -User 'SYSTEM' -RunLevel Highest -Force | Out-Null

Write-Host 'Done. Reboot the VM (shutdown /r /t 0). The ext4 test disk mounts once it is formatted.' -ForegroundColor Green
