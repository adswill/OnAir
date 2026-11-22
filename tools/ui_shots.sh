#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Screenshots of the 4.x interface for a layout review (.github/workflows/ui-shots.yml): every mode group, a sample of modes, the main tabs
# and the dialogs, at one window size and UI scale, then contact sheets of about 12 shots with labels (ImageMagick montage).
#   tools/ui_shots.sh <dect2 binary> <out dir> <WxH> <scale> [quick]
# Needs an X display (xvfb-run) and Mesa. Each shot runs the app once with its own empty HOME, so no choice leaks into the next shot.
set -u
BIN=$(readlink -f "$1"); mkdir -p "$2"; OUT=$(cd "$2" && pwd); SIZE=$3; SCALE=$4; SET=${5:-full}
mkdir -p "$OUT/shots"
TAG="${SIZE}@${SCALE}"

# name | arguments. The mode rows are picked as a click would (--pick); the tabs by name (--tab).
SHOTS=(
  "g00-tv-dvb|--pick DVB"
  "g01-broadcast-dab|--pick DAB / DAB+"
  "g02-aviation-adsb|--pick ADS-B"
  "g03-maritime-ais|--pick AIS"
  "g04-satnav-gnss|--pick GNSS"
  "g05-twoway-dmr|--pick DMR"
  "g06-cell-towers|--pick Cell towers"
  "g07-pagers-pocsag|--pick POCSAG"
  "g08-iot-dsrc|--pick DSRC road toll (EN 12253)"
  "g09-weather-hrpt|--pick NOAA HRPT"
  "g10-amdig-ft8|--pick FT8"
  "g11-amvoice-freedv|--pick FreeDV"
  "g12-hfutil-pactor|--pick PACTOR-II"
  "g13-general-occupancy|--pick Spectrum occupancy monitor"
  "g14-plugins|--pick Plugins"
  "g15-kris-kaal|--pick KAAL"
  "g16-custom-builder|--pick Mode Builder"
  "m-fm|--pick FM"
  "m-vdl4|--pick VDL4"
  "m-vdl2|--pick VDL2"
  "m-acars|--pick ACARS"
  "m-aprs|--pick APRS"
  "m-meteor|--pick Meteor HRPT"
  "m-navtex|--pick NAVTEX"
  "m-tetra|--pick TETRA"
  "m-drm|--pick DRM"
  "m-lora|--pick LoRa"
  "t-dvb-overview|--pick DVB --tab Overview"
  "t-dvb-tv|--pick DVB --tab TV"
  "t-dvb-receiver|--pick DVB --tab Receiver"
  "t-dvb-stream|--pick DVB --tab Stream"
  "t-dvb-scan|--pick DVB --tab Scan"
  "t-dvb-analysis|--pick DVB --tab Analysis"
  "t-dvb-history|--pick DVB --tab History"
  "t-dab-ensemble|--pick DAB / DAB+ --tab Ensemble"
  "t-dab-map|--pick DAB / DAB+ --tab Map"
  "t-vdl4-mode|--pick VDL4 --tab Messages"
  "t-adsb-map|--pick ADS-B --tab Map"
  "d-settings|--settings"
  "d-credits|--credits"
  "d-controls|--controls"
  "d-viewmenu|--viewmenu"
  "d-memory|--memory"
  "d-map|--map"
  "d-passes|--passes"
  "d-images|--images"
  "d-scanner|--scanner"
  "d-tour|--tour 0"
  "d-reclib|--reclib"
  "d-packs|--packs"
  "d-picker|--picker"
  "d-star|--star"
  "d-wizard|--tour"
  "d-setupwiz|--open setup"
  "w-backup|--open backup"
  "w-plugins|--open plugins"
  "w-station|--open station"
  "w-wireshark|--open wireshark"
  "w-datasets|--open datasets"
  "w-recmgr|--open recmgr"
  "w-scheduler|--open scheduler"
  "w-freq|--open freq"
  "w-heard|--open heard"
  "w-insight|--open insight"
  "w-doctor|--open doctor"
  "w-stationhealth|--open stationHealth"
  "w-feeds|--open feeds"
  "w-echolink|--open echolink"
  "w-swarm|--open swarm"
  "w-integrations|--open integrations"
  "w-watchlist|--open watchlist"
  "w-msgdb|--open msgdb"
  "w-protolab|--open protolab"
  "w-signallab|--open signallab"
  "w-modebuilder|--open modeBuilder"
  "w-txstudio|--open txStudio"
  "w-videogen|--open videoGen"
  "w-automation|--open automation"
  "w-antd|--open antd"
  "w-rotator|--open rotator"
  "w-satnogs|--open satnogs"
  "w-sweep|--open sweep"
  "w-df|--open df"
  "w-rfi|--open rfi"
  "w-passiveradar|--open passiveRadar"
  "w-trunkpool|--open trunkPool"
  "w-wideall|--open wideAll"
  "w-rffp|--open rfFp"
  "w-hardware|--open hardware"
  "w-voicecodecs|--open voiceCodecs"
  "w-voicekeys|--open voiceKeys"
  "w-lessons|--open lessons"
  "w-glossary|--open glossary"
  "w-checkup|--open checkup"
  "w-modevis|--open modeVis"
  "w-setup|--open setup"
  "w-hamlibadd|--open hamlibAdd"
  "d-trunk|--trunk-pool"
  "e00-settings-cat0|env:DECT2_SETTINGS=0 --settings"
  "e01-settings-cat1|env:DECT2_SETTINGS=1 --settings"
  "e02-settings-cat2|env:DECT2_SETTINGS=2 --settings"
  "e03-settings-cat3|env:DECT2_SETTINGS=3 --settings"
  "e04-settings-cat4|env:DECT2_SETTINGS=4 --settings"
  "e05-settings-cat5|env:DECT2_SETTINGS=5 --settings"
  "e06-settings-cat6|env:DECT2_SETTINGS=6 --settings"
  "e07-settings-cat7|env:DECT2_SETTINGS=7 --settings"
  "e08-settings-cat8|env:DECT2_SETTINGS=8 --settings"
  "e09-settings-cat9|env:DECT2_SETTINGS=9 --settings"
  "e10-settings-cat10|env:DECT2_SETTINGS=10 --settings"
  "e11-settings-cat11|env:DECT2_SETTINGS=11 --settings"
  "e12-settings-cat12|env:DECT2_SETTINGS=12 --settings"
  "e13-settings-cat13|env:DECT2_SETTINGS=13 --settings"
  "e14-settings-cat14|env:DECT2_SETTINGS=14 --settings"
  "e15-settings-cat15|env:DECT2_SETTINGS=15 --settings"
  "e16-settings-cat16|env:DECT2_SETTINGS=16 --settings"
  "e17-settings-cat17|env:DECT2_SETTINGS=17 --settings"
  "x-light-dvb|--light --pick DVB"
  "x-light-ft8|--light --pick FT8"
  "x-light-modes|--light --pick ACARS"
  "x-specdemo|--specdemo --pick FM"
  "s-studio-fm|--pick FM"
  "s-studio-adsb|--pick ADS-B"
  "s-studio-settings|--settings"
)
[ "$SET" = quick ] && SHOTS=("${SHOTS[@]:0:17}" "${SHOTS[@]:27:3}" "d-settings|--settings" "x-light-dvb|--light --pick DVB" "m-fm|--pick FM" "s-studio-fm|--pick FM" "s-studio-adsb|--pick ADS-B")

