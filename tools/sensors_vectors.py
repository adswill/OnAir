#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

"""Run the Sub-GHz sensors decoders over rtl_433's test recordings and compare with the expected JSON.

  tools/sensors_vectors.py <sensorstool binary> <rtl_433_tests/tests dir> [--report out.json]

Same rules as rtl_433_tests/bin/run_test.py: the "time" field is ignored, output of another model than the expected one is a false
positive and dropped, and the first output line is compared with the first expected line (the repository's `make test` mode).
Samples of a directory with an `ignore` file are skipped; a `protocol` file restricts the run to that protocol (-R), a `demod` file
gives pulse detector options (-Y). cs16 samples are not supported (counted as failed). Prints the totals; the report has per sample and per model results.
"""
import json, os, subprocess, sys, shlex, collections

def read_lines(path):
    out = []
    with open(path) as f:
        for l in f:
            l = l.strip()
            if l:
                try: out.append(json.loads(l))
                except ValueError: pass
    return out

def norm(d):
    d = dict(d)
    d.pop("time", None)
    return d

def same(a, b):
    if isinstance(a, float) or isinstance(b, float):
        try: return abs(float(a) - float(b)) < 1e-6 + 1e-9 * abs(float(a))
        except Exception: return False
    return a == b

def eq(exp, got):
    if set(exp) != set(got): return False
    return all(same(exp[k], got[k]) if not isinstance(exp[k], (dict, list)) else exp[k] == got[k] for k in exp)

def run(binary, args):
    p = subprocess.run([binary] + args, capture_output=True, text=True, timeout=300)
    res = []
    for l in p.stdout.splitlines():
        l = l.strip()
        if l:
            try: res.append(json.loads(l))
            except ValueError: pass
    return res

def main():
    binary, root = sys.argv[1], sys.argv[2]
    report = sys.argv[sys.argv.index("--report") + 1] if "--report" in sys.argv else None
    samples = {}
    codes_res = {}
    for dirpath, _, files in os.walk(root):
        if "ignore" in files: continue
        proto = None
        if "protocol" in files:
            proto = open(os.path.join(dirpath, "protocol")).readline().strip()
            if not proto.replace(",", "").isdigit(): proto = None
        demod = shlex.split(open(os.path.join(dirpath, "demod")).read()) if "demod" in files else []
        args0 = (["-R", proto] if proto else []) + demod
        for fn in files:
            if not fn.endswith(".json") or fn in ("codes_test.json", "known_false_positives.json"): continue
            base = os.path.join(dirpath, fn[:-5])
            inp = next((base + e for e in (".cu8", ".cs8", ".cs16") if os.path.isfile(base + e)), None)
            if not inp: continue
            exp = [norm(x) for x in read_lines(os.path.join(dirpath, fn))]
            models = {e["model"] for e in exp if "model" in e}
            rel = os.path.relpath(inp, root)
            if inp.endswith(".cs16"):
                samples[rel] = (models, False, "cs16 not supported"); continue
            try:
                got = [norm(x) for x in run(binary, args0 + [inp])]
            except Exception as e:
                samples[rel] = (models, False, "error %s" % e); continue
            got = [g for g in got if "model" not in g or g["model"] in models]
            e0 = exp[0] if exp else {}
            g0 = got[0] if got else {}
            ok_first = eq(e0, g0)
            ok_all = len(exp) == len(got) and all(eq(a, b) for a, b in zip(exp, got))
            samples[rel] = (models, ok_first, "" if ok_first else "expected %s got %s" % (json.dumps(e0)[:160], json.dumps(g0)[:160]), ok_all)
        if "codes_test.txt" in files and "codes_test.json" in files:
            codes = []
            for l in open(os.path.join(dirpath, "codes_test.txt")):
                l = l.split("#", 1)[0].strip()
                if l.startswith("{"): codes.append(l)
            exp = [norm(x) for x in read_lines(os.path.join(dirpath, "codes_test.json"))]
            if len(codes) != len(exp): continue
            rep = int(open(os.path.join(dirpath, "repeat")).read().strip()) if "repeat" in files else 1
            ok = 0
            models = {e["model"] for e in exp if "model" in e}
            for c, e in zip(codes, exp):
                got = [norm(x) for x in run(binary, (["-R", proto] if proto else []) + ["-y", c * rep])]
                got = [g for g in got if "model" not in g or g["model"] in models]
                if got and eq(e, got[0]): ok += 1
            codes_res[os.path.relpath(dirpath, root)] = (models, ok, len(codes))
    # totals
    n = len(samples); okn = sum(1 for v in samples.values() if v[1])
    per_model = collections.defaultdict(lambda: [0, 0])
    for v in samples.values():
        for m in v[0]:
            per_model[m][1] += 1
            if v[1]: per_model[m][0] += 1
    for m_, ok, tot in codes_res.values():
        for m in m_:
            per_model[m][1] += tot; per_model[m][0] += ok
    allpass = sum(1 for a, b in per_model.values() if a == b)
    anypass = sum(1 for a, b in per_model.values() if a > 0)
    print("samples (.cu8/.cs8/.cs16): %d, passing (first line equal): %d" % (n, okn))
    print("code tests: %d directories, codes passing %d of %d" % (len(codes_res), sum(v[1] for v in codes_res.values()), sum(v[2] for v in codes_res.values())))
    print("models with a test: %d, all tests pass: %d, at least one passes: %d" % (len(per_model), allpass, anypass))
    if report:
        json.dump({"samples": {k: {"models": sorted(v[0]), "ok": v[1], "note": v[2], "ok_all_lines": v[3] if len(v) > 3 else None} for k, v in samples.items()},
                   "codes": {k: {"models": sorted(v[0]), "ok": v[1], "total": v[2]} for k, v in codes_res.items()},
                   "models": {m: {"pass": a, "total": b} for m, (a, b) in sorted(per_model.items())}}, open(report, "w"), indent=1)

main()
