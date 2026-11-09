#!/bin/bash
# OnAir release, one command: build every asset on GitHub, collect them, make a DRAFT release; --publish makes it public, and only after every asset is on the draft.
#
#   tools/release/release.sh 3.0.0 [options]
#
#   --ref BRANCH        branch to build (default: the current branch; it must be pushed to the `pub` remote and equal HEAD)
#   --apk FILE          a signed APK you built yourself (android/README.md "Signing"); without it the APK of the CI is used and
#                       the script stops if that one is unsigned (a release APK must carry the project's signing key)
#   --windows DIR       the Windows zip and installer you built yourself (tools/package/make_windows_cross.sh); without it the files of release.yml
#                       are used, which needs the prefix of tools/release/upload_win_prefix.sh on the draft release ci-windows-prefix
#   --dist DIR          where the files are collected (default: ./release-dist-<version>); files already there are reused
#   --reuse-runs        do not start the builds; use the newest finished successful runs of the commit (release.yml per platform, android.yml)
#   --skip-ci-check     do not require green CI runs on the commit
#   --no-draft          stop after collecting the assets (dry run: no GitHub release object is created)
#   --publish           after the draft is complete and verified, make it public (GitHub creates the tag v<version> on the commit then)
#
# What it never does: push a branch or a tag, change main, delete anything, publish without --publish.
# Builds, all with workflow_dispatch on the branch (no tag needed), each platform as a run of its own so a failed one can be run again while the
# others go on (GitHub only reruns failed jobs once their run has ended): release.yml with platform=windows, macos, linux, and android.yml.
# A run that fails is run again automatically once with `gh run rerun --failed` (a network error; the steps retry too); ONAIR_RELEASE_RERUNS=0 turns that off.
set -eu
V=${1:?usage: release.sh <x.y.z> [--ref BRANCH] [--apk FILE] [--windows DIR] [--dist DIR] [--reuse-runs] [--skip-ci-check] [--no-draft] [--publish]}; shift
REPO=adswill/OnAir; TAG=v$V
REF=""; APK=""; WINDIR=""; DIST=""; REUSE=0; SKIPCI=0; DRAFT=1; PUBLISH=0
while [ $# -gt 0 ]; do
  case $1 in
    --ref) REF=$2; shift;; --apk) APK=$2; shift;; --windows) WINDIR=$2; shift;; --dist) DIST=$2; shift;;
    --reuse-runs) REUSE=1;; --skip-ci-check) SKIPCI=1;; --no-draft) DRAFT=0;; --publish) PUBLISH=1;;
    *) echo "unknown option $1"; exit 2;;
  esac; shift
done
case $V in [0-9]*.[0-9]*.[0-9]*) ;; *) echo "version must look like 3.0.0"; exit 2;; esac
cd "$(dirname "$0")/../.."
command -v gh >/dev/null || { echo "gh is not installed"; exit 2; }
[ -n "$REF" ] || REF=$(git rev-parse --abbrev-ref HEAD)
[ -n "$DIST" ] || DIST=$PWD/release-dist-$V
say() { printf '\n== %s\n' "$*"; }
die() { echo "STOP: $*" >&2; exit 1; }

say "1. Preflight"
[ -z "$(git status --porcelain --untracked-files=no)" ] || die "the working tree has uncommitted changes"
SHA=$(git rev-parse HEAD)
REMOTE_SHA=$(git ls-remote pub "refs/heads/$REF" | cut -f1)
[ "$REMOTE_SHA" = "$SHA" ] || die "pub/$REF is ${REMOTE_SHA:-missing}, HEAD is $SHA: push the branch first (this script never pushes)"
echo "commit $SHA on pub/$REF"
bash tools/release/check_versions.sh "$V" --final || die "version / credits / changelog checks failed (above)"
tools/gate/run.sh --no-compile || die "the quick gate failed (above)"
if [ $SKIPCI = 0 ]; then
  # the quick CI (ci.yml) must be green on exactly this commit
  c=$(gh run list --repo $REPO --workflow ci.yml --commit "$SHA" --json conclusion,status --jq '[.[]|select(.status=="completed")|.conclusion]|join(",")')
  case ",$c," in *,success,*) echo "ci.yml green on $SHA";; *) die "no successful ci.yml run on $SHA (found: '${c:-none}'). Run it (gh workflow run ci.yml --repo $REPO --ref $REF), or the laptop worker with the full test list, then use --skip-ci-check if you accept that";; esac
fi
if gh release view "$TAG" --repo $REPO >/dev/null 2>&1; then
  d=$(gh release view "$TAG" --repo $REPO --json isDraft --jq .isDraft)
  [ "$d" = true ] || die "release $TAG already exists and is public"
  echo "a draft $TAG exists: its files will be replaced (--clobber)"
