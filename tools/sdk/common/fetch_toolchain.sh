#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# fetch_toolchain.sh - make the native (Linux x86-64) part of the SDK: the cross tools that run on the build machine.
#
#   llvm/      clang, lld and LLVM's tools, from the official LLVM release (GitHub), GPG-checked against llvm.org's
#              release keys. Only the tools a cross build uses are kept (TOOLS below), not the whole 5 GB; for macOS
#              also ld64.lld and the Mach-O tools (lipo, install-name-tool, otool, libtool, dsymutil).
#   deb/       Ubuntu packages (DEBS below), fetched with apt-get download (checked by apt) and unpacked, not installed:
#              the GNU binutils for mingw-w64 (x86_64-w64-mingw32-ld, -dlltool, -objdump ...) for Windows; left out
#              with --without-mingw (an SDK without Windows needs no GNU tools)
#   rust/      rustc and cargo with the standard libraries of the targets (Linux x86-64 and aarch64, x86_64-pc-windows-gnu,
#              aarch64-apple-darwin, x86_64-apple-darwin), from static.rust-lang.org, GPG-checked against the Rust release
#              key pinned below
#   bin/       relative symlinks to all of these: the one folder to put on PATH
#   share/licenses/  the licences of these tools: llvm (kept in common/licenses: the release has none), rust, binutils-mingw-w64
#
# Nothing is built or installed in the system; the folder can be moved. Reproducible: the files and their sha256 go into
# a lock file (toolchain.lock); later runs check against it. Change a version below and run with --update.
#
# Usage:  ./fetch_toolchain.sh [-o OUTDIR] [--lock FILE] [--cache DIR] [--update] [--without-mingw]
#   -o OUTDIR    default: sdk/sysroots/x86_64-sdk-linux in the repository (replaced if it holds TOOLCHAIN-MANIFEST.txt)
#   --lock FILE  default: common/toolchain.lock (next to this script)
#   --cache DIR  default: cache/ in the repository
#   --update     use the versions below (and the Ubuntu packages apt offers now) and rewrite the lock file
#   --without-mingw  do not install the GNU binutils for mingw-w64 (still checked against the lock file)
# Needs: Ubuntu (apt-get download, dpkg-deb), curl, tar, zstd, xz, gpg, gpgv, sha256sum.
set -Eeuo pipefail
export LC_ALL=C

# ---- versions ---------------------------------------------------------------------------------------------------------
LLVM_VERSION=23.1.5
RUST_VERSION=1.99.1
DEBS=(binutils-mingw-w64-x86-64 binutils-mingw-w64-base)   # Ubuntu packages; their programs go into bin/ (-base: the copyright file)
# the LLVM tools kept (with what their symlinks point to); clang-scan-deps: CMake's C++20 module scanning with Ninja
TOOLS=(clang clang++ clang-cpp "clang-${LLVM_VERSION%%.*}" clang-scan-deps lld ld.lld
       llvm-ar llvm-ranlib llvm-dlltool llvm-nm llvm-objdump llvm-objcopy llvm-strip llvm-readobj
       llvm-rc llvm-windres llvm-cxxfilt llvm-symbolizer llvm-addr2line llvm-size llvm-strings
       ld64.lld llvm-lipo llvm-install-name-tool llvm-otool llvm-libtool-darwin dsymutil llvm-readtapi)   # (macOS)
