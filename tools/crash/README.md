# Crash-consistency test tooling

Hard-reset a Hyper-V guest while a random file workload runs on an ext4
volume served by the Ext4Fsd driver, then check the disk on the host with
`e2fsck` and verify that every file the workload *flushed* survived intact.

```
guest (Hyper-V Gen2, Windows 11)             host (admin PowerShell + WSL2)
--------------------------------             ------------------------------
crashgen.exe  -> random ops on E:\crash      crash-loop.ps1
              -> log C:\crashlog\run.log       Start-VM, wait, Stop-VM -TurnOff
                 (write-through, flushed       mount guest OS VHDX read-only,
                 after every line)             copy run.log
                                               wsl --mount test disk
                                               e2fsck -fn -> verify.py -> e2fsck -fp -> e2fsck -fn
```

Files:

| file | role |
|------|------|
| `crashgen.c`, `build.cmd` | guest generator (Win32, BCrypt SHA-256) |
| `start-crashgen.cmd` | guest launcher: picks a seed from `%RANDOM%` |
| `verify.py` | runs in WSL (or anywhere): checks flushed files against the log |
| `crash-loop.ps1` | host orchestration loop |

## Prerequisites

Host:
- Windows with Hyper-V, an **elevated** PowerShell (Windows PowerShell 5.1 is fine).
- WSL2 with a distro that has `e2fsprogs` (e2fsck) and `python3`
  (`sudo apt install e2fsprogs python3`). The script runs WSL commands as root
  (`wsl -u root`). `wsl --mount` needs a WSL version that supports it (WSL 2).
- The guest OS disk is a VHDX on the host (the script mounts it read-only while
  the VM is off). The ext4 test disk is either a VHDX or a physical disk
  (pass-through). A physical disk must be offline on the host; the script sets
  it offline and (re)attaches it to the VM as needed.
- Nothing else may hold the test disk (WSL, a running VM, a Windows mount).

Guest:
- Test-signing on (`bcdedit /set testsigning on`), the Ext4Fsd driver installed
  and the ext4 volume mounted with a stable drive letter (examples use `E:`).
  The ext4 filesystem should be freshly `mkfs.ext4`'d (whole disk or a
  partition; `crash-loop.ps1` handles both) before the first run.
- Automatic logon for the user that owns the scheduled task (netplwiz or the
  Sysinternals `Autologon` tool), so the task fires after each reset.
- `crashgen.exe` and `start-crashgen.cmd` copied to `C:\crash\` (build with
  `build.cmd` on any machine with Visual Studio; the exe is self-contained).
- A scheduled task that runs the launcher at logon (elevated command prompt in
  the guest):

```
mkdir C:\crash C:\crashlog
schtasks /create /tn crashgen /sc onlogon /rl highest /ru <GuestUser> /tr "C:\crash\start-crashgen.cmd" /f
```

  (`/ru SYSTEM /sc onstart` also works if you do not want autologon; the launcher
  waits up to ~2 minutes for the ext4 volume to appear.)

`start-crashgen.cmd`:

```bat
@echo off
setlocal
set VOL=E:
set ROOT=%VOL%\crash
set LOGDIR=C:\crashlog
if not exist %LOGDIR% mkdir %LOGDIR%
set /a TRIES=0
:wait
if exist %VOL%\ goto ready
set /a TRIES+=1
if %TRIES% GEQ 60 goto ready
ping -n 3 127.0.0.1 >nul
goto wait
:ready
set /a SEED=%RANDOM%*32768+%RANDOM%
echo %date% %time% seed=%SEED% >> %LOGDIR%\launcher.txt
C:\crash\crashgen.exe --root %ROOT% --log %LOGDIR%\run.log --seed %SEED% --max-files 300 --max-size 2M --flush-prob 0.2 >> %LOGDIR%\stdout.txt 2>&1
```

The shipped `start-crashgen.cmd` is equivalent (with comments). On each start crashgen rotates
the previous log to `run.log.prev`, **wipes everything under `--root`**, and
starts a fresh log. The host verifies each iteration before the guest boots
again, so this is safe. The guest log lives on the guest OS disk (NTFS), not
on the disk under test.

### Build

```
cmd /c "call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" && cd /d tools\crash && cl /nologo /O2 /W4 /WX crashgen.c bcrypt.lib"
```
or just run `tools\crash\build.cmd`, which does the same and deletes `crashgen.obj`.

## crashgen

```
crashgen --root E:\crash --log C:\crashlog\run.log --seed N
         [--max-files 300] [--max-size 2M] [--flush-prob 0.2] [--ops 0]
