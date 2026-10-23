#!/usr/bin/env python3
"""Quick CI: markdown summary for $GITHUB_STEP_SUMMARY.
usage: summary.py --title T [--build-log F] [--junit F ...] [--ccache F] [--times label=seconds ...]
JUnit files come from `ctest --output-junit`. Prints markdown."""
import argparse, re
import xml.etree.ElementTree as ET
ap = argparse.ArgumentParser()
ap.add_argument("--title", default="Quick CI")
ap.add_argument("--build-log")
ap.add_argument("--junit", nargs="*", default=[])
ap.add_argument("--ccache")
ap.add_argument("--times", nargs="*", default=[], help="label=seconds")
a = ap.parse_args()

passed = failed = skipped = 0
failing, slowest = [], []
for f in a.junit:
    try:
        root = ET.parse(f).getroot()
    except Exception:
        continue
    for tc in root.iter("testcase"):
        name, t = tc.get("name"), float(tc.get("time") or 0)
        slowest.append((t, name))
        if tc.find("failure") is not None or tc.find("error") is not None:
            failed += 1
            failing.append(name)
        elif tc.find("skipped") is not None or tc.get("status") == "notrun":
            skipped += 1
        else:
            passed += 1

errs = []
if a.build_log:
    try:
        for line in open(a.build_log, errors="replace"):
            if re.search(r"[^\s:]+:\d+(:\d+)?: (fatal )?error|undefined reference|error\[E\d+\]|CMake Error", line):
                errs.append(line.rstrip()[:300])
    except OSError:
        pass

hit = ""
if a.ccache:
    try:
        txt = open(a.ccache, errors="replace").read()
        m = re.search(r"Hits:\s+(\d+)\s*/\s*(\d+)\s*\(([\d.]+)%\)", txt) or re.search(r"cache hit rate\s+([\d.]+) %", txt)
        hit = m.group(0).strip() if m else ""
    except OSError:
        pass

out = [f"## {a.title}", ""]
out.append(f"**Tests:** {passed} passed, {failed} failed, {skipped} skipped" + ("" if (passed + failed + skipped) else "  (none ran)"))
if a.times:
    out.append("**Time:** " + ", ".join(a.times))
if hit:
    out.append(f"**ccache:** {hit}")
if failing:
    out += ["", "### Failing tests", ""] + [f"- `{n}`" for n in sorted(set(failing))]
if errs:
    out += ["", "### First compile/link errors", "", "```"] + errs[:15] + ["```"]
if slowest:
    out += ["", "<details><summary>Slowest tests</summary>", ""] + [f"- {t:.1f} s `{n}`" for t, n in sorted(slowest, reverse=True)[:8]] + ["", "</details>"]
print("\n".join(out))
