#!/usr/bin/env python3
"""verify.py - check that every file crashgen flushed is intact.

Usage:
    verify.py --log run.log --mnt /mnt/crashtest [--subdir crash] [--all]

Runs inside WSL (or anywhere python3 runs; --mnt may be any directory, so it
also works on Windows paths).  Stdlib only.

Rules (see README.md):
  * For each id take its LAST "FLUSHED" record.
  * Any later OP line for the id (even without DONE - the op may have been
    partially applied) with a content-changing kind (WRITE, APPEND, TRUNC,
    REWRITE, VICTIM, DELETE, or an unknown kind) excludes the id from checking.
    A DEAD record after the flush also excludes it.
  * Later RENAME / REPLACE ops (the moving file) only extend the set of
    acceptable paths: the file may be at the flushed path or at any later
    path of its rename chain.
  * A checked id passes if one of its acceptable paths exists with the
    flushed size and sha256.

--subdir: directory below --mnt that corresponds to crashgen's --root.  If not
given it is derived from the HEADER line's root= (drive letter stripped), e.g.
root=E:\\crash -> "crash".  Pass --subdir "" to use --mnt directly.

Output: JSON {checked, ok, missing, mismatched, details[], ...}.
Exit status: 0 iff every checked file is ok; 1 if any is missing/mismatched;
2 on usage/IO errors.
"""
import argparse
import hashlib
import json
import os
import sys

PATH_KINDS = ("RENAME", "REPLACE")


def derive_subdir(root):
    r = root.replace("\\", "/")
    if len(r) >= 2 and r[1] == ":":
        r = r[2:]
    return r.strip("/")


def parse_log(text, ends_with_newline):
    """Return (header dict, per-id state dict, stats)."""
    header = {}
    ids = {}
    stats = {"lines": 0, "flushed_records": 0, "torn_tail": False}
    lines = text.split("\n")
    if lines and lines[-1] == "":
        lines.pop()
    last_index = len(lines) - 1
    for n, raw in enumerate(lines):
        line = raw.rstrip("\r")
        if not line:
            continue
        stats["lines"] += 1
        torn = (n == last_index and not ends_with_newline)
        if torn:
            stats["torn_tail"] = True
        f = line.split("\t")
        kind = f[0]
        if kind == "HEADER":
            for kv in f[1:]:
                if "=" in kv:
                    k, v = kv.split("=", 1)
                    header[k] = v
        elif kind == "FLUSHED":
            if torn or len(f) != 6:
                continue
            try:
                fid = int(f[2])
                size = int(f[4])
            except ValueError:
                continue
            sha = f[5].lower()
            if len(sha) != 64 or any(c not in "0123456789abcdef" for c in sha):
                continue
            stats["flushed_records"] += 1
            ids[fid] = {
                "path": f[3], "size": size, "sha": sha, "seq": f[1],
                "chain": [f[3]], "tainted": False, "taint": None, "dead": False,
            }
        elif kind == "OP":
            if len(f) < 4:
                # possibly a torn tail: conservatively taint the id if parsable
                if len(f) >= 3:
                    try:
                        st = ids.get(int(f[2]))
                    except ValueError:
                        st = None
                    if st:
                        st["tainted"] = True
                        st["taint"] = "torn OP"
                continue
            try:
                fid = int(f[2])
            except ValueError:
                continue
            st = ids.get(fid)
            if st is None or fid == 0:
                continue
            okind = f[3]
            if torn:
                st["tainted"] = True
                st["taint"] = "torn OP " + okind
            elif okind in PATH_KINDS:
                if len(f) >= 6:
                    st["chain"].append(f[5])
                else:
                    st["tainted"] = True
                    st["taint"] = "malformed " + okind
            else:
                st["tainted"] = True
                st["taint"] = okind
        elif kind == "DEAD":
            if torn or len(f) < 3:
                continue
            try:
                st = ids.get(int(f[2]))
            except ValueError:
                continue
            if st:
                st["dead"] = True
    return header, ids, stats


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as fp:
        while True:
            b = fp.read(1 << 20)
            if not b:
                break
            h.update(b)
    return h.hexdigest()


def check_id(fid, st, base):
    tried = []
    first_bad = None
    seen = set()
    for rel in st["chain"]:
        if rel in seen:
            continue
        seen.add(rel)
        full = os.path.join(base, *rel.split("/"))
        try:
            real = os.stat(full)
        except OSError:
            tried.append({"path": rel, "state": "absent"})
            continue
        if real.st_size != st["size"]:
            tried.append({"path": rel, "state": "size", "actual_size": real.st_size})
            if first_bad is None:
                first_bad = tried[-1]
            continue
        try:
            actual = sha256_file(full)
        except OSError as e:
            tried.append({"path": rel, "state": "unreadable", "error": str(e)})
            if first_bad is None:
                first_bad = tried[-1]
            continue
        if actual != st["sha"]:
            tried.append({"path": rel, "state": "sha", "actual_sha": actual})
            if first_bad is None:
                first_bad = tried[-1]
            continue
        return "ok", None
    detail = {
        "id": fid,
        "flushed_seq": st["seq"],
        "expected_size": st["size"],
        "expected_sha": st["sha"],
        "paths_tried": tried,
    }
    if first_bad is None:
        detail["status"] = "missing"
        return "missing", detail
    detail["status"] = "mismatch"
    return "mismatched", detail


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--log", required=True)
    ap.add_argument("--mnt", required=True)
    ap.add_argument("--subdir", default=None)
    ap.add_argument("--all", action="store_true",
                    help="list ok entries in details too")
    a = ap.parse_args()

    try:
        with open(a.log, "rb") as fp:
            raw = fp.read()
    except OSError as e:
        print(json.dumps({"error": "cannot read log: %s" % e}))
        return 2
    text = raw.decode("utf-8", errors="replace")
    header, ids, stats = parse_log(text, raw.endswith(b"\n") or not raw)

    subdir = a.subdir
    if subdir is None:
        subdir = derive_subdir(header.get("root", ""))
    base = a.mnt
    if subdir:
        base = os.path.join(base, *subdir.replace("\\", "/").split("/"))
    if not os.path.isdir(base):
        print(json.dumps({"error": "directory not found: %s" % base,
                          "header": header}))
        return 2

    warnings = []
    if not header:
        warnings.append("log has no HEADER line (empty or stale log?)")

    checked = ok = missing = mismatched = skipped = 0
    details = []
    for fid in sorted(ids):
        st = ids[fid]
        if st["tainted"] or st["dead"]:
            skipped += 1
            continue
        checked += 1
        res, detail = check_id(fid, st, base)
        if res == "ok":
            ok += 1
            if a.all:
                details.append({"id": fid, "status": "ok", "path": st["path"]})
        elif res == "missing":
            missing += 1
            details.append(detail)
        else:
            mismatched += 1
            details.append(detail)
    if stats["flushed_records"] == 0:
        warnings.append("log contains no FLUSHED records; nothing to verify")

    out = {
        "checked": checked,
        "ok": ok,
        "missing": missing,
        "mismatched": mismatched,
        "skipped": skipped,
        "flushed_ids": len(ids),
        "log_lines": stats["lines"],
        "torn_tail": stats["torn_tail"],
        "base": base,
        "header": header,
        "warnings": warnings,
        "details": details,
    }
    print(json.dumps(out, indent=2, sort_keys=False))
    return 0 if (missing == 0 and mismatched == 0) else 1


if __name__ == "__main__":
    sys.exit(main())
