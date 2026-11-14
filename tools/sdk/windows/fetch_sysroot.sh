#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# fetch_sysroot.sh - make the Windows x64 (UCRT) target sysroot on Linux, from MSYS2's UCRT64 package repository.
#
# The packages below and everything they depend on are downloaded from repo.msys2.org, checked (sha256 from the signed
# repository database, and each package's own GPG signature, both against MSYS2's master keys pinned in windows/msys2.sh), and their
# headers, libraries and DLLs unpacked into one folder: the --sysroot of clang targeting x86_64-w64-windows-gnu. Nothing
# is installed or run; MSYS2 and Windows are not needed.
#
# Reproducible: the exact package files (name, version, file, sha256) go into a lock file (sysroot.lock). Later runs
# use the lock file and the download cache; --update resolves the newest packages again and rewrites the lock file.
# MSYS2 does not keep old package versions online forever, so keep the cache to rebuild an old lock file.
#
# Usage:  ./fetch_sysroot.sh [-o OUTDIR] [--lock FILE] [--cache DIR] [--update]
#   -o OUTDIR    the sysroot (default: ../x86_64-w64-mingw32; make_sdk.sh puts it in the SDK); an
#                existing OUTDIR is replaced only when it is empty or was made by this script (it holds SYSROOT-MANIFEST.txt)
#   --lock FILE  default: windows/sysroot.lock (next to this script)
#   --cache DIR  the downloaded packages (default: cache/msys2 in the repository)
#   --update     resolve PACKAGES against today's repository and rewrite the lock file (also when PACKAGES changed)
# Needs: bash, curl, tar, zstd, gpg, gpgv, sha256sum, awk, sed, grep (Ubuntu: curl zstd gpg gpgv).
set -Eeuo pipefail
export LC_ALL=C   # the same sort order (lock file) everywhere

# ---- the packages of the sysroot: edit this list (short names; their dependencies come along on their own) ----------
PACKAGES=(
  gcc crt headers libwinpthread                          # the compiler runtime: UCRT, libgcc, libstdc++, winpthreads
  libiconv zlib                                          # iconv and zlib (Ubuntu 26.04's zlib is the minimum: common/check_versions.py)
  # OnAir's other libraries (FFmpeg, GLFW, libusb, OpenSSL, HackRF, SoapySDR ...) are built from source for every
  # target by sources/build_sources.sh: not from MSYS2
  # hamlib libuhd
)
# ----------------------------------------------------------------------------------------------------------------------
PFX=mingw-w64-ucrt-x86_64-
# in the closure (the lock file) but not unpacked: only gcc.exe and the MSYS2 binutils (not shipped: clang and GNU ld from
# Ubuntu are the compilers) need them, and nothing in the SDK links them. Leaving them out also leaves out their licences
# (GPL-3 binutils, LGPL-3 gmp/mpfr/mpc).
SKIP=(binutils gettext-runtime gmp isl mpc mpfr zstd)

# ---- which package files go into the sysroot: the files a cross build needs, as paths relative to ucrt64/ (one per line on stdin, the kept ones on stdout):
#   kept:     include/, lib/ (static and import libraries, pkgconfig, cmake, gcc's libgcc and C++ headers, plug-in DLLs),
#             bin/*.dll (to ship next to the program), share/{licenses,pkgconfig,cmake*,aclocal}
#   left out: every .exe, libexec/, libtool .la files, Python/Tcl/Tk library trees, gcc plug-ins, docs, locales, etc/
sysroot_filter() {
  grep -v '/$' \
    | grep -E '^(include/|lib/|bin/[^/]*\.dll$|share/(licenses|pkgconfig|cmake[^/]*|aclocal)/)' \
    | grep -Ev '(\.exe$|\.la$|^lib/(python[0-9]|tcl[0-9]|tk[0-9]|itcl|tdbc|thread[0-9]|bfd-plugins/)|^lib/gcc/[^/]+/[^/]+/(plugin|install-tools)/)' \
    || true
}