shoot() {   # name, args string
  local name=$1 args=$2 home png
  home=$(mktemp -d); png="$OUT/shots/$name.png"
  # "keep full drawing" answered, so the software-OpenGL hint does not cover the bottom right of every shot
  mkdir -p "$home/.config/onair"; echo "lowPowerDraw=2" > "$home/.config/onair/settings.conf"
  case $name in s-*) echo "uiTheme=5" >> "$home/.config/onair/settings.conf" ;; esac   # s-*: the Studio palette
  # "env:K=V " first in the arguments sets that variable for this shot only
  local -a envs=()
  while [[ $args == env:* ]]; do envs+=("${args%% *}"); envs[-1]=${envs[-1]#env:}; args=${args#* }; done
  # the arguments are one string: split it on " --" so a mode name with spaces stays one argument
  local -a argv=()
  local rest=" $args" part
  while [ -n "$rest" ]; do
    part=${rest##* --}; rest=${rest% --*}
    local flag=${part%% *} val=""
    [ "$flag" != "$part" ] && val=${part#* }
    if [ -n "$val" ]; then argv=("--$flag" "$val" "${argv[@]}"); else argv=("--$flag" "${argv[@]}"); fi
  done
  env "${envs[@]}" HOME=$home XDG_CONFIG_HOME=$home/.config XDG_DATA_HOME=$home/.local/share \
  DECT2_UISCALE=$SCALE ONAIR_WINSIZE=$SIZE DECT2_WINSIZE=$SIZE LIBGL_ALWAYS_SOFTWARE=1 \
    timeout 90 "$BIN" --hidden --mute --no-first-run --shotdelay 6 --shot "$png" "${argv[@]}" > "$OUT/shots/$name.log" 2>&1
  local rc=$?
  [ -f "$png" ] || echo "$TAG $name: no frame (exit $rc)" | tee -a "$OUT/missing.txt"
  rm -rf "$home"
}

for s in "${SHOTS[@]}"; do shoot "${s%%|*}" "${s#*|}"; done

# contact sheets: 12 shots each (4 x 3), labelled with the shot name and the size / scale
cd "$OUT/shots" || exit 1
mapfile -t PNGS < <(ls -1 *.png 2>/dev/null | sort)
safe=$(echo "$TAG" | tr '@x.' '_x_')
n=0
for ((i = 0; i < ${#PNGS[@]}; i += 12)); do
  n=$((n + 1))
  montage -label '%t' -font DejaVu-Sans -pointsize 18 "${PNGS[@]:i:12}" -tile 4x3 -geometry 640x+6+6 -background '#202020' -fill white \
    -title "$TAG  sheet $n" "../sheet-${safe}-$(printf %02d $n).png"
done
# the bottom 100 px (status bar) and top 130 px (menu and top bar) of every shot at full size, stacked: clipping there is hard to see on a sheet
H=$(identify -format '%h' "${PNGS[0]}" 2>/dev/null || echo 0)
if [ "$H" -gt 0 ]; then
  for f in "${PNGS[@]}"; do convert "$f" -gravity South -crop x100+0+0 +repage -gravity NorthWest -fill yellow -pointsize 14 -annotate +4+2 "${f%.png}" "/tmp/foot-$f"; done
  for f in "${PNGS[@]}"; do convert "$f" -gravity North -crop x130+0+0 +repage -gravity SouthWest -fill yellow -pointsize 14 -annotate +4+2 "${f%.png}" "/tmp/head-$f"; done
  for ((i = 0; i < ${#PNGS[@]}; i += 12)); do
    k=$(printf %02d $((i / 12 + 1)))
    convert $(for f in "${PNGS[@]:i:12}"; do echo "/tmp/foot-$f"; done) -append "../footers-${safe}-$k.png"
    convert $(for f in "${PNGS[@]:i:12}"; do echo "/tmp/head-$f"; done) -append "../headers-${safe}-$k.png"
  done
fi
ls -1 "$OUT"