```

Random operations on a set of live files (each with a stable id, content
derived from `(seed, id, version)` and kept in memory so the expected hash is
always known): create, overwrite-at-offset, append, truncate (smaller or to
0), truncate-and-rewrite (`CREATE_ALWAYS`), rename (same dir or across
`d1/d2/d3`), rename-over-another-live-file (`MOVEFILE_REPLACE_EXISTING`; the
replaced id dies), delete, plus occasional mkdir/rmdir of `d1..d3`. After an
op, with probability `--flush-prob`, the file is opened, `FlushFileBuffers`'d
and closed. Memory use is bounded by `max-files x max-size` (defaults: 600 MiB
worst case, far less in practice).

Log lines (tab separated, each written through and flushed before the next op):
`HEADER`, `OP seq id kind path [newpath]` (before the op), `DONE seq`,
`FLUSHED seq id path size sha256`, `DEAD seq id`, `ERR ...`, `END seq`.
A replace-rename logs `OP ... REPLACE old new` for the moving id and
`OP ... VICTIM new` for the id that is overwritten. `<logdir>\alive` is touched
every 5 s.

## verify.py

```
python3 verify.py --log run.log --mnt /mnt/crashtest [--subdir crash] [--all]
```

For each id the last `FLUSHED` record is taken. The id is checked only if no
later `OP` touched its content (WRITE, APPEND, TRUNC, REWRITE, VICTIM, DELETE;
an `OP` without `DONE` counts) and it is not `DEAD`. A later plain
`RENAME`/`REPLACE` of the file only adds acceptable paths: it may be at the
flushed path or any later path of its rename chain. Existence, size and
sha256 are compared. `--subdir` defaults to the `root=` of the log header with
the drive letter stripped (`E:\crash` -> `crash`). Prints JSON
`{checked, ok, missing, mismatched, skipped, details[], ...}`; exit 0 only if
all checked files are ok (1 = data loss, 2 = usage/IO error).

## Running

From an elevated PowerShell on the host (VM off, test disk not mounted anywhere):

```powershell
cd tools\crash

# VHDX test disk
.\crash-loop.ps1 -VMName ext4-test -GuestOsVhd D:\vm\ext4-test.vhdx `
    -TestDiskVhd D:\vm\ext4-data.vhdx -Iterations 20 -Label HEAD

# physical test disk
.\crash-loop.ps1 -VMName ext4-test -GuestOsVhd D:\vm\ext4-test.vhdx `
    -TestDiskNumber 3 -Iterations 20 -Label my-fix

# print the command sequence for one iteration without touching anything
.\crash-loop.ps1 -VMName x -GuestOsVhd x.vhdx -TestDiskVhd y.vhdx -DryRun
```

Useful parameters: `-MinRunSec/-MaxRunSec` (random run time after the guest
heartbeat is OK, default 20..120; make `-MinRunSec` larger than your logon time
or crashgen may not have started and the iteration is meaningless -- the script
warns when the copied log predates the boot), `-BootTimeoutSec 300`,
`-GuestLogPath crashlog\run.log` (relative to the guest OS volume root),
`-ResultsDir` (default `tools\crash\results\<timestamp>`), `-Label`.

Per iteration the results directory `iterNNN\` holds `run.log`, `fsck_n.txt`,
`verify.json`, `fsck_p.txt`, `fsck_n2.txt` and the WSL command outputs
(`*.err` files carry stderr); `summary.csv` gets one row:
`iter,label,run_sec,fsck_n_exit,fsck_p_exit,fsck_n2_exit,verify_ok,checked,missing,mismatched,note`.

Notes on behaviour:
- `e2fsck` is run as `-fn` (read-only, forced) before verification, then
  `-fp` (forced preen; the plain `-p` named in the design would skip a
  volume not flagged dirty), then `-fn` again.
- The verification mount is `mount -o ro,noload`, so the journal is **not**
  replayed: it checks what is on disk right after the crash. (A journal-replay
  variant would simply be the same verification after `e2fsck -fp`.)
- A failing iteration does not stop the loop. The loop stops if
  `fsck_n2_exit != 0` (disk still not clean after `-fp`) -- the disk is left
  unmounted for inspection (`wsl --mount ...` again to look) -- or after three
  consecutive infrastructure errors.
- The guest log is read by mounting the guest OS VHDX read-only on the host
  while the VM is off (NTFS journal is recovered in memory; the log is written
  write-through and flushed after each line, so it is complete up to the
  crash).

## Pass criteria

- `fsck_n_exit` may be non-zero only with "not clean"-class findings that
  `e2fsck -fp` repairs (journal needing recovery, orphan inode list, free
  block/inode counts, group descriptor checksum/flag fixes). Exit 0/1 from
  `-fp` = repaired; `-fp` exit >= 4 = needs manual repair = **fail**.
- `fsck_n2_exit` must be 0 in every iteration.
- `verify_ok = true` in every iteration: every flushed file present, right size,
  right sha256 (`missing = 0`, `mismatched = 0`).
- `HEAD` baseline driver: it never marks the volume not-clean, so `e2fsck`
  without `-f` would skip checking and a crash is silently invisible to the
  flag-based "not clean" path. Judge the baseline by **data loss only**
  (`missing`/`mismatched` in `verify.json`) and by what `fsck_n` finds with
  the forced check; do not expect a "needs recovery" indication from it.

## Not tested on the authoring machine

`crash-loop.ps1` was only parse-checked and run with `-DryRun` (no Hyper-V or
WSL there); the `wsl.exe` invocations (stdin redirect of `verify.py` with
`python3 -`, `wsl --mount --bare` device detection via `lsblk` diff, physical
disk offline/attach) and the Mount-VHD drive-letter lookup are written from
documentation and need a first real run to shake out.
