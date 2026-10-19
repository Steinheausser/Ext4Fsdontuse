/*
 * ext4bench - user-mode filesystem benchmark for Ext4Fsd (Win32 API only).
 *
 * Build:  tools\bench\build.cmd      (cl /nologo /O2 /W4 /WX ext4bench.c)
 * Usage:  ext4bench --help
 *
 * Tests: b1 b2 b3 b4 b5 b6 prep.  See README.md for the standard suite.
 */
#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>
#include <errno.h>

#define UNSET        (~0ULL)
#define BUCKET_FILES 1000ULL
#define MAX_THREADS  64
#define PATH_CAP     1024
#define MAX_IO       0x40000000UL   /* 1 GiB: single WriteFile cap */

typedef enum { DUR_NONE = 0, DUR_FLUSHVOL, DUR_LOCKDISMOUNT } DurMode;

typedef struct Opts {
    const wchar_t *test;
    wchar_t dir[PATH_CAP];
    const wchar_t *label;
    const wchar_t *out;
    DurMode dur;
    unsigned long long count, size, total, io, threads, iters, min_existing;
    BOOL force, cleanup, writethrough, warm, stat;
} Opts;

static Opts g;
static LONGLONG g_freq;

/* ------------------------------------------------------------------ */
/* output helpers                                                      */
/* ------------------------------------------------------------------ */

static void write_out(DWORD which, const wchar_t *s)
{
    HANDLE h = GetStdHandle(which);
    DWORD mode, n;
    int len = (int)wcslen(s);
    if (h == INVALID_HANDLE_VALUE || h == NULL || len == 0)
        return;
    if (GetConsoleMode(h, &mode)) {
        WriteConsoleW(h, s, (DWORD)len, &n, NULL);
    } else {
        int bytes = WideCharToMultiByte(CP_UTF8, 0, s, len, NULL, 0, NULL, NULL);
        char *b = (char *)malloc((size_t)bytes + 1);
        if (b == NULL)
            return;
        WideCharToMultiByte(CP_UTF8, 0, s, len, b, bytes, NULL, NULL);
        WriteFile(h, b, (DWORD)bytes, &n, NULL);
        free(b);
    }
}

static void say(const wchar_t *fmt, ...)
{
    wchar_t buf[4096];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    write_out(STD_OUTPUT_HANDLE, buf);
}

static void esay(const wchar_t *fmt, ...)
{
    wchar_t buf[4096];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    write_out(STD_ERROR_HANDLE, buf);
}

static void appendf(wchar_t *dst, size_t cap, const wchar_t *fmt, ...)
{
    size_t len = wcslen(dst);
    va_list ap;
    if (len >= cap - 1)
        return;
    va_start(ap, fmt);
    _vsnwprintf_s(dst + len, cap - len, _TRUNCATE, fmt, ap);
    va_end(ap);
}

__declspec(noreturn) static void fail_with(const wchar_t *op, const wchar_t *path, DWORD err)
{
    wchar_t msg[512] = L"";
    size_t n;
    FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   NULL, err, 0, msg, _countof(msg), NULL);
    n = wcslen(msg);
    while (n > 0 && (msg[n - 1] == L'\r' || msg[n - 1] == L'\n' || msg[n - 1] == L' '))
        msg[--n] = L'\0';
    esay(L"ERROR: %ls failed: path=\"%ls\" GetLastError=%lu (%ls)\n",
         op, path ? path : L"", (unsigned long)err, msg);
    exit(1);
}

#define fail(op, path) fail_with((op), (path), GetLastError())

__declspec(noreturn) static void die(const wchar_t *fmt, ...)
{
    wchar_t buf[2048];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    esay(L"ERROR: %ls\n", buf);
    exit(1);
}

__declspec(noreturn) static void usage_error(const wchar_t *fmt, ...)
{
    wchar_t buf[2048];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    esay(L"ERROR: %ls\nTry: ext4bench --help\n", buf);
    exit(2);
}

/* ------------------------------------------------------------------ */
/* timing / data helpers                                               */
/* ------------------------------------------------------------------ */

static void init_timer(void)
{
    LARGE_INTEGER f;
    if (!QueryPerformanceFrequency(&f) || f.QuadPart <= 0)
        fail(L"QueryPerformanceFrequency", L"");
    g_freq = f.QuadPart;
}

static LONGLONG qpc(void)
{
    LARGE_INTEGER c;
    if (!QueryPerformanceCounter(&c))
        fail(L"QueryPerformanceCounter", L"");
    return c.QuadPart;
}

static double ticks_ms(LONGLONG a, LONGLONG b)
{
    return (double)(b - a) * 1000.0 / (double)g_freq;
}

/* Deterministic xorshift64 fill, seeded by the file / thread index. */
static void fill_buf(BYTE *buf, size_t n, unsigned long long seed)
{
    unsigned long long x = seed * 0x9E3779B97F4A7C15ULL + 0x2545F4914F6CDD1DULL;
    size_t i = 0;
    if (x == 0)
        x = 0x1234567ULL;
    while (i + 8 <= n) {
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        memcpy(buf + i, &x, 8);
        i += 8;
    }
    if (i < n) {
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        memcpy(buf + i, &x, n - i);
    }
}

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static double pct_sorted(const double *s, size_t n, double p)
{
    size_t idx;
    if (n == 0)
        return -1.0;
    idx = (size_t)(p * (double)n + 0.999999999);   /* nearest rank */
    if (idx < 1) idx = 1;
    if (idx > n) idx = n;
    return s[idx - 1];
}

static void join(wchar_t *out, size_t cap, const wchar_t *dir, const wchar_t *name)
{
    _snwprintf_s(out, cap, _TRUNCATE, L"%ls\\%ls", dir, name);
}

static void numbered_path(wchar_t *out, size_t cap, wchar_t prefix, unsigned long long i)
{
    _snwprintf_s(out, cap, _TRUNCATE, L"%ls\\%lc%08u", g.dir, (unsigned)prefix, (unsigned)i);
}

static void prep_target(const wchar_t *path)
{
    if (g.force && !DeleteFileW(path)) {
        DWORD e = GetLastError();
        if (e != ERROR_FILE_NOT_FOUND)
            fail_with(L"DeleteFileW (--force)", path, e);
    }
}

static void delete_file_checked(const wchar_t *path)
{
    if (!DeleteFileW(path))
        fail(L"DeleteFileW", path);
}

