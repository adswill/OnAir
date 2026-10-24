#!/bin/zsh
# End to end, headless: English speech -> broadcast FM IQ (75 kHz deviation, noise) -> the program's file source with the real FM mode ->
# audio tap -> transcriber with the DEFAULT settings -> a transcript line. Needs a whisper model in the models folder (nothing is downloaded),
# macOS `say` and python3 with numpy + scipy. No window, no sound, and a renamed copy of the program so the real settings are not touched.
# usage: e2e_fm_transcript.sh /path/to/build/dect2 [snr_db=25]
set -e
here=${0:A:h}; BIN=${1:A}; SNR=${2:-25}
W=${TMPDIR:-/tmp}/onair-e2e-fm; mkdir -p $W; cd $W
say -v Samantha -o s.aiff "Good evening, and welcome to the news. The weather in Vilnius tomorrow will be sunny, with a light wind from the north. The temperature will reach twenty degrees. Now, back to the studio."
afconvert -f WAVE -d LEI16@24000 -c 1 s.aiff s.wav
python3 "$here"/gen_fm_speech_iq.py s.wav fm $SNR 93.9
rm -f ./onair-e2etest; cp "$BIN" ./onair-e2etest   # (a fresh file: macOS kills an overwritten signed binary)
defaults delete onair-e2etest 2>/dev/null || true   # default settings
rm -rf data; mkdir -p data
MODELS=${DECT2_E2E_MODELS:-$HOME/Library/Application Support/OnAir/models}
ln -s "$MODELS" data/models
export DECT2_DATA_DIR=$W/data
./onair-e2etest --file fm.sigmf-meta --freq 93.9 --fm --hidden --mute --no-first-run --shot e2e.png --shotdelay 50 > run.log 2>&1 || true
grep '^tx:' run.log
n=0
for w in weather vilnius sunny temperature; do grep -qi "^tx: line.*$w" run.log && n=$((n+1)); done
echo "key words found: $n of 4"
[ $n -ge 2 ] && { echo "e2e_fm_transcript: ok"; exit 0; }
echo "e2e_fm_transcript: FAIL"; exit 1
