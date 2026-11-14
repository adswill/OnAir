#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# build_all_releases.sh - make the SDKs and build OnAir's installers with them, for every platform, from this checkout. It uses
# the CMake targets: sdk_<platform> of tools/sdk (made again only when an input changed, else "up to date" or from the cache),
# then for each platform the steps of README.md: unpack the SDK, source its environment script, configure, --target package.
# On a terminal, without platforms, it asks what to build; without a terminal it builds all five.
#
#   --windows --linux_amd64 --linux_arm64 --macos_amd64 --macos_arm64    the platforms (default: all)
#   -o DIR              where everything goes (default: build-releases/ in this checkout): build-sdk/ (CMake's build folder
#                       of tools/sdk, with the SDK tarballs), installers/ (with SHA256SUMS), logs/
#   --cache DIR         the SDK cache (default: build-sdk/cache in DIR; -DONAIR_SDK_CACHE)
#   --docker            build in the "safe" container: Ubuntu 26.04, no root, no capabilities, this checkout read-only
#   --sdk-only          make the SDKs, do not build OnAir
#   --keep-build-artif  keep the unpacked SDKs and OnAir's build folders in DIR, and the SDKs' work folders (-DONAIR_SDK_KEEP_WORK)
#   -j N                parallel jobs for OnAir's build (default: all CPUs this process may use; the SDKs always use those)
#   --dry-run           print the commands, build nothing
#   -h, --help          this text
#
# Every step stops with an error and a non-zero exit code; a failed OnAir build leaves its log in logs/<platform>.log.
set -Eeuo pipefail
export LC_ALL=C
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)
REPO=$(cd "$HERE/../.." && pwd -P)
IMAGE=onair-sdk-builder
ALL=(windows linux_amd64 linux_arm64 macos_amd64 macos_arm64)
DESC=("Windows x64" "Linux x86-64 (Ubuntu 26.04)" "Linux arm64 (Ubuntu 26.04, Raspberry Pi)" "macOS Intel" "macOS Apple silicon")
OUT="" CACHE="" CHOSEN=() DOCKER=0 ASKED_DOCKER=0 IN_DOCKER=0 SDK_ONLY=0 KEEP=0 JOBS="" DRY=0

usage() { sed -n '4,20p' "$0" | sed 's/^# \{0,1\}//'; }
die() { echo "error: $*" >&2; exit 1; }
while [ $# -gt 0 ]; do
  case "$1" in
    --windows|--linux_amd64|--linux_arm64|--macos_amd64|--macos_arm64) CHOSEN+=("${1#--}");;
    -o) OUT=${2:?-o needs a folder}; shift;;
    --cache) CACHE=${2:?--cache needs a folder}; shift;;
    --docker) DOCKER=1; ASKED_DOCKER=1;;
    --in-docker) IN_DOCKER=1;;   # (the container's own run)
    --sdk-only) SDK_ONLY=1;;
    --keep-build-artif) KEEP=1;;
    -j) JOBS=${2:?-j needs a number}; shift;;
    -j*) JOBS=${1#-j};;
    --dry-run) DRY=1;;
    -h|--help) usage; exit 0;;
    *) echo "unknown argument: $1" >&2; usage >&2; exit 2;;
  esac
  shift
done
[ -z "$JOBS" ] || [[ "$JOBS" =~ ^[1-9][0-9]*$ ]] || die "-j needs a number"

# Ubuntu 26.04 is what the SDK is made on, also in WSL: there the Docker question defaults to no, everywhere else to yes
on_ubuntu_2604() { [ "$(uname -s)" = Linux ] && [ -r /etc/os-release ] && ( . /etc/os-release; [ "${ID:-}" = ubuntu ] && [ "${VERSION_ID:-}" = 26.04 ] ); }

ask_yes() {   # <question> <default y|n> <explanation, shown for ?>
  local a hint='[y/N/?]'; [ "$2" = y ] && hint='[Y/n/?]'
  while :; do
    read -r -p "$1 $hint " a || a=
    a=${a:-$2}
    case "$a" in
      y|Y|yes|YES) return 0;;
      n|N|no|NO) return 1;;
      '?') printf '%s\n' "$3" | sed 's/^/  /';;
      *) echo "  Please answer y or n, or ? for an explanation.";;
    esac
  done
}

