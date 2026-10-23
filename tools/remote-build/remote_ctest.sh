#!/usr/bin/env bash
# Build and test a pushed branch on the remote worker (the owner's Windows laptop, WSL2 Ubuntu).
#   tools/remote-build/remote_ctest.sh ping
#   tools/remote-build/remote_ctest.sh <branch> "<target> [<target>...]" [ctest -R regex] [extra cmake args...]
# Targets: "all" builds everything. An empty regex ("") runs no tests. The branch must already be pushed to the pub remote.
# Connection data: ~/.config/onair-remote.env  (ONAIR_REMOTE_HOST, ONAIR_REMOTE_PORT, ONAIR_REMOTE_USER, ONAIR_REMOTE_KEY; see docs/REMOTE-BUILD.md)
# Optional there: ONAIR_REMOTE_TIMEOUT (seconds, default 5400), ONAIR_REMOTE_JOBS (default 16), ONAIR_REMOTE_REPO (default https://github.com/adswill/OnAir.git).
# Exit: 0 all passed, 1 build/test failed, 2 usage, 3 cannot connect, 4 busy (another job holds the lock), 124 timeout.
set -u
ENVF="${ONAIR_REMOTE_ENV:-$HOME/.config/onair-remote.env}"
[ -f "$ENVF" ] || { echo "missing $ENVF (see docs/REMOTE-BUILD.md)"; exit 2; }
# shellcheck disable=SC1090
. "$ENVF"
: "${ONAIR_REMOTE_HOST:?set ONAIR_REMOTE_HOST in $ENVF}"
PORT="${ONAIR_REMOTE_PORT:-2222}"; RUSER="${ONAIR_REMOTE_USER:-onair}"; KEY="${ONAIR_REMOTE_KEY:-$HOME/.ssh/id_ed25519}"
TIMEOUT="${ONAIR_REMOTE_TIMEOUT:-5400}"; JOBS="${ONAIR_REMOTE_JOBS:-16}"
REPO="${ONAIR_REMOTE_REPO:-https://github.com/adswill/OnAir.git}"
SSH=(ssh -p "$PORT" -i "$KEY" -o IdentitiesOnly=yes -o BatchMode=yes -o ConnectTimeout=10 -o StrictHostKeyChecking=accept-new -o ServerAliveInterval=30 "$RUSER@$ONAIR_REMOTE_HOST")

if [ "${1:-}" = ping ]; then
    if out=$("${SSH[@]}" 'echo ok; nproc; free -g | awk "/Mem/{print \$7\" GB available\"}"; uptime; if flock -n ~/.onair-job.lock true; then echo idle; else echo "job running"; fi' 2>&1); then
        echo "$out"; echo "SUMMARY: worker reachable"; exit 0
    fi
    echo "$out"; echo "SUMMARY: worker NOT reachable ($ONAIR_REMOTE_HOST:$PORT)"; exit 3
fi
[ $# -ge 2 ] || { sed -n 2,6p "$0"; exit 2; }
BRANCH=$1; TARGETS=$2; REGEX=${3:-}
shift 2; [ $# -gt 0 ] && shift
case "$BRANCH" in *[!A-Za-z0-9._/-]*|"") echo "bad branch name"; exit 2 ;; esac
case "$TARGETS" in *[!A-Za-z0-9._\ +-]*|"") echo "bad target list"; exit 2 ;; esac
case "$REGEX" in *\'*|*\`*|*\$*|*\;*) echo "regex may not contain ' \` \$ ;"; exit 2 ;; esac
EXTRA_Q=""
for a in "$@"; do
    case "$a" in *\'*) echo "no quote characters in cmake args"; exit 2 ;; esac
    EXTRA_Q="$EXTRA_Q '$a'"
done

# The script that runs on the worker. It is sent base64-encoded as one argument of bash -c, so nothing reads the ssh stdin.
read -r -d '' REMOTE <<'RS'
set -u
BRANCH=$1; TARGETS=$2; REGEX=$3; JOBS=$4; TIMEOUT=$5; REPO=$6; shift 6
. "$HOME/.cargo/env" 2>/dev/null
mkdir -p "$HOME/onair-logs"
LOG="$HOME/onair-logs/$(echo "$BRANCH" | tr '/' '_')-$(date +%Y%m%d-%H%M%S).log"
find "$HOME/onair-logs" -name '*.log' -mtime +7 -delete 2>/dev/null     # cleanup: logs older than 7 days
exec 9>"$HOME/.onair-job.lock"
if ! flock -n 9; then echo "SUMMARY: BUSY: another job holds the lock"; exit 4; fi
echo "log: $LOG"
run() {
    set -e
    shift 4      # branch, targets, regex, jobs: they are exported; what is left are the extra cmake arguments
    if [ ! -d "$HOME/onair/.git" ]; then git clone --quiet "$REPO" "$HOME/onair"; fi
    cd "$HOME/onair"
    git remote get-url pub >/dev/null 2>&1 || git remote add pub "$REPO"     # public repo over https: no token
    git fetch --quiet --prune pub "+refs/heads/$BRANCH:refs/remotes/pub/$BRANCH"
    git checkout --quiet --force --detach "pub/$BRANCH"
    git clean -fdq -e build      # keep the build folder (incremental), drop stray files
    echo "commit: $(git log -1 --format='%h %s')"
    if [ ! -f build/build.ninja ]; then
        cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DDECT2_BUILD_APP=ON -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache "$@"
    elif [ $# -gt 0 ]; then
        cmake -S . -B build "$@"
    fi
    if [ "$TARGETS" = all ]; then
        cmake --build build -j "$JOBS"
    else
        # shellcheck disable=SC2086
        cmake --build build -j "$JOBS" --target $TARGETS
    fi
    echo "BUILD OK"
    if [ -n "$REGEX" ]; then
        CI=true ctest --test-dir build -j "$JOBS" -R "$REGEX" --output-on-failure --timeout 1500
    fi
}
export -f run
export BRANCH TARGETS REGEX JOBS REPO
timeout -k 30 "$TIMEOUT" bash -c 'run "$@"' _ "$BRANCH" "$TARGETS" "$REGEX" "$JOBS" "$@" </dev/null 2>&1 | tee "$LOG"
rc=${PIPESTATUS[0]}
case $rc in 0) s="PASS" ;; 124) s="TIMEOUT after ${TIMEOUT}s" ;; *) s="FAIL (exit $rc)" ;; esac
fails=$(grep -E "tests failed out of|tests passed" "$LOG" | tail -1)
errs=$(grep -cE "error:|FAILED:" "$LOG")
echo "SUMMARY: $s | $BRANCH | ${fails:-no ctest run} | build errors: $errs | log: $LOG"
exit $rc
RS
B64=$(printf '%s' "$REMOTE" | base64 | tr -d '\n')
"${SSH[@]}" "bash -c \"\$(echo $B64 | base64 -d)\" _ '$BRANCH' '$TARGETS' '$REGEX' '$JOBS' '$TIMEOUT' '$REPO'$EXTRA_Q" </dev/null
rc=$?
if [ $rc -eq 255 ]; then
    echo "SUMMARY: cannot connect to $ONAIR_REMOTE_HOST:$PORT (laptop off, VPN down, or sshd stopped)"; exit 3
fi
exit $rc
