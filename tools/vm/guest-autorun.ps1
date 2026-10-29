# Runs at every guest boot as SYSTEM (task "Ext4Autorun", see guest-setup.ps1).
# Reads PAYLOAD:\mode.txt and acts on it. See stage-payload.ps1 for the modes.
$ErrorActionPreference = 'Stop'

$P = (Get-Volume -FileSystemLabel PAYLOAD | Select-Object -First 1).DriveLetter + ':'
$log = "$P\autorun-log.txt"
function Log([string]$m) { Add-Content -Path $log -Value ("{0} {1}" -f (Get-Date -Format s), $m) }

try {
    # 1. Install a newly staged driver, then reboot into it.
    $staged = "$P\driver\Ext2Fsd.sys"
    $target = "$env:SystemRoot\System32\drivers\Ext2Fsd.sys"
    if (Test-Path $staged) {
        $new = (Get-FileHash $staged -Algorithm SHA256).Hash
        $cur = ''
        if (Test-Path $target) { $cur = (Get-FileHash $target -Algorithm SHA256).Hash }
        if ($new -ne $cur) {
            # The loaded image can be renamed but not overwritten.
            if (Test-Path $target) { Move-Item $target "$target.old" -Force }
            Copy-Item $staged $target -Force
            Set-Content "$P\driver-installed.txt" -Value ("{0} {1} {2}" -f (Get-Date -Format s), (Get-Content "$P\driver\label.txt"), $new)
            Log "installed driver $new; rebooting"
            Restart-Computer -Force
            return
        }
    }

    $mode = ''
    if (Test-Path "$P\mode.txt") { $mode = (Get-Content "$P\mode.txt" -Raw).Trim() }
    $words = $mode -split '\s+'
    Log "boot, mode='$mode'"
    if (-not $mode -or $words[0] -eq 'none') { return }

    # 2. Wait for the ext4 volume (the driver reports EXT2/EXT3/EXT4 as the file system).
    $x = $null
    for ($i = 0; $i -lt 90 -and -not $x; $i++) {
        $x = Get-Volume | Where-Object { $_.FileSystem -match '^EXT' -and $_.DriveLetter } | Select-Object -First 1
        if (-not $x) { Start-Sleep -Seconds 2 }
    }
    if (-not $x) { Log 'no ext4 volume appeared within 180 s'; return }
    $X = "$($x.DriveLetter):"
    Log "ext4 volume $X ($($x.FileSystem))"

    if ($words -notcontains 'nodefender') { Add-MpPreference -ExclusionPath "$X\" }

    $tools = "$P\tools"
    $label = 'unknown'
    if (Test-Path "$P\driver\label.txt") { $label = (Get-Content "$P\driver\label.txt" -Raw).Trim() }
    if ($words -contains 'nodefender') { $label = "$label-defender" }

    switch ($words[0]) {
        'crash' {
            New-Item -ItemType Directory -Force 'C:\crashlog' | Out-Null
            $seed = Get-Random -Minimum 1 -Maximum 2147483647
            Log "crashgen seed $seed"
            & "$tools\crashgen.exe" --root "$X\crash" --log 'C:\crashlog\run.log' --seed $seed `
                --max-files 300 --max-size 2M --flush-prob 0.2 *> 'C:\crashlog\stdout.txt'
            Log "crashgen exited $LASTEXITCODE"
        }
        'bench' {
            $dur = 'flushvol'
            if ($words.Count -gt 1 -and $words[1] -in @('lockdismount', 'flushvol', 'none')) { $dur = $words[1] }
            $out = "$P\results.csv"
            $blog = "$P\bench-log.txt"
            $b = "$X\bench"
            if (Test-Path $b) { Remove-Item $b -Recurse -Force }
            foreach ($d in @('b1', 'b2', 'b3', 'b4', 'b5a', 'b5b', 'b6')) { New-Item -ItemType Directory -Force "$b\$d" | Out-Null }

            $common = @('--label', $label, '--out', $out)
            $runs = @(
                @('b1', '--dir', "$b\b1", '--durable', $dur),
                @('prep', '--dir', "$b\b2", '--count', '10000'),
                @('b2', '--dir', "$b\b2", '--durable', $dur),
                @('b3', '--dir', "$b\b3", '--durable', $dur, '--cleanup'),
                @('b4', '--dir', "$b\b4", '--durable', $dur, '--cleanup'),
                @('prep', '--dir', "$b\b5a", '--count', '10000'),
                @('prep', '--dir', "$b\b5b", '--count', '100000'),
                @('b5', '--dir', "$b\b5a"),
                @('b5', '--dir', "$b\b5b"),
                @('b5', '--dir', "$b\b5b", '--stat'),
                @('b6', '--dir', "$b\b6", '--durable', $dur)
            )
            foreach ($r in $runs) {
                $a = $r + $common
                Add-Content $blog ("`n=== ext4bench " + ($a -join ' '))
                & "$tools\ext4bench.exe" @a *>> $blog
                if ($LASTEXITCODE -ne 0) { Log "ext4bench $($r[0]) failed ($LASTEXITCODE)" }
            }
            Set-Content "$P\mode.txt" -Value 'none' -Encoding ASCII
            Log 'bench done; shutting down'
            Stop-Computer -Force
        }
        default { Log "unknown mode '$mode'" }
    }
} catch {
    Log ("ERROR: " + $_.Exception.Message)
}
