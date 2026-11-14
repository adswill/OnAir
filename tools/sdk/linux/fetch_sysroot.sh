#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# fetch_sysroot.sh - make a Linux target sysroot: Ubuntu 26.04 (resolute) headers and libraries, from Ubuntu's package
# archive, for clang --target=<triple> --sysroot=<this>. Two architectures:
#   --arch amd64   x86_64-linux-gnu    Linux x86-64 PCs (lock file linux/sysroot.lock)
#   --arch arm64   aarch64-linux-gnu   Raspberry Pi 5 with Ubuntu 26.04 (lock file linux/sysroot-arm64.lock); since
#                                      26.04 Ubuntu's main archive (and its snapshots) also carry arm64
#
# The packages below and everything they depend on are read from a snapshot of the archive (snapshot.ubuntu.com, so a
# lock file can be replayed later), checked (the InRelease files' GPG signature against Ubuntu's archive key pinned below,
# the Packages indices' sha256 from InRelease, each .deb's sha256 from the index) and unpacked (dpkg-deb -x; nothing is
# installed or run) into one folder. Only what building and linking needs is kept: headers, libraries, pkg-config and
# CMake files, the licences. OnAir's own libraries are built by sources/build_sources.sh. Ubuntu's versions are the SDK's
# minimum versions: the lock file also records them for the libraries in MINIMUM, and common/check_versions.py checks
# that the Windows and macOS sysroots are not older.
#
# Reproducible: the snapshot time and the exact .deb files (name, version, file, sha256) go into a lock file
# (sysroot.lock). Later runs use the lock file and the download cache; --update resolves against a new snapshot.
#
# Usage:  ./fetch_sysroot.sh [--arch amd64|arm64] [-o OUTDIR] [--lock FILE] [--cache DIR] [--update [--snapshot TIME]]
#   --arch       amd64 (default) or arm64
#   -o OUTDIR    the sysroot (default: ../<triple>; make_sdk.sh puts it in the SDK); an existing OUTDIR is
#                replaced only when it is empty or was made by this script (it holds SYSROOT-MANIFEST.txt)
#   --lock FILE  default: linux/sysroot.lock (amd64), linux/sysroot-arm64.lock (arm64)
#   --cache DIR  the downloaded packages (default: cache/ubuntu in the repository)
#   --update     resolve PACKAGES against today's snapshot and rewrite the lock file (also when PACKAGES changed)
#   --snapshot TIME   with --update: this snapshot (YYYYMMDDTHHMMSSZ) instead of now, e.g. the other architecture's
# Needs: bash, curl, gpg, gpgv, xz, dpkg-deb, python3, sha256sum (Ubuntu: curl gpg gpgv xz-utils python3).
set -Eeuo pipefail
export LC_ALL=C

# ---- the packages of the sysroot: edit this list (their dependencies come along on their own) -------------------------
PACKAGES=(
  libc6-dev libgcc-16-dev libstdc++-16-dev               # the C and C++ runtime: glibc 2.43, GCC 16 (Ubuntu 26.04's libstdc++6)
  libgl-dev libegl-dev zlib1g-dev                        # OpenGL/EGL for the GUI, zlib
  libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libxext-dev   # X11 and Wayland: what GLFW is built
  libwayland-dev libxkbcommon-dev                        # against (sources/build_sources.sh; it loads them at run time)
  # OnAir's other libraries (FFmpeg, GLFW, libusb, OpenSSL, HackRF, SoapySDR ...) are built from source for every
  # target by sources/build_sources.sh: not Ubuntu's
)
# the libraries whose Ubuntu version is the SDK's minimum (recorded in the lock file; common/check_versions.py)
MINIMUM=(zlib1g-dev)
# runtime-only dependencies that linking does not need (drivers, data, tools); globs. (gcc-16-base is kept: it holds the
# copyright file of every GCC runtime package, whose doc folders are links to it)
EXCLUDE=('libgl1-mesa-dri' 'libglx-mesa0' 'libegl-mesa0' 'mesa-*' 'libllvm*' 'libgallium*' 'libvulkan1' 'mesa-vulkan-drivers'
         'debconf' 'debconf-2.0' 'dpkg' 'perl-base' 'perl' 'adduser' 'passwd' 'login' 'init-system-helpers' 'sensible-utils'
         'ucf' 'lsb-base' 'sysvinit-utils' 'libpam*' 'systemd' 'systemd-*' 'udev' 'libsystemd-shared' 'python3*'
         'tzdata' 'coreutils' 'libc-bin' 'libc-dev-bin' 'rpcsvc-proto' 'gcc-16' 'cpp-16' 'binutils*' 'libx11-data'
         'xkb-data' 'libdrm-common' 'libwayland-bin' 'libgles*-mesa*' 'libwayland-egl-backend-dev' 'manpages*' 'install-info'
         'lib*-perl' 'libc-gconv-modules-extra' 'libasound2-data' 'xorg-sgml-doctools')
