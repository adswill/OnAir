#!/bin/bash
# Every package from the clang SDK, one after the other, then SHA256SUMS. A failed package does not stop the others; the exit code says so.
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/common.sh"
set +e
rc=0
run() {
  local name=$1 start=$SECONDS; shift
  ( "$@" ) > "$OUT/$name.log" 2>&1
  local r=$?
  echo "$name: rc=$r $((SECONDS - start)) s"; [ $r = 0 ] || { rc=1; tail -15 "$OUT/$name.log"; }
}
run windows bash "$HERE/windows.sh"
run deb-amd64 bash "$HERE/deb.sh" amd64
run deb-arm64 bash "$HERE/deb.sh" arm64
run dmg-arm64 bash "$HERE/dmg.sh" arm64
run dmg-x86_64 bash "$HERE/dmg.sh" x86_64
(cd "$OUT" && rm -f SHA256SUMS && sha256sum -- *.exe *.zip *.deb *.dmg > SHA256SUMS 2>/dev/null; cat SHA256SUMS; ls -la)
exit $rc
