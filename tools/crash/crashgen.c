/*
 * crashgen.c - crash-consistency workload generator for the Ext4Fsd driver.
 *
 * Runs inside the Windows guest.  Performs random file operations below
 * --root and writes a write-through, flushed-after-every-line text log that
 * the host later uses (verify.py) to decide which files the filesystem was
 * obliged to have persisted when the VM was hard-reset.
 *
 * Log format (tab separated, one event per line):
 *   HEADER  start=<iso> seed=<n> root=<path> max_files=.. max_size=.. ...
 *   OP      <seq> <id> <kind> <relpath> [<newrelpath>]   (before the op)
 *   DONE    <seq>                                        (after op returned)
 *   FLUSHED <seq> <id> <relpath> <size> <sha256hex>      (FlushFileBuffers ok)
 *   DEAD    <seq> <id>                                   (id deleted/replaced)
 *   ERR     <seq> <id> <what> <win32error>               (op failed)
 *   END     <seq>                                        (--ops reached)
 *
 * OP kinds: CREATE WRITE APPEND TRUNC REWRITE RENAME REPLACE VICTIM DELETE
 *           MKDIR RMDIR.  REPLACE (MoveFileEx REPLACE_EXISTING) is logged as
 *           two OP lines with the same seq: "REPLACE" for the moving id
 *           (path = old, newpath = new) and "VICTIM" for the id whose file is
 *           overwritten (path = the replaced path).  MKDIR/RMDIR use id 0.
 *
 * Build: see build.cmd (cl /nologo /O2 /W4 /WX crashgen.c bcrypt.lib)
 */
#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <stdint.h>

/* ------------------------------------------------------------------ */
/* configuration / globals                                            */
/* ------------------------------------------------------------------ */

static char      g_root[MAX_PATH];
static char      g_logpath[MAX_PATH];
static char      g_logdir[MAX_PATH];
static char      g_alivepath[MAX_PATH + 8];
static uint64_t  g_seed;
static uint32_t  g_maxfiles = 300;
static size_t    g_maxsize = 2u * 1024u * 1024u;
static double    g_flushprob = 0.2;
static uint64_t  g_ops = 0;

static HANDLE    g_log = INVALID_HANDLE_VALUE;
static BCRYPT_ALG_HANDLE g_halg = NULL;

typedef struct { uint64_t s; } Rng;
static Rng g_rng;

