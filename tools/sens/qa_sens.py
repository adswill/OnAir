#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

"""Sensitivity of every engine mode through the QA sweep's generator and Engine stages (4.1, docs/SENSITIVITY-4.1-ALL.md).

sens_sweep (tools/sens/sens_sweep.cpp) measures the modes that have a receiver block in sens_modes.h. Most modes added in 3.9 and 4.0 have
none, but every mode with a built-in test signal runs through tests/qa/test_qa_sweep.cpp: generator -> file -> Engine -> the mode's decoder,
counting the receiver's own checked blocks. This driver calls that binary's two stages (--gen, --run) for a series of SNRs and trials:

  awgn      white noise in the signal's own 99 % band (SNR = active signal power / noise power in that band, as the QA sweep's "snr" file)
  cfoclk    +2 kHz carrier and +50 ppm sample clock (a cheap radio's crystal), with the same noise
  offcentre the channel moved by a quarter of the mode's channel width (or 2 kHz, the larger), -50 ppm clock, with the same noise

A trial passes when the receiver counts at least half the units of the clean run (the sens_sweep rule). The threshold of a scenario is
the lowest SNR (2 dB steps after a 4 dB descent) at which every trial passed, scanning down from the top. False decodes: the noise-only
files (the noise of each measured point, no signal) through the same receiver; any unit counted there is a false decode.

  tools/sens/qa_sens.py --bin build/test_qa_sweep --out sens-out [--re REGEX] [--shard k/N] [--jobs 3] [--trials 2]
"""
import argparse, json, math, os, re, shutil, subprocess, sys, tempfile, threading, time
from concurrent.futures import ThreadPoolExecutor

# The TV and wideband modes have their own detail sweeps (sens_sweep, sens_dvb) and need gigabytes per file here
SKIP_DEFAULT = {"t2", "dvbt", "atsc", "dab", "atsc3", "isdbt", "dvbs", "dtmb", "atv", "dvbc", "dvbc2", "dvbsh", "isdbs", "fpv", "cell", "nr", "umts",
                "cdma", "tdscdma", "xlink", "wifi", "dji", "iptv"}


def run(cmd, env, timeout):
    try:
        p = subprocess.run(cmd, env=env, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, timeout=timeout)
        return p.returncode, p.stdout.decode("utf-8", "replace")
    except subprocess.TimeoutExpired:
        return -999, ""


class Mode:
    def __init__(self, a, line):
        self.a = a
        t = line.split()
        self.id = t[0]
        m = re.search(r"std\s+(\d+)\s+([\d.eE+-]+) Msps\s+([\d.eE+-]+) MHz(?:\s+bw ([\d.eE+-]+) MHz)?", line)
        self.std = int(m.group(1)); self.rate = float(m.group(2)) * 1e6; self.mhz = float(m.group(3))
        self.bw = float(m.group(4)) * 1e6 if m.group(4) else 25e3
        self.name = line[len(t[0]):m.start()].strip()
        self.dir = tempfile.mkdtemp(prefix="qasens_" + re.sub(r"[^a-z0-9]", "_", self.id.lower()) + "_")
        self.log = []

    def env(self, seed):
        e = dict(os.environ)
        e["QA_NOTX"] = "1"; e["QA_IMP_NOISE"] = "1"; e["QA_SEED"] = str(seed)
        e["QA_NEG_HZ"] = "%.1f" % max(2000.0, 0.25 * self.bw)
        e.pop("QA_PACE", None)
        return e

    def gen(self, snr, seed, secs):
        rc, out = run([self.a.bin, "--gen", self.id, "%.4f" % self.rate, "%.3f" % secs, "%.2f" % snr, self.dir], self.env(seed), 900)
        m = re.search(r"RATE ([\d.eE+-]+) PSIG ([\d.eE+-]+) OBW ([\d.eE+-]+)", out)
        if rc != 0 or not m:
            return None
        t0 = re.search(r"^T0 ([\d.]+)", out, re.M)
        return {"rate": float(m.group(1)), "psig": float(m.group(2)), "obw": float(m.group(3)), "t0": t0.group(1) if t0 else None}

    def runf(self, g, secs, name, seed):
        e = self.env(seed)
        if g.get("t0"): e["QA_T0"] = g["t0"]
        rc, out = run([self.a.bin, "--run", self.id, "%.4f" % g["rate"], "%.3f" % secs, os.path.join(self.dir, name + ".ci16"), "snr"], e, max(300, secs * 30))
        j = out.rfind("{")
        if rc != 0 or j < 0:
            return None
        try:
            return json.loads(out[j:].strip().splitlines()[0])
        except Exception:
            return None


