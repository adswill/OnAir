#!/bin/bash
# OnAir release: build every asset on GitHub / the CI repository, collect them, make a DRAFT release. Publishing needs --publish.
#
#   tools/release/release.sh 3.0.0 [options]
#
#   --ref BRANCH        branch to build (default: the current branch; it must be pushed to the `pub` remote and equal HEAD)
#   --apk FILE          a signed APK you built yourself (android/README.md "Signing"); without it the APK of the CI is used and
#                       the script stops if that one is unsigned (a release APK must carry the project's signing key)
#   --dist DIR          where the files are collected (default: ./release-dist-<version>); files already there are reused
#   --reuse-runs        do not start the builds; use the newest finished successful runs of the branch (release.yml, android.yml, package.yml)
#   --skip-ci-check     do not require green CI runs on the commit
#   --no-draft          stop after collecting the assets (dry run: no GitHub release object is created)
#   --publish           after the draft is complete and verified, make it public (GitHub creates the tag v<version> on the commit then)
#
# What it never does: push a branch or a tag, change main, delete anything, publish without --publish.
# Builds: release.yml (macOS arm64 + x86_64, Linux x86_64 + arm64) and android.yml in adswill/OnAir, package.yml (source tarball, Windows
# zip + installer) in adswill/OnAir-ci. All started with workflow_dispatch on the branch: no tag is needed.
set -eu
V=${1:?usage: release.sh <x.y.z> [--ref BRANCH] [--apk FILE] [--dist DIR] [--reuse-runs] [--skip-ci-check] [--no-draft] [--publish]}; shift
REPO=adswill/OnAir; CI=adswill/OnAir-ci; TAG=v$V
REF=""; APK=""; DIST=""; REUSE=0; SKIPCI=0; DRAFT=1; PUBLISH=0
while [ $# -gt 0 ]; do
  case $1 in
    --ref) REF=$2; shift;; --apk) APK=$2; shift;; --dist) DIST=$2; shift;;
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

# newest run id of a workflow on the branch started after $1 (epoch seconds)
find_run() { # repo workflow after-epoch
  for _ in $(seq 1 30); do
    id=$(gh run list --repo "$1" --workflow "$2" --branch "$REF" --event workflow_dispatch --limit 10 --json databaseId,createdAt \
      --jq "[.[]|select((.createdAt|fromdateiso8601) >= $3)]|sort_by(.databaseId)|last|.databaseId // empty")
    [ -n "$id" ] && { echo "$id"; return; }; sleep 6
  done; die "the $2 run did not appear"
}
latest_ok() { # repo workflow
  gh run list --repo "$1" --workflow "$2" --branch "$REF" --limit 20 --json databaseId,conclusion,headSha \
    --jq "[.[]|select(.conclusion==\"success\" and (\"$1\"==\"$CI\" or .headSha==\"$SHA\"))]|first|.databaseId // empty"
}
wait_run() { # repo id label
  local delay=60
  while :; do
    r=$(gh run view "$2" --repo "$1" --json status,conclusion --jq '"\(.status) \(.conclusion)"' 2>&1) || r="error"
    case $r in completed*) echo "$3: ${r#completed }"; [ "${r#completed }" = success ] || die "$3 failed: https://github.com/$1/actions/runs/$2"; return;; esac
    sleep $delay; [ $delay -lt 300 ] && delay=$((delay+30))
  done
}

mkdir -p "$DIST"; DL=$DIST/.runs; mkdir -p "$DL"
say "2. Build every asset"
T0=$(date +%s)
if [ $REUSE = 1 ]; then
  RID=$(latest_ok $REPO release.yml); AID=$(latest_ok $REPO android.yml); PID=$(latest_ok $CI package.yml)
  [ -n "$RID" ] && [ -n "$AID" ] && [ -n "$PID" ] || die "--reuse-runs: no successful run of release.yml, android.yml and package.yml for $SHA"
else
  gh workflow run release.yml --repo $REPO --ref "$REF"
  gh workflow run android.yml --repo $REPO --ref "$REF" -f native=true
  gh workflow run package.yml --repo $CI -f ref="$REF" -f version="$V" -f tag="rel-$V-$T0"
  RID=$(find_run $REPO release.yml $T0); AID=$(find_run $REPO android.yml $T0)
  PID=""; for _ in $(seq 1 30); do PID=$(gh run list --repo $CI --workflow package.yml --limit 10 --json databaseId,displayTitle --jq "[.[]|select(.displayTitle|contains(\"[rel-$V-$T0]\"))]|first|.databaseId // empty"); [ -n "$PID" ] && break; sleep 6; done
  [ -n "$PID" ] || die "the package.yml run did not appear"
  echo "release.yml $RID, android.yml $AID, package.yml $PID"
  wait_run $REPO "$RID" "release.yml (macOS, Linux)"; wait_run $REPO "$AID" "android.yml (APK)"; wait_run $CI "$PID" "package.yml (source, Windows)"
fi
# release.yml: its Windows job is off (needs the maintainer's dependency prefix), so "Windows x64 package" is skipped, not failed
gh run download "$RID" --repo $REPO --dir "$DL/release"
gh run download "$AID" --repo $REPO --dir "$DL/android"
gh run download "$PID" --repo $CI --dir "$DL/package"

say "3. Collect"
find "$DL" -type f \( -name '*.dmg' -o -name '*.deb' -o -name '*-portable.tar.gz' -o -name '*.AppImage' -o -name '*-windows-x64.zip' -o -name '*-windows-x64-setup.exe' -o -name '*-source.tar.gz' \) -exec cp {} "$DIST/" \;
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
n=$(gh release view "$TAG" --repo $REPO --json assets --jq '.assets|length')
[ "$n" = 10 ] || die "the draft has $n assets, expected 10 (nine files + SHA256SUMS)"
echo "draft complete: $(gh release view "$TAG" --repo $REPO --json url --jq .url)"

if [ $PUBLISH = 1 ]; then
  say "5. Publish"
  gh release edit "$TAG" --repo $REPO --draft=false --latest
  echo "published. GitHub created the tag $TAG on $SHA; release.yml starts for it and skips the rebuild (it only pushes the Docker image)."
  echo "next: website (docs/RELEASE-3.0-CHECKLIST.md), merge $REF into main if not done, announcements."
else
  echo "not published (no --publish). Review the draft, then re-run with --reuse-runs --publish, or publish it on the release page."
fi
