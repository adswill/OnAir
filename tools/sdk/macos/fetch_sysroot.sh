#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# fetch_sysroot.sh - make a macOS target sysroot on Linux, for Apple silicon (arm64) or Intel (x86_64): Apple's macOS
# SDK with conda-forge's binary packages in its usr/local, for clang --target=<arch>-apple-macos<min> --sysroot=<this>.
#
#   Apple's SDK      MacOSX<version>.sdk from the Command Line Tools package in Apple's software update catalog
#                    (swcdn.apple.com; no Apple ID). Checked: the package's xar signature (Apple's "Software Update"
#                    certificate, chained to Apple Root CA, whose sha256 is pinned below) and the payload's checksum
#                    in the signed table of contents. The Swift modules are left out.
#   the libraries    conda-forge's osx-arm64 / osx-64 packages (PACKAGES below) and their dependencies, resolved by
#                    macos/conda_index.py from repodata.json (<subdir> and noarch), unpacked into usr/local: headers, dylibs, pkg-config and
#                    CMake files, licences. conda-forge's dylibs have @rpath install names (ship them next to the program,
#                    collect_dylibs.sh) and use conda-forge's libc++ (LLVM's, the same version as the SDK's clang); the
#                    toolchain file builds programs against that libc++ too. Checked: each file's sha256 from
#                    repodata.json (conda-forge publishes no signatures), over HTTPS, and pinned in the lock file.
#
# Ubuntu 26.04's library versions are the minimum (common/check_versions.py); the deployment target MACOS_MIN below is
# the oldest macOS the libraries (and so the programs) run on: macos/macho.py checks every dylib against it.
#
# Reproducible: the SDK package and the exact conda files (name, version, build, file, sha256) go into a lock file
# per architecture (sysroot-<arch>.lock). conda-forge keeps old packages; Apple drops old Command Line Tools from its
# catalog, so keep cache/apple (or rebuild with --update).
#
# Usage:  ./fetch_sysroot.sh --arch arm64|x86_64 [-o OUTDIR] [--lock FILE] [--cache DIR] [--update]
#   -o OUTDIR    the sysroot (default: ../<arch>-apple-darwin); replaced only when empty or made by this script
#   --lock FILE  default: macos/sysroot-<arch>.lock
#   --cache DIR  default: cache/ in the repository (apple/ and conda/<subdir>/)
#   --update     newest Command Line Tools and conda-forge packages; rewrites the lock file
# Needs: bash, curl, zstd, unzip, openssl, python3 (3.12+), sha256sum.
set -Eeuo pipefail
export LC_ALL=C

# ---- the packages of the sysroot: edit this list (conda match specs; their dependencies come along on their own) -----
PACKAGES=(
  libcxx libcxx-devel                                   # the C++ runtime and headers: LLVM libc++ (the same as clang)
  libzlib                                               # zlib (Ubuntu 26.04's version is the minimum)
  # OnAir's other libraries (FFmpeg, GLFW, libusb, OpenSSL, HackRF, SoapySDR ...) are built from source for every
  # target by sources/build_sources.sh (owner, 2026-11-08): not conda-forge's
)
MACOS_MIN=12.0         # deployment target: macOS 12 Monterey (OnAir's minimum; 11.0 is arm64's first)
# runtime-only dependencies a build does not need (globs)
EXCLUDE=('python' 'python_abi' 'libpython' 'ca-certificates' 'fonts-*' 'font-ttf-*' 'tzdata')
# the parts of Apple's SDK left out (globs, relative to the SDK folder): Swift's modules
SDK_EXCLUDE=('usr/lib/swift/*' '*.swiftmodule' '*.swiftmodule/*' '*.swiftinterface' '*.swiftdoc')
CATALOG_URL=https://swscan.apple.com/content/catalogs/others/index-26-15-14-13-12-10.16-10.15-10.14-10.13-10.12-10.11-10.10-10.9-mountainlion-lion-snowleopard-leopard.merged-1.sucatalog
CLT_PACKAGE=CLTools_macOSNMOS_SDK.pkg      # the Command Line Tools' current SDK (NMOS: the newer of the two)
APPLE_ROOT_SHA256=b0b1730ecbc7ff4505142c49f1295e6eda6bcaed7e2c68c5be91b5a11001f024   # Apple Root CA (apple.com/appleca)
CONDA=https://conda.anaconda.org/conda-forge
# ----------------------------------------------------------------------------------------------------------------------

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"; ROOT="$(dirname "$SCRIPT_DIR")"
ARCH="" OUT="" LOCK="" CACHE="$ROOT/cache" UPDATE=0
while [ $# -gt 0 ]; do
  case "$1" in
    --arch) ARCH="$2"; shift 2;;
    -o) OUT="$2"; shift 2;;
    --lock) LOCK="$2"; shift 2;;
    --cache) CACHE="$2"; shift 2;;
    --update) UPDATE=1; shift;;
    -h|--help) sed -n '2,/^set -E/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 0;;
    *) echo "unknown argument $1 (see --help)" >&2; exit 2;;
  esac