static void require_dir(void)
{
    DWORD a = GetFileAttributesW(g.dir);
    if (a == INVALID_FILE_ATTRIBUTES)
        fail(L"GetFileAttributesW (--dir must exist)", g.dir);
    if (!(a & FILE_ATTRIBUTE_DIRECTORY))
        die(L"--dir is not a directory: %ls", g.dir);
}

/* Count entries (excluding . and ..); stops early once stop_after reached (0 = no limit). */
static unsigned long long count_entries(unsigned long long stop_after)
{
    wchar_t pat[PATH_CAP];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    unsigned long long n = 0;

    join(pat, _countof(pat), g.dir, L"*");
    h = FindFirstFileExW(pat, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        if (e == ERROR_FILE_NOT_FOUND)
            return 0;
        fail_with(L"FindFirstFileExW", pat, e);
    }
    do {
        if (fd.cFileName[0] == L'.' &&
            (fd.cFileName[1] == L'\0' || (fd.cFileName[1] == L'.' && fd.cFileName[2] == L'\0')))
            continue;
        n++;
        if (stop_after != 0 && n >= stop_after)
            break;
    } while (FindNextFileW(h, &fd));
    if (stop_after == 0 || n < stop_after) {
        DWORD e = GetLastError();
        if (e != ERROR_NO_MORE_FILES) {
            FindClose(h);
            fail_with(L"FindNextFileW", pat, e);
        }
    }
    FindClose(h);
    return n;
}

/* ------------------------------------------------------------------ */
/* durability                                                          */
/* ------------------------------------------------------------------ */

static int g_lock_attempts;

static void volume_path(wchar_t *vol, size_t cap)
{
    wchar_t root[PATH_CAP];
    if (!GetVolumePathNameW(g.dir, root, _countof(root)))
        fail(L"GetVolumePathNameW", g.dir);
    if (!iswalpha(root[0]) || root[1] != L':')
        die(L"cannot derive a drive letter from volume root \"%ls\" (dir=%ls); use a drive-letter path", root, g.dir);
    _snwprintf_s(vol, cap, _TRUNCATE, L"\\\\.\\%lc:", (unsigned)towupper(root[0]));
}

static HANDLE open_volume(const wchar_t *vol)
{
    HANDLE h = CreateFileW(vol, GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        if (e == ERROR_ACCESS_DENIED)
            esay(L"NOTE: opening a volume for write needs an elevated (Administrator) prompt.\n");
        fail_with(L"CreateFileW(volume)", vol, e);
    }
    return h;
}

/* Returns the elapsed time of the step in ms. Fatal on error. */
static double volume_flush(void)
{
    wchar_t vol[16];
    HANDLE h;
    LONGLONG t0, t1;
    volume_path(vol, _countof(vol));
    t0 = qpc();
    h = open_volume(vol);
    if (!FlushFileBuffers(h)) {
        DWORD e = GetLastError();
        CloseHandle(h);
        fail_with(L"FlushFileBuffers(volume)", vol, e);
    }
    CloseHandle(h);
    t1 = qpc();
    return ticks_ms(t0, t1);
}

static double volume_lockdismount(void)
{
    wchar_t vol[16];
    HANDLE h;
    LONGLONG t0, t1;
    DWORD br = 0, lasterr = 0;
    int attempt;
    BOOL locked = FALSE;

    volume_path(vol, _countof(vol));
    t0 = qpc();
    h = open_volume(vol);
    for (attempt = 1; attempt <= 10; attempt++) {
        g_lock_attempts = attempt;
        if (DeviceIoControl(h, FSCTL_LOCK_VOLUME, NULL, 0, NULL, 0, &br, NULL)) {
            locked = TRUE;
            break;
        }
        lasterr = GetLastError();
        esay(L"  lock attempt %d/10 on %ls failed (GetLastError=%lu)%ls\n", attempt, vol,
             (unsigned long)lasterr, attempt < 10 ? L", retrying in 500 ms" : L"");
        if (attempt < 10)
            Sleep(500);
    }
    if (!locked) {
        CloseHandle(h);
        esay(L"ERROR: FSCTL_LOCK_VOLUME on %ls never succeeded after 10 attempts "
             L"(last GetLastError=%lu). Something still has open handles on the volume "
             L"(the system volume can never be locked). Use --durable flushvol or none.\n",
             vol, (unsigned long)lasterr);
        exit(1);
    }
    if (!DeviceIoControl(h, FSCTL_DISMOUNT_VOLUME, NULL, 0, NULL, 0, &br, NULL)) {
        DWORD e = GetLastError();
        CloseHandle(h);
        fail_with(L"FSCTL_DISMOUNT_VOLUME", vol, e);
    }
    CloseHandle(h);   /* releases the lock; the volume remounts on next access */
    t1 = qpc();
    return ticks_ms(t0, t1);
}

/* Fail fast (before the workload) if the volume cannot be opened for the durable step. */
static void volume_preflight(void)
{
    wchar_t vol[16];
    HANDLE h;
    volume_path(vol, _countof(vol));
    h = open_volume(vol);
    CloseHandle(h);
}

static const wchar_t *dur_name(DurMode m)
{
    return m == DUR_FLUSHVOL ? L"flushvol" : m == DUR_LOCKDISMOUNT ? L"lockdismount" : L"none";
}

/* ------------------------------------------------------------------ */
/* result reporting                                                    */
/* ------------------------------------------------------------------ */

typedef struct Result {
    const wchar_t *test;
    wchar_t params[256];
    unsigned long long count, bytes;
    double t_app;          /* ms */
    double t_dur;          /* ms, <0 = not measured */
    double work;           /* numerator for rate: files, MB or entries */
    const wchar_t *unit;   /* files/s, MB/s, entries/s */
    double p50, p90, p99, max;   /* <0 = n/a */
    wchar_t notes[512];
    const wchar_t *dur_mode;
} Result;

static void result_init(Result *r, const wchar_t *test)
{
    memset(r, 0, sizeof(*r));
    r->test = test;
    r->t_dur = -1.0;
    r->p50 = r->p90 = r->p99 = r->max = -1.0;
    r->dur_mode = L"none";
}

static void apply_durable(Result *r)
{
    double step;
    if (g.dur == DUR_NONE)
        return;
    step = (g.dur == DUR_FLUSHVOL) ? volume_flush() : volume_lockdismount();
    r->t_dur = r->t_app + step;
    r->dur_mode = dur_name(g.dur);
    say(L"  durability step (%ls): %.3f ms\n", r->dur_mode, step);
    if (g.dur == DUR_LOCKDISMOUNT)
        appendf(r->notes, _countof(r->notes), L"lock_attempts=%d;", g_lock_attempts);
}

