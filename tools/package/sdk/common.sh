# Shared by tools/package/sdk/*.sh: packaging OnAir with the clang cross SDK (onair_windows_sysroot, make_sdk.sh).
#   SDK       the SDK folder (default: ~/onair/onair_windows_sysroot/sdk)
#   VERSION   default: the version in CMakeLists.txt
#   JOBS      parallel jobs (default: all threads but two)
#   OUT       where the packages go (default: <repo>/packages)
#   ONAIR_BUILD  an existing build folder to package: the package target of CMake sets it. Without it, each script configures build-pkg-<platform>.
set -euo pipefail
REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
SDK=${SDK:-$HOME/onair/onair_windows_sysroot/sdk}
VERSION=${VERSION:-$(grep -m1 'project(' "$REPO/CMakeLists.txt" | sed -E 's/.*VERSION ([0-9.]+).*/\1/')}
JOBS=${JOBS:-$(( $(nproc) > 2 ? $(nproc) - 2 : 1 ))}
OUT=${OUT:-$REPO/packages}
mkdir -p "$OUT"
LIBS="libusb openssl glfw hackrf ffmpeg rtlsdr airspy soapysdr soapyremote libxml2 libiio bladerf limesuite hamlib airspyhf"
# configure_build <environment-setup triple> <build folder> [cmake arguments]: the programs the packages ship, not the tests
pkg_dir() { printf '%s' "${ONAIR_BUILD:-$REPO/$1}"; }
configure_build() {
  local triple=$1 dir=$2; shift 2
  [ -f "$SDK/environment-setup-$triple" ] || { echo "error: the SDK has no $triple" >&2; exit 1; }
  # shellcheck disable=SC1090
  . "$SDK/environment-setup-$triple"
  export CCACHE_DISABLE=1
  [ -n "${ONAIR_BUILD:-}" ] || (cd "$REPO" && cmake -S . -B "$dir" -G Ninja -DCMAKE_BUILD_TYPE=Release -DDECT2_BUILD_APP=ON -DDECT2_BUILD_TESTS=ON -DDECT2_CCACHE=OFF \
     -DDECT2_HAVE_HAMLIB=ON -DDECT2_WITH_TLS=ON -DOPENSSL_USE_STATIC_LIBS=TRUE -DONAIR_VERSION="$VERSION" -DCMAKE_SKIP_INSTALL_ALL_DEPENDENCY=ON "$@" >/dev/null)
  cmake --build "$dir" -j"$JOBS" --target dect2 dect2cli dect2scan onair-server
}
# copy_licenses <sysroot licences folder> <destination> <names...>
copy_licenses() {
  local from=$1 to=$2 n; shift 2
  for n in "$@"; do
    [ -d "$from/$n" ] || { echo "error: no licence folder $from/$n" >&2; exit 1; }
    mkdir -p "$to/$n"; cp -R "$from/$n/." "$to/$n/"
  done
}
