#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# build_pkgtools.sh - add the tools that make the packages to the SDK's native sysroot, from common/pkgtools.lock,
# only for the platforms the SDK builds for:
#
#   windows   makensis                    the Windows installer (Ubuntu's nsis, nsis-common: unpacked, not installed)
#   macos     mkfs.hfsplus, fsck.hfsplus  the macOS disk image's volume (Ubuntu's hfsprogs: unpacked, not installed)
#             hfsplus, dmg                fill that volume, make the compressed disk image (Mozilla's libdmg-hfsplus, built
#                                         from source, with Ubuntu's libbz2 linked in statically)
#             rcodesign                   ad-hoc or Developer ID signing of a macOS app (apple-codesign, built from source
#                                         with the SDK's cargo)
#   linux     nothing: a .deb is made with the build machine's dpkg-deb
#
# The result goes into <native sysroot>/pkgtools with links in bin/ and the licences in pkgtools/share/licenses/<name>.
# Each platform's tools are kept in cache/built/pkgtools-<platform>-<key>.tar.zst; the key is the sha256 of this script
# and that platform's lines of the lock file. A separate lock file, so that a new version of these tools does not
# rebuild OnAir's libraries (their key includes toolchain.lock).
#
# Usage:  build_pkgtools.sh -o <native sysroot> --platform windows,macos,linux [--cache DIR]
# Needs:  for macos: gcc (cc), cmake, make, zlib1g-dev, liblzma-dev; the downloads: apt-get, curl, and crates.io for rcodesign.
set -Eeuo pipefail
export LC_ALL=C
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"; ROOT="$(dirname "$SCRIPT_DIR")"
OUT="" CACHE="$ROOT/cache" PLATFORMS=""
while [ $# -gt 0 ]; do
  case "$1" in
    -o) OUT="$2"; shift 2;;
    --platform) PLATFORMS="${2//,/ }"; shift 2;;
    --cache) CACHE="$2"; shift 2;;
    -h|--help) sed -n '2,/^set -E/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 0;;
    *) echo "unknown argument $1 (see --help)" >&2; exit 2;;
  esac
done
die() { echo "error: $*" >&2; exit 1; }
say() { echo "==> $*"; }
[ -n "$OUT" ] && [ -x "$OUT/bin/cargo" ] || die "-o <native sysroot>: the folder fetch_toolchain.sh made (with bin/cargo)"
[ -n "$PLATFORMS" ] || die "--platform windows, macos or linux (comma separated)"
for p in $PLATFORMS; do case "$p" in windows|macos|linux) ;; *) die "--platform: windows, macos or linux, not $p";; esac; done
OUT="$(cd "$OUT" && pwd)"
LOCK="$SCRIPT_DIR/pkgtools.lock"

# which lock entries (by name) and tools belong to which platform
declare -A NAMES=([windows]="nsis nsis-common" [macos]="hfsprogs libbz2-dev libbz2-1.0 libdmg-hfsplus apple-codesign")
declare -A TOOLS=([windows]="makensis" [macos]="mkfs.hfsplus fsck.hfsplus hfsplus dmg rcodesign")
lock_lines() { local n; for n in ${NAMES[$1]}; do grep -E "^[a-z]+ $n " "$LOCK" || die "$n is not in $LOCK"; done; }

links() {   # bin/: relative links, and makensis through a script that says where its stubs are, so the folder can move
  local t
  for t in ${TOOLS[$1]}; do
    if [ "$t" = makensis ]; then
      cat > "$OUT/bin/makensis" <<'SH'
#!/bin/sh
d=$(dirname "$(readlink -f "$0")")/../pkgtools
NSISDIR="$d/share/nsis" exec "$d/bin/makensis" "$@"
SH
      chmod 755 "$OUT/bin/makensis"
    else
      ln -sf "../pkgtools/bin/$t" "$OUT/bin/$t"
    fi
  done
}

# get <work folder>: the platform's lock entries downloaded, checked and unpacked: debs into <work>/debs, sources into <work>/<name>
get() {
  local W=$1 kind name ver file rest url sha f
  mkdir -p "$W/debs" "$CACHE/deb" "$CACHE/src"
  while read -r kind name ver file rest; do
    case "$kind" in
      deb)
        f="$CACHE/deb/$file"
        if [ ! -f "$f" ]; then
          say "downloading $name $ver"
          (cd "$CACHE/deb" && apt-get download -q "$name=$ver" >/dev/null) || die "apt-get download $name=$ver failed (no longer in the archive?)"
        fi
        [ "$(sha256sum "$f" | cut -d' ' -f1)" = "$rest" ] || die "$f: sha256 is not the one in $LOCK"
        dpkg-deb -x "$f" "$W/debs";;
      src)   # name, commit, archive name, "url sha256"
        read -r url sha <<<"$rest"
        f="$CACHE/src/$file"
        [ -f "$f" ] || { say "downloading $name $ver"; curl -fsSL --retry 6 --retry-delay 5 --retry-all-errors --connect-timeout 30 -o "$f.part" "$url" && mv "$f.part" "$f"; }
        [ "$(sha256sum "$f" | cut -d' ' -f1)" = "$sha" ] || die "$f: sha256 is not the one in $LOCK"
        mkdir -p "$W/$name"; tar -xf "$f" -C "$W/$name" --strip-components=1;;
      crate) RC_VERSION=$ver;;
    esac
  done < <(lock_lines "$PLATFORM")
}
# licence <name> <file...>
licence() { local n=$1; shift; mkdir -p "$P/share/licenses/$n"; cp "$@" "$P/share/licenses/$n/"; }

