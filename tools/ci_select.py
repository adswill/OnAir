#!/usr/bin/env python3
"""Affected-test selection for CI and tools/fast_check.sh.

Reads the configured build dir (ctest test list + compile_commands.json, configure with -DCMAKE_EXPORT_COMPILE_COMMANDS=ON),
maps the files changed against --base to test areas (core/src/<mode>/, app/<mode>/, tools/<mode>tool.cpp, tests/<mode>/ -> tests/<mode>/),
adds a small smoke set, optionally takes one shard of the result, and writes:
  <out>/targets.txt      build targets (one per line)  -> cmake --build build --target $(cat targets.txt)
  <out>/fast.regex       ctest -R regex, tests without the slow/timing labels
  <out>/timing.regex     ctest -R regex, timing tests (run apart, -j 2, retried)
  <out>/summary.txt      what was chosen and why
Scope: 'full' (every test incl. slow), 'wide' (every non-slow test: a shared file changed), 'area' (affected areas + smoke), 'smoke',
'long' (the tests with a long form, tests/common/test_long.h: the nightly run sets ONAIR_TEST_LONG=1 for them).

A file of a shared area (dsp, fec, core headers, tests/common ...) is followed through the #include graph (docs/CI-SPEED-3.9.md): the tests
whose own sources reach it and the mode areas whose code reaches it are chosen. It stays 'wide' when the engine, the session or the app
reach it, when a .cpp has no header of its own name, or when more than half of the areas are hit.
--times tools/ci_times.tsv: shards are balanced by measured seconds (longest first onto the least loaded shard) instead of a name hash, and
<build>/Testing/Temporary/CTestCostData.txt is written so ctest -j starts the longest tests first.
"""
import argparse, json, os, re, subprocess, sys, zlib

SHARED = {"dsp", "engine", "common", "fec", "source", "session", "sessionmodes", "state", "files", "net", "ofdm", "audio",
          "codec", "channels", "fastscan", "doctor", "export"}
# a change that reaches these through the includes can touch any mode: wide
HUB_PREFIX = ("core/src/engine/", "core/include/dect2/engine", "core/src/session/", "core/include/dect2/session", "app/main", "app/app.",
              "app/ui/", "core/src/source/", "core/include/dect2/source")
# the checks that protect the whole program run on every change, whatever its area: the real tests behind tools/gate/gate_check.py included
SMOKE = ["bandwidth", "direction", "u8file", "ros_detect", "atsc_fec", "dme", "sync", "i18n", "gui_rules", "credits", "mode_numbers", "web_safety"]
# web/ and relay/ are not here: the JavaScript tests have no build target of their own, so a change there selects every non-slow test
NO_TEST_PREFIX = ("docs/", "android/", "firmware/", "packaging/", "shots/", "scratchpad/", "assets/", "LICENSES/",
                  ".github/workflows/android", ".github/workflows/release", ".github/workflows/nix", ".github/workflows/sens",
                  ".github/workflows/gate", ".github/workflows/windows-check", ".github/workflows/quick")
NO_TEST_EXT = (".md", ".txt", ".png", ".svg", ".toml")
SRC_EXT = (".h", ".hpp", ".cpp", ".c", ".inc", ".mm")
INC_ROOTS = ("core/include", "core/src", "app", "tests", ".", "sdk/include", "server")
DEFAULT_COST = 5.0   # seconds of a test with no measured time (a new test)

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

def area_of_src(f, known=()):
    """the test area a source file belongs to (core/src/<a>/, core/include/dect2/<a>/, app/<a>/, tests/<a>/), or None; app/<a>/ with no
    tests/<a>/ is tested by tests/app/"""
    p = f.split("/")
    if len(p) > 3 and p[0] == "core" and p[1] == "src": return p[2]
    if len(p) > 4 and p[0] == "core" and p[1] == "include" and p[2] == "dect2": return p[3]
    if len(p) > 2 and p[0] == "app": return p[1] if p[1] in known else "app"
    if len(p) > 2 and p[0] == "tests": return p[1]
    return None

