#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

"""The tests that failed in a CI run, and the one command that runs only them again (docs/CI-FAST.md (4.7)).

Before a release, a full run that found five failures used to mean: fix them, then another full run of hours, which found two more. The
gather jobs of ci.yml call this on the shards' ctest logs and print the failures with a quick.yml command that rebuilds and reruns only
those tests (one job, minutes). Run the full scope again only once that passes.
  tools/ci_failed.py --logs DIR --os linux|macos|windows --ref BRANCH [--repo adswill/OnAir]
"""
import argparse, glob, os, re

PAT = re.compile(r"^\s*\d+\s+-\s+(\S+)\s+\(([^)]+)\)")

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--logs", required=True, help="directory with the downloaded shard artifacts (ctest-*.log anywhere below)")
    ap.add_argument("--os", required=True)
    ap.add_argument("--ref", required=True)
    ap.add_argument("--repo", default="adswill/OnAir")
    a = ap.parse_args()
    failed = {}
    for f in glob.glob(os.path.join(a.logs, "**", "ctest-*.log"), recursive=True):
        timing = "timing" in os.path.basename(f)
        inside = False
        for line in open(f, errors="replace"):
            if line.startswith("The following tests FAILED"):
                inside = True; continue
            if inside:
                m = PAT.match(line)
                if not m:
                    inside = False; continue
                # a timing test retried once (--repeat until-pass:2) lists only the tests that failed both times
                failed[m.group(1)] = m.group(2) + (" (timing, failed twice)" if timing else "")
    if not failed:
        print("No failed tests in the shard logs.")
        return
    names = sorted(failed)
    print("### %d failed test(s) on %s\n" % (len(names), a.os))
    for n in names:
        print("- `%s`: %s" % (n, failed[n]))
    print("\nRun only these again after a fix (one job, minutes), and the full scope once they pass:\n")
    print("```\ngh workflow run quick.yml -R %s --ref %s -f os=%s -f tests=\"%s\"\n```" % (a.repo, a.ref, a.os, " ".join(names)))

if __name__ == "__main__":
    main()
