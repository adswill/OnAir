#!/bin/bash
# Build and test the project on Linux inside a container (needs Docker, for example via Colima on macOS).
#   tools/dev/linux-test.sh                 build, then run the quick tests
#   tools/dev/linux-test.sh all             build, then run every test
#   tools/dev/linux-test.sh shell           open a shell in the container
# The build tree lives in a Docker volume, so it is fast after the first run and does not touch the macOS build directory.
set -e
cd "$(dirname "$0")/../.."
# PLATFORM=linux/amd64 runs the x86-64 build under emulation (checks correctness of the SSE/AVX paths; the timing means nothing).
PLATFORM=${PLATFORM:-}
IMAGE=onair-linux-dev
VOL=onair-build
PF=
if [ -n "$PLATFORM" ]; then IMAGE=onair-linux-dev-x86; VOL=onair-build-x86; PF="--platform $PLATFORM"; fi
if ! docker image inspect $IMAGE >/dev/null 2>&1; then
  docker build ${PLATFORM:+--platform $PLATFORM} -t $IMAGE -f tools/dev/Dockerfile.linux tools/dev
fi
MODE=${1:-quick}
run() { docker run --rm $PF -v "$PWD":/src:ro -v $VOL:/build -w /build $IMAGE "$@"; }
if [ "$MODE" = shell ]; then exec docker run --rm -it $PF -v "$PWD":/src:ro -v $VOL:/build -w /build $IMAGE bash; fi
CMAKE_EXTRA=${CMAKE_EXTRA:-}
run bash -c "cmake -S /src -B /build -DCMAKE_BUILD_TYPE=Release -DDECT2_BUILD_APP=OFF $CMAKE_EXTRA >/dev/null && cmake --build /build -j\$(nproc) 2>&1 | grep -E 'error|warning: unused|Error' -A3 | head -60; echo BUILD_FINISHED"
if [ "$MODE" = all ]; then
  run ctest --test-dir /build --output-on-failure --timeout 1500
else
  run ctest --test-dir /build --output-on-failure --timeout 600 -R 'sync|l1|pilots|fec|ts|teletext|dsp|dvbt$|bandwidth|direction|atsc_fec|soapy|conceal'
fi
