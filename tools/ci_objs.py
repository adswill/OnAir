#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

"""One part of the compile of a CI selection, for the compile jobs of ci.yml (docs/CI-SPEED-4.7.md).

The test shards all need the same core library (2200 files) and many of the same tests; when a shared header changes, every shard
compiled all of it again on 3-4 cores (55-63 min on macOS and Windows). The compile jobs split that work: part k of N compiles every N-th
object file (by estimated cost: source size) of the targets the whole selection needs, into a ccache that the shards then read, so a
shard's build is cache hits and links.
  tools/ci_objs.py --build-dir build --targets sel/targets.txt --part k/N [--extra dect2]   -> object paths, one per line (build-relative)
"""
import argparse, os, re, subprocess, sys

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build-dir", required=True)
    ap.add_argument("--targets", required=True, help="file with build targets, one per line (tools/ci_select.py targets.txt)")
    ap.add_argument("--extra", default="", help="more targets, space separated")
    ap.add_argument("--part", default="1/1")
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
        objs[m.group(1)] = cost
    k, n = (int(x) for x in a.part.split("/"))
    load, mine = [0] * n, []
    for o in sorted(objs, key=lambda o: (-objs[o], o)):   # biggest first onto the least loaded part; the same split in every part
        i = min(range(n), key=lambda j: (load[j], j))
        load[i] += objs[o]
        if i == k - 1:
            mine.append(o)
    sys.stderr.write("ci_objs: %d objects in all, part %s has %d\n" % (len(objs), a.part, len(mine)))
    print("\n".join(sorted(mine)))

if __name__ == "__main__":
    main()
