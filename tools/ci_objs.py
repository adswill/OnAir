#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

"""One part of the compile of a CI selection, for the compile jobs of ci.yml (docs/CI-FAST.md (4.7)).

The test shards all need the same core library (2200 files) and many of the same tests; when a shared header changes, every shard
compiled all of it again on 3-4 cores (55-63 min on macOS and Windows). The compile jobs split that work: part k of N compiles every N-th
object file (by estimated cost: source size) of the targets the whole selection needs, into a ccache that the shards then read, so a
shard's build is cache hits and links.
  tools/ci_objs.py --build-dir build --targets sel/targets.txt --part k/N [--extra dect2]   -> object paths, one per line (build-relative)
  ... --run [-j 4]   compiles them: each object's own command from compile_commands.json. Not through ninja: there every object waits for
                     the libraries its target links, so each part built the whole core library first (2870 compiles per part instead of 878,
                     run 46003088536). An object that fails here (a header made by a later build step) is left to the shard.
"""
import argparse, json, os, re, subprocess, sys

FIXED_COST = 200000   # bytes of source a C++ file's headers and start-up are worth
FIXED_COST_C = 10000
from concurrent.futures import ThreadPoolExecutor

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build-dir", required=True)
    ap.add_argument("--targets", required=True, help="file with build targets, one per line (tools/ci_select.py targets.txt)")
    ap.add_argument("--extra", default="", help="more targets, space separated")
    ap.add_argument("--part", default="1/1")
    ap.add_argument("--run", action="store_true", help="compile this part's objects (compile_commands.json) instead of printing them")
    ap.add_argument("-j", type=int, default=4)
    a = ap.parse_args()
    tg = [t.strip() for t in open(a.targets) if t.strip()] + a.extra.split()
    if not tg:
        return
    cmds = subprocess.run(["ninja", "-C", a.build_dir, "-t", "commands"] + tg, capture_output=True, text=True)
    if cmds.returncode != 0:
        sys.exit("ci_objs: ninja -t commands failed: " + cmds.stderr.strip()[:400])
    objs = {}
    for line in cmds.stdout.splitlines():
        if " -c " not in line:
            continue
        m = re.search(r"\s-o\s+(\S+\.(?:o|obj))(?:\s|$)", line)
        s = re.search(r"\s-c\s+(\S+)", line)
        if not m:
            continue
        src = s.group(1) if s else ""
        try: cost = os.path.getsize(src if os.path.isabs(src) else os.path.join(a.build_dir, src))
        except OSError: cost = 20000
        # a file's compile is mostly the same ~130,000 lines of headers whatever its own size: per part, 616 large files took 8.4 min and
        # 880 smaller ones 14.5 min when balanced by size alone (run 46018401928), about 0.9 s an object either way. So the count dominates.
        # (C files - Lua, libacars, sqlite - have no such headers: a small fixed part)
        objs[m.group(1)] = (FIXED_COST_C if src.endswith(".c") else FIXED_COST) + cost
    k, n = (int(x) for x in a.part.split("/"))
    load, mine = [0] * n, []
    for o in sorted(objs, key=lambda o: (-objs[o], o)):   # biggest first onto the least loaded part; the same split in every part
        i = min(range(n), key=lambda j: (load[j], j))
        load[i] += objs[o]
        if i == k - 1:
            mine.append(o)
    sys.stderr.write("ci_objs: %d objects in all, part %s has %d\n" % (len(objs), a.part, len(mine)))
    if not a.run:
        print("\n".join(sorted(mine)))
        return
    bd = os.path.abspath(a.build_dir)
    def rel(o): return os.path.relpath(o if os.path.isabs(o) else os.path.join(bd, o), bd).replace(os.sep, "/")
    entries = {}
    for e in json.load(open(os.path.join(bd, "compile_commands.json"))):
        out = e.get("output")
        if not out:
            m = re.search(r"\s-o\s+(\S+)", e.get("command", ""))
            out = m.group(1) if m else None
        if out: entries[rel(out)] = e
    jobs = [entries[rel(o)] for o in mine if rel(o) in entries]
    def one(e):
        out = e.get("output") or re.search(r"\s-o\s+(\S+)", e["command"]).group(1)
        d = e.get("directory", bd)
        os.makedirs(os.path.dirname(os.path.join(d, out)), exist_ok=True)
        cmd = e["command"] if "command" in e else subprocess.list2cmdline(e["arguments"])
        return subprocess.run(cmd, shell=True, cwd=d, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0
    with ThreadPoolExecutor(max_workers=a.j) as ex:
        res = list(ex.map(one, jobs))
    sys.stderr.write("ci_objs: compiled %d of %d (%d left to the shards: failed here, e.g. a generated header)\n" % (sum(res), len(jobs), len(res) - sum(res)))

if __name__ == "__main__":
    main()
