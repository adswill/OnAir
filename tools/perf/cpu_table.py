#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

"""Merge the --cpu sweep TSVs of test_qa_sweep (cpu.yml) into one Markdown table (docs/CPU-4.1.md).

  cpu_table.py DIR      DIR holds cpu-<os>-<shard>[-cmp].tsv; prints the table, worst first, and the modes above 1.0 CPU s per s.
A -cmp file is the same measuring program built over the comparison ref's receivers (before); without one only one figure per OS.
"""
import csv, glob, os, sys

def load(pattern):
    rows = {}
    for p in sorted(glob.glob(pattern)):
        with open(p, newline="") as f:
            for r in csv.DictReader(f, delimiter="\t"):
                rows[r["id"]] = r
    return rows

def num(r, k):
    try:
        return float(r[k])
    except (KeyError, ValueError, TypeError):
        return None

def main():
    d = sys.argv[1] if len(sys.argv) > 1 else "."
    oses = [o for o in ("linux", "macos") if glob.glob(os.path.join(d, f"cpu-{o}-*.tsv"))]
    now = {o: {} for o in oses}
    for o in oses:   # the -cmp files are the comparison ref's
        for p in sorted(glob.glob(os.path.join(d, f"cpu-{o}-*.tsv"))):
            if p.endswith("-cmp.tsv"):
                continue
            now[o].update(load(p))
    cmp = {o: {} for o in oses}
    for o in oses:
        for p in sorted(glob.glob(os.path.join(d, f"cpu-{o}-*-cmp.tsv"))):
            cmp[o].update(load(p))
    ids = sorted({i for o in oses for i in now[o]}, key=lambda i: -max((num(now[o].get(i, {}), "all") or 0) for o in oses))
    def cell(o, i, k):
        r = now[o].get(i)
        v = num(r, k) if r else None
        if v is None or v < 0:
            return ""
        c = cmp[o].get(i)
        b = num(c, k) if c else None
        return f"{b:.3f} -> {v:.3f}" if b is not None and b >= 0 else f"{v:.3f}"
    hdr = ["mode", "name", "Msps"]
    for o in oses:
        hdr += [f"{o} all threads", f"{o} busiest", f"{o} noise all", f"{o} noise busiest"]
    hdr += ["blocks", "busiest thread", "threads by CPU"]
    print("| " + " | ".join(hdr) + " |")
    print("|" + "|".join(["---", "---", "--:"] + ["--:"] * (4 * len(oses)) + ["--:", "---", "---"]) + "|")
    over = []
    for i in ids:
        any_r = next(now[o][i] for o in oses if i in now[o])
        line = [i, any_r["name"], f"{float(any_r['rate']) / 1e6:.4g}"]
        hi = False
        for o in oses:
            line += [cell(o, i, "all"), cell(o, i, "busiest"), cell(o, i, "noise_all"), cell(o, i, "noise_busiest")]
            r = now[o].get(i)
            if r and any((num(r, k) or 0) > 1.0 for k in ("all", "busiest", "noise_all", "noise_busiest")):
                hi = True
        line += [any_r["blocks"] + ("" if any_r["locked"] == "1" else " (no lock)") + (" " + any_r["err"].strip() if any_r["err"].strip() else ""),
                 any_r["busiest_name"], any_r["top"]]
        if hi:
            over.append(i)
        print("| " + " | ".join(s.replace("|", "\\|") for s in line) + " |")
    print()
    print(f"{len(ids)} modes; above 1.0 CPU s per s of signal (all threads or busiest, signal or noise alone, either OS): {len(over)}")
    print(", ".join(over))

if __name__ == "__main__":
    main()