static void csv_field(wchar_t *dst, size_t cap, const wchar_t *s)
{
    if (wcspbrk(s, L",\"\r\n") == NULL) {
        appendf(dst, cap, L"%ls", s);
    } else {
        wchar_t tmp[2048];
        size_t j = 0;
        tmp[j++] = L'"';
        for (; *s && j + 3 < _countof(tmp); s++) {
            if (*s == L'"')
                tmp[j++] = L'"';
            tmp[j++] = *s;
        }
        tmp[j++] = L'"';
        tmp[j] = L'\0';
        appendf(dst, cap, L"%ls", tmp);
    }
}

static void opt_num(wchar_t *dst, size_t cap, double v)
{
    if (v >= 0.0)
        appendf(dst, cap, L"%.4f", v);
}

static void write_csv(const Result *r, double rate, const wchar_t *notes)
{
    static const char header[] =
        "timestamp_utc,label,test,params,count,bytes,t_app_ms,t_durable_ms,durable_mode,"
        "rate,p50_ms,p90_ms,p99_ms,max_ms,notes\r\n";
    wchar_t row[4096];
    SYSTEMTIME st;
    HANDLE h;
    LARGE_INTEGER sz;
    int len, bytes;
    char *buf;
    size_t hlen = sizeof(header) - 1;
    DWORD n;

    GetSystemTime(&st);
    row[0] = L'\0';
    appendf(row, _countof(row), L"%04u-%02u-%02uT%02u:%02u:%02uZ,", st.wYear, st.wMonth, st.wDay,
            st.wHour, st.wMinute, st.wSecond);
    csv_field(row, _countof(row), g.label ? g.label : L"");
    appendf(row, _countof(row), L",%ls,", r->test);
    csv_field(row, _countof(row), r->params);
    appendf(row, _countof(row), L",%llu,%llu,", r->count, r->bytes);
    opt_num(row, _countof(row), r->t_app);
    appendf(row, _countof(row), L",");
    opt_num(row, _countof(row), r->t_dur);
    appendf(row, _countof(row), L",%ls,", r->dur_mode);
    opt_num(row, _countof(row), rate);
    appendf(row, _countof(row), L",");
    opt_num(row, _countof(row), r->p50);
    appendf(row, _countof(row), L",");
    opt_num(row, _countof(row), r->p90);
    appendf(row, _countof(row), L",");
    opt_num(row, _countof(row), r->p99);
    appendf(row, _countof(row), L",");
    opt_num(row, _countof(row), r->max);
    appendf(row, _countof(row), L",");
    csv_field(row, _countof(row), notes);
    appendf(row, _countof(row), L"\r\n");

    h = CreateFileW(g.out, FILE_APPEND_DATA | FILE_READ_ATTRIBUTES, FILE_SHARE_READ, NULL,
                    OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        fail(L"CreateFileW (--out)", g.out);
    if (!GetFileSizeEx(h, &sz)) {
        DWORD e = GetLastError();
        CloseHandle(h);
        fail_with(L"GetFileSizeEx (--out)", g.out, e);
    }
    len = (int)wcslen(row);
    bytes = WideCharToMultiByte(CP_UTF8, 0, row, len, NULL, 0, NULL, NULL);
    buf = (char *)malloc(hlen + (size_t)bytes + 1);
    if (buf == NULL)
        die(L"out of memory");
    n = 0;
    if (sz.QuadPart == 0) {
        memcpy(buf, header, hlen);
        n = (DWORD)hlen;
    }
    WideCharToMultiByte(CP_UTF8, 0, row, len, buf + n, bytes, NULL, NULL);
    n += (DWORD)bytes;
    {
        DWORD written = 0;
        if (!WriteFile(h, buf, n, &written, NULL) || written != n) {
            DWORD e = GetLastError();
            CloseHandle(h);
            free(buf);
            fail_with(L"WriteFile (--out)", g.out, e);
        }
    }
    free(buf);
    if (!CloseHandle(h))
        fail(L"CloseHandle (--out)", g.out);
}

static void report(Result *r)
{
    double rate_app = r->t_app > 0.0 ? r->work / (r->t_app / 1000.0) : 0.0;
    double rate_dur = r->t_dur > 0.0 ? r->work / (r->t_dur / 1000.0) : -1.0;
    double rate = rate_dur >= 0.0 ? rate_dur : rate_app;
    wchar_t notes[1024];

    notes[0] = L'\0';
    appendf(notes, _countof(notes), L"unit=%ls;basis=%ls;rate_app=%.2f;", r->unit,
            rate_dur >= 0.0 ? L"durable" : L"app", rate_app);
    if (rate_dur >= 0.0)
        appendf(notes, _countof(notes), L"rate_durable=%.2f;", rate_dur);
    appendf(notes, _countof(notes), L"%ls", r->notes);
    {
        size_t n = wcslen(notes);
        if (n > 0 && notes[n - 1] == L';')
            notes[n - 1] = L'\0';
    }

    say(L"--- %ls summary ---\n", r->test);
    say(L"  label        : %ls\n", g.label ? g.label : L"");
    say(L"  params       : %ls\n", r->params);
    say(L"  count/bytes  : %llu / %llu\n", r->count, r->bytes);
    say(L"  T_app        : %.3f ms\n", r->t_app);
    if (r->t_dur >= 0.0)
        say(L"  T_durable    : %.3f ms (%ls)\n", r->t_dur, r->dur_mode);
    else
        say(L"  T_durable    : n/a (durable mode none)\n");
    say(L"  rate (app)   : %.2f %ls\n", rate_app, r->unit);
    if (rate_dur >= 0.0)
        say(L"  rate (durable): %.2f %ls\n", rate_dur, r->unit);
    if (r->p90 >= 0.0)
        say(L"  latency ms   : p50=%.4f p90=%.4f p99=%.4f max=%.4f\n", r->p50, r->p90, r->p99, r->max);
    else if (r->max >= 0.0)
        say(L"  time ms      : median=%.4f max=%.4f (per thread)\n", r->p50, r->max);
    say(L"  notes        : %ls\n", notes);
    if (g.out != NULL) {
        write_csv(r, rate, notes);
        say(L"  appended row to %ls\n", g.out);
    }
}

static void stats_from(Result *r, double *lat, size_t n)
{
    qsort(lat, n, sizeof(double), cmp_double);
    r->p50 = pct_sorted(lat, n, 0.50);
    r->p90 = pct_sorted(lat, n, 0.90);
    r->p99 = pct_sorted(lat, n, 0.99);
    r->max = n ? lat[n - 1] : -1.0;
}

/* ------------------------------------------------------------------ */
/* file writing primitive                                              */
/* ------------------------------------------------------------------ */

typedef struct ErrInfo {
    const wchar_t *op;
    DWORD err;
} ErrInfo;

/* create (CREATE_NEW) + write total bytes in io-sized chunks + close. */
static BOOL write_big(const wchar_t *path, unsigned long long total, DWORD io, BOOL wt,
                      const BYTE *buf, ErrInfo *e)
{
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                           FILE_ATTRIBUTE_NORMAL | (wt ? FILE_FLAG_WRITE_THROUGH : 0), NULL);
    unsigned long long remaining = total;
    if (h == INVALID_HANDLE_VALUE) {
        e->op = L"CreateFileW(CREATE_NEW)";
        e->err = GetLastError();
        return FALSE;
    }
    while (remaining > 0) {
        DWORD chunk = remaining < io ? (DWORD)remaining : io;
        DWORD w = 0;
        if (!WriteFile(h, buf, chunk, &w, NULL) || w != chunk) {
            e->op = L"WriteFile";
            e->err = GetLastError();
            if (e->err == ERROR_SUCCESS)
                e->err = ERROR_WRITE_FAULT;
            CloseHandle(h);
            return FALSE;
        }
        remaining -= chunk;
    }
    if (!CloseHandle(h)) {
        e->op = L"CloseHandle";
        e->err = GetLastError();
        return FALSE;
    }
    return TRUE;
}