class IncludeGraph:
    """reverse #include "..." graph of the repository's own sources"""
    RX = re.compile(r'^[ \t]*#[ \t]*include[ \t]*"([^"]+)"', re.M)

    def __init__(self, src):
        self.files = set()
        for d in ("core", "app", "tests", "tools", "server", "sdk", "voice_listen"):
            for dp, _, fn in os.walk(os.path.join(src, d)):
                for f in fn:
                    if f.endswith(SRC_EXT):
                        self.files.add(os.path.relpath(os.path.join(dp, f), src).replace(os.sep, "/"))
        self.rev, self.fwd = {}, {}
        for f in self.files:
            try:
                txt = open(os.path.join(src, f), errors="replace").read()
            except OSError:
                continue
            for inc in self.RX.findall(txt):
                cand = [os.path.normpath(os.path.join(os.path.dirname(f), inc))] + [os.path.normpath(os.path.join(r, inc)) for r in INC_ROOTS]
                hit = next((c.replace(os.sep, "/") for c in cand if c.replace(os.sep, "/") in self.files), None)
                if hit:
                    self.rev.setdefault(hit, set()).add(f)
                    self.fwd.setdefault(f, set()).add(hit)

    def reach(self, start, known):
        """every file that includes one of start, directly or through other headers (start included). A library .cpp reached passes the
        change on to the headers of its own name (their callers run it). A hub (engine, session, app) reached through a mode's header only
        sees that mode's types, whose tests run: it is not followed further; reached any other way, the change can touch every mode.
        -> (files, library .cpp files with no header of their own, hubs reached directly)"""
        seen, todo, blind, hub = set(start), [(s, False) for s in start], set(), set()
        while todo:
            f, via_mode = todo.pop()
            if f.startswith(HUB_PREFIX) and f not in start:
                if not via_mode: hub.add(f)
                continue
            ar = area_of_src(f, known)
            via_mode = via_mode or (ar is not None and ar not in SHARED and not f.startswith("tests/"))
            nxt = set(self.rev.get(f, ()))
            if f.endswith((".cpp", ".c", ".mm")) and f.startswith("core/") and f not in start:
                own = self.seeds(f)
                if own is None: blind.add(f)
                else: nxt |= own
            for g in nxt:
                if g not in seen:
                    seen.add(g); todo.append((g, via_mode))
        return seen, blind, hub

    def seeds(self, f):
        """what a change to f can reach: a header itself; a .cpp itself plus the headers of its own name it includes"""
        if not f.endswith((".cpp", ".c", ".mm")):
            return {f}
        stem = os.path.splitext(os.path.basename(f))[0]
        inc = self.fwd.get(f, ())
        own = {h for h in inc if os.path.splitext(os.path.basename(h))[0] == stem}
        if not own:   # no header of its name: its functions are declared in a header of its own area that it includes
            ar = area_of_src(f)
            own = {h for h in inc if ar and area_of_src(h) == ar and not h.endswith((".cpp", ".c", ".mm"))}
        return ({f} | own) if own else None   # None: callers unknown