RUST_KEY=108F66205EAEB0AAA8DD5E1C85AB96E6FA1BE5FE      # Rust Language (Tag and Release Signing Key)
LLVM_KEYS_URL=https://releases.llvm.org/release-keys.asc
RUST_KEY_URL=https://static.rust-lang.org/rust-key.gpg.ascii
# ----------------------------------------------------------------------------------------------------------------------
LLVM_FILE=LLVM-$LLVM_VERSION-Linux-X64.tar.zst
LLVM_URL=https://github.com/llvm/llvm-project/releases/download/llvmorg-$LLVM_VERSION
RUST_URL=https://static.rust-lang.org/dist
RUST_FILES=(rustc-$RUST_VERSION-x86_64-unknown-linux-gnu.tar.xz cargo-$RUST_VERSION-x86_64-unknown-linux-gnu.tar.xz
            rust-std-$RUST_VERSION-x86_64-unknown-linux-gnu.tar.xz rust-std-$RUST_VERSION-x86_64-pc-windows-gnu.tar.xz
            rust-std-$RUST_VERSION-aarch64-apple-darwin.tar.xz rust-std-$RUST_VERSION-x86_64-apple-darwin.tar.xz
            rust-std-$RUST_VERSION-aarch64-unknown-linux-gnu.tar.xz)

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"; ROOT="$(dirname "$SCRIPT_DIR")"
OUT="$ROOT/sdk/sysroots/x86_64-sdk-linux" LOCK="$SCRIPT_DIR/toolchain.lock" CACHE="$ROOT/cache" UPDATE=0 MINGW=1
while [ $# -gt 0 ]; do
  case "$1" in
    -o) OUT="$2"; shift 2;;
    --lock) LOCK="$2"; shift 2;;
    --cache) CACHE="$2"; shift 2;;
    --update) UPDATE=1; shift;;
    --without-mingw) MINGW=0; shift;;
    -h|--help) sed -n '2,/^set -E/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 0;;
    *) echo "unknown argument $1 (see --help)" >&2; exit 2;;
  esac
done
die() { echo "error: $*" >&2; exit 1; }
say() { echo "==> $*"; }
for t in curl tar zstd xz gpg gpgv sha256sum apt-get dpkg-deb; do command -v $t >/dev/null || die "$t not found"; done

TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
export GNUPGHOME="$TMP/gnupg"; mkdir -m 700 "$GNUPGHOME"
mkdir -p "$CACHE/llvm" "$CACHE/rust" "$CACHE/deb"
sha() { sha256sum "$1" | cut -d' ' -f1; }
download() { curl -fsSL --retry 6 --retry-delay 5 --retry-all-errors --connect-timeout 30 -o "$2.part" "$1" || { rm -f "$2.part"; die "download failed: $1"; }; mv "$2.part" "$2"; }

# --- the lock file ------------------------------------------------------------------------------------------------------
declare -A LOCKED
if [ "$UPDATE" = 0 ] && [ -f "$LOCK" ]; then
  while read -r kind _ ver file sum; do LOCKED[$kind:$file]="$ver $sum"; done < <(grep -v '^#' "$LOCK")
  [ -n "${LOCKED[llvm:$LLVM_FILE]:-}" ] || die "the versions above differ from the lock file $LOCK: run with --update"
  for f in "${RUST_FILES[@]}"; do
    [ -n "${LOCKED[rust:$f]:-}" ] || die "$f is not in the lock file $LOCK: run with --update"
  done
  say "lock file $LOCK"
fi
locked_sum() { local v=${LOCKED[$1]:-}; echo "${v#* }"; }
check_locked() {   # kind:file path: when there is a lock file, the file must have its sha256
  local want; want=$(locked_sum "$1")
  [ -z "$want" ] || [ "$(sha "$2")" = "$want" ] || die "$(basename "$2"): sha256 differs from the lock file"
}

# --- LLVM ---------------------------------------------------------------------------------------------------------------
say "LLVM $LLVM_VERSION"
L="$CACHE/llvm/$LLVM_FILE"
want=$(locked_sum "llvm:$LLVM_FILE")
if [ ! -f "$L" ] || { [ -n "$want" ] && [ "$(sha "$L")" != "$want" ]; }; then
  say "downloading $LLVM_FILE (about 1.2 GB)"; download "$LLVM_URL/$LLVM_FILE" "$L"