def measure(a, line):
    md = Mode(a, line)
    t0 = time.time()
    res = {"mode": md.id, "name": md.name, "std": md.std, "rate": md.rate, "bw_hz": md.bw}
    try:
        res.update(sweep(a, md))
    except Exception as ex:   # one mode's trouble must not end the shard
        res["error"] = "driver: %r" % (ex,)
    shutil.rmtree(md.dir, ignore_errors=True)
    res["wall_s"] = round(time.time() - t0, 1)
    return res


def sweep(a, md):
    out = {}
    # the reference: the clean signal (60 dB), its silence baseline and which measure the mode has
    ref = None
    for secs in ([a.secs, 32.0] if a.secs < 32 else [a.secs]):
        secs = min(secs, a.max_samples / max(md.rate, 1.0))
        g = md.gen(60, 0, secs)
        if g is None:
            return {"error": "no test signal or generator failed"}
        c = md.runf(g, secs, "clean", 0); z = md.runf(g, secs, "zero", 0)
        if c is None or z is None:
            return {"error": "receiver crashed or hung on the clean signal"}
        if c["good"] > 0: kind, m0 = "blocks", c["good"]
        elif c["res"] > z["res"] + 16: kind, m0 = "saved", c["res"] - z["res"]
        elif c["locked"]: kind, m0 = "lock", 1
        else: continue
        p = md.runf(g, secs, "pos", 0); n = md.runf(g, secs, "neg", 0)
        ref = {"secs": secs, "kind": kind, "m0": m0, "zero_res": z["res"], "obw_hz": g["obw"], "first_s": c["first"],
               "clean_cfoclk": p is not None and metric(kind, p, z["res"]) >= need(kind, m0),
               "clean_offcentre": n is not None and metric(kind, n, z["res"]) >= need(kind, m0)}
        break
    if ref is None:
        return {"error": "no valid output on the clean test signal (8 and 32 s)"}
    out["ref"] = ref
    secs, kind, m0, zr = ref["secs"], ref["kind"], ref["m0"], ref["zero_res"]
    scen = {"awgn": "snr", "cfoclk": "pos", "offcentre": "neg"}
    pts = {}       # snr -> {scenario: [pass per trial]}
    falses = []    # units counted on noise alone, per trial
    units = {}     # snr -> mean units in awgn

    def point(snr):
        if snr in pts: return pts[snr]
        r = {k: [] for k in scen}; u = []
        for t in range(a.trials):
            g = md.gen(snr, t + 1, secs)
            if g is None:
                for k in scen: r[k].append(False)
                continue
            for k, f in scen.items():
                x = md.runf(g, secs, f, t + 1)
                v = metric(kind, x, zr) if x is not None else -1
                r[k].append(v >= need(kind, m0))
                if k == "awgn": u.append(max(0, v))
            if snr <= a.false_at:   # false decodes: the noise of this point alone
                x = md.runf(g, secs, "noise", t + 1)
                if x is not None:
                    falses.append(metric(kind, x, zr) if kind != "lock" else (1 if x["locked"] else 0))
        pts[snr] = r; units[snr] = sum(u) / max(1, len(u))
        return r

    ok = lambda snr, k: all(point(snr)[k])
    hi = a.hi
    while not ok(hi, "awgn") and hi < 44: hi += 8
    if not ok(hi, "awgn"):
        out.update(thresholds(pts, scen, None)); out["note"] = "no 90 % point up to %d dB" % hi
    else:
        s = hi
        while s - 4 >= a.lo and ok(s - 4, "awgn"): s -= 4
        if s - 2 >= a.lo: ok(s - 2, "awgn")
        # impairments whose descent stopped above the awgn floor: refine their own threshold in 2 dB too
        out.update(thresholds(pts, scen, a))
        for k in ("cfoclk", "offcentre"):
            t = out["thr"].get(k)
            if t is not None and (t - 2) not in pts and t - 2 >= a.lo:
                point(t - 2)
        out.update(thresholds(pts, scen, a))
    # always a false-decode check at the lowest point measured, even when the descent stopped high
    if not falses:
        lo = min(pts)
        for t in range(a.trials):
            g = md.gen(min(lo, a.false_at), 50 + t, secs)
            if g is None: continue
            x = md.runf(g, secs, "noise", 50 + t)
            if x is not None: falses.append(metric(kind, x, zr) if kind != "lock" else (1 if x["locked"] else 0))
    out["false_trials"] = len(falses)
    out["false_units"] = sum(max(0, f) for f in falses)
    out["false_rate"] = (sum(1 for f in falses if f > 0) / len(falses)) if falses else None
    out["points"] = {("%g" % k): {kk: vv for kk, vv in v.items()} for k, v in sorted(pts.items())}
    out["units"] = {("%g" % k): v for k, v in sorted(units.items())}
    return out


