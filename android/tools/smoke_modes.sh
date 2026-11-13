#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

# The all-modes smoke test: on an emulator or phone with a debug APK installed and the test signal as the source, runs every mode of the list for a few
# seconds and writes one line per mode: did the process stay up, did the decoder report, lock, CPU use. Debug builds only (the run=1 link).
# Usage: smoke_modes.sh <output file> [seconds per mode] [first index] [last index]
set -u
OUT="$1"; SECS="${2:-6}"; FROM="${3:-0}"; TO="${4:-$(($(grep -c '|' "$(dirname "$0")/mode_order.txt") - 1))}"
ADB="${ADB:-adb}"; PKG=io.github.adswill.onair.debug
"$ADB" logcat -c
: > "$OUT"
for ((i = FROM; i <= TO; i++)); do
  "$ADB" logcat -c
  "$ADB" shell am start -W -a android.intent.action.VIEW -d "'onair://tune?mode=%23$i&run=1'" "$PKG" > /dev/null 2>&1
  sleep "$SECS"
  pid=$("$ADB" shell pidof "$PKG" | tr -d '\r')
  cpu=""
  [ -n "$pid" ] && cpu=$("$ADB" shell top -b -n 1 -p "$pid" | awk -v p="$pid" '$1 == p {print $9}' | tr -d '\r')
  log=$("$ADB" logcat -d -s OnAirSmoke:I | tail -n 1 | sed 's/.*OnAirSmoke: //')
  crash=$("$ADB" logcat -d | grep -cE 'FATAL EXCEPTION|Fatal signal|did not then call')
  if [ -z "$pid" ]; then
    "$ADB" logcat -d | grep -E 'Fatal signal|FATAL EXCEPTION|signal [0-9]+ \(SIG|Cause:|#00 pc' | head -n 6 | sed 's/^/    /' >> "$OUT"
  fi
  printf '%s\tpid=%s\tcpu=%s\tcrash=%s\t%s\n' "$i" "${pid:-DEAD}" "${cpu:-?}" "$crash" "$log" >> "$OUT"
  if [ -z "$pid" ]; then
    "$ADB" shell am start -W -n "$PKG/io.github.adswill.onair.MainActivity" > /dev/null 2>&1
    sleep 3
  fi
done
echo done >> "$OUT"