fi

# newest run of a workflow on the branch started after <epoch>, with this run name (release.yml run-name: "Release <platform>"; "" = any)
find_run() { # workflow after-epoch [run-name]
  local q
  q="[.[]|select((.createdAt|fromdateiso8601) >= $2)|select(\"${3:-}\" == \"\" or .displayTitle == \"${3:-}\")]|sort_by(.databaseId)|last|.databaseId // empty"
  for _ in $(seq 1 30); do
    id=$(gh run list --repo $REPO --workflow "$1" --branch "$REF" --event workflow_dispatch --limit 20 --json databaseId,createdAt,displayTitle --jq "$q")
    [ -n "$id" ] && { echo "$id"; return; }; sleep 6
  done; die "the $1 run ${3:-} did not appear"
}
# newest finished successful run on this commit (name: run name or ""); a "Release all" run has every platform
latest_ok() { # workflow name
  gh run list --repo $REPO --workflow "$1" --branch "$REF" --limit 30 --json databaseId,conclusion,headSha,displayTitle \
    --jq "[.[]|select(.conclusion==\"success\" and .headSha==\"$SHA\" and (\"$2\"==\"\" or .displayTitle==\"$2\" or .displayTitle==\"Release all\"))]|first|.databaseId // empty"
}
wait_run() { # id label
  local delay=60 reruns=${ONAIR_RELEASE_RERUNS:-1} r c
  while :; do
    r=$(gh run view "$1" --repo $REPO --json status,conclusion --jq '"\(.status) \(.conclusion)"' 2>&1) || r="error"
    case $r in
      completed*)
        c=${r#completed }; echo "$2: $c"
        [ "$c" = success ] && return 0
        if [ "$reruns" -gt 0 ] && [ "$c" != cancelled ]; then
          reruns=$((reruns-1)); echo "$2: running the failed jobs again: https://github.com/$REPO/actions/runs/$1"
          gh run rerun "$1" --repo $REPO --failed || true
          delay=60; sleep 60; continue
        fi
        echo "STOP: $2 failed: https://github.com/$REPO/actions/runs/$1" >&2; return 1;;
    esac
    sleep $delay; [ $delay -lt 300 ] && delay=$((delay+30))
  done
}

mkdir -p "$DIST"
say "2. Build every asset"
T0=$(date +%s)
DL=$DIST/.runs-$T0; mkdir -p "$DL"   # a new folder per attempt: gh run download refuses to overwrite files
if [ $REUSE = 1 ]; then
  WID=$(latest_ok release.yml "Release windows"); MID=$(latest_ok release.yml "Release macos"); LID=$(latest_ok release.yml "Release linux"); AID=$(latest_ok android.yml "")
  [ -n "$WID" ] && [ -n "$MID" ] && [ -n "$LID" ] && [ -n "$AID" ] || die "--reuse-runs: no successful run on $SHA of every platform of release.yml and android.yml (windows '$WID', macos '$MID', linux '$LID', android '$AID')"
else
  gh workflow run release.yml --repo $REPO --ref "$REF" -f platform=windows
  gh workflow run release.yml --repo $REPO --ref "$REF" -f platform=macos
  gh workflow run release.yml --repo $REPO --ref "$REF" -f platform=linux
  gh workflow run android.yml --repo $REPO --ref "$REF" -f native=true
  WID=$(find_run release.yml $T0 "Release windows"); MID=$(find_run release.yml $T0 "Release macos"); LID=$(find_run release.yml $T0 "Release linux"); AID=$(find_run android.yml $T0)
  echo "runs: windows $WID, macos $MID, linux $LID, android $AID"
  wait_run "$WID" "Windows" & p1=$!
  wait_run "$MID" "macOS" & p2=$!
  wait_run "$LID" "Linux" & p3=$!
  wait_run "$AID" "Android" & p4=$!
  bad=0; for p in $p1 $p2 $p3 $p4; do wait "$p" || bad=1; done
  [ $bad = 0 ] || die "a build failed (above); fix it, push, and start again, or run the failed jobs by hand and use --reuse-runs"
fi
for pair in "windows:$WID" "macos:$MID" "linux:$LID" "android:$AID"; do
  n=0; until gh run download "${pair#*:}" --repo $REPO --dir "$DL/${pair%%:*}-$n"; do n=$((n+1)); [ $n -lt 4 ] || die "could not download the artifacts of run ${pair#*:}"; sleep 20; done
done

