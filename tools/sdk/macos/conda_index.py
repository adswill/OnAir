# SPDX-License-Identifier: GPL-3.0-or-later
"""macos/conda_index.py - resolve conda-forge packages for a macOS target sysroot (used by macos/fetch_sysroot.sh).

    python3 -I conda_index.py resolve REPODATA.json[,NOARCH.json] --macos-min 11.0 [--exclude GLOB ...] SPEC ...
    python3 -I conda_index.py vcmp A B          # prints -1, 0 or 1 (conda's version order)

    python3 -I conda_index.py unpack PKG DEST NAME   # unpack a .conda/.tar.bz2 into DEST (the sysroot's usr/local)

REPODATA.json is conda-forge's <subdir>/repodata.json (unpacked), and noarch/repodata.json after a comma; the file
column is <subdir>/<file>. SPEC is a conda match spec: "glfw",
"openssl >=3.5", "ffmpeg * gpl_*", "ffmpeg=*=lgpl_*". The dependencies come along, resolved like conda does it for a
fresh environment: a backtracking search (with conflict-directed backjumping) that settles the specs first, in their
order, each to its newest record that still has a solution, then their dependencies, newest first (version, build
number, timestamp). Every record must fit every constraint on its name: the specs, the "depends" and "constrains" of
the chosen records. The virtual package __osx is the deployment target (--macos-min): records needing a newer macOS
are not taken. Names matching --exclude are left out (runtime-only
packages such as python or fonts). Prints one line per package: name version build file sha256 size license.
Exits 1 with the constraints when a name has no record that fits.
"""
import fnmatch
import json
import os
import re
import sys

# --- conda's version order (conda.models.version.VersionOrder) ------------------------------------------------------


def _split(v):
    v = v.strip().lower()
    epoch, _, v = v.rpartition("!") if "!" in v else ("0", "", v)
    v, _, local = v.partition("+")
    out = []
    for part in (v, local):
        comps = []
        for c in part.replace("_", ".").split(".") if part else []:
            items = [int(x) if x.isdigit() else x for x in re.findall(r"\d+|[^\d]+", c)] or [0]
            if not isinstance(items[0], int):
                items.insert(0, 0)
            comps.append(items)
        out.append(comps)
    return [[int(epoch) if epoch.isdigit() else 0]] + out[0], out[1]


def _key(x):
    # dev < other strings (alphabetical) < numbers < post
    if isinstance(x, int):
        return (2, x, "")
    if x == "dev":
        return (0, 0, "")
    if x == "post":
        return (3, 0, "")
    return (1, 0, x)


def _cmp_lists(a, b):
    n = max(len(a), len(b))
    for i in range(n):
        ca = a[i] if i < len(a) else [0]
        cb = b[i] if i < len(b) else [0]
        m = max(len(ca), len(cb))
        for j in range(m):
            xa = _key(ca[j] if j < len(ca) else 0)
            xb = _key(cb[j] if j < len(cb) else 0)
            if xa != xb:
                return -1 if xa < xb else 1
    return 0


def vcmp(a, b):
    ma, la = _split(a)
    mb, lb = _split(b)
    return _cmp_lists(ma, mb) or _cmp_lists(la, lb)


# --- match specs ------------------------------------------------------------------------------------------------------


def _one(constraint, v):
    c = constraint.strip()
    if c in ("", "*"):
        return True
    m = re.match(r"^(>=|<=|==|!=|~=|>|<|=)?\s*(.+)$", c)
    op, ver = m.group(1) or "", m.group(2)
    if ver.endswith(".*"):
        ver, op = ver[:-2], {"": "=", "==": "=", "!=": "!=*"}.get(op, op)
    elif ver.endswith("*"):
        ver, op = ver[:-1].rstrip("."), {"": "=", "==": "="}.get(op, op)
    if op == "=":      # prefix match: 1.2 matches 1.2, 1.2.3; not 1.20
        pv, pc = _split(ver)[0], _split(v)[0]
        return len(pc) >= len(pv) and all(_cmp_lists([pv[i]], [pc[i]]) == 0 for i in range(len(pv)))
    if op == "!=*":
        return not _one("=" + ver, v)
    r = vcmp(v, ver)
    if op == "~=":     # compatible release: >= ver and == ver's prefix without its last part
        pre = ver.rsplit(".", 1)[0]
        return r >= 0 and _one("=" + pre, v)
    return {"": r == 0, "==": r == 0, "!=": r != 0, ">=": r >= 0, "<=": r <= 0, ">": r > 0, "<": r < 0}[op]


def version_matches(spec, v):
    """spec: "1.2.*", ">=1.0,<2.0a0", "1.0|1.2", "*"; and binds tighter than or."""
    return any(all(_one(c, v) for c in alt.split(",")) for alt in spec.split("|"))


