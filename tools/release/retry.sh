#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

# Runs a command again when it fails, for the network steps of the release (brew, apt, rustup, downloads): retry.sh [-n tries] <command...>
# A failure that is not the network costs only the extra tries; the last exit status is passed on.
n=4
if [ "${1:-}" = "-n" ]; then n=$2; shift 2; fi
i=1
while :; do
    "$@" && exit 0
    rc=$?
    [ "$i" -ge "$n" ] && { echo "retry.sh: giving up after $i tries: $*" >&2; exit "$rc"; }
    echo "retry.sh: try $i of $n failed (exit $rc), waiting $((i * 20)) s: $*" >&2
    sleep $((i * 20))
    i=$((i + 1))
done
