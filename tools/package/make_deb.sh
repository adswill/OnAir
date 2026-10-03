#!/bin/bash
# Build the Linux packages (.deb and .tar.gz) inside the Linux container (needs Docker, for example via Colima on macOS).
#   tools/package/make_deb.sh            native architecture (arm64 on an Apple-silicon Mac); packages land in build-packages/
#   PLATFORM=linux/amd64 tools/package/make_deb.sh   the x86-64 packages (built under emulation: slow, but the result is a normal x86-64 binary)
set -e
cd "$(dirname "$0")/../.."
PLATFORM=${PLATFORM:-}
IMAGE=onair-linux-dev; VOL=onair-build-pkg; PF=
if [ -n "$PLATFORM" ]; then IMAGE=onair-linux-dev-x86; VOL=onair-build-pkg-x86; PF="--platform $PLATFORM"; fi
if ! docker image inspect $IMAGE >/dev/null 2>&1; then
  if [ -n "$PLATFORM" ]; then
    # the legacy docker builder ignores --platform when the base image is cached for another architecture, so the x86 image is made by hand
    PKGS=$(tr '\n\\' '  ' < tools/dev/Dockerfile.linux | grep -o 'install -y [^&]*' | sed 's/install -y //')
    docker rm -f onair-x86-setup >/dev/null 2>&1 || true
    docker run --platform $PLATFORM --name onair-x86-setup -e DEBIAN_FRONTEND=noninteractive ubuntu:24.04 bash -c "apt-get update && apt-get install -y $PKGS"
    docker commit onair-x86-setup $IMAGE >/dev/null && docker rm onair-x86-setup >/dev/null
  else
    docker build -t $IMAGE -f tools/dev/Dockerfile.linux tools/dev
  fi
fi
mkdir -p build-packages
docker run --rm $PF -v "$PWD":/src:ro -v $VOL:/build -v "$PWD/build-packages":/out -w /build $IMAGE bash -c '
  cmake -S /src -B /build -DCMAKE_BUILD_TYPE=Release -DDECT2_BUILD_APP=ON -DDECT2_BUILD_TESTS=ON -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_SKIP_INSTALL_ALL_DEPENDENCY=ON >/dev/null &&
  cmake --build /build -j$(nproc) --target dect2 dect2cli dect2scan 2>&1 | grep -E "error|Error" -A3 | head -30
  cpack -G "DEB;TGZ" 2>&1 | tail -4
  cp /build/onair_*.deb /build/onair-*.tar.gz /out/ 2>/dev/null; ls -la /out'
