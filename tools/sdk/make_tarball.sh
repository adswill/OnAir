#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# make_tarball.sh - make one cross SDK, from scratch, into <out>/sdk_<platform>.tar.gz: sysroot_<platform>/ and
# environment_<platform>.sh. CMake runs it through cached_tarball.sh (CMakeLists.txt); OnAir.sdk's build_sdk.sh runs it too.
#
# Usage: make_tarball.sh <platform> -o OUTDIR [--cache DIR] [--work DIR] [--keep-work]
#   <platform>   windows, linux_amd64, linux_arm64, macos_amd64 or macos_arm64
#   -o OUTDIR    where sdk_<platform>.tar.gz goes
#   --cache DIR  the downloads and the built libraries (default: $ONAIR_SDK_CACHE, else cache/ in this folder)
#   --work DIR   the unpacked SDK while it is made (default: OUTDIR/sdk_<platform>), deleted once it is in the tarball
#   --keep-work  keep the work folder and the libraries' build folders (to see why a build failed)
set -Eeuo pipefail
export LC_ALL=C
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)
ARCH="" OUT="" CACHE=${ONAIR_SDK_CACHE:-$HERE/cache} WORK="" KEEP=0
die() { echo "error: $*" >&2; exit 1; }
while [ $# -gt 0 ]; do
  case "$1" in
    -o) OUT=$2; shift 2;;
    --cache) CACHE=$2; shift 2;;
    --work) WORK=$2; shift 2;;
    --keep-work) KEEP=1; shift;;
    -h|--help) sed -n '4,13p' "$0" | sed 's/^# \{0,1\}//'; exit 0;;
    -*) die "unknown argument $1 (see --help)";;
    *) [ -z "$ARCH" ] || die "one platform at a time"; ARCH=$1; shift;;
  esac
done
case "$ARCH" in
  windows)     target=windows      triple=x86_64-w64-mingw32;;
  linux_amd64) target=linux        triple=x86_64-linux-gnu;;
  linux_arm64) target=rpi5         triple=aarch64-linux-gnu;;
  macos_amd64) target=macos-x86_64 triple=x86_64-apple-darwin;;
  macos_arm64) target=macos-arm64  triple=arm64-apple-darwin;;
  *) die "the platform is windows, linux_amd64, linux_arm64, macos_amd64 or macos_arm64 (see --help)";;
esac
[ -n "$OUT" ] || die "-o OUTDIR is missing (see --help)"
mkdir -p "$OUT" "$CACHE"; OUT=$(cd "$OUT" && pwd)
WORK=${WORK:-$OUT/sdk_$ARCH}
tarball=$OUT/sdk_$ARCH.tar.gz
rm -rf "$WORK" "$tarball"; mkdir -p "$WORK"
keep=(); [ "$KEEP" = 0 ] || keep=(--keep-work)
"$HERE/common/make_sdk.sh" -o "$WORK/sysroot_$ARCH" --target "$target" --cache "$CACHE" "${keep[@]}"
rm -f "$WORK/sysroot_$ARCH/.made-by-make_sdk"
cat > "$WORK/environment_$ARCH.sh" <<ENV
# Source this file, in bash or zsh, to build for $ARCH in this shell:  . ./environment_$ARCH.sh
if [ -n "\${BASH_SOURCE:-}" ]; then _sdk_self=\${BASH_SOURCE[0]}; elif [ -n "\${ZSH_VERSION:-}" ]; then _sdk_self=\${(%):-%x}; else _sdk_self=\$0; fi
. "\$(cd "\$(dirname "\$_sdk_self")" && pwd)/sysroot_$ARCH/environment-setup-$triple"
unset _sdk_self
ENV
chmod 644 "$WORK/environment_$ARCH.sh"
gz=gzip; command -v pigz >/dev/null && gz=pigz
tar -C "$WORK" -I "$gz" -cf "$tarball.part" "sysroot_$ARCH" "environment_$ARCH.sh"
mv "$tarball.part" "$tarball"
if [ "$KEEP" = 0 ]; then rm -rf "$WORK"; fi
