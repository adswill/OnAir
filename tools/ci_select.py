#!/usr/bin/env python3
"""Affected-test selection for CI and tools/fast_check.sh.

Reads the configured build dir (ctest test list + compile_commands.json, configure with -DCMAKE_EXPORT_COMPILE_COMMANDS=ON),
maps the files changed against --base to test areas (core/src/<mode>/, app/<mode>/, tools/<mode>tool.cpp, tests/<mode>/ -> tests/<mode>/),
adds a small smoke set, optionally takes one shard of the result, and writes:
  <out>/targets.txt      build targets (one per line)  -> cmake --build build --target $(cat targets.txt)
  <out>/fast.regex       ctest -R regex, tests without the slow/timing labels
  <out>/timing.regex     ctest -R regex, timing tests (run apart, -j 2, retried)
  <out>/summary.txt      what was chosen and why
Scope: 'full' (every test incl. slow), 'wide' (every non-slow test: a shared file changed), 'area' (affected areas + smoke), 'smoke'.
"""
import argparse, json, os, re, subprocess, sys, zlib

SHARED = {"dsp", "engine", "common", "fec", "source", "session", "sessionmodes", "state", "files", "net", "ofdm", "audio",
          "codec", "channels", "fastscan", "doctor", "export"}
# the checks that protect the whole program run on every change, whatever its area: the real tests behind tools/gate/gate_check.py included
SMOKE = ["bandwidth", "direction", "u8file", "ros_detect", "atsc_fec", "dme", "sync", "i18n", "gui_rules", "credits", "mode_numbers", "web_safety"]
# web/ and relay/ are not here: the JavaScript tests have no build target of their own, so a change there selects every non-slow test
NO_TEST_PREFIX = ("docs/", "android/", "firmware/", "packaging/", "shots/", "scratchpad/", "assets/", "LICENSES/",
                  ".github/workflows/android", ".github/workflows/release", ".github/workflows/nix", ".github/workflows/sens",
                  ".github/workflows/gate", ".github/workflows/windows-check")
NO_TEST_EXT = (".md", ".txt", ".png", ".svg", ".toml")

def sh(*a):
    return subprocess.run(a, capture_output=True, text=True).stdout

