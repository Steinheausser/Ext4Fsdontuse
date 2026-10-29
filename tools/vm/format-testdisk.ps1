<#
.SYNOPSIS
    Formats the VM's ext4 test VHDX from WSL with a pinned feature set. Host side,
    elevated, VM off.

.DESCRIPTION
    -FromVolume E:   reads the superblock of an existing ext4 volume (read-only, raw
                     read of \\.\E:) and copies its features, block size, inode size,
                     flex_bg size and hash settings. Use the real 16 TB volume here.
    -Features ...    explicit mke2fs -O list instead.
    -UnsignedHash    marks the result unsigned_directory_hash (for P1-0b tests).
    -ShowOnly        just print what -FromVolume found.

.EXAMPLE
    .\tools\vm\format-testdisk.ps1 -FromVolume E: -ShowOnly
    .\tools\vm\format-testdisk.ps1 -FromVolume E:
#>
param(
    [string] $VMName = 'ext4test',
    [string] $Vhd = 'D:\VMs\ext4test\ext4test-ext4.vhdx',
    [string] $FromVolume = '',
    [string] $Features = '',
    [string] $Label = 'ext4test',
    [switch] $UnsignedHash,
    [switch] $ShowOnly
)

$ErrorActionPreference = 'Stop'

$id = [Security.Principal.WindowsIdentity]::GetCurrent()
if (-not (New-Object Security.Principal.WindowsPrincipal($id)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Run this from an elevated (Administrator) PowerShell.'
}

Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
public static class RawVol {
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern SafeFileHandle CreateFileW(string name, uint access, uint share, IntPtr sa,
                                             uint disposition, uint flags, IntPtr template);
    public static byte[] ReadSuper(string vol) {
        // GENERIC_READ, share read|write, OPEN_EXISTING. Read-only access.
        SafeFileHandle h = CreateFileW(@"\\.\" + vol, 0x80000000, 3, IntPtr.Zero, 3, 0, IntPtr.Zero);
        if (h.IsInvalid) throw new IOException("CreateFile " + vol + " failed: " + Marshal.GetLastWin32Error());
        using (FileStream fs = new FileStream(h, FileAccess.Read)) {
            byte[] buf = new byte[4096];
            int n = 0;
            while (n < buf.Length) { int r = fs.Read(buf, n, buf.Length - n); if (r <= 0) break; n += r; }
            byte[] sb = new byte[1024];
            Array.Copy(buf, 1024, sb, 0, 1024);
            return sb;
        }
    }
}
'@

function Get-SuperInfo([string]$vol) {
    $sb = [RawVol]::ReadSuper($vol.TrimEnd('\').TrimEnd(':') + ':')
    if ([BitConverter]::ToUInt16($sb, 0x38) -ne 0xEF53) { throw "$vol does not have an ext2/3/4 superblock" }
    $u32 = { param($o) [BitConverter]::ToUInt32($sb, $o) }
    $compat = & $u32 0x5C; $incompat = & $u32 0x60; $ro = & $u32 0x64
    $names = @()
    $map = @(
        @($compat, 0x4, 'has_journal'), @($compat, 0x8, 'ext_attr'), @($compat, 0x10, 'resize_inode'),
        @($compat, 0x20, 'dir_index'), @($compat, 0x200, 'sparse_super2'), @($compat, 0x400, 'fast_commit'),
        @($compat, 0x1000, 'orphan_file'),
        @($incompat, 0x2, 'filetype'), @($incompat, 0x10, 'meta_bg'), @($incompat, 0x40, 'extent'),
        @($incompat, 0x80, '64bit'), @($incompat, 0x100, 'mmp'), @($incompat, 0x200, 'flex_bg'),
        @($incompat, 0x400, 'ea_inode'), @($incompat, 0x2000, 'metadata_csum_seed'),
        @($incompat, 0x4000, 'large_dir'), @($incompat, 0x8000, 'inline_data'),
        @($incompat, 0x10000, 'encrypt'), @($incompat, 0x20000, 'casefold'),
        @($ro, 0x1, 'sparse_super'), @($ro, 0x2, 'large_file'), @($ro, 0x8, 'huge_file'),
        @($ro, 0x10, 'uninit_bg'), @($ro, 0x20, 'dir_nlink'), @($ro, 0x40, 'extra_isize'),
        @($ro, 0x100, 'quota'), @($ro, 0x200, 'bigalloc'), @($ro, 0x400, 'metadata_csum'),
        @($ro, 0x2000, 'project'), @($ro, 0x8000, 'verity')
    )
    foreach ($m in $map) { if ($m[0] -band $m[1]) { $names += $m[2] } }
    $hashNames = @('legacy', 'half_md4', 'tea')
    $flags = & $u32 0x160
    [pscustomobject]@{
        Features      = ($names -join ',')
        BlockSize     = 1024 -shl (& $u32 0x18)
        InodeSize     = [BitConverter]::ToUInt16($sb, 0x58)
        FlexBgSize    = 1 -shl $sb[0x174]
        HashAlg       = $hashNames[$sb[0xFC]]
        UnsignedHash  = [bool]($flags -band 2)
        BlocksCount   = [uint64](& $u32 0x04) + ([uint64](& $u32 0x150) -shl 32)
        UnknownBits   = ('compat 0x{0:X} incompat 0x{1:X} ro 0x{2:X}' -f $compat, $incompat, $ro)
    }
}

$info = $null
if ($FromVolume) {
    $info = Get-SuperInfo $FromVolume
    $info | Format-List
    if ($ShowOnly) { return }
    $Features = $info.Features
}
if ($ShowOnly) { throw '-ShowOnly needs -FromVolume' }
if (-not $Features) { throw 'Give -FromVolume E: or -Features <list>.' }

$vm = Get-VM -Name $VMName
if ($vm.State -ne 'Off') { throw "$VMName must be off." }

$mk = "mke2fs -F -t ext4 -L $Label -O none,$Features"
if ($info) {
    $mk += " -b $($info.BlockSize) -I $($info.InodeSize) -E hash_alg=$($info.HashAlg)"
    if ($Features -match 'flex_bg') { $mk += " -G $($info.FlexBgSize)" }
}

$before = wsl.exe -u root -e lsblk -dno NAME
wsl.exe --mount --vhd $Vhd --bare
if ($LASTEXITCODE -ne 0) { throw "wsl --mount failed ($LASTEXITCODE)" }
try {
    $after = wsl.exe -u root -e lsblk -dno NAME
    $new = @($after | Where-Object { $before -notcontains $_ })
    if ($new.Count -ne 1) { throw "could not identify the new WSL disk (got: $($new -join ', '))" }
    $dev = "/dev/$($new[0].Trim())"
    Write-Host "Formatting $dev : $mk" -ForegroundColor Cyan
    wsl.exe -u root -e sh -c "$mk $dev"
    if ($LASTEXITCODE -ne 0) { throw "mke2fs failed ($LASTEXITCODE)" }
    if ($UnsignedHash) {
        wsl.exe -u root -e debugfs -w -R 'ssv flags 2' $dev
        if ($LASTEXITCODE -ne 0) { throw "debugfs failed ($LASTEXITCODE)" }
    }
    wsl.exe -u root -e sh -c "dumpe2fs -h $dev 2>/dev/null | grep -E 'features|flags|hash|Block size|Inode size|Flex'"
} finally {
    wsl.exe --unmount $Vhd | Out-Null
}
Write-Host 'Done. The guest mounts it at next boot.' -ForegroundColor Green
