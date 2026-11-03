# Mode audit 3.6: how each mode family can beat its rival

Read-only audit (2026-11-03, branch feat36-audit). Inputs: `scratchpad/DECODER-FEATURE-PROPOSALS.md`, `docs/EXPORT-AUDIT.md`, `docs/BANDWIDTH-AUDIT.md`, and greps of `app/` and `core/`.

Method and honesty notes:
- Features that exist are not listed as improvements. Several proposals in the 2026-10-31 file already exist in the tree (T2-MI out `core/src/t2/t2mi_out.cpp`, ACARS reassembly `acars_reasm.h`, CPA/TCPA `ais_alerts`, RadioID lookup `app/voice/radioid_ui.cpp`, PSK Reporter / WSPRnet / APRS-IS feeds `app/panels/feeds_ui.cpp`, XMLTV EPG in IPTV, pass automation `automation_ui.cpp`), so they are omitted.
- A "missing" claim below was confirmed by a zero-hit grep for the specific term (SondeHub upload, Meisei/MRZ-N1, FM AF switching, analog-TV frame save, ADS-B polar coverage, blind transponder scan, AIS slot-map, DMR soft-decision path). Other items marked "verify" need a 5-minute look before a worker starts.
- The checklist letters come from a regex scan of `app/<folder>`; a Y can be a generic hit (for example "record" in a shared helper), an N means no hit in that folder. Treat them as pointers, not proof. The export column is the real source in `docs/EXPORT-AUDIT.md`: folders listed there as "no export yet" stay N.
- Custom bandwidth and sample rate are Y for every mode (`docs/BANDWIDTH-AUDIT.md`, `app/state/rx_width.h`). Rival tools were named from knowledge; no web research was done.
- Folders are the grouping unit (about 145 `app/` folders folded into the 27 families below, since many share one UI). Hardware-free: nothing here was run.

## Top 30 improvements (value/effort, S first; "RS" = needs real-signal data we cannot get here)

