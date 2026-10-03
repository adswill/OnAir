#!/bin/bash
# Build the GUI in the Linux container and run it on a virtual display (software OpenGL), saving a screenshot.
#   tools/dev/linux-app.sh [dect2 arguments...]     e.g. tools/dev/linux-app.sh --autostart
# The screenshot lands in build-shots/linux.png (the --shot argument is added for you).
set -e
cd "$(dirname "$0")/../.."
IMAGE=onair-linux-dev
mkdir -p build-shots
docker run --rm -v "$PWD":/src:ro -v onair-build-app:/build -v "$PWD/build-shots":/out -w /build $IMAGE bash -c '
  cmake -S /src -B /build -DCMAKE_BUILD_TYPE=Release -DDECT2_BUILD_APP=ON -DDECT2_BUILD_TESTS=OFF >/dev/null &&
  cmake --build /build -j$(nproc) --target dect2 2>&1 | grep -E "error|Error" -A3 | head -40
  export LIBGL_ALWAYS_SOFTWARE=1 MESA_GL_VERSION_OVERRIDE=3.3 MESA_GLSL_VERSION_OVERRIDE=330
  xvfb-run -a -s "-screen 0 1600x1000x24" /build/dect2 "$@" --shot /out/linux.png 2>&1 | tail -5
' bash "$@"