static void write_big_or_die(const wchar_t *path, unsigned long long total, DWORD io, BOOL wt, const BYTE *buf)
{
    ErrInfo e;
    if (!write_big(path, total, io, wt, buf, &e))
        fail_with(e.op, path, e.err);
}

static BYTE *alloc_buf(size_t n)
{
    BYTE *b = (BYTE *)malloc(n ? n : 1);
    if (b == NULL)
        die(L"out of memory allocating %llu bytes", (unsigned long long)n);
    return b;
}

/* ------------------------------------------------------------------ */
/* b1 / b2 / prep                                                      */
/* ------------------------------------------------------------------ */

static void delete_range(wchar_t prefix, unsigned long long count)
{
    unsigned long long i;
    wchar_t path[PATH_CAP];
    LONGLONG t0 = qpc();
    for (i = 0; i < count; i++) {
        numbered_path(path, _countof(path), prefix, i);
        delete_file_checked(path);
    }
    say(L"  cleanup: deleted %llu files in %.1f ms (not part of timings)\n", count, ticks_ms(t0, qpc()));
}

static void run_create(const wchar_t *test, wchar_t prefix, BOOL is_b2)
{
    unsigned long long count = g.count != UNSET ? g.count : 2000ULL;
    unsigned long long size = g.size != UNSET ? g.size : 4096ULL;
    unsigned long long i, nb, b;
    double *lat;
    BYTE *buf;
    wchar_t path[PATH_CAP];
    LONGLONG t_start, t_end;
    Result r;
    ErrInfo e;
    double first_mean = 0, last_mean = 0;

    if (size > MAX_IO)
        usage_error(L"--size too large (max 1 GiB)");
    if (count == 0 || count >= 0xFFFFFFFFULL)
        usage_error(L"--count must be 1..4294967294");
    require_dir();
    if (is_b2) {
        unsigned long long need = g.min_existing != UNSET ? g.min_existing : 10000ULL;
        unsigned long long have = count_entries(need);
        if (have < need)
            die(L"b2 needs a directory with at least %llu existing entries; %ls has %llu. Run: ext4bench prep --dir ... --count %llu",
                need, g.dir, have, need);
        say(L"  b2: directory has >= %llu existing entries\n", need);
    } else if (!g.force && count_entries(1) > 0) {
        die(L"%ls is not empty; b1 expects an empty directory (use --force to override)", g.dir);
    }

    lat = (double *)malloc((size_t)count * sizeof(double));
    if (lat == NULL)
        die(L"out of memory for %llu latency samples", count);
    buf = alloc_buf((size_t)size);

    result_init(&r, test);
    _snwprintf_s(r.params, _countof(r.params), _TRUNCATE, L"count=%llu;size=%llu", count, size);
    r.count = count;
    r.bytes = count * size;
    r.work = (double)count;
    r.unit = L"files/s";

    say(L"%ls: creating %llu files of %llu bytes in %ls ...\n", test, count, size, g.dir);
    t_start = qpc();
    for (i = 0; i < count; i++) {
        LONGLONG t0, t1;
        numbered_path(path, _countof(path), prefix, i);
        fill_buf(buf, (size_t)size, i);
        t0 = qpc();
        if (!write_big(path, size, size ? (DWORD)size : 1, FALSE, buf, &e))
            fail_with(e.op, path, e.err);
        t1 = qpc();
        lat[i] = ticks_ms(t0, t1);
    }
    t_end = qpc();
    r.t_app = ticks_ms(t_start, t_end);

    /* per-bucket mean latency (flat per-create cost check) */
    nb = (count + BUCKET_FILES - 1) / BUCKET_FILES;
    say(L"  per-%llu-file bucket mean latency (create+write+close):\n", BUCKET_FILES);
    for (b = 0; b < nb; b++) {
        unsigned long long lo = b * BUCKET_FILES, hi = lo + BUCKET_FILES;
        double sum = 0, mean;
        if (hi > count)
            hi = count;
        for (i = lo; i < hi; i++)
            sum += lat[i];
        mean = sum / (double)(hi - lo);
        if (b == 0) first_mean = mean;
        if (b == nb - 1) last_mean = mean;
        say(L"    bucket %6llu (files %llu-%llu): mean %.4f ms\n", b, lo, hi - 1, mean);
    }
    appendf(r.notes, _countof(r.notes), L"first_bucket_mean_ms=%.4f;last_bucket_mean_ms=%.4f;last_over_first=%.2f;",
            first_mean, last_mean, first_mean > 0 ? last_mean / first_mean : 0.0);
    stats_from(&r, lat, (size_t)count);
    free(lat);
    free(buf);

    apply_durable(&r);
    report(&r);
    if (g.cleanup)
        delete_range(prefix, count);
}