def load_times(path, plat):
    t = {}
    if not path or not os.path.exists(path):
        return t
    cols = None
    for line in open(path):
        if line.startswith("#") or not line.strip():
            continue
        c = line.rstrip("\n").split("\t")
        if cols is None:
            cols = c; continue
        if plat in cols and len(c) == len(cols):
            try: t[c[0]] = float(c[cols.index(plat)])
            except ValueError: pass
    return t

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build-dir", required=True)
    ap.add_argument("--src", default=os.getcwd())
    ap.add_argument("--base", default="")
    ap.add_argument("--scope", default="auto", choices=["auto", "full", "wide", "area", "smoke", "long"])
    ap.add_argument("--shard", default="1/1")
    ap.add_argument("--out", required=True)
    ap.add_argument("--changed", help="file with changed paths instead of git diff")
    ap.add_argument("--times", help="tools/ci_times.tsv: measured seconds per test and platform (balanced shards, ctest cost data)")
    ap.add_argument("--plat", default="linux", help="column of --times: linux, macos or windows")
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
        m = re.search(r"CMakeFiles/([^/]+)\.dir/", e.get("output", "") or e.get("command", ""))
        if m:
            fp = e["file"] if os.path.isabs(e["file"]) else os.path.join(e.get("directory", bd), e["file"])
            tgt_src.setdefault(m.group(1), set()).add(os.path.relpath(fp, src).replace(os.sep, "/"))
    cm = open(os.path.join(src, "CMakeLists.txt")).read()
    cmd = dict(re.findall(r"add_test\(\s*NAME\s+(\S+)\s+COMMAND\s+([\w-]+)", cm))
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
    direct = set()   # tests chosen because their own sources reach a changed shared file
    graph = None
    known = set().union(*test_area.values()) if test_area else set()
    for f in files:
        k, ar = areas_of(f)
        if k == "wide" and f.endswith(SRC_EXT) and (f.startswith("core/") or f.startswith("tests/common/")):
            # a shared source file: follow the includes instead of running everything
            graph = graph or IncludeGraph(src)
            seeds = graph.seeds(f) if f in graph.files and not f.startswith(HUB_PREFIX) else None
            if seeds is None:
                scope = "wide"; why.append("shared: " + f + (" (engine, session, source or app)" if f.startswith(HUB_PREFIX) else " (no header of its own)")); continue
            hit, blind, hub = graph.reach(seeds, known)
            hub = sorted(hub)
            all_areas = {x for x in (area_of_src(h, known) for h in hit if not h.startswith("tests/")) if x and x not in SHARED}
            hit_areas = all_areas & known | {x for x in (area_of_src(h, known) for h in hit if h.startswith("tests/")) if x and x not in SHARED and x in known}
            untested = sorted(all_areas - known)   # code reached in an area with no tests/ dir of its own: its tests live elsewhere
            if hub or blind or untested or len(hit_areas) * 2 > max(1, len(known)):
                if hub: r = hub[0]
                elif blind: r = sorted(blind)[0] + ", which has no header of its own"
                elif untested: r = "areas without tests: " + ",".join(untested[:3])
                else: r = "%d areas" % len(hit_areas)
                scope = "wide"; why.append("shared: %s (reaches %s)" % (f, r))
                continue
            areas |= hit_areas
            direct |= {n for n, tg in test_tgt.items() if tgt_src[tg] & hit}
            why.append("followed: %s -> %d areas, %d tests" % (f, len(hit_areas), len({n for n, tg in test_tgt.items() if tgt_src[tg] & hit})))
            if scope != "wide": scope = "area"
        elif k == "wide":
            scope = "wide"; why.append("shared: " + f)
        elif k == "area":
            areas |= ar
            if scope != "wide": scope = "area"
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
        sel = {n for n in tests if (test_area.get(n, set()) & areas or n in direct) and not is_slow(n)} | {n for n in SMOKE if n in tests}
        if not test_tgt.keys() & sel: sel = {n for n in SMOKE if n in tests}
    elif scope == "long":
        def has_long(n):
            for s in tgt_src.get(test_tgt.get(n), ()):
                try:
                    if "test_long.h" in open(os.path.join(src, s), errors="replace").read(): return True
                except OSError: pass
            return False
        sel = {n for n in tests if n in test_tgt and has_long(n)}
    else: sel = {n for n in SMOKE if n in tests}
    k, n_sh = (int(x) for x in a.shard.split("/"))
    times = load_times(a.times, a.plat)
    cost = lambda n: times.get(n, DEFAULT_COST)
    if times and n_sh > 1:
        # longest first onto the least loaded shard; ties by name, so every shard computes the same split
        load, mine = [0.0] * n_sh, set()
        for n in sorted(sel, key=lambda n: (-cost(n), n)):
            i = min(range(n_sh), key=lambda j: (load[j], j))
            load[i] += cost(n)
            if i == k - 1: mine.add(n)
        why.append("balanced shards, estimated seconds: " + " ".join("%.0f" % x for x in load))
        sel = mine
    else:
        sel = {n for n in sel if zlib.crc32(n.encode()) % n_sh == k - 1}
    if times:   # ctest -j starts the longest tests first when it has cost data (the format ctest itself writes)
        td = os.path.join(bd, "Testing", "Temporary")
        os.makedirs(td, exist_ok=True)
        with open(os.path.join(td, "CTestCostData.txt"), "w", newline="\n") as fo:
            for n in sorted(tests):
                fo.write("%s 1 %.2f\n" % (n, cost(n)))
            fo.write("---\n")
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
    est = sum(cost(n) for n in sel) if times else 0
    s = (f"scope={scope} areas={sorted(areas)} changed={len(files)} shard={a.shard}\n"
         f"tests={len(sel)} (timing {len(timing)}) targets={len(targets)} of {len(set(test_tgt.values()))} unmapped_tests={len(unmapped)}"
         + (f" estimated_test_seconds={est:.0f}" if times else "") + "\n" + "\n".join(why[:12]))
    open(os.path.join(a.out, "summary.txt"), "w", newline="\n").write(s + "\n")
    print(s)
    if sel and not targets:
        sys.exit("ci_select: tests selected but no build targets found (compile_commands.json format?)")

if __name__ == "__main__":
    main()
