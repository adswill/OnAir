#!/usr/bin/env bash
# The quick gate: every check that fails a release late, in a few minutes, with no build of the receiver library.
# CI runs it on every push (.github/workflows/gate.yml; the "Lint (seconds)" job of ci.yml). It works the same on a laptop:
#   tools/gate/run.sh               everything
#   tools/gate/run.sh --no-compile  skips the three small C++ test programs (seconds, no compiler)
# Exits 1 when a check fails; the failing checks are listed at the end. docs/CI-FAST.md says what each one replaces.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"
COMPILE=1
for a in "$@"; do case "$a" in --no-compile) COMPILE=0 ;; *) echo "unknown argument: $a"; exit 2 ;; esac; done

FAILED=""
step() {   # step <name> <command...>
    local name=$1; shift
    local t0=$SECONDS
    echo "::group::$name" 2>/dev/null
    if "$@"; then echo "::endgroup::"; printf 'ok   %3ds  %s\n' $((SECONDS - t0)) "$name"
    else echo "::endgroup::"; printf 'FAIL %3ds  %s\n' $((SECONDS - t0)) "$name"; FAILED="$FAILED
  $name"; fi
}

shell_syntax() { local f; for f in tools/*.sh tools/gate/*.sh tools/release/*.sh tools/package/*.sh android/tools/*.sh; do bash -n "$f" || return 1; done; }
workflow_yaml() {
    if ! python3 -c 'import yaml' 2>/dev/null; then
        [ -z "${CI:-}" ] && { echo "note: no PyYAML here, workflows not parsed"; return 0; }
        echo "PyYAML is missing"; return 1
    fi
    python3 -c "import sys,yaml;[yaml.safe_load(open(f)) for f in sys.argv[1:]]" .github/workflows/*.yml
}
python_syntax() { python3 -m py_compile tools/*.py tools/gate/*.py android/tools/*.py; }
credits() {
    python3 tools/gen_credits.py > /dev/null || return 1
    git diff --exit-code --stat -- THIRD_PARTY.md app/panels/credits_data.h docs/credits-for-website.md || { echo "credits out of date: run python3 tools/gen_credits.py and commit"; return 1; }
}
android_generated() {
    ( cd android && python3 tools/check_jni.py && python3 tools/gen_modes.py && python3 tools/gen_results_map.py && \
      git diff --exit-code --stat -- tools/mode_order.txt app/src/main/res/values/modes.xml app/src/main/java/io/github/adswill/onair/Modes.kt native/src/results_generic_map.inc ) \
      || { echo "android generated files out of date: in android/ run python3 tools/gen_modes.py and python3 tools/gen_results_map.py, then commit"; return 1; }
}

step "shell syntax" shell_syntax
step "workflow YAML" workflow_yaml
step "python syntax" python_syntax
step "FFI widths" python3 tools/check_ffi_widths.py
step "mode numbers (source)" python3 tools/gate/gate_check.py modes
step "web_policy classification (source)" python3 tools/gate/gate_check.py web_policy
step "Android native link (stand-ins)" python3 tools/gate/gate_check.py android
step "credits generator" credits
step "Android JNI table and generated files" android_generated

# The real tests that need no receiver library: built with the same sources and defines as CMakeLists.txt.
if [ $COMPILE = 1 ]; then
    CXX="${CXX:-c++}"
    OUT="${GATE_OUT:-$ROOT/build-gate}"
    mkdir -p "$OUT/generated"
    python3 - "$OUT/generated/i18n_catalogs.inc" <<'PY'
import glob, os, sys
# the same text CMake generates for the app (CMakeLists.txt, "the interface translations")
out = 'static const Catalog kCatalogs[] = {\n'
for f in sorted(glob.glob('app/i18n/*.txt')):
    with open(f, encoding='utf8', newline='') as h:
        out += '    {"%s", R"OnAirI18n(%s)OnAirI18n"},\n' % (os.path.splitext(os.path.basename(f))[0], h.read())
out += '    {"", ""}};\n'
with open(sys.argv[1], 'w', encoding='utf8', newline='') as h:
    h.write(out)
PY
    build_and_run() {   # build_and_run <test> <sources...>
        local name=$1; shift
        "$CXX" -std=c++20 -O1 -g0 -pthread -Iapp -I"$OUT/generated" -DDECT2_SOURCE_DIR="\"$ROOT\"" -o "$OUT/$name" "$@" > "$OUT/$name.log" 2>&1 \
          && "$OUT/$name" >> "$OUT/$name.log" 2>&1
        local rc=$?
        echo "$rc" > "$OUT/$name.rc"
    }
    t0=$SECONDS
    build_and_run test_gui_rules tests/app/test_gui_rules.cpp &
    build_and_run test_credits tests/app/test_credits.cpp &
    build_and_run test_i18n tests/app/test_i18n.cpp app/i18n/i18n.cpp &
    wait
    echo "the three test programs: $((SECONDS - t0)) s"
    report() { local n=$1 rc; rc=$(cat "$OUT/$n.rc" 2>/dev/null || echo 1); if [ "$rc" != 0 ]; then tail -40 "$OUT/$n.log"; return 1; fi; tail -2 "$OUT/$n.log"; }
    step "gui_rules" report test_gui_rules
    step "credits table" report test_credits
    step "i18n catalogs" report test_i18n
fi

if [ -n "$FAILED" ]; then echo; echo "The gate failed:$FAILED"; exit 1; fi
echo; echo "The gate passed."