done
die() { echo "error: $*" >&2; exit 1; }
say() { echo "==> $*"; }
case "$ARCH" in arm64) SUBDIR=osx-arm64;; x86_64) SUBDIR=osx-64;; *) die "--arch arm64 or x86_64";; esac
# compiler-rt's macOS builtins (lib/clang/<major>/lib/darwin/libclang_rt.osx.a: __isPlatformVersionAtLeast for
# @available, __cpu_model for __builtin_cpu_supports ...), which clang links into every macOS program. LLVM's Linux
# release (common/fetch_toolchain.sh) has no darwin compiler-rt; conda-forge's (noarch) is taken for clang's major version.
LLVM_MAJOR=$(sed -n 's/^LLVM_VERSION=\([0-9]*\)\..*/\1/p' "$ROOT/common/fetch_toolchain.sh")
[ -n "$LLVM_MAJOR" ] || die "no LLVM_VERSION in common/fetch_toolchain.sh"
PACKAGES+=("compiler-rt_$SUBDIR=$LLVM_MAJOR.*")
TRIPLE=$ARCH-apple-darwin
OUT=${OUT:-$ROOT/$TRIPLE} LOCK=${LOCK:-$SCRIPT_DIR/sysroot-$ARCH.lock}
for t in curl zstd unzip openssl python3 sha256sum; do command -v $t >/dev/null || die "$t not found (apt install curl zstd unzip openssl python3)"; done
download() { curl -fsSL --retry 6 --retry-delay 5 --retry-all-errors --connect-timeout 30 -o "$2.part" "$1" || { rm -f "$2.part"; die "download failed: $1"; }; mv "$2.part" "$2"; }
sha() { sha256sum "$1" | cut -d' ' -f1; }
PY="python3 -I"

TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
mkdir -p "$CACHE/apple" "$CACHE/conda"
ASKED="${PACKAGES[*]}"

# --- the lock file: read it, or resolve again ----------------------------------------------------------------------------
if [ "$UPDATE" = 0 ] && [ -f "$LOCK" ]; then
  [ "$(sed -n 's/^asked //p' "$LOCK")" = "$ASKED" ] || die "PACKAGES differs from the lock file $LOCK: run with --update"
  [ "$(sed -n 's/^macos-min //p' "$LOCK")" = "$MACOS_MIN" ] || die "MACOS_MIN differs from the lock file $LOCK: run with --update"
  say "lock file $LOCK: $(grep -c '^pkg ' "$LOCK") packages"
else
  say "Apple's software update catalog: the newest $CLT_PACKAGE"
  download "$CATALOG_URL" "$TMP/catalog"
  read -r PRODUCT POSTED PKG_URL DIST_URL < <($PY "$SCRIPT_DIR/apple_sdk.py" latest "$TMP/catalog" "$CLT_PACKAGE")
  download "$DIST_URL" "$TMP/dist"
  TITLE=$($PY "$SCRIPT_DIR/apple_sdk.py" title "$TMP/dist")
  say "$TITLE (product $PRODUCT, $POSTED)"
  SDK_PKG="$CACHE/apple/$PRODUCT-$CLT_PACKAGE"
  [ -f "$SDK_PKG" ] || download "$PKG_URL" "$SDK_PKG"
  for sub in "$SUBDIR" noarch; do
    say "conda-forge $sub/repodata.json"
    download "$CONDA/$sub/repodata.json.zst" "$TMP/$sub.json.zst"
    zstd -dq "$TMP/$sub.json.zst" -o "$TMP/$sub.json"
  done
  ex=(); for e in "${EXCLUDE[@]}"; do ex+=(--exclude "$e"); done
  $PY "$SCRIPT_DIR/conda_index.py" resolve "$TMP/$SUBDIR.json,$TMP/noarch.json" --macos-min "$MACOS_MIN" "${ex[@]}" "${PACKAGES[@]}" > "$TMP/resolved"
  {
    echo "# fetch_sysroot.sh lock file: Apple's SDK and the exact conda-forge $SUBDIR packages of the macOS $ARCH sysroot."
    echo "# Rewrite with: fetch_sysroot.sh --arch $ARCH --update. Resolved $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "asked $ASKED"
    echo "macos-min $MACOS_MIN"
    echo "sdk $PRODUCT ${TITLE// /_} $PKG_URL $(sha "$SDK_PKG")"
    while read -r name ver build fn sum _size lic; do echo "pkg $name $ver $build $fn $sum $lic"; done < "$TMP/resolved"
  } > "$LOCK"
  say "$(grep -c '^pkg ' "$LOCK") packages: lock file $LOCK written"