build_windows() {
  local U="$WORK/debs"
  cp "$U/usr/bin/makensis" "$P/bin/"; mkdir -p "$P/share"; cp -R "$U/usr/share/nsis" "$P/share/"
  licence nsis "$U/usr/share/doc/nsis/copyright"
}

build_macos() {
  local U="$WORK/debs" BZ crate
  cp "$U/usr/sbin/mkfs.hfsplus" "$U/usr/sbin/fsck.hfsplus" "$P/bin/"
  licence hfsprogs "$U/usr/share/doc/hfsprogs/copyright"
  say "libdmg-hfsplus"
  BZ=$(find "$U" -name libbz2.a -print -quit); [ -n "$BZ" ] || die "no libbz2.a in libbz2-dev"
  # OpenSSL only decrypts images, which these packages do not use; the sources say -lbz2, so the folder goes to the linker as well
  run cmake -S "$WORK/libdmg-hfsplus" -B "$WORK/dmg-build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_DISABLE_FIND_PACKAGE_OpenSSL=ON \
    "-DBZIP2_INCLUDE_DIR=$U/usr/include" "-DBZIP2_LIBRARY_RELEASE=$BZ" "-DCMAKE_EXE_LINKER_FLAGS=-L$(dirname "$BZ")"
  run cmake --build "$WORK/dmg-build" -j"$(nproc)" --target dmg-bin hfsplus
  cp "$WORK/dmg-build/dmg/dmg" "$WORK/dmg-build/hfs/hfsplus" "$P/bin/"
  licence libdmg-hfsplus "$WORK/libdmg-hfsplus/LICENSE"
  licence bzip2 "$U/usr/share/doc/libbz2-1.0/copyright"   # (libbz2.a is linked into dmg and hfsplus)
  say "rcodesign (apple-codesign $RC_VERSION)"
  # --locked: the versions of the crate's own Cargo.lock; cargo checks each crate against the registry's checksum
  run env CARGO_HOME="$CACHE/cargo" CARGO_TARGET_DIR="$WORK/rc-build" "$OUT/bin/cargo" install apple-codesign --version "$RC_VERSION" --locked --no-default-features --root "$WORK/rc" --quiet
  cp "$WORK/rc/bin/rcodesign" "$P/bin/"
  crate=$(find "$CACHE/cargo/registry/src" -maxdepth 2 -type d -name "apple-codesign-$RC_VERSION" -print -quit)
  [ -n "$crate" ] && ls "$crate"/LICENSE* >/dev/null 2>&1 || die "no licence file in the apple-codesign $RC_VERSION crate"
  licence apple-codesign "$crate"/LICENSE*
}

rm -rf "$OUT/pkgtools"; mkdir -p "$OUT/pkgtools"
for PLATFORM in $PLATFORMS; do
  [ "$PLATFORM" = linux ] && continue
  KEY=$( { cat "$0"; lock_lines "$PLATFORM"; } | sha256sum | cut -c1-16)
  BUILT="$CACHE/built/pkgtools-$PLATFORM-$KEY.tar.zst"
  if [ -f "$BUILT" ]; then
    say "packaging tools for $PLATFORM, from $BUILT"
  else
    mkdir -p "$CACHE/work"
    WORK=$(mktemp -d "$CACHE/work/pkgtools.XXXXXX"); trap 'rm -rf "$WORK"' EXIT
    # run <command...>: its output, with the compilers' warnings, goes to a log; when it fails, the end of the log is shown and everything stops
    run() { "$@" >> "$WORK/log" 2>&1 || { echo "---- the end of the log:" >&2; tail -40 "$WORK/log" >&2; die "failed: $*"; }; }
    P="$WORK/pkgtools"; mkdir -p "$P/bin" "$P/share/licenses"
    get "$WORK"
    "build_$PLATFORM"
    for t in ${TOOLS[$PLATFORM]}; do [ -x "$P/bin/$t" ] || die "$t was not made"; done
    mkdir -p "$CACHE/built"; rm -f "$CACHE"/built/pkgtools-"$PLATFORM"-*.tar.zst "$CACHE"/built/pkgtools-[0-9a-f]*.tar.zst
    tar -C "$WORK" -I 'zstd -T0 -10' -cf "$BUILT.part" pkgtools && mv "$BUILT.part" "$BUILT"
    rm -rf "$WORK"; trap - EXIT
    say "packaging tools for $PLATFORM made, kept in $BUILT"
  fi
  tar -I zstd -xf "$BUILT" -C "$OUT"   # (the platforms' pkgtools/ folders do not overlap)
  links "$PLATFORM"
done
[ -n "$(ls -A "$OUT/pkgtools")" ] || rmdir "$OUT/pkgtools"