def areas_of(f):
    """-> ('none'|'wide'|'area', set_of_areas)"""
    if f.startswith(NO_TEST_PREFIX) or f.endswith(NO_TEST_EXT) and not f.startswith("tests/"):
        return "none", set()
    p = f.split("/")
    if f in ("CMakeLists.txt",) or p[0] in ("cmake", "third_party", "rust") or f.startswith(".github/workflows/ci") or f.startswith("tools/ci_"):
        return "wide", set()
    if p[0] == "core" and len(p) > 2 and p[1] in ("src", "include"):
        a = p[3] if p[1] == "include" and len(p) > 3 and p[2] == "dect2" and len(p) > 4 else p[2]
        if p[1] == "include" and len(p) <= 4:
            return "wide", set()   # core/include/dect2/<header>.h: shared
        return ("wide", set()) if a in SHARED else ("area", {os.path.splitext(a)[0]})
    if p[0] == "app" and len(p) > 2:
        return ("wide", set()) if p[1] in SHARED or p[1] in ("i18n",) else ("area", {p[1]})
    if p[0] == "app":
        return "area", {"app"}
    if p[0] == "tools":
        m = re.match(r"([a-z0-9]+)tool\.cpp$", p[-1])
        return ("area", {m.group(1)}) if m else ("none", set())
    if p[0] == "tests" and len(p) > 2:
        return ("wide", set()) if p[1] in ("common",) else ("area", {p[1]})
    if p[0] in ("data", "sdk", "server", "voice_listen"):
        return "wide", set()
    return "wide", set()

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build-dir", required=True)
    ap.add_argument("--src", default=os.getcwd())
    ap.add_argument("--base", default="")
    ap.add_argument("--scope", default="auto", choices=["auto", "full", "wide", "area", "smoke"])
    ap.add_argument("--shard", default="1/1")
    ap.add_argument("--out", required=True)
    ap.add_argument("--changed", help="file with changed paths instead of git diff")
    a = ap.parse_args()
    bd, src = a.build_dir, os.path.realpath(a.src)
    tj = json.loads(sh("ctest", "--test-dir", bd, "--show-only=json-v1"))["tests"]
    tests = {}
    for t in tj:
        lab = set()
        for pr in t.get("properties", []):
            if pr["name"] == "LABELS":
                lab = set(pr["value"])
        tests[t["name"]] = lab
    cc = json.load(open(os.path.join(bd, "compile_commands.json")))
    tgt_src = {}
    for e in cc:
        m = re.search(r"CMakeFiles/([^/]+)\.dir/", e.get("output", ""))
        if m:
            tgt_src.setdefault(m.group(1), set()).add(os.path.relpath(e["file"], src))
    cm = open(os.path.join(src, "CMakeLists.txt")).read()
    cmd = dict(re.findall(r"add_test\(\s*NAME\s+(\S+)\s+COMMAND\s+(\w+)", cm))
    for n, tg in re.findall(r"add_test\(\s*NAME\s+(\S+)\s+COMMAND[^\n]*?TARGET_FILE:(\w+)", cm):
        cmd.setdefault(n, tg)
    test_tgt, test_area = {}, {}
    for n in tests:
        tg = next((c for c in ("test_" + n, n, cmd.get(n)) if c in tgt_src), None)
        if tg in tgt_src:
            test_tgt[n] = tg
            test_area[n] = {s.split("/")[1] for s in tgt_src[tg] if s.startswith("tests/") and s.count("/") >= 2}
    # changed files -> scope
    if a.changed:
        files = [l.strip() for l in open(a.changed) if l.strip()]
    elif a.base:
        files = sh("git", "-C", src, "diff", "--name-only", a.base + "...HEAD").split() or sh("git", "-C", src, "diff", "--name-only", a.base).split()
    else:
        files = []
    scope, areas, why = "smoke", set(), []
    for f in files:
        k, ar = areas_of(f)
        if k == "wide":
            scope = "wide"; why.append("shared: " + f)
        elif k == "area":
            areas |= ar
            if scope != "wide": scope = "area"
    known = set().union(*test_area.values()) if test_area else set()
    # an area without a tests/ dir of that name has no tests of its own: treat as wide (safe)
    lost = {x for x in areas if x not in known}
    if lost and a.base:
        scope = "wide"; why.append("no tests dir for: " + ",".join(sorted(lost)))
    if a.scope != "auto":
        scope = a.scope
    # tests changed directly are already covered by their area
    def is_slow(n): return "slow" in tests[n] or "perf" in tests[n]
    if scope == "full": sel = set(tests)
    elif scope == "wide": sel = {n for n in tests if not is_slow(n)}
    elif scope == "area":
        sel = {n for n in tests if test_area.get(n, set()) & areas and not is_slow(n)} | {n for n in SMOKE if n in tests}
        if not test_tgt.keys() & sel: sel = {n for n in SMOKE if n in tests}
    else: sel = {n for n in SMOKE if n in tests}
    k, n_sh = (int(x) for x in a.shard.split("/"))
    sel = {n for n in sel if zlib.crc32(n.encode()) % n_sh == k - 1}
    os.makedirs(a.out, exist_ok=True)
    unmapped = sorted(n for n in sel if n not in test_tgt)
    if os.path.exists(os.path.join(bd, "build.ninja")):
        real = {l.split(":")[0] for l in sh("ninja", "-C", bd, "-t", "targets", "all").splitlines()}
    else:
        real = {l.replace("... ", "").split(" ")[0] for l in sh("cmake", "--build", bd, "--target", "help").splitlines()}
    # a name seen in compile_commands that is no real build target here (OBJECT libraries, platform conditionals) is dropped
    targets = sorted({test_tgt[n] for n in sel if n in test_tgt and test_tgt[n] in real})
    # tests with no known target (fuzz, tools-driven): only built when the whole tree is, so they need --target all
    need_all = False
    if unmapped:   # python/node tests drive the server binary
        targets = sorted(set(targets) | ({"onair-server"} & set(tgt_src)))
    rx = lambda names: "^(" + "|".join(re.escape(x) for x in sorted(names)) + ")$" if names else "^$NONE$"
    timing = {n for n in sel if "timing" in tests[n]}
    open(os.path.join(a.out, "targets.txt"), "w", newline="\n").write("\n".join(targets) + "\n")
    open(os.path.join(a.out, "fast.regex"), "w", newline="\n").write(rx(sel - timing))
    open(os.path.join(a.out, "timing.regex"), "w", newline="\n").write(rx(timing))
    open(os.path.join(a.out, "need_all"), "w", newline="\n").write("1" if need_all else "0")
    s = f"scope={scope} areas={sorted(areas)} changed={len(files)} shard={a.shard}\ntests={len(sel)} (timing {len(timing)}) targets={len(targets)} of {len(set(test_tgt.values()))} unmapped_tests={len(unmapped)}\n" + "\n".join(why[:10])
    open(os.path.join(a.out, "summary.txt"), "w", newline="\n").write(s + "\n")
    print(s)
    if sel and not targets:
        sys.exit("ci_select: tests selected but no build targets found (compile_commands.json format?)")

if __name__ == "__main__":
    main()
