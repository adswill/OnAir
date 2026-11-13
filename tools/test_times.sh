#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

# Lists the tests that took 20 s or more in the last ctest run of a build directory, one name per line, for
# tests/long_tests.txt: tools/test_times.sh build > tests/long_tests.txt (run the full suite first: ctest --test-dir build).
set -e
log="${1:-build}/Testing/Temporary/LastTest.log"
[ -f "$log" ] || { echo "no ctest log at $log" >&2; exit 1; }
printf '# Tests that take 20 s or more on a laptop core: the quick CI (ctest -LE slow) skips them, the full CI runs\n'
printf '# them nightly and on release tags. Regenerate after a full run: tools/test_times.sh build > tests/long_tests.txt\n'
awk '/^[0-9]+\/[0-9]+ Testing: /{name=$3} /^Test time = /{t=$4; if (t+0 >= 20) print name}' "$log" | sort -u
