#!/usr/bin/env bash
# Checks the layout of the interface for overlapping, cut-off and squashed controls: runs the developer build of the app (configured with
# -DDECT2_UI_LAYOUT_CHECK=ON, see app/layout_check.cpp) hidden and muted for every mode and tab at the window sizes and display scales of
# common Windows laptops and monitors, and sums up the reports. It never captures the screen and never plays sound. The app runs as a renamed
# copy, so its settings are its own (macOS: the defaults domain of the copy, deleted after each run) and the real ones are not touched.
#
#   tools/ui_layout_check.sh [build dir] [output dir]
#   environment: JOBS=4 (runs at the same time), QUICK=1 (the three smallest sizes, fewer tabs), SIZES="1366x660@1.25 ..." (WxH@scale),
#                MODES="dvb fm adsb ..." (only these), VARIANT=n (layout of the new interface, 0-7), CLASSIC=1 (the classic interface),
#                SHOTS=1 (also a screenshot of each run), RADIO=1 (a fake radio, tests/native_fakes.cpp, selected and stopped instead of the
#                running test signal: the radio's gain, bias-tee and settings controls show)
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${1:-$ROOT/build-ui}"
OUT="${2:-${TMPDIR:-/tmp}/onair-layout-$(date +%Y%m%d-%H%M%S)}"
JOBS="${JOBS:-4}"
APP="$BUILD/dect2"
[ -x "$APP" ] || APP="$BUILD/dect2.exe"
if [ ! -x "$APP" ]; then echo "no app in $BUILD: cmake -S . -B build-ui -DCMAKE_BUILD_TYPE=Release -DDECT2_BUILD_APP=ON -DDECT2_UI_LAYOUT_CHECK=ON && cmake --build build-ui --target dect2" >&2; exit 1; fi
mkdir -p "$OUT/reports" "$OUT/shots" "$OUT/bin"
START=--autostart
if [ "${RADIO:-0}" = 1 ]; then   # the fake vendor libraries of the driver tests, under every name the drivers look for
    ext=.so; [ "$(uname)" = Darwin ] && ext=.dylib
    mkdir -p "$OUT/fakes"
    c++ -std=c++20 -O1 -shared -fPIC -fvisibility=hidden "$ROOT/tests/native_fakes.cpp" -o "$OUT/fakes/fake$ext" || exit 1
    for stem in rtlsdr airspy bladeRF LimeSuite iio uhd airspyhf; do cp "$OUT/fakes/fake$ext" "$OUT/fakes/lib$stem$ext"; done
    cp "$OUT/fakes/fake$ext" "$OUT/fakes/libsdrplay_api.so"
    export DECT2_NATIVE_LIBDIR="$OUT/fakes"
    START=""
fi

# a maximised window (or the default window, 1500 x 900 points at most) on: 1366x768 at 100/125/150 %, 1920x1080 at 100/125/150 %, 1280x720, 1024x600
SIZES="${SIZES:-1366x680@1.0 1366x660@1.25 1366x642@1.5 1500x900@1.0 1875x973@1.25 1904x954@1.5 1264x633@1.0 1008x513@1.0}"
[ "${QUICK:-0}" = 1 ] && SIZES="${QUICK_SIZES:-1366x660@1.25 1366x642@1.5 1008x513@1.0}"

# mode, its arguments and its tabs (the TV and receiver tabs also by their sub-views)
MODELIST="dvb	--dvb	Overview|TV|Guide|Teletext|Receiver|Signalling|FEC|Frame map|Channel|Stream|TS|Scan|Antenna|History
atsc	--atsc	Overview|TV|Receiver|Stream|TS|Scan|Antenna|History
atsc3	--atsc3	Overview|TV|Receiver|Stream|TS|Scan|Antenna|History
isdbt	--isdbt	Overview|TV|Receiver|Stream|TS|Scan|Antenna|History
dab	--dab	Overview|Radio|Ensemble|Map|Scan|Antenna|History
fm	--fm	Overview|Radio|Scan|History
dvbs	--mode dvbs	Overview|TV|Receiver|Stream|TS|History
dtmb	--mode dtmb	Overview|TV|Receiver|Stream|TS|Scan|History
atv	--mode atv	Overview|Video|Receiver|History
dmr	--mode dmr	Overview|Calls|Receiver|History
drm	--mode drm	Overview|Radio|Receiver|History
adsb	--mode adsb	Overview|Aircraft|Receiver|History
gnss	--mode gnss	Overview|Sky|Receiver|History
sonde	--mode sonde	Overview|Sondes|Receiver|History
ais	--mode ais	Overview|Ships|Receiver|History
marine	--mode marine	Overview|Messages|Receiver|History
acars	--mode acars	Overview|Messages|Receiver|History
inmc	--mode inmc	Overview|Messages|Receiver|History
aero	--mode aero	Overview|Messages|Receiver|History
iridium	--mode iridium	Overview|Iridium|Receiver|History
mesh	--mode mesh	Overview|Mesh|Receiver|History"