static void run_prep(void)
{
    unsigned long long count = g.count != UNSET ? g.count : 10000ULL;
    unsigned long long i;
    wchar_t path[PATH_CAP];
    LONGLONG t0;
    Result r;

    if (count == 0 || count >= 0xFFFFFFFFULL)
        usage_error(L"--count must be 1..4294967294");
    require_dir();
    result_init(&r, L"prep");
    _snwprintf_s(r.params, _countof(r.params), _TRUNCATE, L"count=%llu", count);
    r.count = count;
    r.work = (double)count;
    r.unit = L"files/s";
    say(L"prep: creating %llu empty files p%%08u in %ls ...\n", count, g.dir);
    t0 = qpc();
    for (i = 0; i < count; i++) {
        HANDLE h;
        numbered_path(path, _countof(path), L'p', i);
        h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h == INVALID_HANDLE_VALUE)
            fail(L"CreateFileW(CREATE_NEW)", path);
        if (!CloseHandle(h))
            fail(L"CloseHandle", path);
    }
    r.t_app = ticks_ms(t0, qpc());
    appendf(r.notes, _countof(r.notes), L"prep only (no durable step);");
    report(&r);
    if (g.cleanup)
        delete_range(L'p', count);
}

/* ------------------------------------------------------------------ */
/* b3                                                                  */
/* ------------------------------------------------------------------ */

static void run_b3(void)
{
    unsigned long long total = g.total != UNSET ? g.total : (1ULL << 30);
    unsigned long long io = g.io != UNSET ? g.io : (1ULL << 20);
    wchar_t path[PATH_CAP];
    BYTE *buf;
    LONGLONG t0, t1;
    Result r;

    if (io == 0 || io > MAX_IO)
        usage_error(L"--io must be 1..1GiB");
    require_dir();
    join(path, _countof(path), g.dir, L"b3.dat");
    prep_target(path);
    buf = alloc_buf((size_t)io);
    fill_buf(buf, (size_t)io, 3);

    result_init(&r, L"b3");
    _snwprintf_s(r.params, _countof(r.params), _TRUNCATE, L"total=%llu;io=%llu;writethrough=%d",
                 total, io, g.writethrough ? 1 : 0);
    r.count = 1;
    r.bytes = total;
    r.work = (double)total / 1048576.0;
    r.unit = L"MB/s";
    say(L"b3: sequential write of %llu bytes, io=%llu, writethrough=%d -> %ls\n", total, io,
        g.writethrough ? 1 : 0, path);
    t0 = qpc();
    write_big_or_die(path, total, (DWORD)io, g.writethrough, buf);
    t1 = qpc();
    r.t_app = ticks_ms(t0, t1);
    free(buf);
    apply_durable(&r);
    report(&r);
    if (g.cleanup) {
        delete_file_checked(path);
        say(L"  cleanup: deleted %ls\n", path);
    }
}

/* ------------------------------------------------------------------ */
/* b4                                                                  */
/* ------------------------------------------------------------------ */

typedef struct Worker {
    wchar_t path[PATH_CAP];
    unsigned long long total;
    DWORD io;
    BYTE *buf;
    volatile LONG *ready;
    HANDLE start;
    double ms;
    BOOL ok;
    ErrInfo e;
} Worker;

static DWORD WINAPI b4_thread(LPVOID p)
{
    Worker *w = (Worker *)p;
    LONGLONG t0, t1;
    InterlockedIncrement(w->ready);
    WaitForSingleObject(w->start, INFINITE);
    t0 = qpc();
    w->ok = write_big(w->path, w->total, w->io, FALSE, w->buf, &w->e);
    t1 = qpc();
    w->ms = ticks_ms(t0, t1);
    return 0;
}

static void run_b4(void)
{
    unsigned long long total = g.total != UNSET ? g.total : (1ULL << 30);
    unsigned long long io = g.io != UNSET ? g.io : (1ULL << 20);
    unsigned nthreads = (unsigned)(g.threads != UNSET ? g.threads : 4ULL);
    Worker *w;
    HANDLE hs[MAX_THREADS];
    HANDLE start;
    volatile LONG ready = 0;
    unsigned i;
    LONGLONG t0, t1;
    Result r;
    double *tm;
    wchar_t nm[64];

    if (nthreads < 1 || nthreads > MAX_THREADS)
        usage_error(L"--threads must be 1..%d", MAX_THREADS);
    if (io == 0 || io > MAX_IO)
        usage_error(L"--io must be 1..1GiB");
    require_dir();

    w = (Worker *)calloc(nthreads, sizeof(Worker));
    if (w == NULL)
        die(L"out of memory");
    start = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (start == NULL)
        fail(L"CreateEventW", L"");
    for (i = 0; i < nthreads; i++) {
        _snwprintf_s(nm, _countof(nm), _TRUNCATE, L"b4_%02u.dat", i);
        join(w[i].path, _countof(w[i].path), g.dir, nm);
        prep_target(w[i].path);
        w[i].total = total;
        w[i].io = (DWORD)io;
        w[i].buf = alloc_buf((size_t)io);
        fill_buf(w[i].buf, (size_t)io, 4000ULL + i);
        w[i].ready = &ready;
        w[i].start = start;
    }

    result_init(&r, L"b4");
    _snwprintf_s(r.params, _countof(r.params), _TRUNCATE, L"threads=%u;total=%llu;io=%llu", nthreads, total, io);
    r.count = nthreads;
    r.bytes = total * nthreads;
    r.work = (double)total * (double)nthreads / 1048576.0;
    r.unit = L"MB/s";

    say(L"b4: %u writers x %llu bytes, io=%llu, in %ls\n", nthreads, total, io, g.dir);
    for (i = 0; i < nthreads; i++) {
        hs[i] = CreateThread(NULL, 0, b4_thread, &w[i], 0, NULL);
        if (hs[i] == NULL)
            fail(L"CreateThread", L"");
    }
    while (ready < (LONG)nthreads)
        Sleep(1);
    t0 = qpc();
    if (!SetEvent(start))
        fail(L"SetEvent", L"");
    if (WaitForMultipleObjects(nthreads, hs, TRUE, INFINITE) == WAIT_FAILED)
        fail(L"WaitForMultipleObjects", L"");
    t1 = qpc();
    r.t_app = ticks_ms(t0, t1);
    for (i = 0; i < nthreads; i++) {
        CloseHandle(hs[i]);
        if (!w[i].ok)
            fail_with(w[i].e.op, w[i].path, w[i].e.err);
    }
    CloseHandle(start);

    tm = (double *)malloc(nthreads * sizeof(double));
    if (tm == NULL)
        die(L"out of memory");
    appendf(r.notes, _countof(r.notes), L"thread_ms=");
    for (i = 0; i < nthreads; i++) {
        tm[i] = w[i].ms;
        say(L"  thread %u: %.3f ms (%.2f MB/s)\n", i, w[i].ms,
            (double)total / 1048576.0 / (w[i].ms / 1000.0));
        appendf(r.notes, _countof(r.notes), L"%ls%.1f", i ? L"/" : L"", w[i].ms);
    }
    appendf(r.notes, _countof(r.notes), L";");
    qsort(tm, nthreads, sizeof(double), cmp_double);
    r.p50 = pct_sorted(tm, nthreads, 0.5);
    r.max = tm[nthreads - 1];
    free(tm);

    for (i = 0; i < nthreads; i++)
        free(w[i].buf);
    apply_durable(&r);
    report(&r);
    if (g.cleanup) {
        for (i = 0; i < nthreads; i++)
            delete_file_checked(w[i].path);
        say(L"  cleanup: deleted %u files\n", nthreads);
    }
    free(w);
}