def need(kind, m0):
    return 1 if kind == "lock" else max(1, math.floor(0.5 * m0))


def metric(kind, x, zr):
    if x is None: return -1
    if kind == "blocks": return x["good"]
    if kind == "saved": return x["res"] - zr
    return 1 if x["locked"] else 0


def thresholds(pts, scen, a):
    thr = {}
    for k in scen:
        best = None
        for snr in sorted(pts, reverse=True):   # the lowest SNR reached from the top without a failing point in between
            if all(pts[snr][k]): best = snr
            else: break
        thr[k] = best
    return {"thr": thr}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", default="build/test_qa_sweep")
    ap.add_argument("--out", default="sens-out")
    ap.add_argument("--re", default=".*", help="regex on the mode id")
    ap.add_argument("--skip", default="", help="more ids to skip, comma separated")
    ap.add_argument("--all", action="store_true", help="do not skip the TV / wideband modes")
    ap.add_argument("--shard", default="1/1")
    ap.add_argument("--jobs", type=int, default=3)
    ap.add_argument("--trials", type=int, default=2)
    ap.add_argument("--secs", type=float, default=8.0)
    ap.add_argument("--hi", type=float, default=24.0)
    ap.add_argument("--lo", type=float, default=-24.0)
    ap.add_argument("--false-at", type=float, default=6.0, help="noise-only runs at every point at or below this SNR")
    ap.add_argument("--max-samples", type=float, default=4e7)
    ap.add_argument("--budget-min", type=float, default=330, help="stop starting modes after this many minutes")
    a = ap.parse_args()
    a.bin = os.path.abspath(a.bin)
    os.makedirs(a.out, exist_ok=True)
    rc, lst = run([a.bin, "--list"], dict(os.environ), 120)
    lines = [l for l in lst.splitlines() if re.search(r"\bstd\s+\d+", l)]
    skip = set() if a.all else set(SKIP_DEFAULT)
    skip |= {s for s in a.skip.split(",") if s}
    rx = re.compile(a.re)
    sel = [l for l in lines if rx.search(l.split()[0]) and l.split()[0] not in skip]
    k, n = (int(x) for x in a.shard.split("/"))
    sel = [l for i, l in enumerate(sel) if i % n == k - 1]
    print("%d modes in this shard" % len(sel), flush=True)
    t0 = time.time()
    results, lock = [], threading.Lock()
    jl = open(os.path.join(a.out, "qa-sens-%d.jsonl" % k), "w")

    def one(line):
        if (time.time() - t0) / 60 > a.budget_min:
            return {"mode": line.split()[0], "error": "time budget of the shard used up: not measured"}
        r = measure(a, line)
        with lock:
            results.append(r); jl.write(json.dumps(r) + "\n"); jl.flush()
            th = r.get("thr", {})
            f = lambda v: "-" if v is None else "%g" % v
            print("%-16s %-7s awgn %4s cfoclk %4s offc %4s  false %s/%s  obw %s Hz  %s%s (%.0f s)" % (
                r["mode"], r.get("ref", {}).get("kind", "?"), f(th.get("awgn")), f(th.get("cfoclk")), f(th.get("offcentre")),
                r.get("false_units", "-"), r.get("false_trials", "-"), "%.0f" % r["ref"]["obw_hz"] if "ref" in r else "-",
                r.get("note", ""), r.get("error", ""), r.get("wall_s", 0)), flush=True)
        return r

    with ThreadPoolExecutor(max_workers=a.jobs) as ex:
        list(ex.map(one, sel))
    jl.close()
    print("done: %d modes, %.0f min" % (len(results), (time.time() - t0) / 60))


if __name__ == "__main__":
    main()
