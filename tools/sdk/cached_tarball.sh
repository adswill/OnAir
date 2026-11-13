#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# cached_tarball.sh - CMake's step for one SDK (CMakeLists.txt): <out>/sdk_<platform>.tar.gz, made again only when an input
# changed. The key is a sha256 of every input: each file of this folder that goes into an SDK (common/, sources/, windows/,
# linux/, macos/ and the scripts here: name, executable bit and contents), the platform, the options passed after --, and
# the versions of the Ubuntu host packages that build parts of it (check_host.sh --versions).
#   - the key of <out>/sdk_<platform>.tar.gz (in <out>/sdk_<platform>.key): nothing to do
#   - an SDK with this key in <cache>/sdk/<platform>/: linked (or copied) to <out>, in seconds
#   - else make_tarball.sh makes it from scratch and it goes into the cache; the two newest per platform are kept
# One SDK is made at a time per cache folder (flock): they share the downloads.
#
# Usage: cached_tarball.sh <platform> -o OUTDIR --cache DIR [--keep-work] [-- make_tarball.sh options]
set -Eeuo pipefail
export LC_ALL=C
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)
ARCH="" OUT="" CACHE="" KEEP=() OPTS=()
die() { echo "error: $*" >&2; exit 1; }
while [ $# -gt 0 ]; do
  case "$1" in
    -o) OUT=$2; shift 2;;
    --cache) CACHE=$2; shift 2;;
    --keep-work) KEEP=(--keep-work); shift;;
    --) shift; OPTS=("$@"); break;;
    -h|--help) sed -n '4,15p' "$0" | sed 's/^# \{0,1\}//'; exit 0;;
    -*) die "unknown argument $1 (see --help)";;
    *) ARCH=$1; shift;;
  esac
done
[ -n "$ARCH" ] && [ -n "$OUT" ] && [ -n "$CACHE" ] || die "usage: cached_tarball.sh <platform> -o OUTDIR --cache DIR (see --help)"
mkdir -p "$OUT" "$CACHE/sdk/$ARCH"; OUT=$(cd "$OUT" && pwd); CACHE=$(cd "$CACHE" && pwd)
tarball=$OUT/sdk_$ARCH.tar.gz keyfile=$OUT/sdk_$ARCH.key

key=$(
  echo "platform $ARCH"
  echo "options ${OPTS[*]}"
  "$HERE/check_host.sh" --versions
  cd "$HERE"
  find common sources windows linux macos -type f ! -path '*/__pycache__/*' -print0 | sort -z |
    while IFS= read -r -d '' f; do printf '%s %s %s\n' "$([ -x "$f" ] && echo x || echo -)" "$(sha256sum < "$f" | cut -c1-64)" "$f"; done
  for f in CMakeLists.txt check_host.sh make_tarball.sh cached_tarball.sh; do printf '%s %s\n' "$(sha256sum < "$f" | cut -c1-64)" "$f"; done
)
key=$(printf '%s\n' "$key" | sha256sum | cut -c1-32)
store=$CACHE/sdk/$ARCH/$key.tar.gz

if [ -f "$tarball" ] && [ "$(cat "$keyfile" 2>/dev/null)" = "$key" ]; then
  echo "sdk_$ARCH: up to date (key $key)"; exit 0
fi
exec 9> "$CACHE/.lock"
flock -n 9 || { echo "sdk_$ARCH: waiting for the SDK being made with $CACHE"; flock 9; }
rm -f "$keyfile"
if [ -f "$store" ]; then
  rm -f "$tarball"; ln "$store" "$tarball" 2>/dev/null || cp "$store" "$tarball"
  touch "$store" "$tarball"   # (newest first in the cache; and newer than the inputs for the build tool)
  echo "$key" > "$keyfile"
  echo "sdk_$ARCH: from the cache (key $key): $tarball"; exit 0
fi
printf '\n==== %s: SDK %s (key %s): made from scratch\n' "$(date +%H:%M:%S)" "$ARCH" "$key"
start=$SECONDS
"$HERE/make_tarball.sh" "$ARCH" -o "$OUT" --cache "$CACHE" --work "$OUT/work/sdk_$ARCH" "${KEEP[@]}" "${OPTS[@]}"
ln "$tarball" "$store.part" 2>/dev/null || cp "$tarball" "$store.part"
mv -f "$store.part" "$store"
echo "$key" > "$keyfile"
ls -t "$CACHE/sdk/$ARCH"/*.tar.gz | tail -n +3 | xargs -r rm -f   # the two newest stay
echo "---- $ARCH: sdk_$ARCH.tar.gz, $(du -h "$tarball" | cut -f1), $((SECONDS - start)) s"
