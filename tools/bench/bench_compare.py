#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors
"""Compare bench_core output with a baseline.

  bench_compare.py bench.txt baseline.tsv [--threshold PCT] [--update]

bench.txt is the plain output of bench_core ("name cost X CPU s/s ..."). baseline.tsv has "name<TAB>cost". An item fails when its
cost is more than PCT percent above the baseline. Items missing from either side are reported, not failed. With no baseline file
yet, or with --update, the new baseline is printed and the exit code is 0.
"""
import re
import sys
from pathlib import Path


def parse(path):
    out = {}
    for line in Path(path).read_text().splitlines():
        m = re.match(r"^(\S+)\s+cost\s+([0-9.eE+-]+)\s+CPU s/s", line)
        if m:
            out[m.group(1)] = float(m.group(2))
    return out


def main(argv):
    args = [a for a in argv[1:] if not a.startswith("--")]
    thr = 25.0
    if "--threshold" in argv:
        thr = float(argv[argv.index("--threshold") + 1])
        args = [a for a in args if a != argv[argv.index("--threshold") + 1]]
    cur = parse(args[0])
    if not cur:
        print("no benchmark lines found in", args[0])
        return 1
    base_path = Path(args[1])
    if "--update" in argv or not base_path.exists():
        print("No baseline to compare with; new baseline.tsv:\n")
        for k, v in sorted(cur.items()):
            print(f"{k}\t{v:.6f}")
        return 0
    base = {}
    for line in base_path.read_text().splitlines():
        if "\t" in line:
            k, v = line.split("\t")
            base[k] = float(v)
    print("| Item | Baseline CPU s/s | Now | Change |\n|---|--:|--:|--:|")
    bad = []
    for k in sorted(set(cur) | set(base)):
        if k not in cur or k not in base:
            print(f"| `{k}` | {base.get(k, '-')} | {cur.get(k, '-')} | not on both sides |")
            continue
        ch = (cur[k] / base[k] - 1) * 100 if base[k] > 0 else 0
        flag = " **slower**" if ch > thr else ""
        print(f"| `{k}` | {base[k]:.4f} | {cur[k]:.4f} | {ch:+.1f} %{flag} |")
        if ch > thr:
            bad.append(k)
    if bad:
        print(f"\nMore than {thr:.0f} % slower than the baseline: {', '.join(bad)}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
