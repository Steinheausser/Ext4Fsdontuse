# ext4bench

User-mode filesystem benchmark for Ext4Fsd. One C file, Win32 API only.

## Build

```
tools\bench\build.cmd
```

This runs `vcvars64.bat` (VS 18 Community; override with the `VCVARS64` env var if
needed), then `cl /nologo /O2 /W4 /WX ext4bench.c`, then deletes the `.obj`. The result
is `tools\bench\ext4bench.exe`. `ext4bench --help` documents every option.

## Usage

```
ext4bench <test> --dir <path> [options]
```

`--dir` must be on a drive-letter volume (the tool derives `\\.\X:` from it) and must
exist. Volume operations (`flushvol`, `lockdismount`, cold `b5`) require an elevated
(Administrator) prompt. The tool checks that the volume can be opened before running
the workload, so it fails fast if it cannot.

Common options:

| Option | Meaning |
| --- | --- |
| `--label <text>` | Free text (e.g. driver build id) written to every result row |
| `--out <file.csv>` | Append rows to a CSV (header created if the file is missing/empty). A human summary is always printed too |
| `--durable none\|flushvol\|lockdismount` | Durability step after the timed workload (default `flushvol`) |
| `--cleanup` | Delete created files after measuring (not timed) |
| `--force` | b1: allow a non-empty dir. b3/b4/b6: delete leftover same-named files first |

Sizes accept `K`/`M`/`G` suffixes (powers of 1024). File contents are deterministic
(xorshift64 seeded by file/thread index). Any API failure prints the operation, path
and `GetLastError` and exits non-zero (1 = runtime failure, 2 = usage error).

### Timing model

* `T_app` = wall time (QueryPerformanceCounter) of the workload only. For b1/b2 the
  buffer fill (xorshift, well under 1 us per 4 KiB) is inside `T_app` but not inside
  the per-create latency.
* `T_durable` = `T_app` + time of the durability step, reported in ms.
* The CSV `rate` column is based on `T_durable` when a durable step ran, otherwise on
  `T_app`. The `notes` column always carries the unit, the basis, `rate_app` and (if
  measured) `rate_durable`.
* Test names in the CSV: `b1 b2 b3 b4 b5 prep b6_copy b6_rename`.

### Durable modes

* `flushvol` (default): open `\\.\X:` (GENERIC_READ|GENERIC_WRITE, share R/W),
  `FlushFileBuffers`, close.
* `lockdismount`: open `\\.\X:`, `FSCTL_LOCK_VOLUME` (up to 10 tries, 500 ms apart,
  to wait out handles that are still closing), `FSCTL_DISMOUNT_VOLUME`, close. If the
  lock never succeeds the tool prints a clear error and exits 1. The reported step
  time includes any retry sleeps; `lock_attempts` is recorded in `notes`. The tool has
  closed all its own file handles before this step. The system volume cannot be locked,
  so use a dedicated test volume.
* `none`: skip; only `T_app` is reported.

**Which mode to use for which driver build:** for the HEAD-baseline driver, use
`--durable lockdismount` for `T_durable`; for later builds, use `--durable flushvol`.
Keep the mode (and `--label`) identical when comparing runs, since the CSV records
`durable_mode` per row.

`prep` and `b5` never run a durable step (`b5` cold performs a lock+dismount *before*
timing, regardless of `--durable`).

## Tests

| Test | What it does |
| --- | --- |
| `b1` | Create `--count` (2000) files of `--size` (4096) bytes in an empty `--dir` (refuses a non-empty dir without `--force`). Per file: `CreateFileW(CREATE_NEW)`, `WriteFile`, `CloseHandle`; latency recorded per create. Reports total, files/s, p50/p90/p99/max, and one mean-latency line per 1000-file bucket (checks that per-create cost stays flat; works for 200k+ files, memory is one double per file). Notes include first/last bucket mean and their ratio |
| `prep` | Create `--count` (10000) empty files `p%08u`. Setup for B2/B5; also writes a summary row |
| `b2` | Same as b1, but `--dir` must already hold at least 10000 entries (`--min-existing N` overrides). New files are `n%08u`, so they never collide with `prep` files |
| `b3` | Sequential cached write of `--total` (1G) bytes in `--io` (1M) writes to one new file `b3.dat`. MB/s for `T_app` and `T_durable`. `--writethrough` adds `FILE_FLAG_WRITE_THROUGH` |
| `b4` | `--threads` (4) writers, each writing its own `--total` (1G) file `b4_NN.dat` with `--io` (1M) writes, released together by a manual-reset event. Create, write and close are inside each thread's timing. Reports aggregate MB/s (all bytes / wall time) and per-thread time (also in `notes`; `p50_ms`/`max_ms` columns hold median/max thread time) |
| `b5` | Enumerate `--dir` with `FindFirstFileExW(FindExInfoBasic, FindExSearchNameMatch, FIND_FIRST_EX_LARGE_FETCH)`. Cold by default: lock+dismount the volume first (untimed). `--warm` skips that. `--stat` also calls `GetFileAttributesExW` on each entry; the time spent inside it is reported as `stat_ms` |
| `b6` | `--iters` (200) iterations at `--size` (1M). Two rows: `b6_copy` (`CopyFileW(src,dst,FALSE)` over an existing dst) and `b6_rename` (write a fresh `b6_tmp.dat`, then `MoveFileExW(tmp, target, MOVEFILE_REPLACE_EXISTING)`, no write-through). Per-iteration mean and p99 (and p50/p90/max) are reported; each sub-workload gets its own durable step |

