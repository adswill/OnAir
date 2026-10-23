#!/usr/bin/env python3
"""Summarise where build time went, from a ninja build directory (docs/BUILD-SPEED.md).

usage: build_profile.py BUILD_DIR [--since-ms N] [--ctest]

Reads BUILD_DIR/.ninja_log (per-edge start/end in ms; with -j the sum is CPU-ish wall time per edge, not elapsed time)
and splits it into compile (.o), static-library archive and link edges. --ctest also prints the slowest tests from
BUILD_DIR/Testing/Temporary/LastTest.log.
"""
import os
import re
import sys
from collections import defaultdict


def main():
    d = sys.argv[1]
    since = 0
    if "--since-ms" in sys.argv:
        since = int(sys.argv[sys.argv.index("--since-ms") + 1])
    edges = {}
    with open(os.path.join(d, ".ninja_log")) as f:
        for line in f:
            if line.startswith("#"):
                continue
            p = line.rstrip("\n").split("\t")
            if len(p) < 5:
                continue
            s, e, out = int(p[0]), int(p[1]), p[3]
            if e < since:
                continue
            edges[out] = (s, e)  # last entry wins
    comp, arch, link, other = {}, {}, {}, {}
    for out, (s, e) in edges.items():
        dur = e - s
        if out.endswith((".o", ".obj")):
            comp[out] = dur
        elif out.endswith(".a"):
            arch[out] = dur
        elif "CMakeFiles" in out or out.endswith((".json", ".h", ".txt")):
            other[out] = dur
        else:
            link[out] = dur
    span = (max(e for s, e in edges.values()) - min(s for s, e in edges.values())) / 1000 if edges else 0

    def tot(x):
        return sum(x.values()) / 1000

    print(f"edges={len(edges)} wall_span={span:.0f}s")
    print(f"compile: n={len(comp)} sum={tot(comp):.0f}s  archive: n={len(arch)} sum={tot(arch):.0f}s  "
          f"link/other-bin: n={len(link)} sum={tot(link):.0f}s  rest: n={len(other)} sum={tot(other):.0f}s")
    # compile by target dir
    by = defaultdict(lambda: [0, 0.0])
    for o, t in comp.items():
        m = re.search(r"CMakeFiles/([^/]+)\.dir/", o)
        k = m.group(1) if m else "?"
        # tests: group
        by[k][0] += 1
        by[k][1] += t / 1000
    big = sorted(by.items(), key=lambda kv: -kv[1][1])[:12]
    print("compile sum by target (top 12):", "; ".join(f"{k} n={v[0]} {v[1]:.0f}s" for k, v in big))
    one = [t for o, t in comp.items() if "test" in o.lower() or "tools/" in o]
    print("30 slowest compiles:")
    for o, t in sorted(comp.items(), key=lambda kv: -kv[1])[:30]:
        print(f"  {t/1000:6.1f}s {o}")
    print("15 slowest archive/link:")
    for o, t in sorted({**arch, **link}.items(), key=lambda kv: -kv[1])[:15]:
        print(f"  {t/1000:6.1f}s {o}")
    if link:
        vals = sorted(link.values())
        print(f"link edges: n={len(vals)} median={vals[len(vals)//2]/1000:.1f}s max={vals[-1]/1000:.1f}s sum={sum(vals)/1000:.0f}s")
    if "--ctest" in sys.argv:
        log = os.path.join(d, "Testing", "Temporary", "LastTest.log")
        if os.path.exists(log):
            txt = open(log, errors="replace").read()
            rows = re.findall(r"\n\d+/\d+ Test\s+#\d+: (\S+) \.+\s*(\S.*?)\s*Test time =\s*([\d.]+) sec", txt)
            times = sorted(((float(t), n) for n, _, t in rows), reverse=True)
            print(f"ctest: n={len(times)} sum={sum(t for t,_ in times):.0f}s")
            for lo, hi in ((0, 1), (1, 5), (5, 60), (60, 1e9)):
                sel = [t for t, _ in times if lo <= t < hi]
                print(f"  {lo}-{hi}s: n={len(sel)} sum={sum(sel):.0f}s")
            print("20 slowest tests:")
            for t, n in times[:20]:
                print(f"  {t:7.1f}s {n}")


if __name__ == "__main__":
    main()