if [ ${#CHOSEN[@]} -eq 0 ] && [ "$IN_DOCKER" = 0 ] && [ -t 0 ] && [ "$DRY" = 0 ]; then
  echo "Which platforms do you want to build?"
  for i in "${!ALL[@]}"; do printf '  %d) %-12s %s\n' $((i + 1)) "${ALL[$i]}" "${DESC[$i]}"; done
  echo "  a) all of them (Enter)"
  while :; do
    read -r -p "Numbers separated by spaces, a for all, ? for an explanation: " answer || answer=a
    if [ "$answer" = '?' ]; then
      cat <<'HELP' | sed 's/^/  /'
Each platform gets its SDK (a compiler and libraries, about 800 MB) and OnAir's installers made with it.
An SDK takes about 5 minutes the first time plus its downloads, and nothing once it is made and its inputs did not change.
OnAir takes about 15 to 20 minutes per platform on 8 CPUs. For example, 1 3 builds Windows and Linux arm64.
HELP
      continue
    fi
    CHOSEN=(); ok=1
    for w in ${answer:-a}; do
      case "${w//,/}" in
        a|A|all) CHOSEN=("${ALL[@]}");;
        [1-5]) CHOSEN+=("${ALL[$((${w//,/} - 1))]}");;
        '') ;;
        *) echo "  not a choice: $w"; ok=0;;
      esac
    done
    [ "$ok" = 1 ] && [ ${#CHOSEN[@]} -gt 0 ] && break
  done
  if [ "$ASKED_DOCKER" = 0 ]; then
    default=y; if on_ubuntu_2604; then default=n; fi
    if ask_yes "Build in the Docker \"safe build\"?" "$default" "Builds in a container: Ubuntu 26.04, no root, and this checkout read-only.
You need only Docker, on any system, and nothing is installed on this machine.
No: builds directly here, which needs Ubuntu 26.04 and its packages (CMake checks them first)."; then DOCKER=1; fi
  fi
  if [ "$KEEP" = 0 ] && ask_yes "Keep the build artifacts?" n "Yes keeps the unpacked SDKs, OnAir's build folders and the SDKs' work folders. Useful for finding out why a build failed.
They take several GB per platform. No deletes each one as soon as it is done with.
The SDK tarballs, the cache and the installers always stay."; then KEEP=1; fi
  if [ "$SDK_ONLY" = 0 ] && ! ask_yes "Build OnAir's installers with these SDKs?" y "Yes builds the installers of this checkout with each SDK (cmake --build ... --target package), into installers/.
No makes only the SDKs. You can build OnAir with them later: see README.md."; then SDK_ONLY=1; fi
fi
[ ${#CHOSEN[@]} -gt 0 ] || CHOSEN=("${ALL[@]}")
ORDERED=()   # each once, in the usual order
for a in "${ALL[@]}"; do for c in "${CHOSEN[@]}"; do [ "$c" != "$a" ] || { ORDERED+=("$a"); break; }; done; done

OUT=${OUT:-$REPO/build-releases}
[ "$DRY" = 1 ] || mkdir -p "$OUT"
OUT=$(cd "$OUT" 2>/dev/null && pwd -P || echo "$OUT")
show() { printf '  $ %s\n' "$*"; }
run() { show "$*"; [ "$DRY" = 1 ] || "$@"; }

# ---- the safe build: this script in a container, the checkout read-only -----------------------------------------------------
if [ "$DOCKER" = 1 ] && [ "$IN_DOCKER" = 0 ]; then
  args=(--in-docker -o /out); for a in "${ORDERED[@]}"; do args+=("--$a"); done
  [ "$SDK_ONLY" = 0 ] || args+=(--sdk-only)
  [ "$KEEP" = 0 ] || args+=(--keep-build-artif)
  [ -z "$JOBS" ] || args+=(-j "$JOBS")
  run_=(docker run --rm --init --platform linux/amd64 --cap-drop=ALL --security-opt=no-new-privileges
        -e PYTHONDONTWRITEBYTECODE=1 -v "$REPO":/onair:ro -v "$OUT":/out)
  if [ -n "$CACHE" ]; then [ "$DRY" = 1 ] || mkdir -p "$CACHE"; run_+=(-v "$(cd "$CACHE" 2>/dev/null && pwd -P || echo "$CACHE")":/cache); args+=(--cache /cache); fi
  [ ! -t 1 ] || run_+=(-t); [ ! -t 0 ] || run_+=(-i)   # attached: the log stays in view, and Ctrl-C stops the build
  cpus=$(sed -n 's/^Cpus_allowed_list:[[:space:]]*//p' /proc/self/status 2>/dev/null || true)
  [ -z "$cpus" ] || run_+=("--cpuset-cpus=$cpus")      # the CPUs this process may use
  run_+=("$IMAGE" bash /onair/tools/sdk/build_all_releases.sh "${args[@]}")
  echo "Platforms: ${ORDERED[*]} (in Docker)"
  # no build context: the image is only the Dockerfile's packages
  show "docker build --platform linux/amd64 --build-arg UID=$(id -u) --build-arg GID=$(id -g) -t $IMAGE - < tools/sdk/Dockerfile"
  show "${run_[*]}"
  [ "$DRY" = 0 ] || exit 0
  command -v docker >/dev/null || die "docker is not installed"
  docker info >/dev/null 2>&1 || die "cannot reach Docker. Is it running, and are you in the docker group? (sudo usermod -aG docker \$USER, then log in again)"
  docker build -q --platform linux/amd64 --build-arg "UID=$(id -u)" --build-arg "GID=$(id -g)" -t "$IMAGE" - < "$HERE/Dockerfile" >/dev/null
  exec "${run_[@]}"
fi

if [ "$IN_DOCKER" = 0 ] && [ "$(uname -s)" != Linux ]; then die "on $(uname -s) the SDKs are made in Docker: use --docker"; fi

# ---- the SDKs: tools/sdk's CMake targets ----------------------------------------------------------------------------------
B=$OUT/build-sdk
cfg=(cmake -S "$HERE" -B "$B" -G Ninja "-DONAIR_SDK_KEEP_WORK=$([ "$KEEP" = 1 ] && echo ON || echo OFF)")
[ -z "$CACHE" ] || cfg+=("-DONAIR_SDK_CACHE=$CACHE")
# a build folder configured from another path to this checkout (in Docker, or outside it) is configured again; its cache stays
if [ -f "$B/CMakeCache.txt" ] && ! grep -qx "CMAKE_HOME_DIRECTORY:INTERNAL=$HERE" "$B/CMakeCache.txt"; then
  run rm -rf "$B/CMakeCache.txt" "$B/CMakeFiles"
fi
printf '\n==== %s: SDKs: %s\n' "$(date +%H:%M:%S)" "${ORDERED[*]}"
run "${cfg[@]}"
targets=(); for a in "${ORDERED[@]}"; do targets+=("sdk_$a"); done
run cmake --build "$B" --target "${targets[@]}"

sha256() { if command -v sha256sum >/dev/null; then sha256sum "$@"; else shasum -a 256 "$@"; fi; }
if [ "$SDK_ONLY" = 1 ]; then
  [ "$DRY" = 1 ] || (cd "$B" && sha256 sdk_*.tar.gz > SHA256SUMS)
  printf '\nSDKs: %s\n' "$B"; exit 0
fi

# ---- OnAir with each SDK: the steps of README.md, each platform in a shell of its own --------------------------------------
INST=$OUT/installers LOGS=$OUT/logs
[ "$DRY" = 1 ] || mkdir -p "$INST" "$LOGS"
size_mb() { echo "$(( ($(stat -c %s "$1") + 524288) / 1048576 )) MB"; }
progress() { awk '/^\[[0-9]+\/[0-9]+\]/ { split(substr($1, 2, length($1) - 2), n, "/"); if (n[2] < 10) next; p = int(n[1] * 10 / n[2]) * 10; if (p > last) { last = p; printf "    %d%% (%d of %d)\n", p, n[1], n[2]; fflush() } }'; }
failed() {   # <platform> <log>
  echo "---- the end of the log (all of it: $2):" >&2; tail -40 "$2" >&2
  die "OnAir failed for $1"
}

onair_one() {   # <platform>
  local p=$1 d=$OUT/$1 log=$LOGS/$1.log start=$SECONDS extra=() par=() inst="" f
  [ "$p" != linux_arm64 ] || extra=(-DRPI_CPU=generic)   # any 64-bit ARM
  [ -z "$JOBS" ] || par=(-j "$JOBS")
  printf '\n==== %s: OnAir for %s (log: %s)\n' "$(date +%H:%M:%S)" "$p" "$log"
  show "tar xzf $B/sdk_$p.tar.gz -C $d"
  show ". $d/environment_$p.sh"
  show "cmake -S $REPO -B $d/build -G Ninja -DCMAKE_BUILD_TYPE=Release${extra[*]:+ ${extra[*]}}"
  show "cmake --build $d/build${par[*]:+ ${par[*]}} --target package"
  [ "$DRY" = 0 ] || return 0
  rm -rf "$d"; mkdir -p "$d"; : > "$log"
  tar xzf "$B/sdk_$p.tar.gz" -C "$d" >> "$log" 2>&1 || failed "$p" "$log"
  (
    # shellcheck source=/dev/null  # (the SDK unpacked above)
    . "$d/environment_$p.sh" >> "$log" 2>&1
    export CCACHE_DISABLE=1
    cmake -S "$REPO" -B "$d/build" -G Ninja -DCMAKE_BUILD_TYPE=Release "${extra[@]}" >> "$log" 2>&1
    cmake --build "$d/build" "${par[@]}" --target package 2>&1 | tee -a "$log" | progress
  ) || failed "$p" "$log"
  for f in "$d/build/packages"/*.exe "$d/build/packages"/*.zip "$d/build/packages"/*.deb "$d/build/packages"/*.dmg; do
    [ -f "$f" ] || continue
    cp "$f" "$INST/"
    inst+="${inst:+, }$(basename "$f") $(size_mb "$f")"
  done
  [ -n "$inst" ] || failed "$p" "$log"
  if [ "$KEEP" = 0 ]; then rm -rf "$d"; fi   # as soon as the installers are out
  echo "---- $p: $inst, $(( (SECONDS - start) / 60 )) min"
}

for a in "${ORDERED[@]}"; do onair_one "$a"; done
[ "$DRY" = 1 ] && exit 0
(cd "$INST" && for f in *.exe *.zip *.deb *.dmg; do if [ -f "$f" ]; then sha256 "$f"; fi; done > SHA256SUMS.new && mv SHA256SUMS.new SHA256SUMS)
printf '\nInstallers: %s\n' "$INST"; cat "$INST/SHA256SUMS"
