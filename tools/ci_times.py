#!/usr/bin/env python3
"""Refresh tools/ci_times.tsv (the measured seconds per test that tools/ci_select.py balances the CI shards with) from ci.yml runs.

  python3 tools/ci_times.py RUN_ID [RUN_ID ...]          reads the logs with `gh run view --log`, keeps the larger time per test
  python3 tools/ci_times.py --report RUN_ID [...]        prints the slowest tests and the job/step minutes instead (docs/CI-SPEED-3.9.md)
Tests not in the given runs keep their old line, so a run of a few areas only updates those.
"""
import json, os, re, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "tools", "ci_times.tsv")
REPO = "adswill/OnAir"
PAT = re.compile(r"^([^\t]*)\t.*?Test\s+#\d+:\s+(\S+)\s+\.+.*?(?:Passed|Failed|\*\*\*\S+)\s+([\d.]+)\s+sec")
PLATS = ("linux", "macos", "windows")

def plat_of(job):
    return "linux" if "Linux" in job else "macos" if "macOS" in job else "windows" if "Windows" in job else None

def read_run(rid, times):
    log = subprocess.run(["gh", "run", "view", str(rid), "-R", REPO, "--log"], capture_output=True, text=True, errors="replace").stdout
    for line in log.splitlines():
        m = PAT.search(line)
        if m and plat_of(m.group(1)):
            d = times.setdefault(plat_of(m.group(1)), {})
            d[m.group(2)] = max(d.get(m.group(2), 0.0), float(m.group(3)))

def report(rids, times):
    for p in PLATS:
        d = times.get(p, {})
        print("%s: %d tests, %.0f test-minutes" % (p, len(d), sum(d.values()) / 60))
    allt = sorted(set().union(*[set(d) for d in times.values()]), key=lambda t: -sum(times[p].get(t, 0) for p in times))
    print("%-36s %7s %7s %7s" % ("test", *PLATS))
    for t in allt[:40]:
        print("%-36s %7.0f %7.0f %7.0f" % (t, *(times.get(p, {}).get(t, 0) for p in PLATS)))
    for rid in rids:
        jobs = json.loads(subprocess.run(["gh", "api", "repos/%s/actions/runs/%s/jobs?per_page=100" % (REPO, rid)], capture_output=True, text=True).stdout)["jobs"]
        from datetime import datetime
        P = lambda s: datetime.fromisoformat(s.replace("Z", "+00:00"))
        tot = 0.0
        print("run", rid)
        for j in jobs:
            if not (j.get("started_at") and j.get("completed_at")): continue
            m = (P(j["completed_at"]) - P(j["started_at"])).total_seconds() / 60
            tot += m
            steps = {s["name"][:24]: round((P(s["completed_at"]) - P(s["started_at"])).total_seconds() / 60, 1)
                     for s in j["steps"] if s.get("started_at") and s.get("completed_at") and (P(s["completed_at"]) - P(s["started_at"])).total_seconds() > 30}
            print("  %-30s %-9s %6.1f min  %s" % (j["name"][:30], j["conclusion"], m, steps))
        print("  runner minutes: %.0f" % tot)

def main():
    a = sys.argv[1:]
    rep = "--report" in a
    rids = [x for x in a if x != "--report"]
    if not rids: sys.exit(__doc__)
    times = {}
    for r in rids: read_run(r, times)
    if rep: return report(rids, times)
    old, head = {}, []
    if os.path.exists(OUT):
        for line in open(OUT):
            if line.startswith("#"): head.append(line); continue
            if line.startswith("test\t"): continue
            c = line.rstrip("\n").split("\t")
            old[c[0]] = c[1:] + [""] * (3 - len(c[1:]))
    for p_i, p in enumerate(PLATS):
        for t, s in times.get(p, {}).items():
            old.setdefault(t, ["", "", ""])[p_i] = "%.1f" % s
    with open(OUT, "w", newline="\n") as f:
        f.writelines(head)
        f.write("test\t" + "\t".join(PLATS) + "\n")
        for t in sorted(old): f.write(t + "\t" + "\t".join(old[t][:3]) + "\n")
    print("wrote %s: %d tests" % (OUT, len(old)))

if __name__ == "__main__":
    main()
