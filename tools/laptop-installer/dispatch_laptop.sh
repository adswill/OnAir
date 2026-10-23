#!/bin/zsh
# Mac side of the laptop worker (see docs/LAPTOP-WORKER.md).
#   dispatch_laptop.sh status
#   dispatch_laptop.sh <branch> '<ctest -R regex>' [target ...]
# Starts build-test.yml in adswill/OnAir-ci, waits for it (polls every 45 s, backs off on 403/429), prints the
# summary and returns 0 when the run succeeded. Environment: LAPTOP_MAX_WAIT (seconds, default 7200), LAPTOP_JOBS (default 16),
# LAPTOP_CMAKE_ARGS (extra cmake arguments).
set -u
REPO="adswill/OnAir-ci"
MAX_WAIT="${LAPTOP_MAX_WAIT:-7200}"
POLL=45

die() { print -u2 -- "dispatch_laptop: $*"; exit 2; }
command -v gh >/dev/null || die "gh is not installed"

if [[ "${1:-}" == "status" || $# -eq 0 ]]; then
  out=$(gh api "repos/$REPO/actions/runners" --jq '.runners[] | "\(.name)\t\(.status)\t\(if .busy then "busy" else "idle" end)\t\([.labels[].name] | join(","))"' 2>&1) || { print -- "cannot read runners: $out"; exit 3; }
  [[ -n "$out" ]] || { print -- "no runner registered in $REPO"; exit 3; }
  print -- "$out"
  print -- "$out" | grep -q $'\tonline\t' && { print -- "laptop: READY"; exit 0; }
  print -- "laptop: OFFLINE"; exit 1
fi

[[ $# -ge 1 ]] || die "usage: dispatch_laptop.sh <branch> '<tests regex>' [targets...]"
BRANCH="$1"; TESTS="${2:-}"; shift; shift 2>/dev/null
TARGETS="$*"

# is a runner online? (not fatal: the job just waits in the queue)
gh api "repos/$REPO/actions/runners" --jq '.runners[].status' 2>/dev/null | grep -q online || print -u2 -- "warning: no online runner; the job will wait in the queue"

before=$(gh run list --repo "$REPO" --workflow build-test.yml --limit 1 --json databaseId --jq '.[0].databaseId // 0' 2>/dev/null)
before=${before:-0}
args=(-f "ref=$BRANCH" -f "tests=$TESTS" -f "targets=$TARGETS" -f "jobs=${LAPTOP_JOBS:-16}")
[[ -n "${LAPTOP_CMAKE_ARGS:-}" ]] && args+=(-f "cmake_args=$LAPTOP_CMAKE_ARGS")
gh workflow run build-test.yml --repo "$REPO" "${args[@]}" || die "could not start the workflow"

# find the new run
run=""
for i in {1..20}; do
  sleep 5
  run=$(gh run list --repo "$REPO" --workflow build-test.yml --limit 5 --event workflow_dispatch --json databaseId --jq "[.[] | select(.databaseId > $before)] | last | .databaseId // empty" 2>/dev/null)
  [[ -n "$run" ]] && break
done
[[ -n "$run" ]] || die "the run did not appear"
print -- "run $run: https://github.com/$REPO/actions/runs/$run"

start=$SECONDS; delay=$POLL
while (( SECONDS - start < MAX_WAIT )); do
  res=$(gh run view "$run" --repo "$REPO" --json status,conclusion --jq '"\(.status) \(.conclusion)"' 2>&1)
  if [[ "$res" == *"HTTP 403"* || "$res" == *"HTTP 429"* || "$res" == *"rate limit"* ]]; then
    delay=$(( delay * 2 > 600 ? 600 : delay * 2 )); print -u2 -- "rate limited, waiting ${delay}s"
  elif [[ "$res" == completed* ]]; then
    concl=${res#completed }
    print -- "finished: $concl"
    gh api "repos/$REPO/actions/runs/$run/jobs" --jq '.jobs[].steps[] | select(.conclusion == "failure") | "failed step: \(.name)"' 2>/dev/null
    # the job summary is also printed into the job log by the Summary step: show those lines
    gh run view "$run" --repo "$REPO" --log 2>/dev/null | awk -F'\t' 'NF >= 3 { print $3 }' | sed 's/^[0-9TZ:.-]* //' | grep -E 'Total time:|Tests: |error:|^- |Failing tests|Hits|Cache size' | head -60
    [[ "$concl" == "success" ]] && exit 0
    exit 1
  elif [[ "$res" == *"error"* || "$res" == *"HTTP"* ]]; then
    print -u2 -- "poll problem: $res"; delay=$(( delay * 2 > 600 ? 600 : delay * 2 ))
  else
    delay=$POLL
  fi
  sleep "$delay"
done
print -u2 -- "gave up waiting after ${MAX_WAIT}s; the run may still finish: https://github.com/$REPO/actions/runs/$run"
exit 4
