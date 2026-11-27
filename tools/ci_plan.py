#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

"""How many shards each platform of ci.yml gets (docs/CI-FAST.md (4.7)).

Runs tools/ci_select.py once per platform on the whole selection (shard 1/1), reads its estimated test seconds (tools/ci_times.tsv: seconds
under ctest -j4 on a 4-core runner) and gives each platform enough shards that one shard's tests take about --minutes of wall time, within
the platform's maximum (GitHub's free plan runs 20 jobs at once, 5 of them macOS). Writes the matrices as GitHub outputs:
  linux=<json>  macos=<json>  windows=<json>    (strategy.matrix: fromJSON(needs.lint.outputs.linux))
  fanout=1|0  fanout_<plat>=1|0  compile_<plat>=<json> (linux, macos, windows)   (the compile jobs: plat, os, part, of)
and a table for the step summary. A small change gets one shard per platform instead of four that each pay the set-up and build.
fanout: the selection reaches the core (scope wide or full), so a cold build is likely: the compile jobs split the compile first
(tools/ci_objs.py) and the shards read their ccache parts. A small change (area, smoke) skips them: their start-up would cost more.
"""
import argparse, json, math, os, re, subprocess, sys

PLATS = {   # plat: (job name, runner, most shards)
    "linux": ("Linux x86_64", "ubuntu-24.04", 9),    # max-parallel 9 in ci.yml: more would wait for a second wave
    "macos": ("macOS arm64", "macos-15", 5),     # the free plan runs 5 macOS jobs at once
    "windows": ("Windows", "windows-2022", 4),
}

REDUCED = {"linux": 7}   # a worker's check (platforms linux): leaves runners for the other workers' quick.yml checks

# compile jobs per platform when the selection reaches the core
COMPILE_PARTS = {"linux": 6, "macos": 4, "windows": 6}

def estimate(args, plat):
    out = os.path.join(args.work, "plan-" + plat)
    cmd = [sys.executable, os.path.join(os.path.dirname(os.path.abspath(__file__)), "ci_select.py"), "--build-dir", args.build_dir,
           "--scope", args.scope, "--shard", "1/1", "--times", args.times, "--plat", plat, "--out", out]
    if args.base: cmd += ["--base", args.base]
    if plat != "linux": cmd += ["--no-slow"]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.stderr.write(r.stdout + r.stderr)
        sys.exit("ci_plan: ci_select failed for " + plat)
    m = re.search(r"estimated_test_seconds=([0-9.]+)", r.stdout)
    n = re.search(r"tests=(\d+)", r.stdout)
    sc = re.search(r"scope=(\w+)", r.stdout)
    return float(m.group(1)) if m else 0.0, int(n.group(1)) if n else 0, sc.group(1) if sc else ""

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build-dir", required=True)
    ap.add_argument("--scope", default="auto")
    ap.add_argument("--base", default="")
    ap.add_argument("--times", default="tools/ci_times.tsv")
    ap.add_argument("--minutes", type=float, default=10.0, help="wall minutes of tests per shard to aim for")
    ap.add_argument("--slots", type=float, default=3.2, help="tests ctest -j4 keeps running on average (measured 2.96-3.4)")
    ap.add_argument("--work", default="ci-plan")
    ap.add_argument("--platforms", default="all", help="all, or linux (an agent's wide check: the Linux shards only)")
    ap.add_argument("--github-output", default=os.environ.get("GITHUB_OUTPUT", ""))
    a = ap.parse_args()
    budget = a.minutes * 60 * a.slots
    rows, outs, scopes = [], {}, set()
    for plat, (name, os_, most) in PLATS.items():
        est, n_tests, sc = estimate(a, plat)
        scopes.add(sc)
        # an integration batch (wide) and the nightly / release run (full) take every shard they need; a worker's Linux-only check fewer
        cap = REDUCED.get(plat, most) if a.platforms == "linux" else most
        n = max(1, min(cap, math.ceil(est / budget))) if n_tests else 1
        outs[plat] = json.dumps({"include": [{"name": name, "os": os_, "shard": k, "of": n, "plat": plat} for k in range(1, n + 1)]},
                                separators=(",", ":"))
        rows.append((plat, n_tests, est, n, est / n / a.slots / 60))
    fan = bool(scopes & {"wide", "full"})
    plats = [p for p in COMPILE_PARTS if a.platforms == "all" or p == a.platforms]
    for p in COMPILE_PARTS:   # one compile job per platform, so the Linux shards do not wait for the macOS parts
        outs["compile_" + p] = json.dumps({"include": [{"plat": p, "os": PLATS[p][1], "part": k, "of": COMPILE_PARTS[p]}
                                                       for k in range(1, COMPILE_PARTS[p] + 1)]}, separators=(",", ":"))
        outs["fanout_" + p] = "1" if fan and p in plats else "0"
    outs["fanout"] = "1" if fan and plats else "0"
    print("compile jobs: %s (scope %s, platforms %s)\n" % (", ".join("%s %d" % (p, COMPILE_PARTS[p]) for p in plats) if fan else "none",
                                                           "/".join(sorted(scopes)), a.platforms))
    print("| platform | tests | estimated test seconds | shards | minutes of tests per shard |")
    print("|---|---|---|---|---|")
    for p, t, e, n, m in rows:
        print("| %s | %d | %.0f | %d | %.1f |" % (p, t, e, n, m))
    if a.github_output:
        with open(a.github_output, "a") as f:
            for p, v in outs.items():
                f.write("%s=%s\n" % (p, v))

if __name__ == "__main__":
    main()