SUITES=(resolute resolute-updates resolute-security)
COMPONENTS=(main universe)
SNAPSHOT=https://snapshot.ubuntu.com/ubuntu       # + /<time>/dists/... and /<time>/pool/...
UBUNTU_KEY=F6ECB3762474EDA9D21B7022871920D1991BC93C  # Ubuntu Archive Automatic Signing Key (2018)
UBUNTU_KEY_URL="https://keyserver.ubuntu.com/pks/lookup?op=get&options=mr&search=0x$UBUNTU_KEY"
# ----------------------------------------------------------------------------------------------------------------------

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"; ROOT="$(dirname "$SCRIPT_DIR")"
ARCH=amd64 OUT="" LOCK="" CACHE="$ROOT/cache/ubuntu" UPDATE=0 TS=""
while [ $# -gt 0 ]; do
  case "$1" in
    --arch) ARCH="$2"; shift 2;;
    --snapshot) TS="$2"; shift 2;;
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
case "$ARCH" in
  amd64) TRIPLE=x86_64-linux-gnu; : "${LOCK:=$SCRIPT_DIR/sysroot.lock}";;
  arm64) TRIPLE=aarch64-linux-gnu; : "${LOCK:=$SCRIPT_DIR/sysroot-arm64.lock}";;
  *) die "--arch amd64 or arm64";;
esac
: "${OUT:=$ROOT/$TRIPLE}"
for t in curl gpg gpgv xz dpkg-deb python3 sha256sum; do command -v $t >/dev/null || die "$t not found (apt install curl gpg gpgv xz-utils python3)"; done
download() { curl -fsSL --retry 6 --retry-delay 5 --retry-all-errors --connect-timeout 30 -o "$2.part" "$1" || { rm -f "$2.part"; die "download failed: $1"; }; mv "$2.part" "$2"; }
sha() { sha256sum "$1" | cut -d' ' -f1; }

TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
export GNUPGHOME="$TMP/gnupg"; mkdir -m 700 "$GNUPGHOME"
mkdir -p "$CACHE"
ASKED=$(printf '%s\n' "${PACKAGES[@]}" | sort -u | tr '\n' ' '); ASKED=${ASKED% }

# --- the lock file: read it, or resolve against a new snapshot ----------------------------------------------------------
if [ "$UPDATE" = 0 ] && [ -f "$LOCK" ]; then
  [ "$(sed -n 's/^asked //p' "$LOCK")" = "$ASKED" ] || die "PACKAGES differs from the lock file $LOCK: run with --update"
  TS=$(sed -n 's/^snapshot //p' "$LOCK")
  grep '^deb ' "$LOCK" | cut -d' ' -f2- > "$TMP/closure"
  say "lock file $LOCK: snapshot $TS, $(wc -l < "$TMP/closure") packages"
