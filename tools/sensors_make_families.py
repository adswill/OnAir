#!/usr/bin/env python3
"""Build tests/sensors/families.txt: for each protocol family one real recording of rtl_433's test set, reduced to its pulse trains
(rtl_433 -w), so that the unit test can rebuild an IQ signal from them and run the whole receiver.
  tools/sensors_make_families.py <rtl_433_tests/tests dir> <report.json from sensors_vectors.py> > tests/sensors/families.txt
Line: family|expected model|expected id or -|package type (ook, fsk)|f1 offset Hz|f2 offset Hz|pulse,gap,pulse,gap ... (microseconds, rounded)"""
import json, os, subprocess, sys, tempfile

FAMILIES = [  # family label, directory prefixes of rtl_433_tests/tests
    ("Acurite", ["acurite"]), ("LaCrosse", ["lacrosse"]), ("Oregon Scientific", ["oregon_scientific"]),
    ("Fine Offset / Ecowitt", ["fineoffset", "ecowitt", "EcoWitt"]), ("Bresser", ["bresser_5in1", "bresser_6in1", "bresser_3ch"]),
    ("TFA", ["TFA", "tfa_"]), ("Nexus / Prologue", ["nexus", "prologue"]), ("Hideki", ["hideki"]), ("Ambient Weather", ["ambient_weather"]),
    ("Honeywell / Visonic / DSC security", ["honeywell_5816", "honeywell_5890PI", "visonic_powercode", "dsc"]), ("Tyre pressure", ["Toyota_TPMS", "Schrader"]),
    ("Smart meters (ERT)", ["ert"]), ("X10", ["x10"]), ("Alecto / Esperanza", ["davis", "alecto", "esperanza"]),
]

def pulses_of(path):
    pk, cur = [], None
    for l in open(path):
        l = l.strip()
        if l.startswith(";pulse data"): cur = {"type": "ook", "f1": 0, "f2": 0, "c": 0, "p": []}
        elif cur is None: continue
        elif l.startswith(";fsk"): cur["type"] = "fsk"
        elif l.startswith(";freq1"): cur["f1"] = int(l.split()[1])
        elif l.startswith(";freq2"): cur["f2"] = int(l.split()[1])
        elif l.startswith(";centerfreq"): cur["c"] = int(l.split()[1])
        elif l.startswith(";end"): pk.append(cur); cur = None
        elif l and not l.startswith(";"):
            a, b = l.split()[:2]; cur["p"] += [int(a), int(b)]
    return pk

def main():
    root, rep = sys.argv[1], json.load(open(sys.argv[2]))
    for fam, prefixes in FAMILIES:
        cands = [k for k, v in rep["samples"].items() if v["ok"] and k.split("/")[0].startswith(tuple(prefixes)) and v["models"]]
        cands = [k for k in cands if not os.path.exists(os.path.join(root, os.path.dirname(k), "demod"))]
        cands.sort(key=lambda k: os.path.getsize(os.path.join(root, k)))
        done = 0
        for k in cands:
            if done >= 2: break
            f = os.path.join(root, k)
            exp = json.loads(open(f.rsplit(".", 1)[0] + ".json").readline())
            if f.endswith(".cs16"): continue
            with tempfile.TemporaryDirectory() as d:
                out = os.path.join(d, "x.ook")
                subprocess.run(["rtl_433", "-c", "0", "-r", f, "-w", out], capture_output=True)
                if not os.path.exists(out): continue
                pk = [p for p in pulses_of(out) if len(p["p"]) >= 48]
            if not pk or sum(len(p["p"]) for p in pk) > 5000: continue
            for p in pk:
                print("|".join([fam, exp["model"], str(exp.get("id", "-")), p["type"], str(p["f1"] - p["c"]), str(p["f2"] - p["c"]), ",".join(map(str, p["p"]))]))
            done += 1

main()
