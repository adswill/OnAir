#!/bin/bash
# Build every installer on this Mac and collect them in release/ with checksums:
#   OnAir-<v>-macos-arm64.dmg                         macOS (Apple silicon)
#   onair_<v>_arm64.deb, onair_<v>_amd64.deb (+ .tar.gz)   Linux
#   OnAir-<v>-windows-x64-setup.exe, ...-windows-x64.zip   Windows
# Needs Docker (Colima) for Linux, mingw-w64 + the Windows dependencies (tools/package/cross_deps_windows.sh) for Windows.
#
#   tools/package/make_release.sh                 build everything
#   tools/package/make_release.sh --skip-linux-x86   leave out the slow x86-64 Linux build
#   tools/package/make_release.sh --publish v0.1.0   also create the GitHub release and upload the files (gh must be logged in)
set -e
cd "$(dirname "$0")/../.."
VERSION=$(grep -m1 'project(' CMakeLists.txt | sed -E 's/.*VERSION ([0-9.]+).*/\1/')
SKIP_X86=0; TAG=""
while [ $# -gt 0 ]; do
  case "$1" in --skip-linux-x86) SKIP_X86=1;; --publish) TAG=$2; shift;; esac
  shift
done
OUT=release
mkdir -p $OUT   # files are overwritten, nothing is deleted

echo "== macOS"
tools/package/make_dmg.sh "$VERSION" >/dev/null
cp build-release/OnAir-$VERSION-macos-*.dmg $OUT/

echo "== Linux arm64"
tools/package/make_deb.sh >/dev/null
cp build-packages/onair_*_arm64.deb $OUT/ ; for f in build-packages/onair-*.tar.gz; do cp "$f" "$OUT/$(basename "$f" .tar.gz)-arm64.tar.gz"; done
if [ $SKIP_X86 = 0 ]; then
  echo "== Linux x86-64 (emulated, slow)"
  PLATFORM=linux/amd64 tools/package/make_deb.sh >/dev/null
  cp build-packages/onair_*_amd64.deb $OUT/ ; for f in build-packages/onair-*.tar.gz; do cp "$f" "$OUT/$(basename "$f" .tar.gz)-x86_64.tar.gz"; done
fi

echo "== Windows x64"
tools/package/make_windows_cross.sh "$VERSION" >/dev/null
cp build-windows/OnAir-$VERSION-windows-x64-setup.exe build-windows/OnAir-$VERSION-windows-x64.zip $OUT/

(cd $OUT && shasum -a 256 * > SHA256SUMS.txt)
echo; ls -la $OUT

if [ -n "$TAG" ]; then
  echo "== publishing $TAG"
  gh release create "$TAG" $OUT/* --title "OnAir $TAG" --generate-notes
fi