def parse_spec(s):
    """'name [version [build]]' or 'name==version' -> (name, version spec, build glob)"""
    s = s.strip()
    m = re.match(r"^([A-Za-z0-9_.\-]+)=([^=<>!~,|\s][^=\s]*)(?:=(\S+))?$", s)   # name=version[=build] (conda's CLI form)
    if m:
        return m.group(1), m.group(2) if m.group(2).endswith("*") else m.group(2) + "*", m.group(3) or "*"
    if " " not in s:
        m = re.match(r"^([A-Za-z0-9_.\-]+)(.*)$", s)
        name, rest = m.group(1), m.group(2)
        return name, (rest if rest else "*"), "*"
    p = s.split()
    return p[0], (p[1] if len(p) > 1 else "*"), (p[2] if len(p) > 2 else "*")


def matches(spec, rec):
    _, ver, build = spec
    return version_matches(ver, rec["version"]) and fnmatch.fnmatchcase(rec["build"], build)


# --- resolution -------------------------------------------------------------------------------------------------------


def resolve(repodata, specs, macos_min, exclude):
    by_name = {}
    for key, rd in ((k, rd) for rd in repodata for k in ("packages", "packages.conda")):
        sub = rd.get("info", {}).get("subdir", "")
        for fn, r in rd.get(key, {}).items():
            r = dict(r, fn=f"{r.get('subdir', sub)}/{fn}")
            by_name.setdefault(r["name"], {})[r["fn"].rsplit(".", 2 if fn.endswith(".tar.bz2") else 1)[0]] = r
    # one record per (name, version, build): .conda preferred over .tar.bz2 (the dict above keeps the last; redo)
    recs = {}
    for name, d in by_name.items():
        best = {}
        for stem, r in d.items():
            k = (r["version"], r["build"])
            if k not in best or best[k]["fn"].endswith(".tar.bz2"):
                best[k] = r
        recs[name] = list(best.values())

    def excluded(n):
        return any(fnmatch.fnmatchcase(n, g) for g in exclude)

    def osx_ok(r):
        for d in r.get("depends", []):
            n, v, _ = parse_spec(d)
            if n == "__osx" and not version_matches(v, macos_min):
                return False
        return True

    import functools

    def newer(a, b):   # sort key helper: newest first (version, build number, timestamp)
        c = vcmp(a["version"], b["version"])
        if c:
            return -c
        ka = (a.get("build_number", 0), a.get("timestamp", 0))
        kb = (b.get("build_number", 0), b.get("timestamp", 0))
        return -1 if ka > kb else (1 if ka < kb else 0)

    sorted_recs = {}

    def candidates(name):
        if name not in sorted_recs:
            sorted_recs[name] = sorted((r for r in recs.get(name, []) if osx_ok(r)), key=functools.cmp_to_key(newer))
        return sorted_recs[name]

    def deps(r):
        out = []
        for d in r.get("depends", []):
            sp = parse_spec(d)
            if not sp[0].startswith("__") and not excluded(sp[0]):
                out.append(sp)
        return out

    # backtracking search with conflict-directed backjumping. A name with constraints but no record yet is "open"; the
    # specs are settled first (in their order, so the first spec gets its newest fitting record), then the open name with
    # the fewest fitting records. Records are tried newest first. A record must fit every constraint on its name: the
    # specs, the "depends" of the chosen records, and the "constrains" of the chosen records. A dead end returns the
    # names whose choices caused it, so the search jumps straight back to the latest of them.
    roots = [parse_spec(s) for s in specs]
    for sp in roots:
        if sp[0] not in recs:
            sys.exit(f"error: conda-forge has no package {sp[0]}")
    budget = [200000]
    failed_on = {}

    def fits(r, cons):
        return all(matches(sp, r) for sp, _ in cons.get(r["name"], []))

    def search(chosen, cons):
        budget[0] -= 1
        if budget[0] < 0:
            sys.exit("error: the resolution takes too long (no solution found in 200000 steps)")
        open_names = [n for n in cons if n not in chosen]
        if not open_names:
            return chosen, None
        best_n, best_c = None, None
        first = [sp[0] for sp in roots if sp[0] in open_names][:1]
        for n in first or open_names:
            c = [r for r in candidates(n) if fits(r, cons)]
            if best_c is None or len(c) < len(best_c):
                best_n, best_c = n, c
            if not c:
                break
        n = best_n
        conflict = {src for _, src in cons.get(n, []) if src != "asked"}   # who limited n's records
        if not best_c:
            failed_on[n] = cons.get(n, [])
            return None, conflict
        for r in best_c:
            bad = None
            new_cons = {k: list(v) for k, v in cons.items()}
            for sp in deps(r):
                new_cons.setdefault(sp[0], []).append((sp, n))
                if sp[0] in chosen and not matches(sp, chosen[sp[0]]):
                    bad = sp[0]
                    break
            if bad is None:
                for d in r.get("constrains", []):
                    sp = parse_spec(d)
                    if sp[0] in chosen and not matches(sp, chosen[sp[0]]):
                        bad = sp[0]
                        break
            if bad is None:
                for c in chosen.values():
                    if any(parse_spec(d)[0] == n and not matches(parse_spec(d), r) for d in c.get("constrains", [])):
                        bad = c["name"]
                        break
            if bad is not None:
                conflict.add(bad)
                continue
            for d in r.get("constrains", []):   # constrains on names not chosen yet: they bind when those are chosen
                sp = parse_spec(d)
                if sp[0] in new_cons:
                    new_cons[sp[0]].append((sp, n))
            res, c = search({**chosen, n: r}, new_cons)
            if res is not None:
                return res, None
            if n not in c:
                return None, c                  # this choice is not part of the conflict: jump back over it
            conflict |= c - {n}
        failed_on.setdefault(n, cons.get(n, []))
        return None, conflict

    cons0 = {}
    for sp in roots:
        cons0.setdefault(sp[0], []).append((sp, "asked"))
    res, _ = search({}, cons0)
    if res is None:
        msg = ["error: no solution. Names that had no fitting record:"]
        for n, cs in failed_on.items():
            msg.append(f"  {n}: " + "; ".join(f"{' '.join(sp[1:])} (from {src})" for sp, src in cs))
        sys.exit("\n".join(msg))
    return [res[n] for n in sorted(res)]


