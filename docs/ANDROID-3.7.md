# Android 3.7 audit

Branch `feat37-android`. Every row of `app/ui/mode_table.cpp` against what the Android build offers and runs.

**How this was tested.** Debug APK built on GitHub (workflow `android.yml`, input `debug`), installed on an Android 15 arm64 emulator (no window, no sound, 4 virtual cores on an Apple Silicon Mac). `android/tools/smoke_modes.sh` ran every mode for 7 s against the built-in **test signal**. **No physical phone and no physical radio were tested.** USB OTG, real antennas, thermal throttling and battery behaviour are unverified. The USB permission code is covered by unit tests only.

Column notes: "Test signal" is the receiver state after 7 s (locked / converging / searching). Searching or converging after 7 s does not mean broken: some test signals are bursty, need longer or do not model lock at all. "CPU %" is the app in `top` on the emulator (400 = 4 cores) and is a best case: an Apple Silicon core is faster than a typical phone core. Rate is the capture rate seen by the spectrum (blank when not reported in time).

## Result

Modes in the list: **108 before, 183 now (every row of the desktop table)**. All 183 started and changed without a crash in the emulator run.

Missing: 3, Present, limited: 43, Present, runs: 53, Present, working: 84.

## Problems found

### Crashes and blockers (fixed)
- **Receiver froze for good, roughly one mode change in 3 to 15 in the test runs.** `ReceiverEngine.loop` used `withTimeoutOrNull` around a channel receive; a command delivered as the timeout fired was lost, the caller waited forever and Start, Stop and mode changes all hung until the app was killed. Found by the all-modes run, fixed with `select`. This may also explain reports of the app "freezing" (#201), but that was not confirmed on the reporter's phone.
- **Wrong panels on every mode.** Panels compared the engine code (family + 2) with numbers in telemetry numbering (family + 1), so FM never got its RDS tab, ADS-B and AIS never got their dedicated panels, DRM got the ADS-B panel, and the general receiver lost its squelch slider. Fixed (`telemetryStdOf`).
- **Foreground service crash path.** If Android refused the service type (USB permission gone, microphone denied) the service stopped without `startForeground`, which Android turns into a crash. It now falls back to plain media playback.
- **Mode list generator was broken.** `gen_modes.py` read `app/toolbar.cpp`, where the table no longer lives, so 75 modes never reached Android. It now reads `mode_table.cpp` and runs from `build_native.sh`; CI fails if the generated files are stale. Indices are stable (`android/tools/mode_order.txt`), so saved favourites and frequencies survive.
- `nativeModeTable` stopped at standard 120, so newer modes had no tuning data. Volume only reached FM and the general receiver; it now reaches every receiver that plays sound.

### Results (fixed, automatic for future modes)
Before: about 30 standards had hand-written exporters, the rest a one-line summary. Now `tools/gen_results_map.py` reads the telemetry structs and generates cards and tables with the field names for 107 further standards, regenerated at each native build. Labels come from field names, so they are English and plainer than the desktop's.

### UI (fixed)
Text sizes raised and table text from 11 to 13 sp; one-column lists wrap instead of scrolling sideways; hard-coded map strings moved to `strings.xml`. Picking a mode now starts it (Settings can turn that off). Sample rate and radio-specific settings moved under Advanced on the Radio tab. A USB radio that becomes ready replaces the test signal automatically. Settings and a one-time banner offer Android's battery exemption so a long decode is not stopped. Portrait and landscape were checked on the emulator; touch targets were already 48 dp.

### Not fixed
- **TV picture and sound.** FFmpeg is stubbed on Android (`native/src/stubs_ext.c`) and patented codecs must not ship. Android's own MediaCodec could do this without bundling a codec, but it is not built. TV modes show lock and service data only.
- **DAB+, DRM, HD Radio, CDR audio** (same cause) and **digital voice** (the user-installed codec plug-in path has no Android import screen yet).
- **Pictures** (weather satellites, fax, SSTV, analogue TV): decoded, but the phone has no image panel.
- **No channel scan on Android.** #435 asks for it; the core scanner is not exposed.
- **Desktop-only rows** (Mode Builder, Plugins, Internet TV) are listed with an explanation and cannot be selected. "TV (auto)" and "Digital voice (auto)" open the family receiver, because the detector is a desktop screen.

## CPU limits, stated plainly
Not measured on a phone. On the emulator the heaviest modes used 2 to 2.7 cores of a much faster CPU than most phones have, so these will usually **not keep up on a typical phone**: DVB-T2/T, DVB-C, DVB-S/S2, ATSC, ATSC 3.0, ISDB-T, DTMB, ISDB-S. The mode list marks anything at 6 Msps or more as heavy; it warns and never blocks. Narrowband modes (most of the list, median about 0.16 core on the emulator) should be comfortable, and sustained load on a real phone is still unknown.

## Issues read
- **#463** (Feedback on Android 3.6.0): the report is a screenshot plus the text "AF ----"; the screenshot was not downloaded, so its content was not reviewed.
- **#435** (DTMB on Android): good SNR but no channels or parameters. Not reproducible without a signal; DTMB's lock cards are shown, and scanning is listed above as not built.
- **#201** (frozen spectrum, RTL-SDR): earlier USB fixes are in docs/PATHWAY.md (2026-10-31); the lost-command fix above is new. Needs retesting by the reporter.
- **#122, #139**: the Android build and touch UI requests this work continues.

## Mode table

| # | Mode | In the list before | State now | Test signal on emulator | CPU % (of 400) | Rate Msps | Why / limit |
|---|---|---|---|---|---|---|---|
| 0 | DVB | yes | Present, limited | locked, 7 cards | 52.0 | 9.14 | No picture or TV sound: FFmpeg is stubbed out of the Android build; wideband: too slow for most phones |
| 1 | ATSC | yes | Present, limited | locked, 4 cards | 204 | 8 | No picture or TV sound: FFmpeg is stubbed out of the Android build; wideband: too slow for most phones |
| 2 | ATSC 3.0 | yes | Present, limited | searching | 124 | 8 | No picture or TV sound: FFmpeg is stubbed out of the Android build; wideband: too slow for most phones |
| 3 | ISDB-T | yes | Present, limited | locked | 130 | 10 | No picture or TV sound: FFmpeg is stubbed out of the Android build; wideband: too slow for most phones |
| 4 | DVB-S/S2 | yes | Present, limited | locked, 4 cards | 80.0 | 10 | No picture or TV sound: FFmpeg is stubbed out of the Android build; wideband: too slow for most phones |
| 5 | DTMB | yes | Present, limited | locked, 4 cards | 152 | 10 | No picture or TV sound: FFmpeg is stubbed out of the Android build; wideband: too slow for most phones |
| 6 | DVB-C | yes | Present, limited | locked, 4 cards | 268 | 10 | No picture or TV sound: FFmpeg is stubbed out of the Android build; wideband: too slow for most phones |
| 7 | Analog TV | yes | Present, limited | locked, 10 cards | 188 | 10 | No picture or TV sound: FFmpeg is stubbed out of the Android build; wideband: too slow for most phones |
| 8 | MAC | yes | Present, limited | locked, 8 cards | 76.0 | 20 | No picture or TV sound: FFmpeg is stubbed out of the Android build; wideband: too slow for most phones |
| 9 | DAB / DAB+ | yes | Present, limited | locked, 5 cards, 4 rows | 16.0 | 2.05 | Compressed audio codec not in the Android build |
| 10 | FM | yes | Present, working | locked, 6 cards | 28.0 | 4 |  |
| 11 | DRM | yes | Present, limited | locked, 10 cards, 1 rows | 24.0 | 2 | Compressed audio codec not in the Android build |
| 12 | HD Radio | yes | Present, limited | locked, 11 cards, 6 rows | 12.0 | 2 | Compressed audio codec not in the Android build |
| 13 | CDR | yes | Present, limited | locked, 9 cards, 3 rows | 12.0 | 2 | Compressed audio codec not in the Android build |
| 14 | ADS-B | yes | Present, working | locked, 5 cards, 12 rows | 24.0 | 4 |  |
| 15 | Mode S uplink | yes | Present, working | locked, 11 cards, 97 rows | 111 | 10 | wideband: too slow for most phones |
| 16 | ACARS | yes | Present, working | locked, 2 cards, 24 rows | 8.0 | 2 |  |
| 17 | VDL2 | yes | Present, working | locked, 2 cards, 29 rows | 44.0 | 2 |  |
| 18 | HFDL | yes | Present, working | locked, 5 cards, 12 rows | 72.0 | 2 |  |
| 19 | Airband | yes | Present, working | locked, 11 cards, 10 rows | 36.0 | 2 |  |
| 20 | VOR | yes | Present, working | locked, 11 cards | 16.0 | 2 |  |
| 21 | ILS | yes | Present, working | locked, 8 cards | 20.0 | 2 |  |
| 22 | NDB | yes | Present, runs | converging, 8 cards | 0.0 | 0.256 |  |
| 23 | Drone Remote ID | yes | Present, working | locked, 11 cards, 81 rows | 160 | 20 | wideband: too slow for most phones |
| 24 | AIS | yes | Present, working | locked, 3 cards, 2 rows | 0.0 | 2 |  |
| 25 | VDES | yes | Present, runs | converging, 5 cards, 17 rows | 4.0 | 1 |  |
| 26 | NAVTEX | yes | Present, limited | locked, 9 cards | 12.0 | 2 | Pictures decode, but there is no picture view on the phone |
| 27 | DSC | yes | Present, limited | locked, 11 cards, 2 rows | 12.0 | 2 | Pictures decode, but there is no picture view on the phone |
| 28 | Weather fax | yes | Present, limited | locked, 7 cards, 2 rows | 11.5 | 2 | Pictures decode, but there is no picture view on the phone |
| 29 | GNSS | yes | Present, runs | converging, 9 cards, 16 rows | 124 | 4 |  |
| 30 | Inmarsat-C | yes | Present, runs | converging, 7 cards, 1 rows | 12.0 | 2 |  |
| 31 | Inmarsat Aero | yes | Present, working | locked, 7 cards, 3 rows | 32.0 | 4 |  |
| 32 | Iridium | yes | Present, working | locked, 8 cards, 16 rows | 40.0 | 10 | wideband: too slow for most phones |
| 33 | Meteor-M LRPT | yes | Present, limited | searching, 9 cards | 40.0 | 1 | Pictures decode, but there is no picture view on the phone |
| 34 | GOES HRIT | yes | Present, limited | locked, 9 cards, 13 rows | 60.0 | 2.4 | Pictures decode, but there is no picture view on the phone |
| 35 | Elektro-L | yes | Present, limited | locked | 32.0 | 2.4 | Pictures decode, but there is no picture view on the phone |
| 36 | HRPT / AHRPT | yes | Present, limited | converging, 9 cards | 184 | 6 | Pictures decode, but there is no picture view on the phone; wideband: too slow for most phones |
| 37 | Orbcomm | yes | Present, working | locked, 8 cards, 82 rows | 20.0 | 1 |  |
| 38 | QO-100 | yes | Present, working | locked, 11 cards, 14 rows | 48.0 | 10 | wideband: too slow for most phones |
| 39 | FLARM | yes | Present, working | locked, 9 cards, 3 rows | 12.0 | 1 |  |
| 40 | DJI drones | yes | Present, working | locked, 9 cards, 81 rows | 88.0 | 20 | wideband: too slow for most phones |
| 41 | CubeSat telemetry | yes | Present, working | locked, 8 cards, 11 rows | 8.0 | 2 |  |
| 42 | Globalstar | yes | Present, working | locked, 6 cards, 41 rows | 16.0 | 10 | wideband: too slow for most phones |
| 43 | DMR | yes | Present, limited | locked, 12 cards, 16 rows | 4.0 | 2.4 | Voice needs a user-installed codec plug-in; the app cannot load one yet |
| 44 | NXDN | yes | Present, limited | searching, 6 cards | 4.0 | 2 | Voice needs a user-installed codec plug-in; the app cannot load one yet |
| 45 | P25 | yes | Present, limited | searching, 6 cards | 4.0 | 2 | Voice needs a user-installed codec plug-in; the app cannot load one yet |
| 46 | dPMR / PMR446 | yes | Present, limited | locked, 9 cards, 16 rows | 8.0 | 2 | Voice needs a user-installed codec plug-in; the app cannot load one yet |
| 47 | TETRA | yes | Present, limited | locked, 7 cards, 5 rows | 16.0 | 2 | Voice needs a user-installed codec plug-in; the app cannot load one yet |
| 48 | TETRAPOL | yes | Present, limited | locked, 11 cards, 6 rows | 4.0 | 0.25 | Voice needs a user-installed codec plug-in; the app cannot load one yet |
| 49 | Cell towers | yes | Present, working | locked, 10 cards, 3 rows | 208 | 20 | wideband: too slow for most phones |
| 50 | 5G NR cells | yes | Present, working | locked, 10 cards, 4 rows | 220 | 20 | wideband: too slow for most phones |
| 51 | NB-IoT cells | yes | Present, runs | converging, 5 cards | 132 | 2 |  |
| 52 | GSM-R | yes | Present, working | locked, 10 cards, 8 rows | 196 | 5 |  |
| 53 | LBJ | yes | Present, working | locked, 9 cards, 4 rows | 8.0 | 0.96 |  |
| 54 | POCSAG | yes | Present, working | locked, 3 cards, 7 rows | 24.0 | 2 |  |
| 55 | FLEX | yes | Present, working | locked, 3 cards, 22 rows | 16.0 | 2 |  |
| 56 | ISM sensors | yes | Present, working | locked, 7 cards, 3 rows | 12.0 | 2 |  |
| 57 | LoRaWAN | yes | Present, runs | searching, 6 cards | 4.0 | 2 |  |
| 58 | Meshtastic | yes | Present, working | locked, 7 cards, 9 rows | 12.0 | 2 |  |
| 59 | MeshCore | yes | Present, runs | converging, 6 cards, 9 rows | 16.0 | 2 |  |
| 60 | Bluetooth LE | yes | Present, working | locked, 6 cards, 5 rows | 48.0 | 4 |  |
| 61 | Zigbee / 802.15.4 | yes | Present, working | locked, 10 cards, 89 rows | 40.0 | 4 |  |
| 62 | Wi-Fi HaLow (802.11ah) | yes | Present, working | locked, 10 cards, 84 rows | 164 | 6 | wideband: too slow for most phones |
| 63 | Datawell Buoy HF Link | yes | Present, runs | searching, 4 cards | 8.0 | 0.25 |  |
| 64 | CODAR / HF ocean radar | yes | Present, working | locked, 9 cards, 44 rows | 36.0 | 2 |  |
| 65 | Multitone Paging | yes | Present, working | locked, 4 cards, 4 rows | 12.0 | 2 |  |
| 66 | NOAA Weather Radio SAME | yes | Present, working | locked, 7 cards, 1 rows | 12.0 | 2 |  |
| 67 | ANT+ sensors | yes | Present, runs | searching, 4 cards | 24.0 | 4 |  |
| 68 | DECT cordless phones | yes | Present, runs | searching, 4 cards | 40.0 | 5 |  |
| 69 | LoRa (raw) | yes | Present, working | locked, 7 cards, 34 rows | 8.0 | 2 |  |
| 70 | Wi-Fi networks | yes | Present, working | locked, 7 cards, 83 rows | 112 | 20 | wideband: too slow for most phones |
| 71 | Radiosonde | yes | Present, working | locked, 8 cards, 4 rows | 32.0 | 8 | wideband: too slow for most phones |
| 72 | Time signals | yes | Present, working | locked, 9 cards | 8.0 | 0.256 |  |
| 73 | Ionospheric sounders | yes | Present, runs | converging, 9 cards, 1 rows | 112 | 20 | wideband: too slow for most phones |
| 74 | Radio astronomy | yes | Present, runs | converging, 7 cards | 20.0 | 2.5 |  |
| 75 | FT8 | yes | Present, runs | searching, 4 cards | 4.0 | 2 |  |
| 76 | FT4 | yes | Present, runs | searching, 4 cards | 8.0 | 2 |  |
| 77 | FT2 | yes | Present, runs | searching, 5 cards, 6 rows | 8.0 | 2 |  |
| 78 | WSPR | yes | Present, runs | searching, 5 cards, 6 rows | 4.0 | 2 |  |
| 79 | JT65 | yes | Present, runs | searching, 5 cards, 6 rows | 4.0 | 2 |  |
| 80 | JT9 | yes | Present, runs | searching, 5 cards, 6 rows | 8.0 | 2 |  |
| 81 | Q65 | yes | Present, runs | searching, 5 cards, 6 rows | 8.0 | 2 |  |
| 82 | FST4 | yes | Present, runs | searching, 5 cards, 6 rows | 8.0 | 2 |  |
| 83 | MSK144 | yes | Present, runs | searching, 5 cards, 6 rows | 4.0 | 2 |  |
| 84 | PSK31 | yes | Present, working | locked, 5 cards, 4 rows | 24.0 | 2 |  |
| 85 | JS8Call | yes | Present, runs | converging, 4 cards | 24.0 | 2 |  |
| 86 | RTTY | yes | Present, runs | searching, 5 cards, 7 rows | 4.0 | 2 |  |
| 87 | CW | yes | Present, working | locked, 5 cards, 4 rows | 12.0 | 2 |  |
| 88 | SSTV | yes | Present, limited | searching, 5 cards, 7 rows | 8.0 | 2 | Pictures decode, but there is no picture view on the phone |
| 89 | HamDRM | yes | Present, runs | searching, 5 cards, 7 rows | 8.0 | 2 |  |
| 90 | Olivia | yes | Present, runs | searching, 5 cards, 7 rows | 8.0 | 2 |  |
| 91 | Contestia | yes | Present, runs | searching, 5 cards, 8 rows | 8.0 | 2 |  |
| 92 | MFSK | yes | Present, runs | searching, 5 cards, 8 rows | 8.0 | 2 |  |
| 93 | THOR | yes | Present, runs | searching, 5 cards, 8 rows | 4.0 | 2 |  |
| 94 | DominoEX | yes | Present, runs | searching, 5 cards, 8 rows | 8.0 | 2 |  |
| 95 | Hellschreiber | yes | Present, runs | searching, 5 cards, 8 rows | 8.0 | 2 |  |
| 96 | FreeDV | yes | Present, runs | searching, 5 cards, 8 rows | 8.0 | 2 |  |
| 97 | M17 | yes | Present, limited | converging, 8 cards, 14 rows | 20.0 | 2 | Voice needs a user-installed codec plug-in; the app cannot load one yet |
| 98 | D-STAR | yes | Present, limited | locked, 8 cards, 6 rows | 24.0 | 2 | Voice needs a user-installed codec plug-in; the app cannot load one yet |
| 99 | APRS (AFSK) | yes | Present, working | locked, 3 cards, 19 rows | 16.0 | 2 |  |
| 100 | LoRa APRS | yes | Present, runs | searching, 6 cards, 9 rows | 0.0 | 2 |  |
| 101 | MeshCom | yes | Present, working | locked, 7 cards, 10 rows | 12.0 | 2 |  |
| 102 | HF ALE | yes | Present, working | locked, 9 cards, 15 rows | 24.0 | 2 |  |
| 103 | HF beacons | yes | Present, runs | converging, 9 cards, 87 rows | 108 | 10 | wideband: too slow for most phones |
| 104 | Signal workbench | yes | Present, working | locked, 3 cards | 8.0 | 2 |  |
| 105 | General receiver | yes | Present, working | locked, 5 cards | 56.0 | 2 |  |
| 106 | Spectrum explorer | yes | Present, runs | converging, 9 cards | 20.0 | 10 | wideband: too slow for most phones |
| 107 | Spectrum analyser | yes | Present, working | locked, 8 cards | 52.0 | 20 | wideband: too slow for most phones |
| 108 | TV (auto) | missing | Present, limited | locked, 7 cards | 56.0 | 9.14 | Desktop auto-detect screen; opens the family receiver; wideband: too slow for most phones |
| 109 | Internet TV (IPTV) | missing | Missing | searching, 7 cards | 4.0 | 2 | Not on Android: needs the desktop screens |
| 110 | TV sat beacon decoder | missing | Present, working | locked, 11 cards, 102 rows | 52.0 | 2 |  |
| 111 | ISDB-S (BS) | missing | Present, limited | locked, 8 cards, 3 rows | 152 | 48 | No picture or TV sound: FFmpeg is stubbed out of the Android build; wideband: too slow for most phones |
| 112 | FPV video (analog) | missing | Present, limited | locked, 11 cards, 56 rows | 124 | 20 | No picture or TV sound: FFmpeg is stubbed out of the Android build; wideband: too slow for most phones |
| 113 | AM broadcast | missing | Present, working | locked, 5 cards | 32.0 | 2 |  |
| 114 | AM stereo (C-QUAM) | missing | Present, working | locked, 12 cards | 8.0 | 2 |  |
| 115 | Mode A/C | missing | Present, working | locked, 8 cards, 25 rows | 16.0 | 4 |  |
| 116 | VDL4 | missing | Present, working | locked, 6 cards, 19 rows | 8.0 | 2 |  |
| 117 | Marine VHF | missing | Present, runs | converging, 4 cards, 59 rows | 8.0 | 8 | wideband: too slow for most phones |
| 118 | SELCAL (aircraft tone call) | missing | Present, working | locked, 7 cards, 2 rows | 16.0 | 2 |  |
| 119 | ATIS (inland waterways) | missing | Present, working | locked, 5 cards, 6 rows | 16.0 | 1 |  |
| 120 | DGPS beacon | missing | Present, runs | converging, 2 cards | 8.0 | 0.96 |  |
| 121 | GK-2A LRIT | missing | Present, limited | locked | 44.0 | 2.4 | Pictures decode, but there is no picture view on the phone |
| 122 | GK-2A HRIT | missing | Present, limited | locked | 204 | 10 | Pictures decode, but there is no picture view on the phone; wideband: too slow for most phones |
| 123 | FY-4 LRIT | missing | Present, limited | locked | 20.0 | 2 | Pictures decode, but there is no picture view on the phone |
| 124 | FY-4A HRIT-II/III | missing | Present, limited | locked | 48.0 | 4 | Pictures decode, but there is no picture view on the phone |
| 125 | FY-2 S-VISSR | missing | Present, limited | locked | 52.0 | 2.4 | Pictures decode, but there is no picture view on the phone |
| 126 | COSPAS-SARSAT 406 MHz | missing | Present, working | locked, 7 cards, 1 rows | 20.0 | 0.256 |  |
| 127 | Argos | missing | Present, working | locked, 8 cards, 6 rows | 0.0 | 0.256 |  |
| 128 | UAT | missing | Present, runs | searching, 2 cards | 8.0 | 2 |  |
| 129 | FY-3 AHRPT | missing | Present, limited | locked, 9 cards, 1 rows | 188 | 6 | Pictures decode, but there is no picture view on the phone; wideband: too slow for most phones |
| 130 | FY-3 MPT | missing | Present, limited | locked, 9 cards, 1 rows | 152 | 20 | Pictures decode, but there is no picture view on the phone; wideband: too slow for most phones |
| 131 | PACTOR-II | missing | Present, runs | converging, 8 cards, 3 rows | 28.0 | 2 |  |
| 132 | PACTOR-III | missing | Present, runs | converging, 8 cards, 4 rows | 24.0 | 2 |  |
| 133 | PACTOR-IV | missing | Present, runs | converging, 7 cards, 1 rows | 12.0 | 2 |  |
| 134 | Digital voice (auto) | missing | Present, limited | locked, 12 cards, 32 rows | 12.0 | 2.4 | Desktop auto-detect screen; opens the family receiver |
| 135 | P25 Phase 2 | missing | Present, limited | searching, 4 cards | 4.0 | 2 | Voice needs a user-installed codec plug-in; the app cannot load one yet |
| 136 | Tone decoders (CTCSS, DCS, DTMF, selcall) | missing | Present, working | locked, 8 cards, 1 rows | 8.0 | 2 |  |
| 137 | MPT1327 trunking | missing | Present, runs | converging, 4 cards | 4.0 | 0.24 |  |
| 138 | LTR trunking | missing | Present, runs | converging, 5 cards | 4.0 | 0.48 |  |
| 139 | EDACS trunking | missing | Present, runs | converging, 4 cards | 4.0 | 0.48 |  |
| 140 | Sub-GHz sensors | missing | Present, working | locked, 9 cards, 8 rows | 4.0 | 0.25 |  |
| 141 | Sferics (lightning) | missing | Present, working | locked, 8 cards, 13 rows | 24.0 | 2 |  |
| 142 | BLE tracker detector | missing | Present, working | locked, 10 cards, 5 rows | 24.0 | 4 |  |
| 143 | Thread | missing | Present, working | locked, 10 cards, 89 rows | 76.0 | 4 |  |
| 144 | Wi-SUN FAN | missing | Present, working | locked, 10 cards, 86 rows | 184 | 20 | wideband: too slow for most phones |
| 145 | Wireless M-Bus (meters) | missing | Present, working | locked, 10 cards, 25 rows | 20.0 | 2 |  |
| 146 | NFC / RFID 13.56 MHz | missing | Present, working | locked, 5 cards, 82 rows | 108 | 4 |  |
| 147 | Z-Wave | missing | Present, working | locked, 11 cards, 90 rows | 12.0 | 2 |  |
| 148 | Sigfox | missing | Present, working | locked, 9 cards | 20.0 | 1.02 |  |
| 149 | e-Czas (225 kHz, Poland) | missing | Present, working | locked, 10 cards, 2 rows | 0.0 | 0.256 |  |
| 150 | Meteor scatter | missing | Present, working | locked, 10 cards, 7 rows | 20.0 | 2 |  |
| 151 | GOES DCS | missing | Present, working | locked, 11 cards, 9 rows | 20.0 | 1 |  |
| 152 | SBAS | missing | Present, working | locked, 10 cards, 18 rows | 40.0 | 4 |  |
| 153 | JT4 | missing | Present, runs | searching, 5 cards, 8 rows | 4.0 | 2 |  |
| 154 | MT63 | missing | Present, runs | converging, 7 cards | 12.0 | 2 |  |
| 155 | FSQ | missing | Present, working | locked | 4.0 | 2 |  |
| 156 | IFKP | missing | Present, working | locked | 8.0 | 2 |  |
| 157 | SITOR | missing | Present, working | locked, 4 cards | 4.0 | 2 |  |
| 158 | DME | missing | Present, runs | searching, 1 cards | 4.0 | 2.4 |  |
| 159 | eLoran / Loran-C | missing | Present, runs | converging, 4 cards | 4.0 | 0.5 |  |
| 160 | ASTERIX | missing | Present, runs | searching, 7 cards | 8.0 | 2 |  |
| 161 | ARDOP | missing | Present, runs | converging, 7 cards, 4 rows | 8.0 | 2 |  |
| 162 | Yaesu Fusion | missing | Present, limited | converging, 1 cards | 12.0 | 2 | Voice needs a user-installed codec plug-in; the app cannot load one yet |
| 163 | Shortwave | missing | Present, working | locked, 7 cards | 12.0 | 2 |  |
| 164 | LoRaWAN (all channels) | missing | Present, working | locked, 9 cards, 13 rows | 64.0 | 4 |  |
| 165 | LoRa satellites | missing | Present, working | locked, 10 cards, 3 rows | 20.0 | 2 |  |
| 166 | PSK demodulator (experimental) | missing | Present, working | locked, 10 cards, 13 rows | 24.0 | 2 |  |
| 167 | HF waveform ID | missing | Present, working | locked, 7 cards | 4.0 | 2 |  |
| 168 | PACTOR-I (experimental) | missing | Present, working | locked, 10 cards, 11 rows | 8.0 | 2 |  |
| 169 | ROS (HF) | missing | Present, runs | searching, 4 cards | 12.0 | 2 |  |
| 170 | SSDV pictures | missing | Present, limited | locked, 6 cards, 5 rows | 24.0 | 2 | Pictures decode, but there is no picture view on the phone |
| 171 | EBICab / Eurobalise (needs a user test) | missing | Present, runs | converging, 7 cards, 80 rows | 68.0 | 10 | wideband: too slow for most phones |
| 172 | Inmarsat (all carriers) | missing | Present, working | locked | 60.0 | 6 | wideband: too slow for most phones |
| 173 | nRF24L01 (needs a user test) | missing | Present, working | locked, 11 cards, 146 rows | 52.0 | 8 | wideband: too slow for most phones |
| 174 | Flipper Zero Sub-GHz chat | missing | Present, working | locked, 10 cards, 16 rows | 8.0 | 2 |  |
| 175 | Mode Builder | missing | Missing | searching, 7 cards | 8.0 | 2 | Not on Android: needs the desktop screens |
| 176 | Plugins | missing | Missing | searching, 7 cards | 4.0 | 2 | Not on Android: needs the desktop screens |
| 177 | KAAL | missing | Present, working | locked, 12 cards | 12.0 | 0.24 |  |
| 178 | KAVB | missing | Present, working | locked, 8 cards, 32 rows | 116 | 2.4 |  |
| 179 | KAIT | missing | Present, runs | converging, 1 cards | 20.0 | 1 |  |
| 180 | KAPP | missing | Present, working | locked, 11 cards, 3 rows | 24.0 | 0.25 |  |
| 181 | KAFT | missing | Present, working | locked, 11 cards, 1 rows | 40.0 | 2.4 |  |
| 182 | KAWS | missing | Present, runs | converging, 5 cards | 12.0 | 2 |  |

