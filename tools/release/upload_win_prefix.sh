#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

# Stores the Windows dependency prefix for release.yml: run this on the maintainer's Mac once, and again when tools/package/cross_deps_windows.sh or
# packaging/windows/mingw-toolchain.cmake changes (after running cross_deps_windows.sh). The file goes on the draft release "ci-windows-prefix" of
# adswill/OnAir: a draft is visible only to people with write access, and it never becomes a public release.
#   tools/release/upload_win_prefix.sh [prefix folder, default ~/onair-win-prefix]
set -eu
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
REPO=adswill/OnAir
TAG=ci-windows-prefix
P=${1:-$HOME/onair-win-prefix}
[ -f "$P/bin/libSoapySDR.dll" ] && [ -f "$P/lib/libssl.a" ] || { echo "error: $P does not look like a finished prefix (tools/package/cross_deps_windows.sh)"; exit 1; }
ID=$("$ROOT/tools/package/win_prefix_id.sh")
OUT=$(mktemp -d)/onair-win-prefix-$ID.tar.gz
echo "packing $P as $(basename "$OUT")"
tar -C "$(dirname "$P")" -czf "$OUT" -s "#^$(basename "$P")#onair-win-prefix#" "$(basename "$P")" 2>/dev/null || tar -C "$(dirname "$P")" -czf "$OUT" --transform "s#^$(basename "$P")#onair-win-prefix#" "$(basename "$P")"
ls -la "$OUT"
if ! gh release view "$TAG" --repo "$REPO" > /dev/null 2>&1; then
  gh release create "$TAG" --repo "$REPO" --draft --title "CI: Windows dependency prefix (draft, never published)" \
    --notes "Used by release.yml to build the Windows packages. Replaced by tools/release/upload_win_prefix.sh. Do not publish this draft."
fi
gh release upload "$TAG" "$OUT" --repo "$REPO" --clobber
gh release view "$TAG" --repo "$REPO" --json assets --jq '.assets[]|"\(.name)\t\(.size)"'
echo "done. Older onair-win-prefix-*.tar.gz files on $TAG are not removed: delete them by hand when no release branch uses them."
