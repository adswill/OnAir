#!/usr/bin/env bash
# Verify a pushed branch on GitHub runners (workflow quick.yml / quick-full.yml), without building on this Mac.
#
#   tools/quick_ci.sh <branch> '<ctest -R regex>' [target ...] [--os macos-latest|ubuntu-24.04-arm] [--via <ref>] [--jobs N] [--timeout S]
#   tools/quick_ci.sh <branch> --full [--os ...] [--via <ref>]       whole project, build once + 8 test shards
#
# Pushes nothing: the branch must already be on GitHub. Dispatches the workflow, waits (one API call every 45 s, backs off on
# 403/429, gives up after --max-wait, default 90 min), prints the summary and exits 0 only when the run succeeded.
# --via <ref>: the branch the WORKFLOW FILE is taken from (default main). GitHub only dispatches a workflow that exists on the
# default branch, so this works once quick.yml is merged into main (a dispatch of a workflow only on a branch returns 404).
# The repository is the one of the git remote "pub" (override with QUICK_CI_REPO=owner/name). See docs/QUICK-CI.md.
set -u
BRANCH="${1:-}"; [ -z "$BRANCH" ] && { sed -n 2,11p "$0"; exit 2; }
shift
TESTS=""; TARGETS=(); OS="ubuntu-24.04"; VIA="main"; JOBS=""; TMO=""; FULL=0; MAXWAIT=5400; INTERVAL=45
while [ $# -gt 0 ]; do
    case "$1" in
        --os) OS="$2"; shift 2 ;;
        --via) VIA="$2"; shift 2 ;;
        --jobs) JOBS="$2"; shift 2 ;;
        --timeout) TMO="$2"; shift 2 ;;
        --max-wait) MAXWAIT="$2"; shift 2 ;;
        --full) FULL=1; shift ;;
        --*) echo "unknown option $1"; exit 2 ;;
        *) if [ -z "$TESTS" ] && [ $FULL -eq 0 ]; then TESTS="$1"; else TARGETS+=("$1"); fi; shift ;;
    esac
done
if [ $FULL -eq 0 ] && [ -z "$TESTS" ]; then echo "need a tests regex (or --full)"; exit 2; fi
if [ $FULL -eq 1 ]; then WF="quick-full.yml"; else WF="quick.yml"; fi

REPO="${QUICK_CI_REPO:-$(git remote get-url pub 2>/dev/null | sed -E 's#.*github.com[:/]##; s#\.git$##')}"
[ -z "$REPO" ] && REPO="adswill/OnAir"
export GH_REPO="$REPO"
START=$(date -u +%s)
ARGS=(-f "ref=$BRANCH" -f "os=$OS")
if [ $FULL -eq 0 ]; then
    ARGS+=(-f "tests=$TESTS")
    [ ${#TARGETS[@]} -gt 0 ] && ARGS+=(-f "targets=${TARGETS[*]}")
    [ -n "$JOBS" ] && ARGS+=(-f "jobs=$JOBS")
fi
[ -n "$TMO" ] && ARGS+=(-f "timeout=$TMO")
gh workflow run "$WF" --ref "$VIA" "${ARGS[@]}" || { echo "dispatch failed (is $WF on $VIA?)"; exit 2; }

# Find our run: several agents may dispatch at the same time, so match the run title (it holds the branch and the regex)
# and a creation time not before our dispatch.
RUN=""
for i in 1 2 3 4 5 6 7 8; do
    sleep 8
    RUN=$(gh run list --workflow "$WF" --event workflow_dispatch --limit 20 --json databaseId,createdAt,displayTitle \
        | python3 -c '
import json,sys,datetime
start=int(sys.argv[1]); br=sys.argv[2]; rx=sys.argv[3]
best=None
for r in json.load(sys.stdin):
    t=int(datetime.datetime.fromisoformat(r["createdAt"].replace("Z","+00:00")).timestamp())
    if t>=start-5 and (" "+br+" ") in (r["displayTitle"]+" ") and (not rx or rx in r["displayTitle"]):
        if best is None or r["databaseId"]>best: best=r["databaseId"]
print(best or "")' "$START" "$BRANCH" "$TESTS")
    [ -n "$RUN" ] && break
done
[ -z "$RUN" ] && { echo "could not find the dispatched run"; exit 2; }
echo "run $RUN: https://github.com/$(gh repo view --json nameWithOwner -q .nameWithOwner)/actions/runs/$RUN"

# Wait: one call per interval; on rate limiting, double the wait (up to 10 min).
WAITED=0; DELAY=$INTERVAL; CONCL=""
while [ $WAITED -lt "$MAXWAIT" ]; do
    sleep "$DELAY"; WAITED=$((WAITED + DELAY))
    OUT=$(gh run view "$RUN" --json status,conclusion 2>&1); RC=$?
    if [ $RC -ne 0 ]; then
        case "$OUT" in
            *403*|*429*|*"rate limit"*) DELAY=$((DELAY * 2)); [ $DELAY -gt 600 ] && DELAY=600; echo "rate limited, waiting ${DELAY}s"; continue ;;
            *) echo "gh error: $OUT"; continue ;;
        esac
    fi
    DELAY=$INTERVAL
    ST=$(printf '%s' "$OUT" | python3 -c 'import json,sys;d=json.load(sys.stdin);print(d["status"],d["conclusion"] or "-")')
    case "$ST" in completed*) CONCL="${ST#completed }"; break ;; esac
done
[ -z "$CONCL" ] && { echo "still running after ${MAXWAIT}s: gh run view $RUN"; exit 3; }

# Read the logs only now: the artifacts hold the summary inputs (small text files).
D=$(mktemp -d)
if [ $FULL -eq 1 ]; then
    gh run download "$RUN" -D "$D" -p 'build-logs' -p 'tests-*' >/dev/null 2>&1
    TIMES=""; [ -f "$D/build-logs/times" ] && TIMES="build=$(sed 's/build=//' "$D/build-logs/times")"
    python3 "$(dirname "$0")/quick_ci/summary.py" --title "Quick CI full: $BRANCH on $OS" --build-log "$D/build-logs/build.log" \
        --ccache "$D/build-logs/ccache.txt" --junit "$D"/tests-*/junit-*.xml --times $TIMES 2>&1
else
    gh run download "$RUN" -D "$D" -n quick-logs >/dev/null 2>&1
    TIMES=""; [ -f "$D/times" ] && TIMES=$(tr '\n' ' ' < "$D/times")
    python3 "$(dirname "$0")/quick_ci/summary.py" --title "Quick CI: $BRANCH on $OS (-R $TESTS)" --build-log "$D/build.log" \
        --junit "$D/junit.xml" --ccache "$D/ccache.txt" --times $TIMES 2>&1
fi
rm -rf "$D"
echo "conclusion: $CONCL"
[ "$CONCL" = success ]