else
  [ -n "$TS" ] || TS=$(date -u +%Y%m%dT%H%M%SZ)
  say "Ubuntu archive snapshot $TS"
  K="$CACHE/ubuntu-archive-$UBUNTU_KEY.asc"
  [ -f "$K" ] || download "$UBUNTU_KEY_URL" "$K"
  gpg -q --batch --import "$K" 2>/dev/null
  gpg --batch --with-colons --fingerprint | grep -q "^fpr:::::::::$UBUNTU_KEY:" || die "$K does not hold the key $UBUNTU_KEY"
  gpg -q --batch --export "$UBUNTU_KEY" > "$TMP/ubuntu.gpg"
  idx=()
  for s in "${SUITES[@]}"; do
    download "$SNAPSHOT/$TS/dists/$s/InRelease" "$TMP/$s.InRelease"
    gpgv --keyring "$TMP/ubuntu.gpg" "$TMP/$s.InRelease" >/dev/null 2>&1 || die "$s/InRelease: bad signature"
    for c in "${COMPONENTS[@]}"; do
      p="$c/binary-$ARCH/Packages.xz"
      want=$(awk -v p="$p" '/^SHA256:/{x=1;next} /^[^ ]/{x=0} x && $3==p {print $1}' "$TMP/$s.InRelease")
      [ -n "$want" ] || die "$s/InRelease lists no $p"
      download "$SNAPSHOT/$TS/dists/$s/$p" "$TMP/$s-$c.xz"
      [ "$(sha "$TMP/$s-$c.xz")" = "$want" ] || die "$s/$p: sha256 differs from InRelease"
      xz -d < "$TMP/$s-$c.xz" > "$TMP/$s-$c.Packages"; idx+=("$TMP/$s-$c.Packages")
    done
  done
  say "signatures and indices checked (key $UBUNTU_KEY)"
  printf '%s\n' "${PACKAGES[@]}" > "$TMP/asked"; printf '%s\n' "${EXCLUDE[@]}" > "$TMP/exclude"; printf '%s\n' "${MINIMUM[@]}" > "$TMP/minimum"
  python3 -I "$SCRIPT_DIR/ubuntu_index.py" "$TMP/asked" "$TMP/exclude" "$TMP/minimum" "${idx[@]}" > "$TMP/closure"
  {
    echo "# fetch_sysroot.sh lock file: the exact Ubuntu $ARCH packages of the Linux sysroot. Rewrite with: fetch_sysroot.sh --arch $ARCH --update"
    echo "snapshot $TS"
    echo "asked $ASKED"
    grep '^minimum ' "$TMP/closure"
    grep -v '^minimum ' "$TMP/closure" | sed 's/^/deb /'
  } > "$LOCK"
  sed -i '/^minimum /d' "$TMP/closure"
  say "$(wc -l < "$TMP/closure") packages ($(wc -w <<<"$ASKED") asked for, the rest are their dependencies): lock file $LOCK written"
fi

# --- download and check -------------------------------------------------------------------------------------------------
say "downloading into $CACHE (what is not there yet) and checking sha256"
while read -r n _ f s; do
  d="$CACHE/$(basename "$f")"
  if [ ! -f "$d" ] || [ "$(sha "$d")" != "$s" ]; then
    download "$SNAPSHOT/$TS/$f" "$d"
    [ "$(sha "$d")" = "$s" ] || { rm -f "$d"; die "$(basename "$f"): sha256 differs from the lock file / index"; }
  fi
done < "$TMP/closure"

# --- unpack -------------------------------------------------------------------------------------------------------------
if [ -e "$OUT" ]; then
  if [ -f "$OUT/SYSROOT-MANIFEST.txt" ] || [ -z "$(ls -A "$OUT")" ]; then rm -rf "$OUT"
  else die "$OUT exists and was not made by this script: remove it or choose another -o"; fi
fi
mkdir -p "$OUT"; OUT="$(cd "$OUT" && pwd)"
say "unpacking into $OUT"
STAGE="$OUT.stage.$$"; rm -rf "$STAGE"; mkdir -p "$STAGE"
trap 'rm -rf "$TMP" "$STAGE"' EXIT
declare -A SRC_OF
while read -r n _ f s; do
  dpkg-deb -x "$CACHE/$(basename "$f")" "$STAGE"
  src=$(dpkg-deb -f "$CACHE/$(basename "$f")" Source); SRC_OF[$n]=${src%% *}; SRC_OF[$n]=${SRC_OF[$n]:-$n}
