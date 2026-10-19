<#
.SYNOPSIS
    Builds Ext2Fsd.sys (x64 Release + Debug) with the Enterprise WDK and
    archives the .sys/.pdb under tools\build\out\<commit>\<config>\.

.EXAMPLE
    .\tools\build\build-driver.ps1 -EwdkRoot E:\
    .\tools\build\build-driver.ps1 -EwdkRoot E:\ -Configurations Release -Label baseline
#>
param(
    [Parameter(Mandatory = $true)] [string] $EwdkRoot,
    [string[]] $Configurations = @('Release', 'Debug'),
    [string] $Platform = 'x64',
    [string] $Label = '',
    # Overrides the project's WindowsTargetPlatformVersion (10.0.26100.0) when the
    # EWDK ships a different kit, e.g. 10.0.28000.0.
    [string] $TargetPlatformVersion = ''
)

$ErrorActionPreference = 'Stop'

$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
if (-not (Test-Path $EwdkRoot)) {
    throw "EWDK root $EwdkRoot not found (is the EWDK ISO mounted?)"
}
$setup = Join-Path $EwdkRoot 'BuildEnv\SetupBuildEnv.cmd'
if (-not (Test-Path $setup)) {
    throw "SetupBuildEnv.cmd not found under $EwdkRoot (is the EWDK ISO mounted?)"
}

$commit = (git -C $repo rev-parse --short HEAD).Trim()
$dirty = git -C $repo status --porcelain -- Ext4Fsd
if ($dirty) { $commit = "$commit-dirty" }
$tag = $commit
if ($Label) { $tag = "$commit-$Label" }

$proj = Join-Path $repo 'Ext4Fsd\Ext4Fsd.vcxproj'
$failed = @()

foreach ($cfg in $Configurations) {
    Write-Host "=== $cfg|$Platform ===" -ForegroundColor Cyan
    $log = Join-Path $repo "tools\build\out\$tag\$cfg\build.log"
    New-Item -ItemType Directory -Force (Split-Path $log) | Out-Null

    # The project's OutDir uses $(SolutionDir), so pass it when building the vcxproj alone.
    # Go through a .cmd file: PowerShell 5.1 mangles embedded quotes in native arguments.
    $tpv = ''
    if ($TargetPlatformVersion) { $tpv = " /p:WindowsTargetPlatformVersion=$TargetPlatformVersion" }
    $script = Join-Path (Split-Path $log) 'build.cmd'
    $msb = "msbuild `"$proj`" /nologo /m /t:Rebuild /p:Configuration=$cfg /p:Platform=$Platform$tpv " +
           "/p:SolutionDir=`"$repo\\`" `"/flp:logfile=$log;verbosity=normal`" /fl"
    @(
        '@echo off',
        "call `"$setup`" amd64 >nul",
        'if errorlevel 1 exit /b 1',
        $msb
    ) | Set-Content -Path $script -Encoding ASCII
    # SetupBuildEnv writes harmless noise to stderr (e.g. vswhere not found); judge by exit code only.
    $ErrorActionPreference = 'Continue'
    cmd /c "`"$script`" 2>&1"
    $ErrorActionPreference = 'Stop'
    if ($LASTEXITCODE -ne 0) {
        Write-Host "$cfg build FAILED (exit $LASTEXITCODE); see $log" -ForegroundColor Red
        $failed += $cfg
        continue
    }

    $bin = Join-Path $repo "Ext4Fsd\$cfg\$Platform"
    $dest = Split-Path $log
    foreach ($f in @('Ext2Fsd.sys', 'Ext2Fsd.pdb')) {
        $src = Join-Path $bin $f
        if (Test-Path $src) {
            Copy-Item $src $dest -Force
        } else {
            Write-Host "missing $src" -ForegroundColor Yellow
        }
    }
    Write-Host "$cfg -> $dest" -ForegroundColor Green
}

if ($failed.Count -gt 0) {
    throw "Failed configurations: $($failed -join ', ')"
}
