#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

# Puts the licence files into a package folder: LICENSE (GPL-3), THIRD_PARTY.md, LICENSES/ (the standard texts) and
# LICENSE-NOTICES-BINARIES.md (licence of every bundled library, the source offer).   add_license_files.sh <package folder> [repo folder]
# For the .deb and the macOS app the same files are installed by CMake / make_dmg.sh; this script is for the folders that are zipped or tarred.
set -e
DEST=${1:?usage: add_license_files.sh <package folder> [repo folder]}
REPO=${2:-$(cd "$(dirname "$0")/../.." && pwd)}
mkdir -p "$DEST/LICENSES"
cp "$REPO/LICENSE" "$DEST/LICENSE"
cp "$REPO/THIRD_PARTY.md" "$DEST/THIRD_PARTY.md"
cp "$REPO/docs/LICENSE-NOTICES-BINARIES.md" "$DEST/LICENSE-NOTICES-BINARIES.md"
cp "$REPO"/LICENSES/*.txt "$DEST/LICENSES/"
