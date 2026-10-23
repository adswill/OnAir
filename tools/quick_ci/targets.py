#!/usr/bin/env python3
"""Quick CI: map a ctest -R regex to the cmake targets those tests run.
usage: targets.py BUILD_DIR REGEX   -> prints the target names (one line, space separated; empty = nothing found)
Reads `ctest --show-only=json-v1`, takes every token of every test command, and keeps those that are real targets."""
import json, os, re, subprocess, sys
build, rx = sys.argv[1], sys.argv[2]
tests = json.loads(subprocess.run(["ctest", "--test-dir", build, "-R", rx, "--show-only=json-v1"],
                                  capture_output=True, text=True, check=True).stdout).get("tests", [])
help_out = subprocess.run(["cmake", "--build", build, "--target", "help"], capture_output=True, text=True).stdout
known = set(re.findall(r"^(?:\.\.\. )?([A-Za-z0-9_.+-]+)", help_out, re.M))
out = []
for t in tests:
    for tok in t.get("command", []):
        name = os.path.basename(tok)
        if name in known and name not in out and not name.startswith("cmake"):
            out.append(name)
sys.stderr.write(f"{len(tests)} tests match, {len(out)} targets\n")
print(" ".join(out))
