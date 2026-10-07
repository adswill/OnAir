#!/usr/bin/env bash
# The CI checks, run on this Mac instead of GitHub:
#   macOS    build (app, tools, tests) and run the tests
#   Linux    the same in the onair-linux-dev Docker image (arm64, Colima)
#   Windows  cross-build of the app, tools and tests with MinGW (no Wine here, so the Windows tests do not run)
#
#   tools/ci_local.sh                 the fast tests (the "slow" label is skipped)
#   tools/ci_local.sh --full          every test
#   tools/ci_local.sh mac linux       only the named platforms (mac, linux, windows)
#
# Logs go to build/ci-logs/. The exit status is 0 only when every step passed.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LOGS="$ROOT/build/ci-logs"
JOBS=$(sysctl -n hw.ncpu 2>/dev/null || echo 8)
LABEL=(-LE slow)
PLATFORMS=()
for a in "$@"; do
    case "$a" in
        --full) LABEL=() ;;
        mac|linux|windows) PLATFORMS+=("$a") ;;
        *) echo "unknown argument: $a (use --full, mac, linux, windows)"; exit 2 ;;
    esac
done
[ ${#PLATFORMS[@]} -eq 0 ] && PLATFORMS=(mac linux windows)
mkdir -p "$LOGS"

RESULTS=()
step() {   # step <name> <log> <command...>
    local name=$1 log=$2; shift 2
    local t0=$SECONDS
    printf '%-28s ' "$name"
    if "$@" > "$log" 2>&1; then
        printf 'ok      %4ds\n' $((SECONDS - t0)); RESULTS+=("ok    $name")
    else
        printf 'FAILED  %4ds  (%s)\n' $((SECONDS - t0)) "$log"; RESULTS+=("FAIL  $name")
        grep -E '\*\*\*Failed|\*\*\*Timeout|^FAIL|error:' "$log" | head -15 | sed 's/^/    /'
    fi
}

for p in "${PLATFORMS[@]}"; do
    case "$p" in
    mac)
        step "macOS build" "$LOGS/mac-build.log" \
            bash -c "cmake -S '$ROOT' -B '$ROOT/build' -DCMAKE_BUILD_TYPE=Release -DDECT2_BUILD_APP=ON && cmake --build '$ROOT/build' -j $JOBS"
        # 4 at a time: the real-time tests need the cores
        step "macOS tests" "$LOGS/mac-test.log" \
            ctest --test-dir "$ROOT/build" -j 4 ${LABEL[@]+"${LABEL[@]}"} --output-on-failure --timeout 1500
        ;;
    linux)
        if ! docker info > /dev/null 2>&1; then colima start > "$LOGS/colima.log" 2>&1; fi
        # sources read-only, the build in a Docker volume so that it is incremental
        step "Linux arm64 build" "$LOGS/linux-build.log" \
            docker run --rm -v "$ROOT":/src:ro -v onair-build-ci:/b onair-linux-dev bash -c \
            "cmake -S /src -B /b -DCMAKE_BUILD_TYPE=Release -DDECT2_BUILD_APP=OFF && cmake --build /b -j $JOBS"
        # CI=true: the Colima VM is slower than the Mac, so the speed checks only print (as on GitHub);
        # updater is left out: the image has no curl and the container no network
        step "Linux arm64 tests" "$LOGS/linux-test.log" \
            docker run --rm -e CI=true -v "$ROOT":/src:ro -v onair-build-ci:/b onair-linux-dev \
            ctest --test-dir /b -j 4 ${LABEL[@]+"${LABEL[@]}"} -E '^updater$' --output-on-failure --timeout 1500
        ;;
    windows)
        step "Windows build (MinGW)" "$LOGS/windows-build.log" \
            bash -c "cmake -S '$ROOT' -B '$ROOT/build-windows' -DCMAKE_TOOLCHAIN_FILE='$ROOT/packaging/windows/mingw-toolchain.cmake' -DCMAKE_BUILD_TYPE=Release -DDECT2_BUILD_APP=ON && cmake --build '$ROOT/build-windows' -j $JOBS"
        ;;
    esac
done

echo
printf '%s\n' "${RESULTS[@]}"
for r in "${RESULTS[@]}"; do [[ $r == FAIL* ]] && exit 1; done
exit 0