fi
[ -f "$L.sig" ] || download "$LLVM_URL/$LLVM_FILE.sig" "$L.sig"
download "$LLVM_KEYS_URL" "$TMP/llvm-keys.asc"
gpg -q --batch --dearmor -o "$TMP/llvm-keys.gpg" < "$TMP/llvm-keys.asc"
gpgv --keyring "$TMP/llvm-keys.gpg" "$L.sig" "$L" 2> "$TMP/gpgv.log" || { cat "$TMP/gpgv.log" >&2; die "$LLVM_FILE: bad signature"; }
LLVM_SIGNER=$(sed -n 's/.*using [A-Z]* key \([0-9A-F]*\).*/\1/p' "$TMP/gpgv.log" | head -1)
check_locked "llvm:$LLVM_FILE" "$L"

# --- Ubuntu packages -----------------------------------------------------------------------------------------------------
declare -A DEB_VER DEB_PATH
for pkg in "${DEBS[@]}"; do
  v="" lf=""
  for k in "${!LOCKED[@]}"; do case "$k" in "deb:${pkg}_"*) lf=${k#deb:}; v=${LOCKED[$k]%% *};; esac; done
  if [ -z "$v" ]; then
    [ "$UPDATE" = 1 ] || [ ! -f "$LOCK" ] || die "$pkg is not in the lock file $LOCK: run with --update"
    v=$(apt-cache policy "$pkg" | sed -n 's/^ *Candidate: //p')
    [ -n "$v" ] && [ "$v" != "(none)" ] || die "apt has no $pkg (Ubuntu universe; apt update?)"
  fi
  say "$pkg $v"
  f=$(ls "$CACHE"/deb/"${pkg}_"*.deb 2>/dev/null | while read -r x; do [ "$(dpkg-deb -f "$x" Version)" = "$v" ] && echo "$x"; done | head -1 || true)
  if [ -z "$f" ]; then
    (cd "$CACHE/deb" && apt-get download -q "$pkg=$v" >/dev/null) || die "apt-get download $pkg=$v failed (no longer in the archive? use the cache, or --update)"
    f=$(ls -t "$CACHE"/deb/"${pkg}_"*.deb | head -1)
  fi
  [ -z "$lf" ] || [ "$(basename "$f")" = "$lf" ] || die "$pkg: the file name differs from the lock file"
  check_locked "deb:$(basename "$f")" "$f"
  DEB_VER[$pkg]=$v DEB_PATH[$pkg]=$f
done

# --- Rust ---------------------------------------------------------------------------------------------------------------
say "Rust $RUST_VERSION"
download "$RUST_KEY_URL" "$TMP/rust-key.asc"
gpg -q --batch --import "$TMP/rust-key.asc"
gpg -q --batch --export "$RUST_KEY" > "$TMP/rust-key.gpg"
[ -s "$TMP/rust-key.gpg" ] || die "the Rust key $RUST_KEY is not in rust-key.gpg.ascii"
for f in "${RUST_FILES[@]}"; do
  R="$CACHE/rust/$f"; want=$(locked_sum "rust:$f")
  if [ ! -f "$R" ] || { [ -n "$want" ] && [ "$(sha "$R")" != "$want" ]; }; then download "$RUST_URL/$f" "$R"; fi
  [ -f "$R.asc" ] || download "$RUST_URL/$f.asc" "$R.asc"
  gpgv --keyring "$TMP/rust-key.gpg" "$R.asc" "$R" >/dev/null 2>&1 || die "$f: bad signature"
  check_locked "rust:$f" "$R"
done

# --- lock file ----------------------------------------------------------------------------------------------------------
if [ "$UPDATE" = 1 ] || [ ! -f "$LOCK" ]; then
  {
    echo "# fetch_toolchain.sh lock file: the exact files of the SDK's native tools. Rewrite with: fetch_toolchain.sh --update"
    echo "# resolved $(date -u +%Y-%m-%dT%H:%M:%SZ); LLVM signed by key $LLVM_SIGNER, Rust by $RUST_KEY"
    echo "llvm LLVM $LLVM_VERSION $LLVM_FILE $(sha "$L")"
    for pkg in "${DEBS[@]}"; do echo "deb $pkg ${DEB_VER[$pkg]} $(basename "${DEB_PATH[$pkg]}") $(sha "${DEB_PATH[$pkg]}")"; done
    for f in "${RUST_FILES[@]}"; do echo "rust ${f%%-[0-9]*} $RUST_VERSION $f $(sha "$CACHE/rust/$f")"; done
  } > "$LOCK"
  say "lock file $LOCK written"
fi

# --- install into OUTDIR ------------------------------------------------------------------------------------------------
if [ -e "$OUT" ]; then
  if [ -f "$OUT/TOOLCHAIN-MANIFEST.txt" ] || [ -z "$(ls -A "$OUT")" ]; then rm -rf "$OUT"
  else die "$OUT exists and was not made by this script: remove it or choose another -o"; fi
fi
mkdir -p "$OUT/llvm" "$OUT/deb" "$OUT/rust" "$OUT/bin"; OUT="$(cd "$OUT" && pwd)"

say "unpacking LLVM (the tools a cross build uses)"
top=LLVM-$LLVM_VERSION-Linux-X64
members=()
for t in "${TOOLS[@]}"; do members+=("$top/bin/$t"); done
members+=("$top/lib/clang/${LLVM_VERSION%%.*}/include")   # (the release tarball holds no licence file)
tar -I 'zstd -d --long=31' -xf "$L" -C "$OUT/llvm" --strip-components=1 "${members[@]}" 2>"$TMP/tar.log" \
  || { cat "$TMP/tar.log" >&2; die "unpacking LLVM failed (a tool in TOOLS is not in this release?)"; }

mkdir -p "$OUT/share/licenses/llvm"; cp "$SCRIPT_DIR/licenses/llvm/LICENSE.TXT" "$OUT/share/licenses/llvm/"
if [ "$MINGW" = 1 ]; then
  say "unpacking ${DEBS[*]}"
  for pkg in "${DEBS[@]}"; do dpkg-deb -x "${DEB_PATH[$pkg]}" "$OUT/deb"; done
  # (binutils-mingw-w64-x86-64's doc folder is a link to binutils-mingw-w64-base's, which holds the copyright file)
  mkdir -p "$OUT/share/licenses/binutils-mingw-w64"
  cp "$OUT/deb/usr/share/doc/binutils-mingw-w64-base/copyright" "$OUT/share/licenses/binutils-mingw-w64/"
  rm -rf "$OUT/deb/usr/share/lintian" "$OUT/deb/usr/share/doc" "$OUT/deb/usr/share/man"
else
  rmdir "$OUT/deb"
fi

say "unpacking Rust"
for f in "${RUST_FILES[@]}"; do
  mkdir -p "$TMP/rust"; tar -xJf "$CACHE/rust/$f" -C "$TMP/rust"
  d="$TMP/rust/${f%.tar.xz}"
  mkdir -p "$OUT/share/licenses/rust"
  case "$f" in
    rustc-*) cp "$d/COPYRIGHT" "$d/LICENSE-APACHE" "$d/LICENSE-MIT" "$OUT/share/licenses/rust/";;
    cargo-*) cp "$d/LICENSE-THIRD-PARTY" "$OUT/share/licenses/rust/cargo-LICENSE-THIRD-PARTY";;
  esac
  for c in $(cat "$d/components"); do                 # what the component's install.sh would copy, without running it
    (cd "$d/$c" && tar --exclude=manifest.in -cf - .) | tar -C "$OUT/rust" -xf -
  done
  rm -rf "$TMP/rust"
