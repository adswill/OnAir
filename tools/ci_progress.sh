#!/bin/bash
# Progress bars for a running tools/ci_local.sh: reads the ctest logs in build/ci-logs and redraws every 2 s. Ctrl-C to stop watching
# (the CI run itself goes on). Usage: tools/ci_progress.sh [--once]
cd "$(dirname "$0")/.." || exit 1
LOGS=build/ci-logs
once=0; [ "$1" = "--once" ] && once=1

# when a log was started (its creation time; macOS stat -f, Linux stat -c)
born() { stat -f %B "$1" 2>/dev/null || stat -c %W "$1" 2>/dev/null || echo 0; }
# eta <log> <done> <total>: the time left at the rate so far ("" when nothing is known yet). Tests differ a lot in length, so it is rough.
# eta <log> <done> <total> [after]: [after] = a log whose last write is when this stage began (the tests start when their build ends)
eta() {
    local t0 now left
    t0=$(born "$1"); now=$(date +%s)
    [ -n "$4" ] && [ -f "$4" ] && t0=$(stat -f %m "$4" 2>/dev/null || stat -c %Y "$4")
    # a log kept from an earlier run (truncated, not made new) is as old as that run: never count from before this run's start
    if [ -f "$LOGS/.start" ]; then local st; st=$(stat -f %m "$LOGS/.start" 2>/dev/null || stat -c %Y "$LOGS/.start"); [ "$st" -gt "$t0" ] && t0=$st; fi
    [ "$2" -gt 0 ] && [ "$3" -gt "$2" ] && [ "$t0" -gt 0 ] || return
    left=$(( (now - t0) * ($3 - $2) / $2 ))
    if [ $left -ge 60 ]; then printf '~%dm left' $(( (left + 30) / 60 )); else printf '~%ds left' $left; fi
}

bar() {   # bar <label> <done> <total> <failed> <state>
    local label=$1 done=$2 total=$3 failed=$4 state=$5 width=40 fill pct
    if [ "$total" -gt 0 ]; then pct=$((done * 100 / total)); fill=$((done * width / total)); else pct=0; fill=0; fi
    printf '%-14s [' "$label"
    printf '%*s' "$fill" '' | tr ' ' '#'
    printf '%*s' "$((width - fill))" '' | tr ' ' '.'
    printf '] %3d%%  %d/%d' "$pct" "$done" "$total"
    [ "$failed" -gt 0 ] && printf '  \033[31m%d failed\033[0m' "$failed"
    printf '  %s\033[K\n' "$state"
}

# a log older than the start stamp ci_local.sh writes is left over from an earlier run
stale() { [ ! -f "$1" ] || { [ -f "$LOGS/.start" ] && [ "$1" -ot "$LOGS/.start" ]; }; }

stage() {   # stage <label> <log> <build log>
    local log=$LOGS/$2 after=$LOGS/$3
    if stale "$log"; then bar "$1" 0 0 0 "waiting"; return; fi
    # ctest prints "  12/180 Test  #34: name ....   Passed  1.2 sec" when a test ends
    local done total failed state cur
    done=$(grep -cE '^ *[0-9]+/[0-9]+ Test +#' "$log")
    total=$(grep -oE '^ *[0-9]+/[0-9]+ Test' "$log" | tail -1 | sed -E 's/.*\/([0-9]+) Test/\1/')
    [ -z "$total" ] && total=$(grep -cE '^ *Start +[0-9]+:' "$log")
    failed=$(grep -E '^ *[0-9]+/[0-9]+ Test +#.*(\*\*\*|Failed|Timeout)' "$log" | grep -vc 'Skipped')
    if [ -f "$log.result" ] && ! grep -q '^ok' "$log.result"; then state="FAILED (see $log)"
    elif grep -q 'Total Test time' "$log"; then state="done"
    else cur=$(grep -E '^ *Start +[0-9]+:' "$log" | tail -1 | sed -E 's/^ *Start +[0-9]+: //'); state="$(eta "$log" "${done:-0}" "${total:-0}" "$after")  running ${cur}"; fi
    bar "$1" "${done:-0}" "${total:-0}" "${failed:-0}" "$state"
}

build() {   # build <label> <log>
    local log=$LOGS/$2 pct
    if stale "$log"; then bar "$1" 0 0 0 "waiting"; return; fi
    pct=$(grep -oE '^\[ *[0-9]+%\]' "$log" | tail -1 | tr -dc '0-9')
    if grep -qE 'error:|Error [0-9]' "$log"; then bar "$1" "${pct:-0}" 100 1 "build errors (see $log)"
    elif [ "${pct:-0}" -ge 100 ]; then bar "$1" 100 100 0 "done"
    else bar "$1" "${pct:-0}" 100 0 "$(eta "$log" "${pct:-0}" 100)  building"; fi
}

draw() {
    build "macOS build" mac-build.log
    stage "macOS tests" mac-test.log mac-build.log
    build "Linux build" linux-build.log
    stage "Linux tests" linux-test.log linux-build.log
    build "Windows build" windows-build.log
}

if [ $once = 1 ]; then draw; exit 0; fi
clear
while true; do
    printf '\033[H'
    echo "OnAir local CI ($(date +%H:%M:%S))"
    draw
    sleep 2
done