## Standard suite

Use a scratch directory on the volume under test (here `X:`). Every command carries
the same `--label`, `--out results.csv` and `--durable` mode. `--cleanup` removes files after each
measurement; omit it where a later test needs them (the prep directories).

Commands (from an elevated prompt, `X:\bench\...` directories created beforehand):

```
:: B1  create-heavy, empty directory
ext4bench b1 --dir X:\bench\b1 --count 2000 --label HEAD --out results.csv --durable lockdismount --cleanup
::   scaling check: per-bucket mean should stay flat
ext4bench b1 --dir X:\bench\b1 --count 200000 --label HEAD --out results.csv --durable lockdismount --cleanup

:: B2  create in a populated directory
ext4bench prep --dir X:\bench\b2 --count 10000
ext4bench b2   --dir X:\bench\b2 --count 2000 --label HEAD --out results.csv --durable lockdismount --cleanup

:: B3  sequential cached write (add --writethrough for the write-through variant)
ext4bench b3 --dir X:\bench\b3 --total 1G --label HEAD --out results.csv --durable lockdismount --cleanup

:: B4  4 concurrent writers
ext4bench b4 --dir X:\bench\b4 --threads 4 --total 1G --label HEAD --out results.csv --durable lockdismount --cleanup

:: B5  cold enumeration (10k and 100k entries; prep once, then measure)
ext4bench prep --dir X:\bench\b5_10k  --count 10000
ext4bench prep --dir X:\bench\b5_100k --count 100000
ext4bench b5 --dir X:\bench\b5_10k  --label HEAD --out results.csv
ext4bench b5 --dir X:\bench\b5_10k  --stat --label HEAD --out results.csv
ext4bench b5 --dir X:\bench\b5_100k --label HEAD --out results.csv
ext4bench b5 --dir X:\bench\b5_100k --stat --label HEAD --out results.csv

:: B6  overwrite-copy and replace-rename
ext4bench b6 --dir X:\bench\b6 --iters 200 --size 1M --label HEAD --out results.csv --durable lockdismount --cleanup
```

For later (non-baseline) builds, replace `--durable lockdismount` with
`--durable flushvol` (or just drop it, since it is the default) and change `--label`.

Notes:

* `b1` needs an empty directory each time; `--cleanup` takes care of that between runs.
* Cold `b5` locks and dismounts the volume, so nothing else may have files open on it.
  Because cold `b5` includes a remount, run it on a dedicated test volume.
* B2 and B5 are about directory size, so keep the `prep` directories out of other tests.
  `b2 --cleanup` deletes only its own `n%08u` files, not the `prep` files.
* If a durable step fails (for example the lock never succeeds) the tool exits non-zero
  without running `--cleanup`; delete the leftover files by hand or rerun with `--force`.

## Windows Defender

Run B1 and B2 twice: once with Defender real-time scanning active, and once with the
test directory excluded (`Add-MpPreference -ExclusionPath X:\bench`, run elevated; remove it
afterwards with `Remove-MpPreference -ExclusionPath X:\bench`). Use distinct `--label`
values (for example `HEAD-defender` and `HEAD-nodefender`) so the rows can be told apart.
Create-heavy workloads are the most sensitive to antivirus filter overhead, and the
difference is the cost that is not attributable to the driver.

## CSV format

```
timestamp_utc,label,test,params,count,bytes,t_app_ms,t_durable_ms,durable_mode,rate,p50_ms,p90_ms,p99_ms,max_ms,notes
```

`params` is a short `key=value;...` string. `t_durable_ms` and the latency columns are
empty when not applicable. `rate` units (files/s, MB/s or entries/s) are stated in
`notes` as `unit=...`.
