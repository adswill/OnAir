#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors
"""List the tests that failed and then passed in a ctest log written with --repeat until-pass:N (ci.yml's timing tests).

  ci_flaky_report.py ctest-timing.log [more logs ...] [--tsv flaky.tsv]

Prints a Markdown table (for $GITHUB_STEP_SUMMARY) and, with --tsv, appends "test<TAB>attempts" lines that a nightly job or a person can
add up. Always exits 0: the report must never fail a run. A test that fails on every attempt is a real failure that ctest already reports.
"""
import re
import sys
from collections import defaultdict


def parse(paths):
    seen = defaultdict(list)   # test -> results in order
    for path in paths:
        try:
            text = open(path, errors="replace").read()
        except OSError:
            continue
        for m in re.finditer(r"^\s*\d+/\d+ Test\s+#\d+: (\S+) \.+\s*(?:\*\*\*)?(Passed|Failed|Timeout|Exception|\S+?)\b", text, re.M):
            seen[m.group(1)].append("pass" if m.group(2) == "Passed" else "fail")
        for m in re.finditer(r"^\s*Test (?:Passed|Failed)\.?\s*$", text, re.M):
            pass
    return seen


def main(argv):
    tsv = None
    paths = []
    it = iter(argv[1:])
    for a in it:
        if a == "--tsv":
            tsv = next(it, None)
        else:
            paths.append(a)
    seen = parse(paths)
    flaky = {t: r for t, r in seen.items() if "fail" in r and r[-1] == "pass"}
    if not flaky:
        print("No test needed a retry.")
        return 0
    print("| Test | Attempts | Result |\n|---|--:|---|")
    for t, r in sorted(flaky.items()):
        print(f"| `{t}` | {len(r)} | failed {r.count('fail')}x, then passed |")
    if tsv:
        with open(tsv, "a") as f:
            for t, r in sorted(flaky.items()):
                f.write(f"{t}\t{len(r)}\n")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
