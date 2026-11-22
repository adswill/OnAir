#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors
"""qa_sens.py results (qa-sens-*.jsonl in a directory) -> snr-threshold.csv (mode,name,kind,threshold_dB,notes) and a printed table.
threshold_dB is the AWGN threshold: the lowest SNR (in the signal's 99 % band) from which every trial down to there decoded.
  snr_threshold_table.py DIR   (writes DIR/snr-threshold.csv, prints the table and the worst modes above 0 dB)"""
import csv, glob, json, os, sys

def main():
    d = sys.argv[1]
    rows = []
    for f in sorted(glob.glob(os.path.join(d, "qa-sens-*.jsonl"))):
        for l in open(f):
            if l.strip(): rows.append(json.loads(l))
    out = []
    for r in rows:
        t = r.get("thr", {}).get("awgn")
        notes = []
        if r.get("error"): notes.append(r["error"])
        if r.get("note"): notes.append(r["note"])
        if r.get("capped"): notes.append("runtime cap hit")
        ref = r.get("ref", {})
        if ref.get("kind") == "lock": notes.append("lock only")
        if t is not None and t <= -24: notes.append("passes at the floor of the sweep")
        if r.get("false_units"): notes.append("false decodes on noise: %s" % r["false_units"])
        out.append({"mode": r["mode"], "name": r.get("name", ""), "kind": ref.get("kind", ""),
                    "threshold_dB": "" if t is None else "%g" % t, "notes": "; ".join(notes)})
    with open(os.path.join(d, "snr-threshold.csv"), "w", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=["mode", "name", "kind", "threshold_dB", "notes"]); w.writeheader(); w.writerows(out)
    key = lambda x: (x["threshold_dB"] == "", -(float(x["threshold_dB"]) if x["threshold_dB"] else 0))
    print("SNRTABLE mode,threshold_dB,notes")
    for x in sorted(out, key=key):
        print("SNRTABLE %s,%s,%s" % (x["mode"], x["threshold_dB"] or "none", x["notes"]))
    above = [x for x in out if x["threshold_dB"] and float(x["threshold_dB"]) > 0]
    above.sort(key=lambda x: -float(x["threshold_dB"]))
    print("SNRWORST " + " ".join("%s=%s" % (x["mode"], x["threshold_dB"]) for x in above[:10]))
    print("SNRCOUNT measured=%d above0=%d undecoded=%d" % (len(out), len(above), sum(1 for x in out if not x["threshold_dB"])))

if __name__ == "__main__":
    main()