static uint64_t rng_next(Rng *r)
{
    uint64_t z = (r->s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

static uint64_t rnd(uint64_t n)
{
    return n ? rng_next(&g_rng) % n : 0;
}

static double rnd01(void)
{
    return (double)(rng_next(&g_rng) >> 11) * (1.0 / 9007199254740992.0);
}

/* ------------------------------------------------------------------ */
/* logging                                                            */
/* ------------------------------------------------------------------ */

static void log_line(const char *fmt, ...)
{
    char buf[2048];
    va_list ap;
    int n;
    DWORD wr = 0;

    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
    va_end(ap);
    if (n < 0) n = 0;
    if (n > (int)sizeof(buf) - 2) n = (int)sizeof(buf) - 2;
    buf[n++] = '\n';

    if (!WriteFile(g_log, buf, (DWORD)n, &wr, NULL) || wr != (DWORD)n ||
        !FlushFileBuffers(g_log)) {
        fprintf(stderr, "crashgen: log write failed, error %lu\n", GetLastError());
        exit(2);
    }
}

static void win_path(char *p)
{
    for (; *p; p++)
        if (*p == '/') *p = '\\';
}

static void mkdir_p(const char *path)
{
    char tmp[MAX_PATH];
    size_t i, len = strlen(path);

    if (len >= sizeof(tmp)) return;
    memcpy(tmp, path, len + 1);
    win_path(tmp);
    for (i = 3; i < len; i++) {
        if (tmp[i] == '\\') {
            tmp[i] = 0;
            CreateDirectoryA(tmp, NULL);
            tmp[i] = '\\';
        }
    }
    CreateDirectoryA(tmp, NULL);
}

static void heartbeat(void)
{
    HANDLE h = CreateFileA(g_alivepath, GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        SetFileTime(h, NULL, NULL, &ft);
        CloseHandle(h);
    }
}

/* ------------------------------------------------------------------ */
/* content generation + hashing                                       */
/* ------------------------------------------------------------------ */

static void fill_pattern(unsigned char *p, size_t len, uint32_t id, uint32_t ver)
{
    Rng r;
    size_t i;

    r.s = g_seed ^ ((uint64_t)id << 32) ^ ((uint64_t)ver * 0x9E3779B97F4A7C15ull);
    for (i = 0; i < len; i += 8) {
        uint64_t v = rng_next(&r);
        size_t n = len - i < 8 ? len - i : 8;
        memcpy(p + i, &v, n);
    }
}

static void sha256_hex(const unsigned char *data, size_t len, char out[65])
{
    unsigned char dg[32];
    int i;
    static const char hex[] = "0123456789abcdef";

    if (BCryptHash(g_halg, NULL, 0, (PUCHAR)data, (ULONG)len, dg, 32) != 0) {
        fprintf(stderr, "crashgen: BCryptHash failed\n");
        exit(2);
    }
    for (i = 0; i < 32; i++) {
        out[i * 2]     = hex[dg[i] >> 4];
        out[i * 2 + 1] = hex[dg[i] & 15];
    }
    out[64] = 0;
}

static size_t rand_size(size_t max)
{
    unsigned lg = 0;
    uint64_t sc, v;

    while (((size_t)1 << (lg + 1)) <= max) lg++;
    sc = (uint64_t)1 << rnd(lg + 1);
    v = rnd(sc + 1);
    if (rnd(100) < 15) v &= ~(uint64_t)4095;
    if (v > max) v = max;
    return (size_t)v;
}

/* ------------------------------------------------------------------ */
/* model                                                              */
/* ------------------------------------------------------------------ */

#define NDIRS 4
static const char *const g_dirnames[NDIRS] = { "", "d1", "d2", "d3" };
static int g_dir_exists[NDIRS] = { 1, 0, 0, 0 };

typedef struct {
    uint32_t       id;
    int            dir;
    char           name[64];
    unsigned char *data;
    size_t         size;
    size_t         cap;
    uint32_t       ver;
    uint32_t       nren;
} FileRec;

static FileRec  *g_files;
static uint32_t  g_nfiles;
static uint32_t  g_next_id = 1;
static unsigned  g_consec_err;

static void relpath(const FileRec *f, char *out, size_t outsz)
{
    if (f->dir == 0)
        snprintf(out, outsz, "%s", f->name);
    else
        snprintf(out, outsz, "%s/%s", g_dirnames[f->dir], f->name);
}

static void fullpath(const char *rel, char *out, size_t outsz)
{
    snprintf(out, outsz, "%s\\%s", g_root, rel);
    win_path(out);
}

static void file_resize(FileRec *f, size_t n)
{
    if (n > f->cap || f->data == NULL) {
        size_t nc = n + n / 4 + 64;
        unsigned char *p;
        if (nc > g_maxsize) nc = g_maxsize;
        if (nc < n) nc = n;
        if (nc == 0) nc = 1;
        p = (unsigned char *)realloc(f->data, nc);
        if (!p) {
            fprintf(stderr, "crashgen: out of memory\n");
            exit(2);
        }
        f->data = p;
        f->cap = nc;
    }
    if (n > f->size)
        memset(f->data + f->size, 0, n - f->size);
    f->size = n;
}

static int find_idx(uint32_t id)
{
    uint32_t i;
    for (i = 0; i < g_nfiles; i++)
        if (g_files[i].id == id) return (int)i;
    return -1;
}

static void remove_idx(uint32_t idx)
{
    free(g_files[idx].data);
    g_nfiles--;
    if (idx != g_nfiles)
        g_files[idx] = g_files[g_nfiles];
    memset(&g_files[g_nfiles], 0, sizeof(FileRec));
}

static int pick_dir(void)
{
    int cand[NDIRS], n = 0, i;
    for (i = 0; i < NDIRS; i++)
        if (g_dir_exists[i]) cand[n++] = i;
    return cand[rnd((uint64_t)n)];
}

static int dir_file_count(int dir)
{
    int c = 0;
    uint32_t i;
    for (i = 0; i < g_nfiles; i++)
        if (g_files[i].dir == dir) c++;
    return c;
}

static void log_done(uint64_t seq)
{
    log_line("DONE\t%llu", (unsigned long long)seq);
    g_consec_err = 0;
}

static void note_error(uint64_t seq, uint32_t id, const char *what, DWORD err)
{
    log_line("ERR\t%llu\t%u\t%s\t%lu", (unsigned long long)seq, id, what, err);
    if (++g_consec_err > 100) {
        fprintf(stderr, "crashgen: too many consecutive errors, giving up\n");
        exit(3);
    }
}

/* Op failed in a way that leaves the on-disk state of the file unknown:
 * log it, best-effort delete, forget the id. Returns 0 (no file to flush). */
static uint32_t kill_file(uint32_t idx, uint64_t seq, const char *what, DWORD err)
{
    char rel[160], full[MAX_PATH + 160];
    uint32_t id = g_files[idx].id;

    note_error(seq, id, what, err);
    relpath(&g_files[idx], rel, sizeof(rel));
    fullpath(rel, full, sizeof(full));
    DeleteFileA(full);
    log_line("DEAD\t%llu\t%u", (unsigned long long)seq, id);
    remove_idx(idx);
    return 0;
}

/* ------------------------------------------------------------------ */
/* I/O helpers                                                        */
/* ------------------------------------------------------------------ */

static HANDLE open_f(const char *full, DWORD access, DWORD disp)
{
    return CreateFileA(full, access, FILE_SHARE_READ, NULL, disp,
                       FILE_ATTRIBUTE_NORMAL, NULL);
}

static BOOL write_all(HANDLE h, const unsigned char *p, size_t len)
{
    while (len > 0) {
        DWORD chunk = len > (1u << 20) ? (1u << 20) : (DWORD)len;
        DWORD wr = 0;
        if (!WriteFile(h, p, chunk, &wr, NULL) || wr != chunk)
            return FALSE;
        p += chunk;
        len -= chunk;
    }
    return TRUE;
}

static BOOL seek_to(HANDLE h, LONGLONG off, DWORD method)
{
    LARGE_INTEGER li;
    li.QuadPart = off;
    return SetFilePointerEx(h, li, NULL, method);
}

/* ------------------------------------------------------------------ */
/* operations; each returns the id of a file that may be flushed (or 0) */
/* ------------------------------------------------------------------ */

static uint32_t op_create(uint64_t seq)
{
    FileRec *f;
    char rel[160], full[MAX_PATH + 160];
    HANDLE h;
    BOOL ok;
    DWORD err = 0;
    uint32_t idx;

    idx = g_nfiles++;
    f = &g_files[idx];
    memset(f, 0, sizeof(*f));
    f->id = g_next_id++;
    f->dir = pick_dir();
    snprintf(f->name, sizeof(f->name), "f%u.bin", f->id);
    f->ver = 1;
    file_resize(f, rand_size(g_maxsize));
    fill_pattern(f->data, f->size, f->id, f->ver);

    relpath(f, rel, sizeof(rel));
    log_line("OP\t%llu\t%u\tCREATE\t%s", (unsigned long long)seq, f->id, rel);
    fullpath(rel, full, sizeof(full));
    h = open_f(full, GENERIC_WRITE, CREATE_NEW);
    if (h == INVALID_HANDLE_VALUE)
        return kill_file(idx, seq, "create-open", GetLastError());
    ok = write_all(h, f->data, f->size);
    if (!ok) err = GetLastError();
    CloseHandle(h);
    if (!ok)
        return kill_file(idx, seq, "create-write", err);
    log_done(seq);
    return f->id;
}

static uint32_t op_write(uint32_t idx, uint64_t seq)
{
    FileRec *f = &g_files[idx];
    char rel[160], full[MAX_PATH + 160];
    HANDLE h;
    BOOL ok;
    DWORD err = 0;
    size_t off, len;

    off = (size_t)rnd(f->size);
    len = rand_size(g_maxsize - off);
    if (len == 0) len = 1;
    if (off + len > g_maxsize) len = g_maxsize - off;
    f->ver++;
    if (off + len > f->size) file_resize(f, off + len);
    fill_pattern(f->data + off, len, f->id, f->ver);

    relpath(f, rel, sizeof(rel));
    log_line("OP\t%llu\t%u\tWRITE\t%s", (unsigned long long)seq, f->id, rel);
    fullpath(rel, full, sizeof(full));
    h = open_f(full, GENERIC_WRITE, OPEN_EXISTING);
    if (h == INVALID_HANDLE_VALUE)
        return kill_file(idx, seq, "write-open", GetLastError());
    ok = seek_to(h, (LONGLONG)off, FILE_BEGIN) && write_all(h, f->data + off, len);
    if (!ok) err = GetLastError();
    CloseHandle(h);
    if (!ok)
        return kill_file(idx, seq, "write", err);
    log_done(seq);
    return f->id;
}

static uint32_t op_append(uint32_t idx, uint64_t seq)
{
    FileRec *f = &g_files[idx];
    char rel[160], full[MAX_PATH + 160];
    HANDLE h;
    BOOL ok;
    DWORD err = 0;
    size_t off = f->size, len;

    len = rand_size(g_maxsize - off);
    if (len == 0) len = 1;
    if (off + len > g_maxsize) len = g_maxsize - off;
    f->ver++;
    file_resize(f, off + len);
    fill_pattern(f->data + off, len, f->id, f->ver);

    relpath(f, rel, sizeof(rel));
    log_line("OP\t%llu\t%u\tAPPEND\t%s", (unsigned long long)seq, f->id, rel);
    fullpath(rel, full, sizeof(full));
    h = open_f(full, GENERIC_WRITE, OPEN_EXISTING);
    if (h == INVALID_HANDLE_VALUE)
        return kill_file(idx, seq, "append-open", GetLastError());
    ok = seek_to(h, 0, FILE_END) && write_all(h, f->data + off, len);
    if (!ok) err = GetLastError();
    CloseHandle(h);
    if (!ok)
        return kill_file(idx, seq, "append", err);
    log_done(seq);
    return f->id;
}

static uint32_t op_trunc(uint32_t idx, uint64_t seq)
{
    FileRec *f = &g_files[idx];
    char rel[160], full[MAX_PATH + 160];
    HANDLE h;
    BOOL ok;
    DWORD err = 0;
    size_t nsz = (rnd(4) == 0) ? 0 : (size_t)rnd(f->size);

    file_resize(f, nsz);
    relpath(f, rel, sizeof(rel));
    log_line("OP\t%llu\t%u\tTRUNC\t%s", (unsigned long long)seq, f->id, rel);
    fullpath(rel, full, sizeof(full));
    h = open_f(full, GENERIC_WRITE, OPEN_EXISTING);
    if (h == INVALID_HANDLE_VALUE)
        return kill_file(idx, seq, "trunc-open", GetLastError());
    ok = seek_to(h, (LONGLONG)nsz, FILE_BEGIN) && SetEndOfFile(h);
    if (!ok) err = GetLastError();
    CloseHandle(h);
    if (!ok)
        return kill_file(idx, seq, "trunc", err);
    log_done(seq);
    return f->id;
}

static uint32_t op_rewrite(uint32_t idx, uint64_t seq)
{
    FileRec *f = &g_files[idx];
    char rel[160], full[MAX_PATH + 160];
    HANDLE h;
    BOOL ok;
    DWORD err = 0;

    f->ver++;
    file_resize(f, 0);
    file_resize(f, rand_size(g_maxsize));
    fill_pattern(f->data, f->size, f->id, f->ver);

    relpath(f, rel, sizeof(rel));
    log_line("OP\t%llu\t%u\tREWRITE\t%s", (unsigned long long)seq, f->id, rel);
    fullpath(rel, full, sizeof(full));
    h = open_f(full, GENERIC_WRITE, CREATE_ALWAYS);
    if (h == INVALID_HANDLE_VALUE)
        return kill_file(idx, seq, "rewrite-open", GetLastError());
    ok = write_all(h, f->data, f->size);
    if (!ok) err = GetLastError();
    CloseHandle(h);
    if (!ok)
        return kill_file(idx, seq, "rewrite", err);
    log_done(seq);
    return f->id;
}

static uint32_t op_rename(uint32_t idx, uint64_t seq)
{
    FileRec *f = &g_files[idx];
    char rel[160], nrel[160], full[MAX_PATH + 160], nfull[MAX_PATH + 160];
    char nname[64];
    int ndir = pick_dir();

    snprintf(nname, sizeof(nname), "f%u_r%u.bin", f->id, ++f->nren);
    relpath(f, rel, sizeof(rel));
    if (ndir == 0)
        snprintf(nrel, sizeof(nrel), "%s", nname);
    else
        snprintf(nrel, sizeof(nrel), "%s/%s", g_dirnames[ndir], nname);

    log_line("OP\t%llu\t%u\tRENAME\t%s\t%s", (unsigned long long)seq, f->id, rel, nrel);
    fullpath(rel, full, sizeof(full));
    fullpath(nrel, nfull, sizeof(nfull));
    if (!MoveFileExA(full, nfull, 0)) {
        note_error(seq, f->id, "rename", GetLastError());
        return 0;
    }
    f->dir = ndir;
    snprintf(f->name, sizeof(f->name), "%s", nname);
    log_done(seq);
    return f->id;
}

static uint32_t op_replace(uint32_t idx, uint32_t vidx, uint64_t seq)
{
    FileRec *f = &g_files[idx];
    FileRec *v = &g_files[vidx];
    char rel[160], vrel[160], full[MAX_PATH + 160], vfull[MAX_PATH + 160];
    uint32_t id = f->id, vid = v->id;
    int vdir = v->dir;
    char vname[64];

    snprintf(vname, sizeof(vname), "%s", v->name);
    relpath(f, rel, sizeof(rel));
    relpath(v, vrel, sizeof(vrel));
    log_line("OP\t%llu\t%u\tREPLACE\t%s\t%s", (unsigned long long)seq, id, rel, vrel);
    log_line("OP\t%llu\t%u\tVICTIM\t%s", (unsigned long long)seq, vid, vrel);
    fullpath(rel, full, sizeof(full));
    fullpath(vrel, vfull, sizeof(vfull));
    if (!MoveFileExA(full, vfull, MOVEFILE_REPLACE_EXISTING)) {
        note_error(seq, id, "replace", GetLastError());
        return 0;
    }
    f->dir = vdir;
    snprintf(f->name, sizeof(f->name), "%s", vname);
    log_done(seq);
    log_line("DEAD\t%llu\t%u", (unsigned long long)seq, vid);
    remove_idx(vidx);
    return id;
}

static uint32_t op_delete(uint32_t idx, uint64_t seq)
{
    FileRec *f = &g_files[idx];
    char rel[160], full[MAX_PATH + 160];
    uint32_t id = f->id;

    relpath(f, rel, sizeof(rel));
    log_line("OP\t%llu\t%u\tDELETE\t%s", (unsigned long long)seq, id, rel);
    fullpath(rel, full, sizeof(full));
    if (!DeleteFileA(full))
        return kill_file(idx, seq, "delete", GetLastError());
    log_done(seq);
    log_line("DEAD\t%llu\t%u", (unsigned long long)seq, id);
    remove_idx(idx);
    return 0;
}

static void op_mkdir(int d, uint64_t seq)
{
    char full[MAX_PATH + 160];

    log_line("OP\t%llu\t0\tMKDIR\t%s", (unsigned long long)seq, g_dirnames[d]);
    fullpath(g_dirnames[d], full, sizeof(full));
    if (CreateDirectoryA(full, NULL) || GetLastError() == ERROR_ALREADY_EXISTS) {
        g_dir_exists[d] = 1;
        log_done(seq);
    } else {
        note_error(seq, 0, "mkdir", GetLastError());
    }
}

static void op_rmdir(int d, uint64_t seq)
{
    char full[MAX_PATH + 160];

    log_line("OP\t%llu\t0\tRMDIR\t%s", (unsigned long long)seq, g_dirnames[d]);
    fullpath(g_dirnames[d], full, sizeof(full));
    if (RemoveDirectoryA(full)) {
        g_dir_exists[d] = 0;
        log_done(seq);
    } else {
        note_error(seq, 0, "rmdir", GetLastError());
    }
}

static void maybe_flush(uint64_t seq, uint32_t id)
{
    int idx;
    FileRec *f;
    char rel[160], full[MAX_PATH + 160], hex[65];
    HANDLE h;
    BOOL ok;
    DWORD err = 0;

    if (id == 0 || rnd01() >= g_flushprob) return;
    idx = find_idx(id);
    if (idx < 0) return;
    f = &g_files[idx];
    relpath(f, rel, sizeof(rel));
    fullpath(rel, full, sizeof(full));
    h = open_f(full, GENERIC_WRITE, OPEN_EXISTING);
    if (h == INVALID_HANDLE_VALUE) {
        kill_file((uint32_t)idx, seq, "flush-open", GetLastError());
        return;
    }
    ok = FlushFileBuffers(h);
    if (!ok) err = GetLastError();
    CloseHandle(h);
    if (!ok) {
        note_error(seq, id, "flush", err);
        return;
    }
    sha256_hex(f->data, f->size, hex);
    log_line("FLUSHED\t%llu\t%u\t%s\t%llu\t%s", (unsigned long long)seq, id, rel,
             (unsigned long long)f->size, hex);
}

/* ------------------------------------------------------------------ */
/* startup helpers                                                    */
/* ------------------------------------------------------------------ */

static void wipe_dir(const char *dir)
{
    char pat[MAX_PATH + 8], child[MAX_PATH + 8];
    WIN32_FIND_DATAA fd;
    HANDLE h;

    snprintf(pat, sizeof(pat), "%s\\*", dir);
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        snprintf(child, sizeof(child), "%s\\%s", dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                wipe_dir(child);
            RemoveDirectoryA(child);
        } else {
            SetFileAttributesA(child, FILE_ATTRIBUTE_NORMAL);
            DeleteFileA(child);
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

static int parse_size(const char *s, size_t *out)
{
    char *end;
    unsigned long long v = strtoull(s, &end, 10);

    if (end == s) return 0;
    if (*end == 'k' || *end == 'K') { v <<= 10; end++; }
    else if (*end == 'm' || *end == 'M') { v <<= 20; end++; }
    else if (*end == 'g' || *end == 'G') { v <<= 30; end++; }
    if (*end == 'b' || *end == 'B') end++;
    if (*end) return 0;
    if (v == 0 || v > 0x7fff0000ull) return 0;
    *out = (size_t)v;
    return 1;
}

static void usage(void)
{
    fprintf(stderr,
        "usage: crashgen --root DIR --log FILE --seed N [--max-files 300]\n"
        "                [--max-size 2M] [--flush-prob 0.2] [--ops 0]\n");
}

int main(int argc, char **argv)
{
    int i, have_root = 0, have_log = 0, have_seed = 0;
    uint64_t seq = 0, done = 0;
    ULONGLONG last_hb = 0, last_print = 0;
    char bak[MAX_PATH + 8], *p;
    SYSTEMTIME st;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = (i + 1 < argc) ? argv[i + 1] : NULL;
        if (!v) { usage(); return 1; }
        if (!strcmp(a, "--root")) {
            snprintf(g_root, sizeof(g_root), "%s", v); have_root = 1;
        } else if (!strcmp(a, "--log")) {
            snprintf(g_logpath, sizeof(g_logpath), "%s", v); have_log = 1;
        } else if (!strcmp(a, "--seed")) {
            g_seed = strtoull(v, NULL, 0); have_seed = 1;
        } else if (!strcmp(a, "--max-files")) {
            g_maxfiles = (uint32_t)strtoul(v, NULL, 0);
        } else if (!strcmp(a, "--max-size")) {
            if (!parse_size(v, &g_maxsize)) { usage(); return 1; }
        } else if (!strcmp(a, "--flush-prob")) {
            g_flushprob = strtod(v, NULL);
        } else if (!strcmp(a, "--ops")) {
            g_ops = strtoull(v, NULL, 0);
        } else {
            usage();
            return 1;
        }
        i++;
    }
    if (!have_root || !have_log || !have_seed || g_maxfiles < 2) { usage(); return 1; }

    /* strip trailing slashes from root; refuse a drive root */
    win_path(g_root);
    win_path(g_logpath);
    {
        size_t n = strlen(g_root);
        while (n > 0 && g_root[n - 1] == '\\') g_root[--n] = 0;
        if (n <= 2) {
            fprintf(stderr, "crashgen: refusing to use a drive root as --root\n");
            return 1;
        }
    }
    if ((double)g_maxfiles * (double)g_maxsize > 1024.0 * 1024.0 * 1024.0)
        fprintf(stderr, "crashgen: warning: max-files x max-size exceeds 1 GiB of RAM\n");

    if (BCryptOpenAlgorithmProvider(&g_halg, BCRYPT_SHA256_ALGORITHM, NULL, 0) != 0) {
        fprintf(stderr, "crashgen: BCryptOpenAlgorithmProvider failed\n");
        return 2;
    }
    g_files = (FileRec *)calloc(g_maxfiles, sizeof(FileRec));
    if (!g_files) { fprintf(stderr, "crashgen: out of memory\n"); return 2; }
    g_rng.s = g_seed * 0x2545F4914F6CDD1Dull + 12345;

    /* log directory, heartbeat path, rotate previous log, open new log */
    snprintf(g_logdir, sizeof(g_logdir), "%s", g_logpath);
    p = strrchr(g_logdir, '\\');
    if (p) *p = 0; else snprintf(g_logdir, sizeof(g_logdir), ".");
    mkdir_p(g_logdir);
    snprintf(g_alivepath, sizeof(g_alivepath), "%s\\alive", g_logdir);
    snprintf(bak, sizeof(bak), "%s.prev", g_logpath);
    MoveFileExA(g_logpath, bak, MOVEFILE_REPLACE_EXISTING);
    g_log = CreateFileA(g_logpath, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, NULL);
    if (g_log == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "crashgen: cannot open log %s (error %lu)\n", g_logpath, GetLastError());
        return 2;
    }

    GetSystemTime(&st);
    log_line("HEADER\tstart=%04u-%02u-%02uT%02u:%02u:%02uZ\tseed=%llu\troot=%s\tmax_files=%u\t"
             "max_size=%llu\tflush_prob=%g\tops=%llu",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
             (unsigned long long)g_seed, g_root, g_maxfiles,
             (unsigned long long)g_maxsize, g_flushprob, (unsigned long long)g_ops);

    /* fresh root: create and wipe whatever a previous run left behind */
    mkdir_p(g_root);
    wipe_dir(g_root);
    heartbeat();

    for (;;) {
        enum { K_CREATE, K_WRITE, K_APPEND, K_TRUNC, K_REWRITE, K_RENAME,
               K_REPLACE, K_DELETE, K_MKDIR, K_RMDIR, K_N };
        unsigned w[K_N];
        unsigned total = 0, r, kind, d;
        unsigned frac;
        uint32_t idx = 0, fid = 0;
        int mk_cand = 0, rm_cand = 0, mkd = 0, rmd = 0;
        ULONGLONG now;

        if (g_ops && done >= g_ops) break;

        now = GetTickCount64();
        if (now - last_hb >= 5000) {
            heartbeat();
            last_hb = now;
        }
        if (now - last_print >= 5000) {
            printf("seq=%llu files=%u next_id=%u\n", (unsigned long long)seq, g_nfiles, g_next_id);
            fflush(stdout);
            last_print = now;
        }

        frac = (unsigned)((uint64_t)g_nfiles * 100 / g_maxfiles);
        for (d = 1; d < NDIRS; d++) {
            if (!g_dir_exists[d]) { if (!mk_cand++ || rnd((uint64_t)mk_cand) == 0) mkd = (int)d; }
            else if (dir_file_count((int)d) == 0) { if (!rm_cand++ || rnd((uint64_t)rm_cand) == 0) rmd = (int)d; }
        }
        w[K_CREATE]  = g_nfiles < g_maxfiles ? 2 + 30 * (100 - frac) / 100 : 0;
        w[K_WRITE]   = g_nfiles ? 20 : 0;
        w[K_APPEND]  = g_nfiles ? 18 : 0;
        w[K_TRUNC]   = g_nfiles ? 8 : 0;
        w[K_REWRITE] = g_nfiles ? 8 : 0;
        w[K_RENAME]  = g_nfiles ? 10 : 0;
        w[K_REPLACE] = g_nfiles >= 2 ? 5 : 0;
        w[K_DELETE]  = g_nfiles ? 4 + 20 * frac / 100 : 0;
        w[K_MKDIR]   = mk_cand ? 2 : 0;
        w[K_RMDIR]   = rm_cand ? 1 : 0;
        for (kind = 0; kind < K_N; kind++) total += w[kind];
        r = (unsigned)rnd(total);
        for (kind = 0; kind < K_N; kind++) {
            if (r < w[kind]) break;
            r -= w[kind];
        }

        if (g_nfiles) idx = (uint32_t)rnd(g_nfiles);

        /* fix up kinds whose preconditions fail for the chosen file */
        if (kind == K_WRITE && g_files[idx].size == 0) kind = K_APPEND;
        if (kind == K_APPEND && g_files[idx].size >= g_maxsize) kind = K_TRUNC;
        if (kind == K_TRUNC && g_files[idx].size == 0) kind = K_REWRITE;

        seq++;
        done++;
        switch (kind) {
        case K_CREATE:  fid = op_create(seq); break;
        case K_WRITE:   fid = op_write(idx, seq); break;
        case K_APPEND:  fid = op_append(idx, seq); break;
        case K_TRUNC:   fid = op_trunc(idx, seq); break;
        case K_REWRITE: fid = op_rewrite(idx, seq); break;
        case K_RENAME:  fid = op_rename(idx, seq); break;
        case K_REPLACE: {
            uint32_t vidx = (uint32_t)rnd(g_nfiles - 1);
            if (vidx >= idx) vidx++;
            fid = op_replace(idx, vidx, seq);
            break;
        }
        case K_DELETE:  fid = op_delete(idx, seq); break;
        case K_MKDIR:   op_mkdir(mkd, seq); break;
        default:        op_rmdir(rmd, seq); break;
        }
        maybe_flush(seq, fid);
    }

    log_line("END\t%llu", (unsigned long long)seq);
    CloseHandle(g_log);
    return 0;
}
