#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

# Runs sens_sweep for some modes and compares with the committed baseline. docs/SENSITIVITY.md.
#   tools/sens/run.sh [--quick|--full] [--mode REGEX] [--scenario REGEX] [--trials N] [--out DIR] [--build DIR] [--no-compare] [--update-baseline]
# Examples:
#   tools/sens/run.sh --quick --mode 'dab|fm'              # a few minutes: AWGN, TU6, 20 ppm carrier and the combined case, 4 trials a point
#   tools/sens/run.sh --full --mode dvbt                   # every scenario, 1 dB steps, 12 trials a point
# Output: DIR/sens-<mode>.json for each mode, DIR/sens-baseline.json (all of them), and the delta table against tests/sens/baseline/.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"; root="$(cd "$here/../.." && pwd)"
build="$root/build"; out="$root/build/sens-out"; tier="--quick"; args=(); compare=1; update=0
while [ $# -gt 0 ]; do
  case "$1" in
    --quick|--full) tier="$1" ;;
    --mode|--scenario|--trials|--seed|--threads|--snr-hi|--snr-lo) args+=("$1" "$2"); shift ;;
    --heavy) args+=("$1") ;;
    --out) out="$2"; shift ;;
    --build) build="$2"; shift ;;
    --no-compare) compare=0 ;;
    --update-baseline) update=1 ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
  shift
done
exe="$build/sens_sweep"
[ -x "$exe" ] || { echo "build it first: cmake --build $build --target sens_sweep" >&2; exit 1; }
mkdir -p "$out"
"$exe" "$tier" ${args[@]+"${args[@]}"} --out "$out"
base="$root/tests/sens/baseline/sens-baseline.json"
if [ "$update" = 1 ]; then
  python3 "$here/compare.py" --merge "$base" "$out/sens-baseline.json"
elif [ "$compare" = 1 ] && [ -f "$base" ]; then
  python3 "$here/compare.py" "$base" "$out/sens-baseline.json"
fi