done
rm -rf "$OUT/rust/share/doc" "$OUT/rust/share/man" "$OUT/rust/etc"

# bin/: relative symlinks, so the folder can move
for t in "${TOOLS[@]}"; do ln -s "../llvm/bin/$t" "$OUT/bin/$t"; done
[ "$MINGW" = 0 ] || for t in "$OUT"/deb/usr/bin/*; do ln -s "../deb/usr/bin/$(basename "$t")" "$OUT/bin/$(basename "$t")"; done
for t in "$OUT"/rust/bin/*; do [ "$(basename "$t")" = cargo ] || ln -s "../rust/bin/$(basename "$t")" "$OUT/bin/$(basename "$t")"; done
# cargo runs the first rustc on PATH unless RUSTC says otherwise: this cargo always uses the SDK's rustc (it has the
# x86_64-pc-windows-gnu standard library), also when the build was configured with only the toolchain file
cat > "$OUT/bin/cargo" <<'SH'
#!/bin/sh
# the SDK's cargo, always with the SDK's rustc and rustdoc (made by fetch_toolchain.sh)
d=$(cd "$(dirname "$0")" && pwd)
RUSTC="$d/rustc" RUSTDOC="$d/rustdoc" exec "$d/../rust/bin/cargo" "$@"
SH
chmod 755 "$OUT/bin/cargo"
# the Linux target's compiler names: clang run as x86_64-linux-gnu-clang(++) reads llvm/bin/x86_64-unknown-linux-gnu.cfg,
# which gives it the target sysroot (sysroots/x86_64-linux-gnu, relative to the file) and lld. So builds (also
# configure scripts that record their command line, such as FFmpeg's) need no paths, and the SDK can move.
ln -s ../llvm/bin/clang "$OUT/bin/x86_64-linux-gnu-clang"; ln -s ../llvm/bin/clang++ "$OUT/bin/x86_64-linux-gnu-clang++"
cat > "$OUT/llvm/bin/x86_64-unknown-linux-gnu.cfg" <<'CFG'
# clang reads this when run as x86_64-linux-gnu-clang or x86_64-linux-gnu-clang++ (make_sdk.sh's SDK layout):
# the SDK's Linux target sysroot and the SDK's lld
--sysroot=<CFGDIR>/../../../x86_64-linux-gnu
-fuse-ld=lld
CFG
# the same for the Raspberry Pi 5 (Ubuntu 26.04 arm64, Cortex-A76): aarch64-linux-gnu-clang(++)
ln -s ../llvm/bin/clang "$OUT/bin/aarch64-linux-gnu-clang"; ln -s ../llvm/bin/clang++ "$OUT/bin/aarch64-linux-gnu-clang++"
cat > "$OUT/llvm/bin/aarch64-unknown-linux-gnu.cfg" <<'CFG'
# clang reads this when run as aarch64-linux-gnu-clang or aarch64-linux-gnu-clang++ (make_sdk.sh's SDK layout):
# the SDK's Raspberry Pi 5 sysroot (Ubuntu 26.04 arm64), the Pi 5's CPU and the SDK's lld
--sysroot=<CFGDIR>/../../../aarch64-linux-gnu
-mcpu=cortex-a76
-fuse-ld=lld
CFG

{
  echo "Native (Linux x86-64) tools of the cross SDK, made by fetch_toolchain.sh"
  echo "date:     $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "LLVM:     $LLVM_VERSION ($LLVM_FILE, signed by $LLVM_SIGNER; licence Apache-2.0 WITH LLVM-exception)"
  [ "$MINGW" = 0 ] || for pkg in "${DEBS[@]}"; do echo "Ubuntu:   $pkg ${DEB_VER[$pkg]} ($(basename "${DEB_PATH[$pkg]}"))"; done
  echo "Rust:     $("$OUT/rust/bin/rustc" --version 2>/dev/null || echo "$RUST_VERSION") with x86_64-unknown-linux-gnu, aarch64-unknown-linux-gnu, x86_64-pc-windows-gnu, aarch64-apple-darwin, x86_64-apple-darwin"
} > "$OUT/TOOLCHAIN-MANIFEST.txt"
"$OUT/bin/clang" --version | head -1 >/dev/null || die "the unpacked clang does not run on this machine"
[ "$MINGW" = 0 ] || "$OUT/bin/x86_64-w64-mingw32-ld" --version >/dev/null || die "the unpacked x86_64-w64-mingw32-ld does not run on this machine"
"$OUT/bin/rustc" --version >/dev/null || die "the unpacked rustc does not run on this machine"
say "native tools: $OUT ($(du -sh "$OUT" | cut -f1))"