say "3. Collect"
find "$DL" -type f \( -name '*.dmg' -o -name '*.deb' -o -name '*-portable.tar.gz' -o -name '*.AppImage' -o -name '*-windows-x64.zip' -o -name '*-windows-x64-setup.exe' \) -exec cp {} "$DIST/" \;
if [ -n "$WINDIR" ]; then cp "$WINDIR"/OnAir-"$V"-windows-x64.zip "$WINDIR"/OnAir-"$V"-windows-x64-setup.exe "$DIST/"; fi
if [ -n "$APK" ]; then cp "$APK" "$DIST/OnAir-$V-android.apk"
else
  f=$(find "$DL/android" -name '*.apk' | head -1)
  case "$f" in
    "") die "no APK in the android.yml run";;
    *unsigned*) die "the CI built an UNSIGNED APK (no signing secrets in $REPO). Build the signed one on your Mac (docs/RELEASE-3.0-CHECKLIST.md, step 'Android') and pass --apk FILE";;
    *) cp "$f" "$DIST/OnAir-$V-android.apk";;
  esac
fi
want="OnAir-$V-macos-arm64.dmg OnAir-$V-macos-x86_64.dmg onair_${V}_amd64.deb onair_${V}_arm64.deb onair-$V-linux-x86_64-portable.tar.gz onair-$V-linux-aarch64-portable.tar.gz OnAir-$V-linux-x86_64.AppImage OnAir-$V-linux-aarch64.AppImage OnAir-$V-windows-x64.zip OnAir-$V-windows-x64-setup.exe OnAir-$V-android.apk"
miss=""; for f in $want; do [ -f "$DIST/$f" ] || miss="$miss $f"; done
case "$miss" in *windows-x64*) echo "note: no Windows files came from release.yml. Is onair-win-prefix-$(tools/package/win_prefix_id.sh).tar.gz on the draft release ci-windows-prefix (tools/release/upload_win_prefix.sh)? Or pass --windows DIR." >&2;; esac
[ -z "$miss" ] || die "missing assets:$miss (in $DIST)"
(cd "$DIST" && shasum -a 256 $want > SHA256SUMS && ls -l $want SHA256SUMS | awk '{printf "%12d  %s\n",$5,$9}' | tee SIZES.txt)
# notes: the changelog, without the "draft" title
awk 'NR==1{print "# OnAir '"$V"'"; next} {print}' docs/CHANGELOG-"${V%.*}".md > "$DIST/NOTES.md"
echo "notes: $DIST/NOTES.md ($(wc -l < "$DIST/NOTES.md") lines)"

[ $DRAFT = 1 ] || { say "Stopped before the release (--no-draft). Files: $DIST"; exit 0; }
say "4. Draft release $TAG (not public, no tag yet)"
if gh release view "$TAG" --repo $REPO >/dev/null 2>&1; then
  gh release upload "$TAG" "$DIST"/*.dmg "$DIST"/*.deb "$DIST"/*-portable.tar.gz "$DIST"/*.AppImage "$DIST"/*.zip "$DIST"/*-setup.exe "$DIST"/*.apk "$DIST/SHA256SUMS" --repo $REPO --clobber
  gh release edit "$TAG" --repo $REPO --notes-file "$DIST/NOTES.md" --title "OnAir $V"
else
  gh release create "$TAG" "$DIST"/*.dmg "$DIST"/*.deb "$DIST"/*-portable.tar.gz "$DIST"/*.AppImage "$DIST"/*.zip "$DIST"/*-setup.exe "$DIST"/*.apk "$DIST/SHA256SUMS" \
    --repo $REPO --draft --target "$SHA" --title "OnAir $V" --notes-file "$DIST/NOTES.md"
fi
# every asset is on the draft, with the size collected here, before anything can be published
for f in $want SHA256SUMS; do
  sz=$(gh release view "$TAG" --repo $REPO --json assets --jq ".assets[]|select(.name==\"$f\")|.size"); [ "$sz" = "$(wc -c < "$DIST/$f" | tr -d ' ')" ] || die "$f on the draft has size '${sz:-missing}', collected file is $(wc -c < "$DIST/$f") bytes"
done
n=$(gh release view "$TAG" --repo $REPO --json assets --jq '.assets|length'); w=$(( $(echo $want | wc -w) + 1 ))
[ "$n" = "$w" ] || die "the draft has $n assets, expected $w (the files above and SHA256SUMS)"
echo "draft complete: $(gh release view "$TAG" --repo $REPO --json url --jq .url)"

if [ $PUBLISH = 1 ]; then
  say "5. Publish"
  gh release edit "$TAG" --repo $REPO --draft=false --latest
  echo "published. GitHub created the tag $TAG on $SHA; release.yml starts for it and skips the rebuild (it only pushes the Docker image)."
  echo "next: website (docs/RELEASE-3.0-CHECKLIST.md), merge $REF into main if not done, announcements."
else
  echo "not published (no --publish). Review the draft, then re-run with --reuse-runs --publish, or publish it on the release page."
fi