done < "$TMP/closure"
# usr/share/licenses/<package>/copyright for every package: its doc folder may be a link to another package's (the GCC
# runtime -> gcc-16-base), cp follows it; a package without one (libffi-dev) gets the copyright of a package built from
# the same source (libffi8)
while read -r n _ f s; do
  c="$STAGE/usr/share/doc/$n/copyright"
  if [ ! -f "$c" ]; then
    c=""
    for m in "${!SRC_OF[@]}"; do
      if [ "${SRC_OF[$m]}" = "${SRC_OF[$n]}" ] && [ -f "$STAGE/usr/share/doc/$m/copyright" ]; then c="$STAGE/usr/share/doc/$m/copyright"; break; fi
    done
  fi
  [ -n "$c" ] || die "$n: no copyright file (none in its package nor in another package of the source ${SRC_OF[$n]})"
  mkdir -p "$STAGE/usr/share/licenses/$n"; cp "$c" "$STAGE/usr/share/licenses/$n/copyright"
done < "$TMP/closure"
# merged /usr (Ubuntu 26.04): files some packages still put in /lib, /lib64 go to usr/, and lib -> usr/lib
for d in lib lib64; do
  if [ -d "$STAGE/$d" ] && [ ! -L "$STAGE/$d" ]; then mkdir -p "$STAGE/usr/$d"; cp -a "$STAGE/$d/." "$STAGE/usr/$d/"; rm -rf "${STAGE:?}/$d"; fi
done
# keep what building and linking needs
(cd "$STAGE" && find . \( -type f -o -type l \) | sed 's|^\./||') \
  | grep -E '^usr/(include/|lib/|lib64/|share/(pkgconfig|cmake[^/]*|aclocal|wayland|wayland-protocols|licenses|X11/xcb|xcb)/)' \
  | grep -Ev '^usr/lib/(python3|systemd|udev|tmpfiles\.d|sysusers\.d|modules-load\.d|binfmt\.d|kernel|firmware|locale|bfd-plugins)/|^usr/lib/'"$TRIPLE"'/(gconv|dri|gallium-pipe|vdpau|perl|perl-base|perl5|libexec)/|^usr/lib/gcc/'"$TRIPLE"'/[0-9]+/(plugin|lto|cc1|collect2)' \
  | sort > "$TMP/files"
tar -C "$STAGE" --no-recursion --verbatim-files-from -cf - -T "$TMP/files" | tar -C "$OUT" -xf -
rm -rf "$STAGE"
ln -s usr/lib "$OUT/lib"; [ -d "$OUT/usr/lib64" ] && ln -s usr/lib64 "$OUT/lib64"
say "$(wc -l < "$TMP/files") files"
# absolute symlinks (libfoo.so -> /lib/x86_64-linux-gnu/libfoo.so.1) become relative, so they stay inside the sysroot
python3 -I - "$OUT" <<'PY'
import os, sys
root = sys.argv[1]; n = bad = 0
for d, dirs, files in os.walk(root):
    for x in dirs + files:
        p = os.path.join(d, x)
        if os.path.islink(p):
            t = os.readlink(p)
            if t.startswith("/"):
                os.remove(p); os.symlink(os.path.relpath(root + t, d), p); n += 1
for d, dirs, files in os.walk(root):
    for x in files:
        p = os.path.join(d, x)
        if os.path.islink(p) and not os.path.exists(p):
            bad += 1
print(f"==> {n} absolute symlinks made relative" + (f"; {bad} symlinks point to files left out" if bad else ""))
PY

{
  echo "Ubuntu 26.04 sysroot for $TRIPLE ($ARCH), made by fetch_sysroot.sh"
  echo "date:        $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "source:      $SNAPSHOT/$TS (${SUITES[*]}; ${COMPONENTS[*]}; lock file: $(basename "$LOCK"), $(sha "$LOCK" | cut -c1-16))"
  echo "glibc:       $(awk '$1=="libc6" {print $2}' "$TMP/closure")"
  echo "gcc runtime: $(awk '$1=="libstdc++-16-dev" {print $1, $2}' "$TMP/closure")"
  echo "asked for:   $ASKED"
  echo "minimum:     $(sed -n 's/^minimum //p' "$LOCK" | tr '\n' ' ')"
  echo
  echo "packages (name version):"
  awk '{print $1, $2}' "$TMP/closure"
} > "$OUT/SYSROOT-MANIFEST.txt"
say "sysroot: $OUT ($(du -sh "$OUT" | cut -f1))"
