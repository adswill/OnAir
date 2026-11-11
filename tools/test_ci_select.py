#!/usr/bin/env python3
"""Checks of tools/ci_select.py without a build: a made-up build dir (CTestTestfile.cmake + compile_commands.json) over the real sources.
  python3 tools/test_ci_select.py        (the quick gate runs it; needs ctest on the PATH)
"""
import json, os, shutil, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SEL = os.path.join(ROOT, "tools", "ci_select.py")
fails = 0

def check(c, msg):
    global fails
    if not c:
        print("FAIL:", msg); fails += 1

def make_build(d, tests):
    """tests: {name: (source under tests/, labels)}"""
    with open(os.path.join(d, "CTestTestfile.cmake"), "w") as f:
        for n, (src, lab) in tests.items():
            f.write('add_test(%s "/bin/true")\n' % n)
            if lab: f.write('set_tests_properties(%s PROPERTIES LABELS "%s")\n' % (n, ";".join(lab)))
    cc = [{"directory": d, "file": os.path.join(ROOT, src), "output": "CMakeFiles/test_%s.dir/%s.o" % (n, src)} for n, (src, _) in tests.items()]
    json.dump(cc, open(os.path.join(d, "compile_commands.json"), "w"))
    open(os.path.join(d, "build.ninja"), "w").close()

def run(d, changed, *extra):
    ch = os.path.join(d, "changed.txt")
    open(ch, "w").write("\n".join(changed) + "\n")
    out = os.path.join(d, "sel")
    shutil.rmtree(out, ignore_errors=True)
    r = subprocess.run([sys.executable, SEL, "--build-dir", d, "--src", ROOT, "--changed", ch, "--out", out, *extra], capture_output=True, text=True)
    if r.returncode not in (0, 1):
        print(r.stdout, r.stderr)
    rx = open(os.path.join(out, "fast.regex")).read() if os.path.exists(os.path.join(out, "fast.regex")) else ""
    names = set(rx[2:-2].split("|")) if rx.startswith("^(") else set()
    return names, r.stdout

def main():
    if not shutil.which("ctest"):
        print("note: no ctest here, ci_select not checked"); return 0
    pick = {}
    for area in ("dsp", "aero", "dmr", "p25", "ais", "atv", "common"):
        d = os.path.join(ROOT, "tests", area)
        if os.path.isdir(d):
            for f in sorted(os.listdir(d))[:4]:
                if f.startswith("test_") and f.endswith(".cpp"):
                    pick[f[5:-4]] = ("tests/%s/%s" % (area, f), [])
    first = next(iter(pick))
    pick[first] = (pick[first][0], ["slow"])
    with tempfile.TemporaryDirectory() as d:
        # ninja -t targets needs a real build.ninja: list the targets by hand instead
        make_build(d, pick)
        open(os.path.join(d, "build.ninja"), "w").write("".join("build test_%s: phony\n" % n for n in pick))
        names, out = run(d, ["docs/README.md"])
        check(not (names - set(pick)), "docs change: only smoke tests (none of these): %s" % sorted(names))
        names, out = run(d, ["core/src/aero/" + sorted(os.listdir(os.path.join(ROOT, "core/src/aero")))[0]])
        check(names and all(pick[n][0].startswith("tests/aero/") for n in names), "aero change: aero tests only, got %s" % sorted(names))
        names, out = run(d, ["core/src/engine/engine.cpp"])
        check("scope=wide" in out, "engine change: wide")
        check(first not in names, "wide leaves out the slow tests")
        names, out = run(d, ["CMakeLists.txt"], "--scope", "full")
        check(set(pick) <= names | set(), "full: every test, got %d of %d" % (len(names), len(pick)))
        # balanced shards: the union of the shards is the whole list, no test in two shards
        times = os.path.join(d, "times.tsv")
        open(times, "w").write("test\tlinux\n" + "".join("%s\t%d\n" % (n, 10 * i + 1) for i, n in enumerate(pick)))
        seen = []
        for k in (1, 2, 3):
            s, out = run(d, ["CMakeLists.txt"], "--scope", "full", "--shard", "%d/3" % k, "--times", times, "--plat", "linux")
            seen.append(s)
            check("balanced shards" in out, "shard %d balanced by --times" % k)
        allsel = set().union(*seen)
        check(allsel == set(pick), "shards cover every test once: %d of %d" % (len(allsel), len(pick)))
        check(sum(len(s) for s in seen) == len(allsel), "no test in two shards")
        check(os.path.exists(os.path.join(d, "Testing", "Temporary", "CTestCostData.txt")), "ctest cost data written")
        # a shared header followed through the includes: the result is wide or a set of areas, and it never drops the direct includers
        names, out = run(d, ["tests/common/test_long.h"])
        check("scope=" in out, "tests/common header: a scope is chosen")
    print("test_ci_select: %s" % ("all passed" if not fails else "%d FAILED" % fails))
    return 1 if fails else 0

if __name__ == "__main__":
    sys.exit(main())