/* ------------------------------------------------------------------ */
/* b5                                                                  */
/* ------------------------------------------------------------------ */

static void run_b5(void)
{
    wchar_t pat[PATH_CAP], p2[PATH_CAP];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    unsigned long long n = 0;
    LONGLONG t0, t1, stat_ticks = 0;
    Result r;

    require_dir();
    if (!g.warm) {
        double ms;
        say(L"b5: cold: lock+dismount volume before timing ...\n");
        ms = volume_lockdismount();
        say(L"  lockdismount took %.3f ms (excluded from timing, lock attempts=%d)\n", ms, g_lock_attempts);
    }
    join(pat, _countof(pat), g.dir, L"*");

    result_init(&r, L"b5");
    t0 = qpc();
    h = FindFirstFileExW(pat, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE)
        fail(L"FindFirstFileExW", pat);
    for (;;) {
        BOOL dot = fd.cFileName[0] == L'.' &&
                   (fd.cFileName[1] == L'\0' || (fd.cFileName[1] == L'.' && fd.cFileName[2] == L'\0'));
        if (!dot) {
            n++;
            if (g.stat) {
                WIN32_FILE_ATTRIBUTE_DATA ad;
                LONGLONG s0, s1;
                join(p2, _countof(p2), g.dir, fd.cFileName);
                s0 = qpc();
                if (!GetFileAttributesExW(p2, GetFileExInfoStandard, &ad)) {
                    DWORD e = GetLastError();
                    FindClose(h);
                    fail_with(L"GetFileAttributesExW", p2, e);
                }
                s1 = qpc();
                stat_ticks += s1 - s0;
            }
        }
        if (!FindNextFileW(h, &fd)) {
            DWORD e = GetLastError();
            if (e != ERROR_NO_MORE_FILES) {
                FindClose(h);
                fail_with(L"FindNextFileW", pat, e);
            }
            break;
        }
    }
    t1 = qpc();
    FindClose(h);
    r.t_app = ticks_ms(t0, t1);
    r.count = n;
    r.work = (double)n;
    r.unit = L"entries/s";
    _snwprintf_s(r.params, _countof(r.params), _TRUNCATE, L"warm=%d;stat=%d", g.warm ? 1 : 0, g.stat ? 1 : 0);
    appendf(r.notes, _countof(r.notes), L"cold=%d;", g.warm ? 0 : 1);
    if (g.stat) {
        double sm = ticks_ms(0, stat_ticks);
        appendf(r.notes, _countof(r.notes), L"stat_ms=%.3f;", sm);
        say(L"  entries=%llu, of T_app %.3f ms, %.3f ms was inside GetFileAttributesExW\n", n, r.t_app, sm);
    }
    say(L"  enumerated %llu entries\n", n);
    report(&r);
}

/* ------------------------------------------------------------------ */
/* b6                                                                  */
/* ------------------------------------------------------------------ */

static void b6_row(const wchar_t *test, unsigned long long iters, unsigned long long size,
                   double t_app, double *lat)
{
    Result r;
    double sum = 0;
    unsigned long long i;
    result_init(&r, test);
    _snwprintf_s(r.params, _countof(r.params), _TRUNCATE, L"iters=%llu;size=%llu", iters, size);
    r.count = iters;
    r.bytes = iters * size;
    r.t_app = t_app;
    r.work = (double)iters * (double)size / 1048576.0;
    r.unit = L"MB/s";
    for (i = 0; i < iters; i++)
        sum += lat[i];
    appendf(r.notes, _countof(r.notes), L"mean_iter_ms=%.4f;", sum / (double)iters);
    say(L"  %ls: per-iteration mean %.4f ms\n", test, sum / (double)iters);
    stats_from(&r, lat, (size_t)iters);
    say(L"  %ls: per-iteration p99 %.4f ms\n", test, r.p99);
    apply_durable(&r);
    report(&r);
}

