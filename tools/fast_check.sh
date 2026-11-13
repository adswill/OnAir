#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

# Build and run only the tests a change can affect (tools/ci_select.py), at low priority so the Mac stays usable.
#   tools/fast_check.sh                 changed files against origin/main (or $BASE)
#   tools/fast_check.sh --scope wide    every non-slow test; --scope smoke | area | full
#   tools/fast_check.sh --base main~3   measure the change against another ref
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
B="${BUILD_DIR:-$ROOT/build}"
BASE="${BASE:-origin/main}"; SCOPE=auto
while [ $# -gt 0 ]; do case "$1" in --scope) SCOPE=$2; shift 2;; --base) BASE=$2; shift 2;; *) echo "unknown: $1"; exit 2;; esac; done
git -C "$ROOT" rev-parse --verify -q "$BASE^{commit}" > /dev/null || BASE=HEAD
# the committed diff plus the uncommitted work
CH="$(mktemp)"; { git -C "$ROOT" diff --name-only "$BASE"...HEAD; git -C "$ROOT" diff --name-only HEAD; git -C "$ROOT" ls-files --others --exclude-standard; } | sort -u > "$CH"
nice cmake -S "$ROOT" -B "$B" -DCMAKE_BUILD_TYPE=Release -DDECT2_BUILD_APP=OFF -DCMAKE_EXPORT_COMPILE_COMMANDS=ON > /dev/null
python3 "$ROOT/tools/ci_select.py" --build-dir "$B" --src "$ROOT" --changed "$CH" --scope "$SCOPE" --out "$B/sel"
nice cmake --build "$B" -j 2 --target $(cat "$B/sel/targets.txt") > "$B/fast_check_build.log" 2>&1 || { grep -nE 'error|undefined' "$B/fast_check_build.log" | head -30; exit 1; }
nice ctest --test-dir "$B" -j 2 -R "$(cat "$B/sel/fast.regex")" --output-on-failure --timeout 1500 | tail -25
nice ctest --test-dir "$B" -j 1 -R "$(cat "$B/sel/timing.regex")" --repeat until-pass:2 --output-on-failure --timeout 1500 | tail -8
