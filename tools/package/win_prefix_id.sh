#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

# The identity of a Windows dependency prefix: a short hash of what decides its contents (the build script and the toolchain file).
# release.yml downloads onair-win-prefix-<id>.tar.gz from the draft release ci-windows-prefix, so a changed script needs a new prefix.
cd "$(dirname "$0")/../.." || exit 1
python3 -c "
import hashlib, sys
h = hashlib.sha256()
for f in ('tools/package/cross_deps_windows.sh', 'packaging/windows/mingw-toolchain.cmake'):
    h.update(open(f, 'rb').read())
print(h.hexdigest()[:16])
"