fi
read -r _ PRODUCT TITLE PKG_URL PKG_SHA < <(grep '^sdk ' "$LOCK"); TITLE=${TITLE//_/ }

# --- Apple's SDK: download, check the signature, unpack once into the cache ------------------------------------------------
SDK_PKG="$CACHE/apple/$PRODUCT-$CLT_PACKAGE"
if [ ! -f "$SDK_PKG" ] || [ "$(sha "$SDK_PKG")" != "$PKG_SHA" ]; then
  say "downloading $CLT_PACKAGE ($TITLE)"
  download "$PKG_URL" "$SDK_PKG"
  [ "$(sha "$SDK_PKG")" = "$PKG_SHA" ] || die "$CLT_PACKAGE: sha256 differs from the lock file (Apple removed this version? keep cache/apple)"
fi
SIGNER=$($PY "$SCRIPT_DIR/apple_sdk.py" verify "$SDK_PKG" "$APPLE_ROOT_SHA256")
say "$CLT_PACKAGE: signed by $SIGNER (Apple Root CA)"
SDK_KEY=$(printf '%s %s\n' "$PKG_SHA" "${SDK_EXCLUDE[*]}" | sha256sum | cut -c1-16)
SDK_DIR="$CACHE/apple/sdk-$SDK_KEY"
if [ ! -f "$SDK_DIR.done" ]; then
  say "unpacking the SDK (about 2 minutes, once)"
  rm -rf "$SDK_DIR"
  ex=(); for e in "${SDK_EXCLUDE[@]}"; do ex+=(--exclude "$e"); done
  $PY "$SCRIPT_DIR/apple_sdk.py" extract "$SDK_PKG" "$SDK_DIR" "${ex[@]}" > "$SDK_DIR.done.part"
  mv "$SDK_DIR.done.part" "$SDK_DIR.done"
fi
read -r SDK_NAME _ < "$SDK_DIR.done"

# --- conda-forge packages: download and check -----------------------------------------------------------------------------
say "downloading into $CACHE/conda (what is not there yet) and checking sha256"
grep '^pkg ' "$LOCK" > "$TMP/pkgs"
while read -r _ name ver build fn sum _lic; do   # fn: <subdir>/<file> (osx-arm64, osx-64 or noarch)
  f="$CACHE/conda/$fn"; mkdir -p "$(dirname "$f")"
  if [ ! -f "$f" ] || [ "$(sha "$f")" != "$sum" ]; then
    download "$CONDA/$fn" "$f"
    [ "$(sha "$f")" = "$sum" ] || { rm -f "$f"; die "$fn: sha256 differs from the lock file"; }
  fi
done < "$TMP/pkgs"

# --- the sysroot ----------------------------------------------------------------------------------------------------------
if [ -e "$OUT" ]; then
  if [ -f "$OUT/SYSROOT-MANIFEST.txt" ] || [ -z "$(ls -A "$OUT")" ]; then rm -rf "$OUT"
  else die "$OUT exists and was not made by this script: remove it or choose another -o"; fi
fi
mkdir -p "$(dirname "$OUT")"
say "copying $SDK_NAME"
cp -a "$SDK_DIR" "$OUT"; OUT="$(cd "$OUT" && pwd)"
[ ! -e "$OUT/usr/local" ] || die "the SDK has a usr/local already"
mkdir -p "$OUT/usr/local"
say "unpacking $(wc -l < "$TMP/pkgs") conda-forge packages into usr/local"
nfiles=0 nbin=0
while read -r _ name ver build fn sum _lic; do
  read -r n b < <($PY "$SCRIPT_DIR/conda_index.py" unpack "$CACHE/conda/$fn" "$OUT/usr/local" "$name")
  nfiles=$((nfiles + n)) nbin=$((nbin + b))
done < "$TMP/pkgs"
say "$nfiles files ($nbin binaries keep conda's build prefix compiled in, e.g. OpenSSL's OPENSSLDIR)"
$PY "$SCRIPT_DIR/macho.py" check "$OUT" "$MACOS_MIN"

{
  echo "macOS $ARCH sysroot ($TRIPLE), made by fetch_sysroot.sh"
  echo "date:        $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "Apple SDK:   $SDK_NAME from $TITLE (product $PRODUCT, $CLT_PACKAGE, signed by $SIGNER); macOS SDK and Xcode Agreement"
  echo "packages:    conda-forge $SUBDIR in usr/local (lock file: $(basename "$LOCK"), $(sha256sum "$LOCK" | cut -c1-16))"
  echo "deployment:  macOS $MACOS_MIN or newer"
  echo "asked for:   $ASKED"
  echo
  echo "packages (name version build licence), the closure of the ones asked for:"
  awk '{print $2, $3, $4, $7}' "$TMP/pkgs"
} > "$OUT/SYSROOT-MANIFEST.txt"
say "sysroot: $OUT ($(du -sh "$OUT" | cut -f1))"