| # | Improvement | Size | Family | RS |
|---|---|---|---|---|
| 1 | Per-PID bitrate table + CSV export | S | DVB-T / T2 / DVB-C / DTMB / ISDB-T |  |
| 2 | ESG to XMLTV export | S | ATSC 1.0 / 3.0 |  |
| 3 | Dish aiming screen: margin meter + audible tone (specan_ui.cpp says LNB calibration is still work in progress) | S | DVB-S / S2 / ISDB-S / TV sat beacon / MAC |  |
| 4 | Per-MODCOD histogram with required Es/N0 and margin | S | DVB-S / S2 / ISDB-S / TV sat beacon / MAC |  |
| 5 | Save frame / record video to file from the picture | S | Analog TV / FPV video |  |
| 6 | MOT slideshow gallery with save (verify panel string) | S | DAB / DAB+ |  |
| 7 | Polar coverage plot (max range by bearing/altitude) as aiming tool | S | ADS-B / Mode S / Mode A/C / UAT / FLARM / uplink |  |
| 8 | Track history/replay with KML/CSV export | S | ADS-B / Mode S / Mode A/C / UAT / FLARM / uplink |  |
| 9 | Watchlist + alert for military/interesting hex | S | ADS-B / Mode S / Mode A/C / UAT / FLARM / uplink |  |
| 10 | VDL2/ACARS aircraft type/distance join from ADS-B (#394) | S | ACARS / VDL2 / VDL4 / HFDL / Aero / Mode-S aero |  |
| 11 | Airframes.io opt-in upload | S | ACARS / VDL2 / VDL4 / HFDL / Aero / Mode-S aero |  |
| 12 | EGC dedupe + SafetyNET area filter | S | Satcom: Iridium / Inmarsat / Globalstar / Orbcomm / Argos / Elektro |  |
| 13 | SOTDMA slot-map / channel-load view | S | AIS / VDES / marine DSC / VHF marine |  |
| 14 | APRS message/ack conversation view | S | APRS / AX.25 / packet / BLE-APRS |  |
| 15 | Per-digipeater coverage stats | S | APRS / AX.25 / packet / BLE-APRS |  |
| 16 | Add export to the ISM/Sigfox/wM-Bus/Zigbee/Z-Wave tables (EXPORT-AUDIT list) | S | LoRa / LoRaWAN / Meshtastic / ISM / Sigfox / wM-Bus / Zigbee / Z-Wave / wifi / HaLow / Wi-SUN / NB-IoT |  |
| 17 | Automatic pass record from TLE (automation_ui exists; wire to image modes) | S | Weather sats: APT / LRPT / HRPT / GOES / GK-2A / FY-3 / FY geo |  |
| 18 | RadioReference CSV talkgroup import (verify) | S | DMR / dPMR / NXDN / D-STAR / M17 / Fusion / P25 / P25p2 / TETRA / Tetrapol / voice |  |
| 19 | Add export to the EDACS/LTR/MPT1327 lists | S | Trunking & legacy PMR: MPT1327 / EDACS / LTR / ALE / railway / TETRAPOL / GSM-R / DECT |  |
| 20 | ADIF log with QSO auto-fill for FT8/JS8 | S | Ham/HF digital: FT8/FT4/WSPR/JS8/PSK/RTTY/Olivia/MT63/FSQ/IFKP/ARDOP/PACTOR/CW/Morse |  |
| 21 | Add exports to the psk/mt63/fsq/ardop lists | S | Ham/HF digital: FT8/FT4/WSPR/JS8/PSK/RTTY/Olivia/MT63/FSQ/IFKP/ARDOP/PACTOR/CW/Morse |  |
| 22 | ATIS transcript log with export (transcribe exists) | S | Aviation nav & misc aero: VOR / ILS / DME / NDB / airband / ATIS |  |
| 23 | Airband activity log with export | S | Aviation nav & misc aero: VOR / ILS / DME / NDB / airband / ATIS |  |
| 24 | SAME alert push/webhook | S | Aviation / military HF: milhf / roshf / radar / eloran / time / codar / datawell |  |
| 25 | Export for beacon/time/codar tables | S | Aviation / military HF: milhf / roshf / radar / eloran / time / codar / datawell |  |
| 26 | Memory import/export in common formats (CHIRP, SDR# xml) | S | Scanning, spectrum, tools: scan / specan / specx / rfi / bandplan / freqdb / vfo / memory |  |
| 27 | Squelch-scan audio recording per hit | S | Scanning, spectrum, tools: scan / specan / specx / rfi / bandplan / freqdb / vfo / memory |  |
| 28 | Add export to KA tables | S | Custom KA* modes and plugin/user modes |  |
| 29 | Export for rid/dji/ble tables | S | Positioning & airbands: DF / antenna / propagation |  |
| 30 | HD Radio album art + PSD log with export | S | DRM / HD Radio / CDR / AM stereo | RS |

## Per family

### DVB-T / T2 / DVB-C / DTMB / ISDB-T
- Folders: `dvbc, dtmb, isdbt, general`
- Rivals: TSReader, DVBinspector, SDRangel DVB-T, TVHeadend+dongle, Chinese DTMB boxes
- Rival has, OnAir lacks: Per-service bitrate/PID analyser; EPG grabber to XMLTV; ISDB-T one-seg alone
- OnAir has, rivals lack: Soft IQ source for any SDR, PLP list/select, per-carrier CSI, impulse response, T2-MI out (t2mi_out.h), no tuner card
- Checklist (grep in app/<folder>, heuristic): map:Y export:N record:Y replay:N alerts:Y why-not/hint:N help ?:N bandwidth:Y(all modes, rx_width.h)
- Improvements: Narrowband interferer erasure (LLR=0 on out-of-family carriers) [M]; ISDB-T one-seg-only narrow FFT lock + missing-PAT fallback (#313) [M, needs real signal]; Per-PID bitrate table + CSV export [S]

### ATSC 1.0 / 3.0
- Folders: `atsc3`
- Rivals: HDHomeRun, Tablo, TVHeadend, RF Explorer for ATSC 3.0
- Rival has, OnAir lacks: Long-ghost (200us+) adaptive DFE with ghost display; ATSC 3.0 ESG/service guide export
- OnAir has, rivals lack: ROUTE services over an SDR with no tuner box
- Checklist (grep in app/<folder>, heuristic): map:N export:N record:N replay:N alerts:Y why-not/hint:N help ?:Y bandwidth:Y(all modes, rx_width.h)
- Improvements: Ghost-profile display as an antenna aiming tool [M, needs real signal]; ESG to XMLTV export [S]

### DVB-S / S2 / ISDB-S / TV sat beacon / MAC
- Folders: `dvbs, isdbs, mac, satbeacon`
- Rivals: TBS/Tevii cards + TSReader, SDRangel DVB-S, Linux DVB tools, dish pointer apps
- Rival has, OnAir lacks: Blind transponder scan across 950-2150 MHz with MIS/ISI list; dish-aiming mode with tone; LNB LO ppm calibration
- OnAir has, rivals lack: Works on an SDR; ISI select, blind symbol rate; ACM
- Checklist (grep in app/<folder>, heuristic): map:N export:N record:Y replay:N alerts:Y why-not/hint:N help ?:N bandwidth:Y(all modes, rx_width.h)
- Improvements: Full-span blind transponder scan building a channel list (grep: no blindscan code) [M, needs real signal]; Dish aiming screen: margin meter + audible tone (specan_ui.cpp says LNB calibration is still work in progress) [S]; Per-MODCOD histogram with required Es/N0 and margin [S]

### Analog TV / FPV video
- Folders: `atv, fpv`
- Rivals: TVHeadend analog, tvtime, VLC v4l, Fatshark/ImmersionRC receivers, rtlsdr-analog-tv
- Rival has, OnAir lacks: Frame/clip save from the picture (grep: no atv frame save), vision-carrier AFC, ghost canceller
- OnAir has, rivals lack: PAL/SECAM/NTSC + NICAM from any SDR, FPV channel scan
- Checklist (grep in app/<folder>, heuristic): map:N export:N record:Y replay:N alerts:N why-not/hint:Y help ?:N bandwidth:Y(all modes, rx_width.h)
- Improvements: Save frame / record video to file from the picture [S]; Vision-carrier AFC + sync-stripped AGC [M, needs real signal]; Standard auto-detect from line count + sound spacing [M, needs real signal]

### DAB / DAB+
- Folders: `dab`
- Rivals: welle.io, dablin, SDRangel DAB, OpenDigitalRadio
- Rival has, OnAir lacks: Slideshow viewer polish; DLS history
- OnAir has, rivals lack: TII/transmitter list with map, journaline, announcements, export
- Checklist (grep in app/<folder>, heuristic): map:Y export:Y record:Y replay:N alerts:Y why-not/hint:N help ?:N bandwidth:Y(all modes, rx_width.h)
- Improvements: MOT slideshow gallery with save (verify panel string) [S]; RS-failure AAC concealment statistics [M]

### DRM / HD Radio / CDR / AM stereo
- Folders: `drm, hdr, cdr, cquam`
- Rivals: Dream, DRM Software Radio, nrsc5, SDR++ nrsc5, DSD
- Rival has, OnAir lacks: Dream's multimedia (journaline/slideshow) viewer parity; HD Radio station-logo and PSD history; DRM robustness-mode auto scan
- OnAir has, rivals lack: DRM CIR/SDC, HD+DRM+CDR in one app
- Checklist (grep in app/<folder>, heuristic): map:N export:N record:N replay:Y alerts:Y why-not/hint:N help ?:Y bandwidth:Y(all modes, rx_width.h)
- Improvements: DRM mode A-D auto scan with Doppler/delay-spread readout (#331 stutter) [M, needs real signal]; HD Radio album art + PSD log with export [S, needs real signal]; DRM AFS auto-switch [M, needs real signal]

### FM broadcast / RDS / AM / shortwave / ssbconv
- Folders: `fm, shortwave, ssbconv, wb`
- Rivals: SDR++, SDR#, GQRX, SDRangel, RDS Spy, HDSDR
- Rival has, OnAir lacks: RDS group voting for weak FM DX, AF auto-follow (grep: none), GB2312 RDS charset (#350), wide-band scanner list
- OnAir has, rivals lack: RDS AF/EON/TMC/RT+, multipath and pilot meters, noise blanker in general rx, exports
- Checklist (grep in app/<folder>, heuristic): map:N export:Y record:Y replay:Y alerts:Y why-not/hint:Y help ?:N bandwidth:Y(all modes, rx_width.h)
- Improvements: AF auto-follow keeping PI (grep: no switching code) [M, needs real signal]; RDS soft sync + group voting (rdsBlockOkPct today) [M, needs real signal]; Adaptive IF bandwidth from neighbour level [M, needs real signal]

### ADS-B / Mode S / Mode A/C / UAT / FLARM / uplink
- Folders: `adsb, modeac, uat, flarm, uplink, df`
- Rivals: dump1090-fa/tar1090, readsb, SDRangel ADS-B, Virtual Radar Server, FR24 feeders
- Rival has, OnAir lacks: Polar range/coverage plot by bearing and altitude (grep: none), history/replay of tracks, ADS-B watchlist alert wiring
- OnAir has, rivals lack: Beast/SBS in and out, BDS 4,4/5,0/6,0, 1-2 bit CRC repair, pulse-gain (pulse_gain.h), feeds
- Checklist (grep in app/<folder>, heuristic): map:Y export:Y record:Y replay:N alerts:Y why-not/hint:N help ?:N bandwidth:Y(all modes, rx_width.h)
- Improvements: Polar coverage plot (max range by bearing/altitude) as aiming tool [S]; Track history/replay with KML/CSV export [S]; Watchlist + alert for military/interesting hex [S]

### ACARS / VDL2 / VDL4 / HFDL / Aero / Mode-S aero
- Folders: `acars, vdl2, vdl4, hfdl, aero`
- Rivals: acarsdec/dumpvdl2/dumphfdl, JAERO, acarshub, Airframes, SDRangel ACARS
- Rival has, OnAir lacks: Airframes/acarsdrama upload; aircraft type + distance join with ADS-B (#394)
- OnAir has, rivals lack: Reassembly (acars_reasm.h), all VDL2 channels, HFDL band hop, map, export on acars/vdl2/hfdl
- Checklist (grep in app/<folder>, heuristic): map:Y export:Y record:Y replay:N alerts:Y why-not/hint:Y help ?:Y bandwidth:Y(all modes, rx_width.h)
- Improvements: VDL2/ACARS aircraft type/distance join from ADS-B (#394) [S]; Airframes.io opt-in upload [S]; HFDL auto-hop by SPDU station table (verify) [M, needs real signal]

### Satcom: Iridium / Inmarsat / Globalstar / Orbcomm / Argos / Elektro
- Folders: `iridium, inmsat, inmc, globalstar, orbcomm, argos, elektro`
- Rivals: iridium-toolkit, gr-iridium, JAERO, Inmarsat STD-C decoders, SDRangel
- Rival has, OnAir lacks: Iridium Doppler pre-correction from TLE for narrow captures (#281); EGC area-address filter
- OnAir has, rivals lack: Beam map, STD-C/EGC, Aero all in the app
- Checklist (grep in app/<folder>, heuristic): map:Y export:N record:Y replay:N alerts:Y why-not/hint:Y help ?:Y bandwidth:Y(all modes, rx_width.h)
- Improvements: TLE-driven Doppler tracking for Iridium/Orbcomm/Globalstar narrow capture [M, needs real signal]; EGC dedupe + SafetyNET area filter [S]; Argos status decode (#256) [M, needs real signal]

### AIS / VDES / marine DSC / VHF marine
- Folders: `ais, vdes, marine, marinevhf`
- Rivals: AISHub/OpenCPN AIS, rtl-ais, GNU AIS, SDRangel AIS, MarineTraffic feeders
- Rival has, OnAir lacks: AIS slot-map/channel-load view (grep: none); AISHub/MarineTraffic upload exists in feed_targets
- OnAir has, rivals lack: CPA/TCPA alerts (ais_alerts), satellite AIS collision recovery, msg 27, export, map, ship-type filter
- Checklist (grep in app/<folder>, heuristic): map:Y export:Y record:Y replay:N alerts:Y why-not/hint:Y help ?:N bandwidth:Y(all modes, rx_width.h)
- Improvements: SOTDMA slot-map / channel-load view [S]; Terrestrial collision cancellation in dense ports [M, needs real signal]; VDES ASM/VDE-TER features (#303) [M, needs real signal]

### Pagers: POCSAG / FLEX / others
- Folders: `pager`
- Rivals: multimon-ng, PDW, SDRangel pager, rtl-sdr Pager decoders
- Rival has, OnAir lacks: Multi-copy combining of repeated messages; band carrier ranking
- OnAir has, rivals lack: Watch list, speed auto-detect, whole-span decode, aliases
- Checklist (grep in app/<folder>, heuristic): map:N export:N record:N replay:N alerts:Y why-not/hint:N help ?:N bandwidth:Y(all modes, rx_width.h)
- Improvements: Majority-vote combining of repeated transmissions [M, needs real signal]; Ranked active-carrier list for a whole-span capture (#269) [M, needs real signal]

### APRS / AX.25 / packet / BLE-APRS
- Folders: `packet, packs, ham, mesh`
- Rivals: Dire Wolf, aprs.fi, YAAC, Xastir, direwolf+APRSDroid
- Rival has, OnAir lacks: Dire Wolf multi-slicer ensemble decode; message ack/conversation view
- OnAir has, rivals lack: APRS-IS feed (feeds_ui), digipeat counters, map, export
- Checklist (grep in app/<folder>, heuristic): map:Y export:Y record:Y replay:N alerts:Y why-not/hint:Y help ?:N bandwidth:Y(all modes, rx_width.h)
- Improvements: Multi-slicer ensemble demod for fading 1200 baud [M, needs real signal]; APRS message/ack conversation view [S]; Per-digipeater coverage stats [S]

### Radiosondes / Horus balloons
- Folders: `sonde, horus`
- Rivals: radiosonde_auto_rx, SondeHub tracker, sondemonitor, Horus tools
- Rival has, OnAir lacks: SondeHub upload (grep: not in feeds), Meisei/MRZ-N1/MTS01 types (grep: 0), frequency-agile sonde-type classifier
- OnAir has, rivals lack: Landing prediction, RS41/RS92/DFM/M10/M20/LMS6/iMet in one app, export
- Checklist (grep in app/<folder>, heuristic): map:Y export:Y record:N replay:N alerts:N why-not/hint:N help ?:N bandwidth:Y(all modes, rx_width.h)
- Improvements: SondeHub opt-in upload + Horus listener upload [M]; Meisei, MRZ-N1 (#379), MTS01 decoders [M, needs real signal]; Auto 400-406 MHz scan with type classifier [M, needs real signal]

### LoRa / LoRaWAN / Meshtastic / ISM / Sigfox / wM-Bus / Zigbee / Z-Wave / wifi / HaLow / Wi-SUN / NB-IoT
- Folders: `loraraw, lorawan, lorasat, ism, sigfox, wmbus, zigbee, zwave, wifi, halow, wisun, nbiot, esubghz, nrf24, dect, nfc`
- Rivals: rtl_433, SDRangel LoRa, gr-lora_sdr, TTN/ChirpStack, Wireshark+SDR, Flipper
- Rival has, OnAir lacks: rtl_433 device database breadth; LoRa sample-jump recovery at 2.048 Msps (#386), Qi charger (#379), LoRaWAN key import
- OnAir has, rivals lack: Many protocols in one app with map and export on lorawan
- Checklist (grep in app/<folder>, heuristic): map:N export:Y record:Y replay:Y alerts:Y why-not/hint:Y help ?:Y bandwidth:Y(all modes, rx_width.h)
- Improvements: LoRa re-sync after RTL sample drop (#386) [M, needs real signal]; LoRaWAN session-key import for payloads [M]; Add export to the ISM/Sigfox/wM-Bus/Zigbee/Z-Wave tables (EXPORT-AUDIT list) [S]

### Weather sats: APT / LRPT / HRPT / GOES / GK-2A / FY-3 / FY geo
- Folders: `meteor, hrpt, goes, goesdcs, gk2a, fy3, fygeo, ssdv, cubesat, lorasat, astro`
- Rivals: SatDump, WXtoImg, gpredict, Orbitron, goestools, xrit-rx
- Rival has, OnAir lacks: Map/border overlay with composites (SatDump parity; overlay only in gallery/inmsat), TLE auto pass record
- OnAir has, rivals lack: Passes + sky plot (passes_ui), automation scheduler, one app for all
- Checklist (grep in app/<folder>, heuristic): map:Y export:N record:N replay:N alerts:Y why-not/hint:Y help ?:Y bandwidth:Y(all modes, rx_width.h)
- Improvements: Coast/border overlay + projection on received images [M]; Automatic pass record from TLE (automation_ui exists; wire to image modes) [S]; Soft-symbol LRPT Viterbi + RS recovery [M, needs real signal]

### GNSS / SBAS / DGPS
- Folders: `gnss, sbas, dgps`
- Rivals: GNSS-SDR, gps-sdr-sim, u-center, RTKLIB
- Rival has, OnAir lacks: RINEX export/assisted acquisition; DOP and jamming/spoofing plot
- OnAir has, rivals lack: Per-channel export, sky plot, map
- Checklist (grep in app/<folder>, heuristic): map:Y export:Y record:Y replay:N alerts:Y why-not/hint:Y help ?:N bandwidth:Y(all modes, rx_width.h)
- Improvements: RINEX 3 observation export [M]; Spoofing/jamming detector plot [S, needs real signal]; Almanac-assisted cold start [M, needs real signal]

### DMR / dPMR / NXDN / D-STAR / M17 / Fusion / P25 / P25p2 / TETRA / Tetrapol / voice
- Folders: `dmr, dpmr, nxdn, dstar, m17, fusion, p25, p25p2, tetra, tetrapol, voice, trunkpool, keys`
- Rivals: DSD-FME, SDRtrunk, OP25, Unitrunker, SDRangel, Boatbod, DSDPlus
- Rival has, OnAir lacks: Soft-decision BPTC/Golay/trellis (grep: no soft path), short-burst fast resync, RadioReference CSV import
- OnAir has, rivals lack: RadioID.net lookup (radioid_ui), LRRP map, trunk follower, TG labels, export for dmr/p25/tetra
- Checklist (grep in app/<folder>, heuristic): map:Y export:Y record:Y replay:N alerts:Y why-not/hint:Y help ?:Y bandwidth:Y(all modes, rx_width.h)
- Improvements: DMR short-burst fast resync (3.2 goal) [M, needs real signal]; Soft-decision FEC with eye-based erasures [L, needs real signal]; RadioReference CSV talkgroup import (verify) [S]

### Trunking & legacy PMR: MPT1327 / EDACS / LTR / ALE / railway / TETRAPOL / GSM-R / DECT
- Folders: `mpt1327, edacs, ltr, gsmr, railway, lbj, ale, dect`
- Rivals: Unitrunker, SDRtrunk, ALE decoders, GSM-R sniffers
- Rival has, OnAir lacks: Call-log with map and per-system export; ALE linking history
- OnAir has, rivals lack: Several in one
- Checklist (grep in app/<folder>, heuristic): map:N export:Y record:Y replay:N alerts:N why-not/hint:Y help ?:Y bandwidth:Y(all modes, rx_width.h)
- Improvements: Add export to the EDACS/LTR/MPT1327 lists [S]; Per-system call log and scanner-style hold/skip [M]

### Ham/HF digital: FT8/FT4/WSPR/JS8/PSK/RTTY/Olivia/MT63/FSQ/IFKP/ARDOP/PACTOR/CW/Morse
- Folders: `hfdig, js8, psk, pskdsp, mt63, fsq, ifkp, ardop, pactor, pactorhf, cw, multitone, sweep`
- Rivals: WSJT-X, JS8Call, fldigi, MSHV, CW Skimmer, Winlink, PSK Reporter
- Rival has, OnAir lacks: Log to ADIF auto, rig CAT TX (OnAir receive-focused), CW Skimmer-style multi-signal list
- OnAir has, rivals lack: pskreporter/wsprnet feeds exist, whole-span decode, many modes together
- Checklist (grep in app/<folder>, heuristic): map:Y export:N record:Y replay:N alerts:Y why-not/hint:Y help ?:Y bandwidth:Y(all modes, rx_width.h)
- Improvements: ADIF log with QSO auto-fill for FT8/JS8 [S]; CW skimmer view: every CW signal in the span with call + RBN spot [M, needs real signal]; Add exports to the psk/mt63/fsq/ardop lists [S]

### Aviation nav & misc aero: VOR / ILS / DME / NDB / airband / ATIS
- Folders: `vor, dme, ndb, airband, atis`
- Rivals: VOR/ILS SDR plugins, Liveatc, GQRX airband
- Rival has, OnAir lacks: Airband squelch scanner with channel memory; ATIS text via speech recognition
- OnAir has, rivals lack: Bearing from two VORs, transcribe
- Checklist (grep in app/<folder>, heuristic): map:Y export:N record:N replay:N alerts:Y why-not/hint:Y help ?:N bandwidth:Y(all modes, rx_width.h)
- Improvements: ATIS transcript log with export (transcribe exists) [S]; Airband activity log with export [S]

### Aviation / military HF: milhf / roshf / radar / eloran / time / codar / datawell
- Folders: `milhf, roshf, radar, eloran, time, codar, datawell, tones, same, selcal, multitone`
- Rivals: Signals ID Wiki tools, sdr-audio, EAS decoders, WWV time tools
- Rival has, OnAir lacks: SAME/EAS push alerts
- OnAir has, rivals lack: Many exotic modes with map
- Checklist (grep in app/<folder>, heuristic): map:Y export:N record:Y replay:N alerts:Y why-not/hint:Y help ?:N bandwidth:Y(all modes, rx_width.h)
- Improvements: SAME alert push/webhook [S]; Export for beacon/time/codar tables [S]

### Mobile cell: LTE/NR/GSM scanner
- Folders: `cell, nr`
- Rivals: LTE-Cell-Scanner, srsRAN cell_search, Kalibrate, Scat
- Rival has, OnAir lacks: Operator database (MCC/MNC) and PCI map
- OnAir has, rivals lack: NR+LTE+NB-IoT with export
- Checklist (grep in app/<folder>, heuristic): map:Y export:Y record:N replay:N alerts:N why-not/hint:N help ?:Y bandwidth:Y(all modes, rx_width.h)
- Improvements: Cell-site map from OpenCellID (opt-in) [M]

### Scanning, spectrum, tools: scan / specan / specx / rfi / bandplan / freqdb / vfo / memory
- Folders: `scan, specan, specx, rfi, bandplan, freqdb, vfo, memory, wb, autodet, transcribe`
- Rivals: SDR++, SDRangel, SDR#, GQRX, Unitrunker, ExpertSDR
- Rival has, OnAir lacks: Frequency lists import from RadioReference/CHIRP; band-scope bookmarks sync
- OnAir has, rivals lack: Autodetect of signal type, whole-span scan, doctor, transcribe
- Checklist (grep in app/<folder>, heuristic): map:Y export:Y record:Y replay:Y alerts:Y why-not/hint:Y help ?:Y bandwidth:Y(all modes, rx_width.h)
- Improvements: Memory import/export in common formats (CHIRP, SDR# xml) [S]; Squelch-scan audio recording per hit [S]

### Custom KA* modes and plugin/user modes
- Folders: `kaal, kait, kapp, kavb, kaws, kaft, plugin`
- Rivals: no rival (own modulation)
- Rival has, OnAir lacks: n/a
- OnAir has, rivals lack: Own spec and tools
- Checklist (grep in app/<folder>, heuristic): map:Y export:N record:Y replay:Y alerts:N why-not/hint:N help ?:N bandwidth:Y(all modes, rx_width.h)
- Improvements: Add export to KA tables [S]

### Positioning & airbands: DF / antenna / propagation
- Folders: `df, ant, antd, prop, iono, hfbeacon, qo100, sarsat, dji, rid, bletrk, ble, asterix`
- Rivals: KrakenSDR app, VOACAP/Ham CAP, GPredict, SARSAT tools, OpenDroneID apps
- Rival has, OnAir lacks: Remote ID export; Doppler-based ESA ASTERIX viewers
- OnAir has, rivals lack: Multi-source propagation + map
- Checklist (grep in app/<folder>, heuristic): map:Y export:Y record:Y replay:N alerts:Y why-not/hint:Y help ?:Y bandwidth:Y(all modes, rx_width.h)
- Improvements: Export for rid/dji/ble tables [S]; SARSAT false-FGB suppression (#404/#397/#407) [M, needs real signal]

## Cross-cutting (every mode)
- Export: ~110 folders still have no `kit::exportBar` (list in `docs/EXPORT-AUDIT.md`); wiring them is S each and removes the commonest 'cannot get my data out' gap versus SDRangel/SDRtrunk/rtl_433.
- 'Why not decoding' hint: only some folders have one (gk2a, fygeo, fy3 and a handful more by grep); a shared hint line in `kit` fed by each mode's lock state would cover all (M).
- Help '?': `HelpButton` is in about 28 files; modes without one should get it (S each).
