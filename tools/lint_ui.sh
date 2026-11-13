#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

# Counts raw ImGui calls that the UI kit (app/ui/kit, docs/UI-KIT.md) replaces, per file. Convert file by file; the count should only go down.
cd "$(dirname "$0")/.." || exit 1
grep -rcE 'ImGui::(Button|SmallButton|Checkbox|SameLine|BeginTabBar|BeginTabItem|CollapsingHeader)\(' app --include='*.cpp' --include='*.h' --include='*.inc' \
  | grep -v '^app/ui/kit/' | grep -v ':0$' | awk -F: '{print $2"\t"$1; t+=$2} END {print t"\tTOTAL"}' | sort -rn | head -${1:-40}
