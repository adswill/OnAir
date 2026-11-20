#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

# Authenticode-signs the given .exe/.dll files with osslsigncode, when a certificate is supplied. Does nothing (exit 0) otherwise.
#   WIN_SIGN_PFX_BASE64   the code-signing certificate (.pfx) as base64
#   WIN_SIGN_PFX_PASSWORD its password
# Used by .github/workflows/release.yml; see docs/SIGNING.md.
set -eu
if [ -z "${WIN_SIGN_PFX_BASE64:-}" ]; then echo "no Windows signing certificate: not signing"; exit 0; fi
command -v osslsigncode >/dev/null || { echo "osslsigncode is not installed"; exit 1; }
PFX=$(mktemp); trap 'rm -f "$PFX"' EXIT
printf '%s' "$WIN_SIGN_PFX_BASE64" | base64 -d > "$PFX"
for f in "$@"; do
  osslsigncode sign -pkcs12 "$PFX" -pass "${WIN_SIGN_PFX_PASSWORD:-}" -n "OnAir" -i "https://on-air.dev" \
    -ts http://timestamp.digicert.com -h sha256 -in "$f" -out "$f.signed"
  mv "$f.signed" "$f"
  echo "signed $f"
done