# ---- make the sysroot in $1 relocatable
#  pkg-config: MSYS2 writes prefix=/ucrt64 (its own pkgconf relocates on Windows). ${pcfiledir}/../.. is the sysroot for
#    lib/pkgconfig and share/pkgconfig. Any other absolute path to a ucrt64 folder becomes ${prefix}: /ucrt64,
#    C:/msys64/ucrt64, and the paths of MSYS2's build machines some packages leave behind (D:/M/msys64/ucrt64, e:/ucrt64).
#  CMake package files are normally relocatable already; the few that name the MSYS2 install (FFTW's) get
#    ${CMAKE_CURRENT_LIST_DIR}/<the way up to the sysroot>.
sysroot_relocate() {
  local out=$1 n=0 pc f rel up left
  local upath='([[:alpha:]]:)?(/[^/[:space:]"=]+)*/ucrt64'
  for pc in "$out"/lib/pkgconfig/*.pc "$out"/share/pkgconfig/*.pc; do
    [ -f "$pc" ] || continue
    sed -i -E \
      -e "/^prefix=/!s#(^|[[:space:]=\",;]|-[IL])$upath(/|\$|[[:space:]\"])#\\1\${prefix}\\4#g" \
      -e 's#^prefix=.*#prefix=${pcfiledir}/../..#' "$pc"
    n=$((n + 1))
  done
  echo "==> $n pkg-config files made relocatable"
  left=$(grep -lE "(^|[[:space:]=\",;]|-[IL])$upath(/|\$|[[:space:]\"])" "$out"/lib/pkgconfig/*.pc "$out"/share/pkgconfig/*.pc 2>/dev/null || true)
  [ -z "$left" ] || { echo "warning: pkg-config files still naming a ucrt64 folder:" >&2; echo "    ${left//$'\n'/$'\n    '}" >&2; }

  local cpat="(^|[[:space:]=\"(;]|-[IL])$upath(/|\$|[[:space:]\")])"
  n=0
  while IFS= read -r f; do
    [ -n "$f" ] || continue
    rel=$(dirname "${f#"$out"/}"); up=$(sed -E 's#[^/]+#..#g' <<<"$rel")
    sed -i -E "s#$cpat#\\1\${CMAKE_CURRENT_LIST_DIR}/$up\\4#g" "$f"
    n=$((n + 1))
  done < <(grep -rlE "$cpat" "$out/lib/cmake" "$out/share" --include='*.cmake' 2>/dev/null || true)
  [ "$n" = 0 ] || echo "==> $n CMake files made relocatable"
}

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"; ROOT="$(dirname "$SCRIPT_DIR")"
. "$SCRIPT_DIR/msys2.sh"
OUT="$ROOT/x86_64-w64-mingw32" LOCK="$SCRIPT_DIR/sysroot.lock" CACHE="$ROOT/cache/msys2" UPDATE=0
while [ $# -gt 0 ]; do
  case "$1" in
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
for t in curl tar zstd gpg gpgv sha256sum awk; do command -v $t >/dev/null || die "$t not found (apt install curl zstd gpg gpgv)"; done

TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
export GNUPGHOME="$TMP/gnupg"; mkdir -m 700 "$GNUPGHOME"
mkdir -p "$CACHE"
ASKED=$(for p in "${PACKAGES[@]}"; do case "$p" in "$PFX"*) echo "$p";; *) echo "$PFX$p";; esac; done | sort -u | tr '\n' ' ')
ASKED=${ASKED% }

# --- the lock file: read it, or resolve the packages again ---------------------------------------------------------------
declare -a PKG_NAME PKG_VER PKG_FILE PKG_SHA
KEYRING_FILE="" KEYRING_SHA=""
if [ "$UPDATE" = 0 ] && [ -f "$LOCK" ]; then
  [ "$(sed -n 's/^asked //p' "$LOCK")" = "$ASKED" ] || die "PACKAGES differs from the lock file $LOCK: run with --update"
  read -r _ KEYRING_FILE KEYRING_SHA < <(grep '^keyring ' "$LOCK")
  while read -r _ n v f s; do PKG_NAME+=("$n"); PKG_VER+=("$v"); PKG_FILE+=("$f"); PKG_SHA+=("$s"); done < <(grep '^pkg ' "$LOCK")
  say "lock file $LOCK: ${#PKG_NAME[@]} packages"
  NEED_DB=0
else
  NEED_DB=1
  msys2_latest_keyring
fi

# --- the trusted keys: the master keys pinned in windows/msys2.sh and the packager keys they certified -----------------
msys2_trust "$KEYRING_FILE" "$KEYRING_SHA"

if [ "$NEED_DB" = 1 ]; then
  say "reading ucrt64.db"
  download "$MINGW_REPO/ucrt64.db" "$TMP/ucrt64.db"; download "$MINGW_REPO/ucrt64.db.sig" "$TMP/ucrt64.db.sig"
  msys2_verify "$TMP/ucrt64.db"
  mkdir "$TMP/db"; tar -xf "$TMP/ucrt64.db" -C "$TMP/db"
  declare -A DIR_OF PROVIDER DEPS
  while IFS=$'\t' read -r kind a b; do
    case "$kind" in
      N) DIR_OF[$a]=$b; PROVIDER[$a]=$a;;
      P) [ -n "${PROVIDER[$b]:-}" ] || PROVIDER[$b]=$a;;
      D) DEPS[$a]+="$b ";;
    esac
  done < <(awk '
    FNR == 1 { name = ""; sec = ""; dir = FILENAME; sub(/\/desc$/, "", dir) }
    /^%.*%$/ { sec = $0; next }
    /^$/     { sec = ""; next }
    sec == "%NAME%"     { name = $0; print "N\t" name "\t" dir }
    sec == "%PROVIDES%" { v = $0; sub(/[<>=].*/, "", v); print "P\t" name "\t" v }
    sec == "%DEPENDS%"  { v = $0; sub(/[<>=].*/, "", v); print "D\t" name "\t" v }
  ' $(ls -d "$TMP"/db/*/ | sort | sed 's|/$|/desc|'))
  for p in $ASKED; do [ -n "${DIR_OF[$p]:-}" ] || die "no package $p in $MINGW_REPO"; done
  declare -A SEEN; queue=($ASKED) CLOSURE=()
  while [ ${#queue[@]} -gt 0 ]; do
    p=${queue[0]}; queue=("${queue[@]:1}")
    [ -z "${SEEN[$p]:-}" ] || continue
    SEEN[$p]=1; CLOSURE+=("$p")
    for d in ${DEPS[$p]:-}; do
      q=${PROVIDER[$d]:-}
      if [ -z "$q" ]; then echo "warning: $p depends on '$d', which no package provides (skipped)" >&2; continue; fi
      [ -n "${SEEN[$q]:-}" ] || queue+=("$q")
    done
  done
  for p in $(printf '%s\n' "${CLOSURE[@]}" | sort); do
    d=${DIR_OF[$p]}
    PKG_NAME+=("$p"); PKG_VER+=("$(desc_field "$d/desc" VERSION)"); PKG_FILE+=("$(desc_field "$d/desc" FILENAME)"); PKG_SHA+=("$(desc_field "$d/desc" SHA256SUM)")
  done
  {
    echo "# fetch_sysroot.sh lock file: the exact MSYS2 UCRT64 packages of the sysroot. Rewrite with: fetch_sysroot.sh --update"
    echo "# resolved $(date -u +%Y-%m-%dT%H:%M:%SZ) from $MINGW_REPO"
    echo "asked $ASKED"
    echo "keyring $KEYRING_FILE $KEYRING_SHA"
    for i in "${!PKG_NAME[@]}"; do echo "pkg ${PKG_NAME[$i]} ${PKG_VER[$i]} ${PKG_FILE[$i]} ${PKG_SHA[$i]}"; done
  } > "$LOCK"
  say "${#PKG_NAME[@]} packages ($(wc -w <<<"$ASKED") asked for, the rest are their dependencies): lock file $LOCK written"
fi

# --- download and check -------------------------------------------------------------------------------------------------
say "downloading into $CACHE (what is not there yet) and checking sha256 and signatures"
for i in "${!PKG_NAME[@]}"; do
  cached "$MINGW_REPO" "${PKG_FILE[$i]}" "${PKG_SHA[$i]}"
  msys2_verify "$CACHE/${PKG_FILE[$i]}"
done

# --- unpack -------------------------------------------------------------------------------------------------------------
if [ -e "$OUT" ]; then
  if [ -f "$OUT/SYSROOT-MANIFEST.txt" ] || [ -z "$(ls -A "$OUT")" ]; then rm -rf "$OUT"
  else die "$OUT exists and was not made by this script: remove it or choose another -o"; fi
fi
mkdir -p "$OUT"; OUT="$(cd "$OUT" && pwd)"
say "unpacking into $OUT"
# Each package is unpacked whole into a staging folder (next to OUTDIR, on the same disk), then only the files the
# filter keeps are copied: some archives hold hard links to files the filter leaves out (gcc's liblto_plugin.dll).
STAGE="$OUT.stage.$$"; rm -rf "$STAGE"; mkdir -p "$STAGE"
trap 'rm -rf "$TMP" "$STAGE"' EXIT
skipped() { local s; for s in "${SKIP[@]}"; do [ "$1" = "$PFX$s" ] && return 0; done; return 1; }
for i in "${!PKG_NAME[@]}"; do
  skipped "${PKG_NAME[$i]}" && continue
  tar -xf "$CACHE/${PKG_FILE[$i]}" -C "$STAGE" --exclude='.PKGINFO' --exclude='.MTREE' --exclude='.BUILDINFO' --exclude='.INSTALL'
done
[ -d "$STAGE/ucrt64" ] || die "the packages hold no ucrt64/ folder"
(cd "$STAGE/ucrt64" && find . \( -type f -o -type l \) | sed 's|^\./||') | sysroot_filter | sort > "$TMP/files"
tar -C "$STAGE/ucrt64" --no-recursion --verbatim-files-from -cf - -T "$TMP/files" | tar -C "$OUT" -xf -
rm -rf "$STAGE"
say "$(wc -l < "$TMP/files") files"
sysroot_relocate "$OUT"

{
  echo "MSYS2 UCRT64 sysroot for x86_64-w64-windows-gnu (UCRT), made by fetch_sysroot.sh"
  echo "date:        $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "source:      $MINGW_REPO (lock file: $(basename "$LOCK"), $(sha256sum "$LOCK" | cut -c1-16))"
  echo "gcc runtime: $(for i in "${!PKG_NAME[@]}"; do [ "${PKG_NAME[$i]}" = "${PFX}gcc" ] && echo "${PKG_NAME[$i]} ${PKG_VER[$i]}"; done)"
  echo "asked for:   $ASKED"
  echo
  echo "packages (name version), the closure of the ones asked for, without the ones not unpacked (${SKIP[*]}):"
  for i in "${!PKG_NAME[@]}"; do skipped "${PKG_NAME[$i]}" || echo "${PKG_NAME[$i]} ${PKG_VER[$i]}"; done
} > "$OUT/SYSROOT-MANIFEST.txt"
say "sysroot: $OUT ($(du -sh "$OUT" | cut -f1))"