# --- unpacking a package into the sysroot's usr/local -------------------------------------------------------------------
# kept: headers, libraries (dylibs, static libraries, pkg-config, CMake), the licences (info/licenses). Left out:
# programs (bin/), Python, docs, man pages, locales, etc/, libexec/, libtool .la files.
KEEP = ("include/", "lib/", "share/pkgconfig/", "share/cmake", "share/licenses/")
DROP = ("lib/python", "lib/cmake/Python", "lib/girepository", "lib/gio/", "lib/gdk-pixbuf")


def keep(rel):
    return rel.startswith(KEEP) and not rel.startswith(DROP) and not rel.endswith(".la") and "/__pycache__/" not in rel


def unpack(pkg, dest, name):
    import shutil
    import tarfile
    import tempfile
    import zipfile
    with tempfile.TemporaryDirectory(dir=os.path.dirname(os.path.abspath(dest))) as t:
        x = os.path.join(t, "x")
        if pkg.endswith(".conda"):
            with zipfile.ZipFile(pkg) as z:
                for member in z.namelist():
                    if member.endswith(".tar.zst") and "/" not in member:
                        z.extract(member, t)
                        tz = os.path.join(t, member)
                        with tarfile.open(tz, mode="r:zst") as tf:   # (Python 3.14's own zstd)
                            tf.extractall(x, filter="data")
        else:
            with tarfile.open(pkg, "r:bz2") as tf:
                tf.extractall(x, filter="data")
        paths = {}
        pj = os.path.join(x, "info", "paths.json")
        if os.path.exists(pj):
            for p in json.load(open(pj)).get("paths", []):
                paths[p["_path"]] = p
        n = binary_ph = 0
        for root, dirs, files in os.walk(x):
            for fn in files + [d for d in dirs if os.path.islink(os.path.join(root, d))]:
                src = os.path.join(root, fn)
                rel = os.path.relpath(src, x)
                if rel.startswith("info/licenses/"):
                    out = os.path.join("share", "licenses", name, rel[len("info/licenses/"):])
                elif keep(rel):
                    out = rel
                else:
                    continue
                d = os.path.join(dest, out)
                os.makedirs(os.path.dirname(d), exist_ok=True)
                if os.path.lexists(d):
                    os.remove(d)
                info = paths.get(rel, {})
                ph = info.get("prefix_placeholder")
                if os.path.islink(src):
                    os.symlink(os.readlink(src), d)
                elif ph and info.get("file_mode") == "text":
                    data = open(src, "rb").read()
                    up = os.path.relpath(".", os.path.dirname(out))
                    if out.endswith(".pc"):
                        new = "${pcfiledir}/" + up
                    elif out.endswith(".cmake"):
                        new = "${CMAKE_CURRENT_LIST_DIR}/" + up
                    else:
                        new = "/usr/local"      # the sysroot's usr/local, as seen from the target
                    open(d, "wb").write(data.replace(ph.encode(), new.encode()))
                    shutil.copymode(src, d)
                else:
                    if ph:
                        binary_ph += 1          # a binary with the build prefix compiled in (e.g. OpenSSL's OPENSSLDIR)
                    shutil.copy2(src, d)
                n += 1
        print(n, binary_ph)


def main():
    a = sys.argv[1:]
    if a[:1] == ["unpack"] and len(a) == 4:
        unpack(a[1], a[2], a[3])
        return
    if a[:1] == ["vcmp"]:
        print(vcmp(a[1], a[2]))
        return
    if a[:1] != ["resolve"] or len(a) < 2:
        sys.exit(__doc__)
    path, a = a[1], a[2:]
    macos_min, exclude, specs = "11.0", [], []
    while a:
        if a[0] == "--macos-min":
            macos_min, a = a[1], a[2:]
        elif a[0] == "--exclude":
            exclude.append(a[1])
            a = a[2:]
        else:
            specs.append(a[0])
            a = a[1:]
    repodata = []
    for p in path.split(","):
        with open(p) as f:
            repodata.append(json.load(f))
    for r in resolve(repodata, specs, macos_min, exclude):
        lic = (r.get("license") or "unknown").replace(" ", "_")
        print(r["name"], r["version"], r["build"], r["fn"], r["sha256"], r.get("size", 0), lic)


if __name__ == "__main__":
    main()