static void run_b6(void)
{
    unsigned long long iters = g.iters != UNSET ? g.iters : 200ULL;
    unsigned long long size = g.size != UNSET ? g.size : (1ULL << 20);
    unsigned long long i;
    DWORD io;
    BYTE *buf;
    double *lat;
    wchar_t src[PATH_CAP], dst[PATH_CAP], tmp[PATH_CAP], tgt[PATH_CAP];
    LONGLONG t0, t1;

    if (iters == 0)
        usage_error(L"--iters must be >= 1");
    if (size > 0x7FFFFFFFULL)
        usage_error(L"--size too large for b6 (max 2 GiB - 1)");
    require_dir();
    io = size > (1ULL << 20) ? (DWORD)(1UL << 20) : (size ? (DWORD)size : 1);
    buf = alloc_buf(io);
    fill_buf(buf, io, 6);
    lat = (double *)malloc((size_t)iters * sizeof(double));
    if (lat == NULL)
        die(L"out of memory");
    join(src, _countof(src), g.dir, L"b6_src.dat");
    join(dst, _countof(dst), g.dir, L"b6_dst.dat");
    join(tmp, _countof(tmp), g.dir, L"b6_tmp.dat");
    join(tgt, _countof(tgt), g.dir, L"b6_target.dat");
    prep_target(src); prep_target(dst); prep_target(tmp); prep_target(tgt);

    /* ---- overwrite-copy ---- */
    write_big_or_die(src, size, io, FALSE, buf);
    if (!CopyFileW(src, dst, TRUE))
        fail(L"CopyFileW (initial dst)", dst);
    say(L"b6 overwrite-copy: %llu iterations of CopyFileW(src,dst,FALSE), size=%llu\n", iters, size);
    t0 = qpc();
    for (i = 0; i < iters; i++) {
        LONGLONG a = qpc(), b;
        if (!CopyFileW(src, dst, FALSE))
            fail(L"CopyFileW", dst);
        b = qpc();
        lat[i] = ticks_ms(a, b);
    }
    t1 = qpc();
    b6_row(L"b6_copy", iters, size, ticks_ms(t0, t1), lat);

    /* ---- replace-rename ---- */
    write_big_or_die(tgt, size, io, FALSE, buf);
    say(L"b6 replace-rename: %llu iterations of write tmp + MoveFileExW(REPLACE_EXISTING), size=%llu\n", iters, size);
    t0 = qpc();
    for (i = 0; i < iters; i++) {
        LONGLONG a = qpc(), b;
        write_big_or_die(tmp, size, io, FALSE, buf);
        if (!MoveFileExW(tmp, tgt, MOVEFILE_REPLACE_EXISTING))
            fail(L"MoveFileExW", tgt);
        b = qpc();
        lat[i] = ticks_ms(a, b);
    }
    t1 = qpc();
    b6_row(L"b6_rename", iters, size, ticks_ms(t0, t1), lat);

    free(lat);
    free(buf);
    if (g.cleanup) {
        delete_file_checked(src);
        delete_file_checked(dst);
        delete_file_checked(tgt);
        say(L"  cleanup: deleted b6 files\n");
    }
}

/* ------------------------------------------------------------------ */
/* CLI                                                                 */
/* ------------------------------------------------------------------ */

static void print_help(void)
{
    write_out(STD_OUTPUT_HANDLE,
        L"ext4bench - filesystem benchmark for Ext4Fsd (Win32, single file)\n"
        L"\n"
        L"Usage: ext4bench <test> --dir <path> [options]\n"
        L"\n"
        L"Tests:\n"
        L"  b1    Create --count (2000) files of --size (4096) bytes in an EMPTY --dir.\n"
        L"        Per file: CreateFileW(CREATE_NEW)+WriteFile+CloseHandle. Reports total,\n"
        L"        files/s, p50/p90/p99/max latency and mean latency per 1000-file bucket.\n"
        L"  prep  Create --count (10000) empty files p%08u in --dir (setup for b2 / b5).\n"
        L"  b2    Like b1 but --dir must already hold >= 10000 entries (--min-existing N\n"
        L"        overrides); new files are named n%08u.\n"
        L"  b3    Sequential cached write of --total (1G) bytes in --io (1M) sized writes to\n"
        L"        one new file b3.dat. MB/s for T_app and T_durable. --writethrough adds\n"
        L"        FILE_FLAG_WRITE_THROUGH.\n"
        L"  b4    --threads (4) concurrent writers, each writing its own --total (1G) file\n"
        L"        with --io (1M) writes, released together by a manual-reset event.\n"
        L"        Reports aggregate MB/s and per-thread time.\n"
        L"  b5    Enumerate --dir with FindFirstFileExW(FindExInfoBasic, LARGE_FETCH).\n"
        L"        Cold (default): lock+dismount the volume first (untimed). --warm skips\n"
        L"        that. --stat also calls GetFileAttributesExW per entry.\n"
        L"  b6    --iters (200) iterations, file --size (1M): (a) CopyFileW over an existing\n"
        L"        dst; (b) write tmp file + MoveFileExW(REPLACE_EXISTING) over target.\n"
        L"        Emits two rows: b6_copy and b6_rename (per-iteration mean/p99).\n"
        L"\n");
    write_out(STD_OUTPUT_HANDLE,
        L"Common options:\n"
        L"  --dir <path>        Target directory (must exist, on a drive-letter volume). Required.\n"
        L"  --label <text>      Free text (driver build id) written into every result row.\n"
        L"  --out <file.csv>    Append result rows (header written if file is missing/empty).\n"
        L"                      A human summary is always printed to stdout.\n"
        L"  --durable <mode>    none | flushvol (default) | lockdismount. After the timed\n"
        L"                      workload, measure T_durable = T_app + durability step:\n"
        L"        flushvol      open \\\\.\\X: (R/W, share R/W), FlushFileBuffers, close.\n"
        L"        lockdismount  open \\\\.\\X:, FSCTL_LOCK_VOLUME (up to 10 tries, 500 ms apart),\n"
        L"                      FSCTL_DISMOUNT_VOLUME, close. Fatal if the lock never succeeds.\n"
        L"        none          skip; only T_app is reported.\n"
        L"      Volume access needs an elevated prompt. prep and b5 never run a durable step.\n"
        L"  --count N           File count (b1/b2 default 2000, prep default 10000).\n"
        L"  --size N            File size in bytes (b1/b2 default 4096, b6 default 1M).\n"
        L"  --total N           Bytes per file for b3/b4 (default 1G). Suffix K/M/G accepted.\n"
        L"  --io N              Write size for b3/b4 (default 1M). Suffix K/M/G accepted.\n"
        L"  --threads N         b4 writer count (default 4, max 64).\n"
        L"  --iters N           b6 iterations (default 200).\n"
        L"  --min-existing N    b2 minimum pre-existing entries (default 10000).\n"
        L"  --writethrough      b3: add FILE_FLAG_WRITE_THROUGH.\n"
        L"  --warm              b5: skip the cold lock+dismount.\n"
        L"  --stat              b5: also GetFileAttributesExW each entry.\n"
        L"  --force             b1: allow non-empty --dir; b3/b4/b6: delete same-named leftover files.\n"
        L"  --cleanup           Delete created files after measuring (not counted in timings).\n"
        L"  --help              This text.\n"
        L"\n"
        L"Sizes accept K/M/G suffixes (powers of 1024). File data is deterministic (xorshift64\n"
        L"seeded by file/thread index). Any API failure prints operation, path and GetLastError\n"
        L"and exits non-zero. Exit codes: 0 ok, 1 runtime failure, 2 usage error.\n"
        L"\n"
        L"CSV columns: timestamp_utc,label,test,params,count,bytes,t_app_ms,t_durable_ms,\n"
        L"durable_mode,rate,p50_ms,p90_ms,p99_ms,max_ms,notes   (rate = durable-based when a\n"
        L"durable step ran, else app-based; unit and both rates are in notes).\n");
}

