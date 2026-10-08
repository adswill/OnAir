#!/usr/bin/env bash
# The CI checks, run on this Mac instead of GitHub:
#   macOS    build (app, tools, tests) and run the tests
#   Linux    the same in the onair-linux-dev Docker image (arm64, Colima)
#   Windows  cross-build of the app, tools and tests with MinGW (no Wine here, so the Windows tests do not run)
# The three run at the same time (the Colima VM has 6 of the 10 cores, so one after the other left the Mac half idle), with progress
# bars while they run (tools/ci_progress.sh draws them; it also works on its own in another window).
#
#   tools/ci_local.sh                 the fast tests (the "slow" label is skipped)
#   tools/ci_local.sh --full          every test
#   tools/ci_local.sh mac linux       only the named platforms (mac, linux, windows)
#   tools/ci_local.sh --no-bars       plain output (for logs)
#
# Logs go to build/ci-logs/. The exit status is 0 only when every step passed.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LOGS="$ROOT/build/ci-logs"
LABEL=(-LE slow)
PLATFORMS=()
BARS=1
for a in "$@"; do
    case "$a" in
        --full) LABEL=() ;;
        --no-bars) BARS=0 ;;
        mac|linux|windows) PLATFORMS+=("$a") ;;
        *) echo "unknown argument: $a (use --full, --no-bars, mac, linux, windows)"; exit 2 ;;
    esac
done
[ ${#PLATFORMS[@]} -eq 0 ] && PLATFORMS=(mac linux windows)
mkdir -p "$LOGS"
rm -f "$LOGS"/*.result
touch "$LOGS/.start"   # ci_progress.sh ignores logs older than this

# Cores: the Mac tests and the two builds share the Mac's 10, the Linux stage has the VM's 6. CI=true everywhere: with everything
# running at once the machines are busy, so the speed and real-time checks only print instead of failing (as on GitHub).
MAC_JOBS=6; WIN_JOBS=4; LINUX_JOBS=6

step() {   # step <name> <log> <command...>: runs it, writes "<ok|FAIL> <seconds> <name>" to <log>.result
    local name=$1 log=$2; shift 2
    local t0=$SECONDS st=FAIL
    if "$@" > "$log" 2>&1; then st=ok; fi
    echo "$st $((SECONDS - t0)) $name" > "$log.result"
    [ $st = ok ]
}

mac() {
    step "macOS build" "$LOGS/mac-build.log" \
        bash -c "cmake -S '$ROOT' -B '$ROOT/build' -DCMAKE_BUILD_TYPE=Release -DDECT2_BUILD_APP=ON && cmake --build '$ROOT/build' -j $MAC_JOBS" &&
    step "macOS tests" "$LOGS/mac-test.log" \
        env CI=true ctest --test-dir "$ROOT/build" -j $MAC_JOBS ${LABEL[@]+"${LABEL[@]}"} --output-on-failure --timeout 1500
}
linux() {
    if ! docker info > /dev/null 2>&1; then colima start > "$LOGS/colima.log" 2>&1; fi
    # sources read-only, the build in a Docker volume so that it is incremental; updater is left out: the image has no curl and the
    # container no network
    step "Linux arm64 build" "$LOGS/linux-build.log" \
        docker run --rm -v "$ROOT":/src:ro -v onair-build-ci:/b onair-linux-dev bash -c \
        "cmake -S /src -B /b -DCMAKE_BUILD_TYPE=Release -DDECT2_BUILD_APP=OFF && cmake --build /b -j $LINUX_JOBS" &&
    step "Linux arm64 tests" "$LOGS/linux-test.log" \
        docker run --rm -e CI=true -v "$ROOT":/src:ro -v onair-build-ci:/b onair-linux-dev \
        ctest --test-dir /b -j $LINUX_JOBS ${LABEL[@]+"${LABEL[@]}"} -E '^updater$' --output-on-failure --timeout 1500
}
windows() {
    step "Windows build (MinGW)" "$LOGS/windows-build.log" \
        bash -c "cmake -S '$ROOT' -B '$ROOT/build-windows' -DCMAKE_TOOLCHAIN_FILE='$ROOT/packaging/windows/mingw-toolchain.cmake' -DCMAKE_BUILD_TYPE=Release -DDECT2_BUILD_APP=ON && cmake --build '$ROOT/build-windows' -j $WIN_JOBS"
}

T0=$SECONDS
PIDS=()
for p in "${PLATFORMS[@]}"; do "$p" & PIDS+=($!); done
trap 'kill "${PIDS[@]}" 2>/dev/null; exit 130' INT TERM

running() { for pid in "${PIDS[@]}"; do kill -0 "$pid" 2>/dev/null && return 0; done; return 1; }
if [ $BARS = 1 ] && [ -t 1 ]; then
    clear
    while running; do
        printf '\033[H'; echo "OnAir local CI  $(date +%H:%M:%S)  ($(( (SECONDS - T0) / 60 )) min $(( (SECONDS - T0) % 60 )) s)"
        "$ROOT/tools/ci_progress.sh" --once
        sleep 2
    done
    printf '\033[H'; echo "OnAir local CI  done in $(( (SECONDS - T0) / 60 )) min $(( (SECONDS - T0) % 60 )) s"
    "$ROOT/tools/ci_progress.sh" --once
fi
wait

echo
fail=0
for r in "$LOGS"/*.result; do
    [ -f "$r" ] || continue
    read -r st secs name < "$r"
    printf '%-24s %-6s %5ds\n' "$name" "$st" "$secs"
    if [ "$st" != ok ]; then
        fail=1
        grep -E '\*\*\*Failed|\*\*\*Timeout|^FAIL|error:' "${r%.result}" | head -15 | sed 's/^/    /'
    fi
done
echo "total $(( (SECONDS - T0) / 60 )) min $(( (SECONDS - T0) % 60 )) s"
exit $fail
