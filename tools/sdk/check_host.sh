#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# check_host.sh - does this machine have the host tools to make the SDKs (Ubuntu 26.04 x86-64)? If not, it says what to
# install with apt, and fails. CMake runs it when it configures this folder; OnAir.sdk's build_sdk.sh before it builds.
#   --git        also git (build_sdk.sh checks out OnAir)
#   --versions   print the versions of these Ubuntu packages instead: they are part of an SDK's key (cached_tarball.sh)
set -euo pipefail
GIT=0 VERSIONS=0
for a in "$@"; do
  case "$a" in
    --git) GIT=1;;
    --versions) VERSIONS=1;;
    *) echo "unknown argument $a" >&2; exit 2;;
  esac
done
# <command or file>:<Ubuntu package>
TOOLS="cmake:cmake ninja:ninja-build pkg-config:pkgconf curl:curl /etc/ssl/certs/ca-certificates.crt:ca-certificates
  zstd:zstd xz:xz-utils bzip2:bzip2 unzip:unzip pigz:pigz gpg:gpg gpg-agent:gpg-agent gpgv:gpgv flock:util-linux
  python3:python3 make:make perl:perl nasm:nasm patch:patch wayland-scanner:libwayland-bin
  gcc:gcc g++:g++ /usr/include/stdio.h:libc6-dev /usr/include/zlib.h:zlib1g-dev /usr/include/lzma.h:liblzma-dev"
[ "$GIT" = 0 ] || TOOLS="$TOOLS git:git"
if [ "$VERSIONS" = 1 ]; then
  pkgs=(); for t in $TOOLS; do pkgs+=("${t##*:}"); done
  dpkg-query -W -f='${Package} ${Version}\n' "${pkgs[@]}" 2>/dev/null | LC_ALL=C sort || true
  exit 0
fi
missing=()
for t in $TOOLS; do
  c=${t%:*} p=${t##*:}
  case "$c" in
    /*) [ -e "$c" ] || missing+=("$p");;
    *) command -v "$c" >/dev/null || missing+=("$p");;
  esac
done
if [ ${#missing[@]} -gt 0 ]; then
  echo "error: this machine lacks host tools to make the SDK. Install them with:" >&2
  echo "  sudo apt install ${missing[*]}" >&2
  exit 1
fi
echo "host tools: ok"