JOBLIST="$OUT/jobs.txt"
: > "$JOBLIST"
for size in $SIZES; do
    while IFS=$'\t' read -r id args tabs; do
        [ -n "${MODES:-}" ] && ! echo " $MODES " | grep -q " $id " && continue
        IFS='|' read -r -a tl <<< "$tabs"
        [ "${QUICK:-0}" = 1 ] && tl=("${tl[@]:0:3}")   # quick: the overview, the main tab and the next one
        for t in "${tl[@]}"; do printf '%s\t%s\t%s\t%s\n' "$size" "$id" "$args" "$t" >> "$JOBLIST"; done
    done <<< "$MODELIST"
done
TOTAL=$(wc -l < "$JOBLIST" | tr -d ' ')
echo "layout check: $TOTAL runs, $JOBS at a time, reports in $OUT"

worker() {   # worker n: every JOBS-th line of the job list, with its own copy of the app (its own settings)
    local n="$1" name="onair_layout_$1" i=0
    cp "$APP" "$OUT/bin/$name"
    while IFS=$'\t' read -r size id args tab; do
        i=$((i + 1))
        [ $(((i - 1) % JOBS)) -eq "$n" ] || continue
        local wh="${size%@*}" sc="${size#*@}"
        local tag="${wh}@${sc}_${id}_${tab// /-}"
        local rep="$OUT/reports/$tag.txt"
        [ -s "$rep" ] && continue
        [ "$(uname)" = Darwin ] && defaults delete "$name" >/dev/null 2>&1
        rm -rf "$OUT/home$n"; mkdir -p "$OUT/home$n"
        local extra=(DECT2_LAYOUT_EXIT=1)
        [ "${SHOTS:-0}" = 1 ] && extra=(DECT2_LAYOUT_AT=1000)   # the report at frame 390, the screenshot at 400
        # shellcheck disable=SC2086
        env HOME="$( [ "$(uname)" = Darwin ] && echo "$HOME" || echo "$OUT/home$n")" XDG_CONFIG_HOME="$OUT/home$n" \
            DECT2_WINSIZE="$wh" DECT2_UISCALE="$sc" DECT2_LAYOUT_REPORT="$rep" DECT2_LAYOUT_TAG="$tag" "${extra[@]}" \
            perl -e 'alarm shift; exec @ARGV' 90 "$OUT/bin/$name" --hidden --mute $args --tab "$tab" ${VARIANT:+--variant $VARIANT} ${CLASSIC:+--classic} $START --shot "$OUT/shots/$tag.png" \
            > "$OUT/reports/$tag.log" 2>&1
        [ -s "$rep" ] || echo "no report: $tag (see $OUT/reports/$tag.log)" >&2
    done < "$JOBLIST"
    [ "$(uname)" = Darwin ] && defaults delete "$name" >/dev/null 2>&1
    rm -f "$OUT/bin/$name"; rm -rf "$OUT/home$n"
}
for ((n = 0; n < JOBS; n++)); do worker "$n" & done
wait

python3 - "$OUT" <<'EOF'
import sys, os, re, collections
out = sys.argv[1]
runs = collections.defaultdict(int); kinds = collections.defaultdict(collections.Counter); uniq = collections.defaultdict(dict); missing = 0
allf = collections.OrderedDict()
for fn in sorted(os.listdir(os.path.join(out, "reports"))):
    if not fn.endswith(".txt"): continue
    lines = open(os.path.join(out, "reports", fn), errors="replace").read().splitlines()
    if any("; missing tab " in l for l in lines if l.startswith("# tabs")): missing += 1; continue
    size = fn.split("_")[0]
    runs[size] += 1
    for l in lines:
        if l.startswith("#") or not l.strip(): continue
        f = l.split("\t")
        if len(f) < 5: continue
        key = "\t".join([f[0], f[1], re.sub(r"\d", "#", f[2]), re.sub(r"\d", "#", f[3])])
        if key not in uniq[size]: uniq[size][key] = fn; kinds[size][f[0]] += 1
        e = allf.setdefault(key, {"line": l, "sizes": set(), "runs": 0, "first": fn})
        e["sizes"].add(size); e["runs"] += 1
with open(os.path.join(out, "summary.txt"), "w") as s:
    s.write("size@scale        runs  unique  OVERLAP  CUT  SQUASH\n")
    tot = collections.Counter()
    for size in sorted(runs, key=lambda x: (float(x.split("@")[1]), x)):
        k = kinds[size]
        s.write("%-16s %5d  %6d  %7d  %3d  %6d\n" % (size, runs[size], len(uniq[size]), k["OVERLAP"], k["CUT"], k["SQUASH"]))
    s.write("(%d runs skipped: the mode has no such tab)\n\n" % missing)
    s.write("all unique findings (runs, sizes, first report):\n")
    for key, e in sorted(allf.items(), key=lambda kv: (-len(kv[1]["sizes"]), kv[0])):
        s.write("%4d  %-40s  %s\n      %s\n" % (e["runs"], ",".join(sorted(e["sizes"])), e["first"], e["line"]))
print(open(os.path.join(out, "summary.txt")).read().split("all unique")[0])
print("full list:", os.path.join(out, "summary.txt"))
EOF