static unsigned long long parse_num(const wchar_t *opt, const wchar_t *s, BOOL suffix)
{
    wchar_t *end;
    unsigned long long v, mult = 1;
    if (!iswdigit(s[0]))
        usage_error(L"%ls: expected a number, got \"%ls\"", opt, s);
    errno = 0;
    v = wcstoull(s, &end, 10);
    if (errno == ERANGE)
        usage_error(L"%ls: value out of range: %ls", opt, s);
    if (*end != L'\0') {
        if (!suffix)
            usage_error(L"%ls: expected a plain number, got \"%ls\"", opt, s);
        switch (towupper(*end)) {
        case L'K': mult = 1ULL << 10; break;
        case L'M': mult = 1ULL << 20; break;
        case L'G': mult = 1ULL << 30; break;
        default: usage_error(L"%ls: bad size suffix in \"%ls\" (use K, M or G)", opt, s);
        }
        end++;
        if (towupper(*end) == L'I') end++;
        if (towupper(*end) == L'B') end++;
        if (*end != L'\0')
            usage_error(L"%ls: bad size \"%ls\"", opt, s);
    }
    if (v > (~0ULL - 1) / mult)
        usage_error(L"%ls: value too large: %ls", opt, s);
    return v * mult;
}

int wmain(int argc, wchar_t **argv)
{
    int i;
    BOOL have_dir = FALSE;
    const wchar_t *test;
    DWORD n;

    if (argc < 2 || !wcscmp(argv[1], L"--help") || !wcscmp(argv[1], L"-h") || !wcscmp(argv[1], L"/?")) {
        print_help();
        return argc < 2 ? 2 : 0;
    }
    memset(&g, 0, sizeof(g));
    g.count = g.size = g.total = g.io = g.threads = g.iters = g.min_existing = UNSET;
    g.dur = DUR_FLUSHVOL;
    test = argv[1];
    g.test = test;

    for (i = 2; i < argc; i++) {
        const wchar_t *a = argv[i];
#define NEED_VAL() do { if (i + 1 >= argc) usage_error(L"%ls requires a value", a); } while (0)
        if (!wcscmp(a, L"--help") || !wcscmp(a, L"-h")) { print_help(); return 0; }
        else if (!wcscmp(a, L"--dir"))   { NEED_VAL(); n = GetFullPathNameW(argv[++i], _countof(g.dir), g.dir, NULL);
                                           if (n == 0 || n >= _countof(g.dir)) fail(L"GetFullPathNameW", argv[i]);
                                           have_dir = TRUE; }
        else if (!wcscmp(a, L"--label")) { NEED_VAL(); g.label = argv[++i]; }
        else if (!wcscmp(a, L"--out"))   { NEED_VAL(); g.out = argv[++i]; }
        else if (!wcscmp(a, L"--durable")) {
            NEED_VAL(); i++;
            if (!wcscmp(argv[i], L"none")) g.dur = DUR_NONE;
            else if (!wcscmp(argv[i], L"flushvol")) g.dur = DUR_FLUSHVOL;
            else if (!wcscmp(argv[i], L"lockdismount")) g.dur = DUR_LOCKDISMOUNT;
            else usage_error(L"--durable must be none, flushvol or lockdismount");
        }
        else if (!wcscmp(a, L"--count"))  { NEED_VAL(); g.count = parse_num(a, argv[++i], FALSE); }
        else if (!wcscmp(a, L"--size"))   { NEED_VAL(); g.size = parse_num(a, argv[++i], TRUE); }
        else if (!wcscmp(a, L"--total"))  { NEED_VAL(); g.total = parse_num(a, argv[++i], TRUE); }
        else if (!wcscmp(a, L"--io"))     { NEED_VAL(); g.io = parse_num(a, argv[++i], TRUE); }
        else if (!wcscmp(a, L"--threads")){ NEED_VAL(); g.threads = parse_num(a, argv[++i], FALSE); }
        else if (!wcscmp(a, L"--iters"))  { NEED_VAL(); g.iters = parse_num(a, argv[++i], FALSE); }
        else if (!wcscmp(a, L"--min-existing")) { NEED_VAL(); g.min_existing = parse_num(a, argv[++i], FALSE); }
        else if (!wcscmp(a, L"--writethrough")) g.writethrough = TRUE;
        else if (!wcscmp(a, L"--warm"))   g.warm = TRUE;
        else if (!wcscmp(a, L"--stat"))   g.stat = TRUE;
        else if (!wcscmp(a, L"--force"))  g.force = TRUE;
        else if (!wcscmp(a, L"--cleanup")) g.cleanup = TRUE;
        else usage_error(L"unknown option: %ls", a);
#undef NEED_VAL
    }
    if (!have_dir)
        usage_error(L"--dir is required");
    {   /* strip trailing backslash (keep drive roots like C:\) */
        size_t len = wcslen(g.dir);
        if (len > 3 && g.dir[len - 1] == L'\\')
            g.dir[len - 1] = L'\0';
    }
    init_timer();

    {
        BOOL uses_durable = !_wcsicmp(test, L"b1") || !_wcsicmp(test, L"b2") || !_wcsicmp(test, L"b3") ||
                            !_wcsicmp(test, L"b4") || !_wcsicmp(test, L"b6");
        BOOL cold_b5 = !_wcsicmp(test, L"b5") && !g.warm;
        if (cold_b5 || (uses_durable && g.dur != DUR_NONE)) {
            require_dir();
            volume_preflight();
        }
    }

    if (!_wcsicmp(test, L"b1"))     run_create(L"b1", L'f', FALSE);
    else if (!_wcsicmp(test, L"b2"))   run_create(L"b2", L'n', TRUE);
    else if (!_wcsicmp(test, L"prep")) run_prep();
    else if (!_wcsicmp(test, L"b3"))   run_b3();
    else if (!_wcsicmp(test, L"b4"))   run_b4();
    else if (!_wcsicmp(test, L"b5"))   run_b5();
    else if (!_wcsicmp(test, L"b6"))   run_b6();
    else usage_error(L"unknown test \"%ls\" (expected b1 b2 b3 b4 b5 b6 prep)", test);
    return 0;
}
