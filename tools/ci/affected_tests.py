#!/usr/bin/env python3
"""Test impact selection for the shared incremental build directory (docs/BUILD-SPEED.md).

usage: affected_tests.py BUILD_DIR SINCE_EPOCH [--changed-file LIST] [--max-fraction 0.9]

After `cmake --build` in a build directory that is reused between refs, ninja has relinked exactly the programs whose
sources, headers, libraries or Rust crates changed. So a test is AFFECTED when
  * the program it runs (or any other existing file named in its command line) is newer than SINCE_EPOCH, or
  * a file named in the command line (or below a directory named there) is in the list of changed files (data files).
Always-run smoke tests are added. Prints one ctest -R regex on stdout (nothing else), and the reasoning on stderr.
If the CMake files changed, or every test would run anyway, it prints '.' (= everything).
"""
import json
import os
import subprocess
import sys

SMOKE = ["mode_numbers", "freq_db"]


def main():
    build = sys.argv[1]
    since = float(sys.argv[2])
    changed = []
    if "--changed-file" in sys.argv:
        p = sys.argv[sys.argv.index("--changed-file") + 1]
        if os.path.exists(p):
            changed = [l.strip() for l in open(p) if l.strip()]
    frac = float(sys.argv[sys.argv.index("--max-fraction") + 1]) if "--max-fraction" in sys.argv else 0.9
    root0 = os.path.dirname(os.path.abspath(os.path.join(build, "..", "x")))
    cm = [os.path.join(root0, "CMakeLists.txt")] + [os.path.join(root0, "cmake", f) for f in os.listdir(os.path.join(root0, "cmake"))
                                                    if os.path.isdir(os.path.join(root0, "cmake"))]
    if any(c == "CMakeLists.txt" or c.startswith("cmake/") for c in changed) or any(
            os.path.isfile(f) and os.path.getmtime(f) >= since for f in cm):
        print("CMake files changed: run everything", file=sys.stderr)
        print(".")
        return
    out = subprocess.run(["ctest", "--show-only=json-v1"], cwd=build, capture_output=True, text=True, check=True).stdout
    tests = json.loads(out)["tests"]
    root = os.path.dirname(os.path.dirname(os.path.abspath(os.path.join(build, "CMakeCache.txt"))))
    changed_abs = [os.path.abspath(os.path.join(root, c)) for c in changed]
    hit, why = [], {}
    for t in tests:
        name = t["name"]
        cmd = t.get("command") or []
        reason = None
        for a in cmd:
            if not a or not os.path.isabs(a):
                continue
            try:
                if os.path.isfile(a) and os.path.getmtime(a) >= since:
                    reason = "rebuilt/updated: " + os.path.basename(a)
                    break
            except OSError:
                pass
            for c in changed_abs:
                if c == a or (os.path.isdir(a) and c.startswith(a.rstrip("/") + "/")):
                    reason = "data changed: " + os.path.relpath(c, root)
                    break
            if not reason and os.path.isdir(a) and not a.startswith(os.path.abspath(build)):
                for dp, _, fs in os.walk(a):
                    if any(os.path.getmtime(os.path.join(dp, f)) >= since for f in fs if os.path.exists(os.path.join(dp, f))):
                        reason = "data changed under " + os.path.relpath(a, root)
                        break
            if reason:
                break
        if not cmd:
            reason = "no command"
        if reason:
            hit.append(name)
            why[name] = reason
    names = sorted(set(hit) | {t["name"] for t in tests if t["name"] in SMOKE})
    print(f"affected {len(hit)} of {len(tests)} tests (+{len(names) - len(hit)} smoke)", file=sys.stderr)
    for n in names[:60]:
        print(f"  {n}: {why.get(n, 'smoke')}", file=sys.stderr)
    if len(hit) >= frac * len(tests):
        print("most tests affected: run everything", file=sys.stderr)
        print(".")
        return
    if not names:
        print("nothing affected", file=sys.stderr)
        print("^$")
        return
    print("^(" + "|".join(n.replace("+", "\\+") for n in names) + ")$")


if __name__ == "__main__":
    main()
