#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

"""Compares a sens_sweep result with the committed baseline (tests/sens/baseline/sens-baseline.json).

  compare.py BASELINE NEW [--db 0.3] [--lock 0.2]   table of per-mode, per-scenario deltas; exit 1 on a regression
  compare.py --merge BASELINE NEW                   writes NEW's modes into BASELINE (the other modes stay)

Regression: threshold SNR worse by more than --db (default 0.3 dB), or median lock time more than --lock (default 20 %) longer
(and at least 50 ms longer), or a scenario that had a threshold has none. delta dB is new minus baseline: negative is a gain
(the mode decodes at a lower SNR); lock ratio below 1 is a gain. Only modes and scenarios present in both files are compared.
A quick run measures the threshold in 2 dB steps, so for it the dB tolerance is raised to 2 dB: use quick runs to catch big
regressions and full runs to claim a fraction of a dB.
"""
import json
import sys


def load(p):
    with open(p) as f:
        d = json.load(f)
    return {m["mode"]: m for m in d["modes"] if "scenarios" in m}


def main(a):
    db, lock, merge = 0.3, 0.2, False
    pos = []
    i = 0
    while i < len(a):
        if a[i] == "--db":
            db = float(a[i + 1]); i += 1
        elif a[i] == "--lock":
            lock = float(a[i + 1]); i += 1
        elif a[i] == "--merge":
            merge = True
        else:
            pos.append(a[i])
        i += 1
    if len(pos) != 2:
        print(__doc__)
        return 2
    if merge:
        try:
            base = load(pos[0])
        except FileNotFoundError:
            base = {}
        new = load(pos[1])
        base.update(new)
        with open(pos[0], "w") as f:
            f.write('{"tool":"sens_sweep","version":1,"modes":[\n')
            f.write(",\n".join(json.dumps(base[k], separators=(",", ":")) for k in sorted(base)))
            f.write("\n]}\n")
        print("baseline now holds %d modes (%d updated)" % (len(base), len(new)))
        return 0
    base, new = load(pos[0]), load(pos[1])
    bad = 0
    print("%-9s %-11s %9s %9s %8s | %8s %8s %7s  %s" % ("mode", "scenario", "thr base", "thr new", "delta dB", "lock b s", "lock n s", "ratio", ""))
    for mode in sorted(new):
        if mode not in base:
            print("%-9s (no baseline)" % mode)
            continue
        nb = {s["name"]: s for s in base[mode]["scenarios"]}
        tol = 2.0 + 1e-9 if new[mode].get("quick") and not base[mode].get("quick") else db
        for s in new[mode]["scenarios"]:
            b = nb.get(s["name"])
            if not b:
                continue
            tb, tn = b["threshold_db"], s["threshold_db"]
            lb, ln = b["lock_med_s"], s["lock_med_s"]
            flag = ""
            if tb is not None and tn is None:
                flag = "REGRESSION (no threshold now)"
                bad += 1
            elif tb is None and tn is not None:
                flag = "gain (threshold now exists)"
            d = (tn - tb) if (tb is not None and tn is not None) else None
            if d is not None and d > tol:
                flag = "REGRESSION (threshold)"
                bad += 1
            elif d is not None and d < -tol and not flag:
                flag = "gain"
            r = (ln / lb) if (lb and ln and lb > 0 and ln > 0) else None
            if r is not None and r > 1 + lock and ln - lb > 0.05:
                flag = (flag + " " if flag else "") + "REGRESSION (lock time)"
                bad += 1

            def f(v, p="%.1f"):
                return "-" if v is None or v < 0 else p % v
            print("%-9s %-11s %9s %9s %8s | %8s %8s %7s  %s" % (mode, s["name"], f(tb), f(tn), "-" if d is None else "%+.1f" % d, f(lb, "%.2f"), f(ln, "%.2f"), "-" if r is None else "%.2f" % r, flag))
    print("\n%s" % ("%d regression(s)" % bad if bad else "no regressions"))
    return 1 if bad else 0


sys.exit(main(sys.argv[1:]))
