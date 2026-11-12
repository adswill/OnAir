# OnAir 4.0

OnAir 4.0 adds 128 modes (464 rows in MODES.md, 462 in the app's mode list, which has no row for Plugins or Mode Builder). 3.9 had 336 rows, not the 333 its notes said: the list generator skipped KLW, TDCS and Horus balloons, which were in the app all along. It brings a new General receiver screen, a Workspace layout, one Ham station screen, new transmit screens, a propagation calculator on ITU-R models, a satellite tracker with a world map, a sharper zoomed waterfall, a community plugin marketplace, better sensitivity in many decoders, and fixes from your reports.

## Downloads

| System | File |
|---|---|
| macOS 15 or later, Apple silicon | `OnAir-4.0.0-macos-arm64.dmg` |
| macOS 15 or later, Intel | `OnAir-4.0.0-macos-x86_64.dmg` |
| Windows 10 / 11, 64-bit | `OnAir-4.0.0-windows-x64-setup.exe` or `OnAir-4.0.0-windows-x64.zip` |
| Linux x86-64 | `onair_4.0.0_amd64.deb`, `OnAir-4.0.0-linux-x86_64.AppImage` or `onair-4.0.0-linux-x86_64-portable.tar.gz` |
| Linux arm64 | `onair_4.0.0_arm64.deb`, `OnAir-4.0.0-linux-aarch64.AppImage` or `onair-4.0.0-linux-aarch64-portable.tar.gz` |
| Android 9+, arm64 | `OnAir-4.0.0-android.apk` |
| Checksums | `SHA256SUMS` |

Install notes are in the README. Questions, recordings and reports: the Discord at https://discord.gg/ptxkzCuzR or GitHub issues.

**Measured** numbers come from OnAir's own generated test signals, not from over-the-air recordings, unless a line says so. Read **Known limits** at the end before relying on a new feature.

## Highlights

- 128 new mode-list entries: 52 new receivers (six HF data modes from Rivet, DCR and 45 measurement and monitor modes) and 76 entries that show one type of an existing receiver (sensor brands, radiosonde types, AIS station kinds, time stations, analog TV standards and more).
- Lower thresholds in many decoders, from 1 dB to about 18 dB; the table is below.
- New General receiver screen, Workspace canvas, Ham station screen, transmit screens, propagation calculator and satellite tracker.
- Zoomed in, the waterfall is now sharp: a zoom FFT of the visible span and a pixel-exact waterfall.
- Settings 4.0: a key editor for every shortcut, profiles, about 70 new settings, hover previews of the looks, grouped menus.
- Plugin marketplace, live at plugins.on-air.dev.
- Radiosonde landing predictor, SondeHub upload, HFDL nearby ground stations and a busiest-band search.
- The General receiver's NFM audio filter is now 3 kHz (it was 4 kHz): NFM sounds less bright and less noisy.

## Sensitivity

Threshold = the lowest SNR at which 90 % of seeded trials decode, unless the line says 50 %. All figures are from **simulated** signals (AWGN, and the TU6, echo and carrier-error channels named in the line), not recordings, measured on the GitHub runners. Gains from separate passes on the same receiver were measured one at a time; do not add them. Noise-only runs after each change gave no false decodes.

| Mode | 3.9 | 4.0 | Notes |
|---|---|---|---|
| Argos PTT-A2 uplink | 13 dB | about -5.5 dB | in 5 kHz; the burst gate needed 6 dB over the noise of the whole 32 kHz stream (docs/SENSITIVITY-4.0-A2.md, -B2.md) |
| NDB | 14 dB | 9 dB | synchronous tone detection; reading the ident from the average of its repeats: 50 % point 6.5 to 4.5 dB in 300 Hz (-X1.md) |
| ISDB-T, TU6 | 30 dB | 21 dB | time-interpolated channel estimate; echo, phase noise and AWGN 1 to 2 dB better (-X3.md) |
| ATSC 3.0, TU6 | 16 dB | 6 dB | channel state in the data and L1 soft bits; AWGN unchanged (-X3.md) |
| ATSC 1.0, A/74 echo -30 / -15 / -10 us | none | 17 / 18 / 21 dB | pre-echo canceller 9.3 to 30.7 us; +10 us echo 31 to 27 dB; AWGN 15 dB unchanged (docs/ATSC-4.0.md) |
| Marine VHF DSC | 4 / 6 dB | 2 dB | 4.5 kHz filter with carrier follow-up (-B2.md) |
| MPT 1327 | 9 dB | 6 dB | narrow filter that follows the carrier; decodes with 5 to 6 kHz carrier error (3.9: nothing) |
| Z-Wave | 9.5 dB | 6 dB | chips from tone energies, more sensitive burst detector (-B.md) |
| POCSAG | -1 dB | -3 dB | tone-energy symbols; a second POCSAG-only decoder behind a 7 kHz filter adds about 1.3 dB (-B.md, -A3.md). FLEX unchanged |
| ISM sensors (50 % point) | 6.5 dB | 3 dB | narrow-envelope retry for OOK, tone-energy chips for FSK (-B3.md) |
| Radiosonde RS41 | 9 dB | 7 dB | 4 kHz narrow filter; 9 to 6 dB with a 20 ppm carrier error or 150 kHz off-centre (-A.md) |
| RS92, DFM, iMet-54 | | about 2.5, 2, 3 dB better | same narrow path; iMet-54 about 2.3 dB more from a new slicer threshold (-A2.md, -A4.md) |
| MRZ, iMS-100 | | about 2 + 4.5 dB, about 3 dB | MRZ kept losing its serial at every pause: 7 times the frames; new slicer threshold (-A3.md, -A4.md) |
| Radiosonde mix (RS41 / RS92 / DFM / M10 / M20) | 11 dB | 9 dB | symbols decided by tone energies (-B.md) |
| COSPAS-SARSAT | | about 4 dB better | single burst; repeat combining about 0.5 dB more; 3.9 gave wrong identifications at its edge, 4.0 none measured (-X1.md) |
| Globalstar, HaLow | | about 3.5, 1.5 dB better | activity and preamble gates lowered (-A3.md) |
| Zigbee | | about 2 dB better | amplitude-weighted phase-step chips (-A3.md) |
| SAME, CubeSat 1200 Bd | | about 3 dB better each | own 4.5 kHz channel (-A3.md) |
| CubeSat NGHam / Mobitex, 9600 Bd framings | | about 2 dB, 0.3 - 0.9 dB better | narrow 9600 / 4800 paths; U482C works with a carrier error again (-X3.md) |
| EDACS, SmartNet, Yaesu Fusion | | about 1.5, 1.7, 2.5 dB better | narrow carrier-following filters (-A2.md) |
| LTR | nothing at 3.1 kHz carrier error | decodes | 3.5 kHz filter with carrier follow-up (-A3.md) |
| TETRAPOL | | about 0.6 dB better | 5.8 kHz channel filter |
| SSTV | | | picture filter: Martin M1 at 6 dB mean error 20.2 to 6.6 levels, Robot 36 at 10 dB 27.0 to 12.3 (-X2.md) |
| Weather fax, NOAA APT | | | at 2 dB: fax row correlation 0.46 to 0.63, APT PSNR 10.2 to 12.8 dB (-X2.md) |
| FT8, JT4 | | | wrong-text decodes from a-priori candidates and all-zero words removed (-B2.md, -B3.md) |

Also measured:
- **Test corrections, no receiver change**: the Aero test generator had its own noise built in; the true 90 % point is -1 dB, not 11 dB. The NAVTEX / SITOR-B rows were 5 dB off for the same reason, and the 50 ppm clock rows were trial-length artefacts (docs/SENSITIVITY-4.0-B3.md, -C1.md).
- **At theory, unchanged**: DMR, AIS, APRS, Orbcomm, LoRa SF7 to SF12, AX.25 9600, ALE, Wireless M-Bus, FM RDS, DAB, DVB-T / T2 / C, DTMB, DVB-S2, ATSC 1.0 in AWGN.
- **Tried and not kept**: NFM click removal (0.3 dB at most), time-station a-priori minutes (no gain), soft RS erasures for the RS41, the M10 narrow path, several gate changes (Iridium, Wi-SUN, BLE, VDL2).

## New modes

All receive only. They were tested on simulated signals only: no recording of these signals was available. The full list is in MODES.md, generated from the app's mode table.

- **HF data modes from Rivet** (docs/RIVET-MODES-4.0.md): XPA (10 / 20 Bd), XPA2, CROWD36, FSK200/500 (F01), FSK200/1000 (F06, F06a), GW FSK. 90 % points in 3 kHz: XPA -12.6 dB (10 Bd), XPA2 -14.5, CROWD36 -5.4, F01 +1.3, F06 -0.3, GW FSK +5.0. Protocol tables, framing and CRCs ported from Rivet (GPL-3.0-or-later) with its notices.
- **DCR** (docs/DCR-4.0.md): Japan's digital convenience radio, ARIB STD-T98 volume 3: user codes, call types, call sign, data blocks; voice frames are counted, not decoded. Channel plan of the standard and a scan of the active channels. The standard's test frames decode bit for bit.
- **Measurement and monitor modes (x40)** (docs/X40-4.0.md and one page per mode):
  - Broadcast: FM modulation monitor (deviation, MPX, pilot, RDS, SCA), AM modulation monitor (depth, peaks, hum).
  - Aviation: radar pulse analyser, ILS marker beacons (75 MHz), SSR interrogations (1030 MHz).
  - Satellite and GNSS: GNSS interference detector, satellite Doppler curve logger.
  - IoT: RC model PPM (72 MHz), EM4100 LF RFID (125 kHz), UHF RFID reader commands (EPC Gen2; tag replies not decoded), Qi wireless charging packets.
  - Science: GRAVES meteor echoes, Radio Jove bursts (a burst list next to the 3.9 Radio Jove entry), beacon propagation monitor, fading analyser (with the S4 scintillation index).
  - HF: UVB-76 buzzer monitor, VLF MSK transmitter monitor (no traffic decoded), FSK modems (Bell 103, V.21, V.23, Bell 202), passband signal classifier.
  - Tools: power-line noise locator, spectrum occupancy monitor, MER meter, carrier frequency accuracy meter, PTT and duty-cycle logger, intermodulation finder, spur and image finder, frequency-hopper analyser, birdie finder, burst finder, SDR input health meter, phase noise meter, SSB transmitter test meter, FM SINAD meter, FSK parameter analyser, noise figure (Y-factor) meter, CW keying quality meter, noise floor logger, NFM deviation and CTCSS meter, AFSK tone tuner, OOK / ASK pulse analyser, transmitter frequency stability meter, crest factor (PAPR / CCDF) meter, occupied bandwidth meter, spectral kurtosis RFI finder, SDR stream glitch detector.
- **Entries for one type of an existing receiver** (docs/VIEWS-4.0.md, docs/SENSORVIEWS-4.0.md): no decoder changed, the entry shows only that type. 22 Sub-GHz sensor brands and kinds (Oregon Scientific, Acurite, LaCrosse, Fine Offset / Ecowitt, Bresser, tyre pressure sensors, meters, remotes and others), ten radiosonde types, nine Wireless M-Bus modes and manufacturers, three CubeSat framings, six AIS station kinds, eleven time stations, four GNSS systems (GPS, QZSS, GLONASS, BeiDou), eight analog TV standards and three NFC protocols.

## New features

- **General receiver** (docs/GENERAL-4.0.md): the spectrum and waterfall fill the window; one resizable sidebar (Ctrl+B) with folding sections; a demodulator grid (NFM WFM AM SAM DSB USB LSB CW CW-L RAW; DSB, RAW and CW-L are new); the VFO can be dragged by its body and edges; squelch, AGC and gain are kept per demodulator. The 3.9 layout stays under Display > 4.0 General layout.
- **Workspace** (docs/WORKSPACE-4.0.md): View > Layout > Workspace, a blank canvas of blocks (source, spectrum, demodulator, audio, decoder, map, tracker, rotator, transmitter, recorder, log and others) that you move, resize, tab and link; named workspaces, export / import, four templates. Classic and Unified stay the default.
- **Panels sized to their content** (docs/LAYOUT-4.0.md): in the sidebar layout an empty lower row is hidden, small plots and short lists move to where they fit. View > Fit panels to their content.
- **Ham station** (docs/HAM-4.0.md): one screen with spots (cluster, RBN, POTA, SOTA, PSK Reporter), callbook (QRZ, HamQTH), needed and alert lists, activation and log tools (Cabrillo, LoTW, eQSL), band memory per mode, CW speed matching, fake-it split with a CAT rig; FT8 / FT4 Field Day and RTTY Roundup exchanges; Q65, FST4 and MSK144 in the slot sequencer. Every network feed is off by default.
- **Transmit screens** (docs/TX-4.0.md): the panel is drawn from one parameter table of every transmit mode; Send to radio, dummy load or IQ file; ARM then TRANSMIT, STOP always visible; a rig's PTT can be keyed while the SDR transmits. 66 controls that did nothing were removed or fixed, missing ones added. Link budget, ramp and SWR notes.
- **Propagation** (docs/PROPAGATION-4.0.md): ITU-R P.1812-8 for VHF and up (matches the 21 x 3 ITU-R validation results), P.676 gases, P.838 / P.530 / P.618 rain; HF sky wave after P.533-14 with an hours x bands chart; satellite links; "Will it decode?" from the measured mode thresholds; terrain profile with the Fresnel zone, opt-in SRTM tile download; antenna and cable presets.
- **Satellite tracker** (docs/TRACKER-4.0.md): View > Passes has a world map with orbits, footprints and day / night, a satellite search with groups (Iridium added), a pass panel with countdown, Doppler, sky plot and "Track and record this pass".
- **Mode browser** (docs/MODELIST-4.0.md): each category tinted with its colour, a clearer selected row; the Mode menu has one submenu per category.
- **Waterfall and spectrum** (docs/WATERFALL-4.0.md): zoomed in, 3.9 stretched coarse bins over many pixels and blurred them. 4.0 runs a zoom FFT of the visible span (about 78 dB alias rejection), draws one texel per screen pixel, and traces the zoom bins; display windows (Hann, Blackman-Harris, Nuttall, flat top, rectangular), FFT up to 1M points, a resolution readout. The 3.9 look stays as "Soft zoom". The Zoom slider and keys zoom around the tuned frequency.
- **Settings** (docs/SETTINGS-4.0.md): every shortcut in one key editor (two keys per action, conflicts marked, press to record, import / export); pages Units, Keyboard and mouse, Notifications, Performance, Profiles; about 70 new settings; profiles saved, switched and exported. 3.9 settings keep their keys. **Hover previews** (docs/UI-PREVIEW-4.0.md): hovering a palette, layout, font, text size or waterfall colour shows a miniature of the interface before you pick it. **Menus** (docs/MENUS-4.0.md): the Tools menu in 9 groups with search, View down to 9 entries. **Status bar** (docs/STATUSBAR-4.0.md): one flat row; one action row above each mode's tabs.
- **Plugin marketplace** (docs/PLUGIN-MARKETPLACE-4.0.md): the Store tab has a Community marketplace, live at plugins.on-air.dev, next to the reviewed registry of 3.9. Search, sort, tags, likes, reports and upload with GitHub sign-in. Community plugins are not reviewed and are marked so; installing asks twice. Listings are signed; withdrawn plugins are switched off.
- **Map badges** (docs/MAP-BADGES-4.0.md): GitHub sign-in for the live map, per-user statistics and 19 badges; the Badges window and a toast in the app. Posts are only signed in while Earn badges is on; signed-out posts are unchanged. No IP or position is kept.
- **Radiosonde landing predictor** (docs/SONDE-PREDICT-4.0.md): ascent winds, a fall rate fitted to the descent, a drift filter for the final approach, the terrain stop with the EGM96 geoid, opt-in GFS (Open-Meteo) or Tawhiri winds, and an ellipse of 50 / 95 % radii on the map. Accuracy below.
- **SondeHub** (docs/SONDEHUB-4.0.md): upload to SondeHub (off by default; only good frames with a fix, the sonde's own time and a serial). Launch sites are named from the IGRA 2 list instead of "Site 1"; the map shows your station or a chase car from a live GPS, and an optional online prediction drawn as one.
- **HFDL** (docs/HFDL-STATIONS-4.0.md): "Follow the best band" stayed on the first band that gave anything; it is now "Find a working band", plus a new "Busiest band" that surveys each band for 75 s and stays on the best. New Nearby view: ground stations nearest first with distance, bearing and their active frequencies as tune buttons. The hfdl.observer list is opt-in and off in Offline mode.
- **NFM audio filter**: the General receiver's NFM audio low-pass is 3 kHz instead of 4 kHz. This changes how NFM sounds: about 3.7 dB less noise, speech up to about 2.7 kHz keeps its level, less treble above it. WFM, AM, SSB and CW are unchanged.
- **Per-mode cards** (docs/FEATURES-4.0-A.md to -E.md, docs/WIRING-4.0.md): DVB-S blind scan list, modcod fit and dish pointing, LNB drift; FM station names and DX opening alarm; AM carrier offset, best sideband and station ID from a schedule file; DAB service ranking and transmitter table; ADS-B replay and holding patterns; AIS coverage by bearing; ACARS channel load; talkgroup activity and names for P25, NXDN and TETRA; pager capcode groups; COSPAS-SARSAT incidents; FT8 heard stations and clear frequency; CW heard list; ATIS letter tracker; HF beacon best frequency; sweep activity map and new-signal alert, and others. Some helpers are not wired into the screens yet; WIRING-4.0.md lists them.
- **Android** (docs/ANDROID-4.0.md): MPEG layer II sound in the app, Airspy libraries in the APK, all 42 RTL2832U USB ids of librtlsdr. See Known limits.

### Radiosonde landing predictor accuracy

Median landing error, simulated (137 flights; the forecast is the true wind with its fine structure removed and a 2 m/s error):

| Sonde heard down to | 10 km | 2 km | 500 m | 200 m | last frame |
|---|---|---|---|---|---|
| 3.9 (ground at sea level) | 17.6 km | 1.7 km | 1.2 km | 946 m | 1.1 km |
| 3.9, true ground | 14.0 km | 1.1 km | 308 m | 213 m | 366 m |
| 4.0 offline | 2.1 km | 477 m | 112 m | 46 m | 116 m |
| 4.0 with forecast | 1.2 km | 402 m | 151 m | 51 m | 121 m |

Three real RS41 flights from SondeHub (offline, no forecast): from 2 km 0.8 - 4.6 km (3.9) to 0.5 - 0.85 km; at the last frame 12 to 27 m. A 20 m prediction is not reached in general. Network winds are one opt-in switch, off in Offline mode.

## Performance

docs/PERF-4.0.md. Four reports from users with fast PCs after 3.9:
- "This computer is too slow" at 3 % CPU: a network radio's reconnect counted as a ring overflow, and every overflow was called a slow computer. Losses at the source are kept apart now, and each overflow gets its cause; only a real CPU shortage says "lower the sample rate".
- "USB 3 port" warnings at low sample rates: the delivered rate read 78 % at 250 ksps. It is now measured from the radio's own write stamps: 99 - 101 % at every rate from 48 kHz to 20 Msps.
- nRSP-ST (SDRconnect): messages over 1 MiB were refused and the connection dropped above a few Msps. The limit is 16 MiB, with buffered reads; 10 Msps paced with no drop on the CI runners.
- The receiver runs on its own thread behind the front end at 4 Msps and up on 4+ cores (not in Low power mode; Settings > Advanced): 1.0 - 1.45 times the rate the receive path sustains, same results.
- ATSC 1.0 lag and stutter: a deeper field queue, only the oldest field dropped, the equaliser kept, the packet callback on its own thread.
- Zoom spectrum mixer vectorised.

## Fixes

- Reports: #479 (DCS codes on transmit: OnAir sent D023N as D411N; the transmitter now uses the decoder's table), #484 (ATSC 3.0: signed and gzip-compressed service signalling, signed LLS tables, compressed-IP warning; the two recordings in the issue were not tried yet), #437 / #481 (Japanese and Chinese text mixed fonts and sizes in one line).
- #463 (CDR on Android locked but played no sound; docs/ISSUES2-4.0.md): the app never chose a CDR service to play, and CDR sound needs the DRA+ add-on, which the phone had no way to load. The receiver now plays the first audio service by default, and CDR has a **Sound** tab that says whether the add-on is installed, with Import add-on (.so) and Remove add-on and the service list. You build `libonair_dra.so` for arm64 yourself (docs/CODECS.md, "DRA+ on Android"); OnAir does not ship it.
- #435 (DTMB on Android; docs/ISSUES2-4.0.md): the reported crashes match early returns inside six Compose panels, which are now plain if / else; this is the most likely cause, not a proven one. "No channels, no parameters" came from the SpyServer link over Wi-Fi losing 0.6 to 2.8 s every 10 s: missing SpyServer IQ messages are now filled with silence of their length and counted as lost, and the DTMB receiver coasts over such gaps (simulated Wi-Fi link case: 0.92 of the packets with the lock found once, was 0.46 with the lock lost 11 times). DTMB on the phone shows the frame header, system information, LDPC and offsets like the desktop, and every mode shows cards and an amber line when the network link loses signal.
- #442 (bug list; docs/ISSUES2-4.0.md): the FM right panel no longer jumps when RDS rows come and go; the Mode A/C full-screen button opens the map; Tools > Images > Choose... and every folder Choose... open a folder picker on Windows; the OnAir icon shows on the Windows title bar and taskbar. The wheel zoom crash and the tiny dB axis were already fixed in 3.7.
- The previous mode's last report came back after a mode switch; HF ID and End of train counted noise as decodes; VDL Mode 3 and lightning position stayed idle through the engine; the UAT Aircraft, FIS-B, Radar and Log views did not respond to clicks; a weak Robot 36 picture crashed the SSTV decoder; the ATSC screen showed the DVB-T plots while searching; F6 and Ctrl+L fired twice in the FT8 screen; the LMS6 entry did not tune 1680 MHz; a TVCT entry without a program was listed as a service; JT6M false pings in noise; Fleet Broadcast locked only through an alias; the UMTS test signal was wrong at any rate but 7.68 Msps.
- QA sweep (docs/QA-4.0.md): a non-default sample rate and bandwidth condition, noise-only runs for modes without a generator, one engine through every mode twice (no hang, no stale report, no growth; ASan and UBSan clean).

## Contributors

Thanks to everyone who reported the issues named above, and to the Discord users whose reports led to the performance fixes, the HFDL band search and the Rivet modes. External pull requests since 3.9: #481 (Japanese font sizes, by bellx2) and #483 (LoRa bank: the last block at the end of a file, by Streetblock). The full list is in CONTRIBUTORS.md.

OnAir's own code carries one attribution term under GPL section 7(b) (see ADDITIONAL-TERMS.md): keep the notices and the line "Based on OnAir by Kristijonas Adomaitis - https://on-air.dev" in a program that contains it. New third-party material of 4.0 is credited in THIRD_PARTY.md and in the in-app credits.

## Known limits

Please read these before relying on a new feature.

- **Almost everything new was tested on simulated signals only.** This holds for all 52 new receivers (the Rivet modes, DCR and every measurement and monitor mode), the 76 type entries, every sensitivity figure above, the landing predictor's simulated table, the HFDL band search, the ham tools (no spot, callbook or log service was contacted) and the transmit screens. If you have a recording, please share it.
- **Not decoded**: DCR voice (frames counted only), UHF RFID tag replies, VLF MSK traffic. Encrypted content is never decrypted.
- **Transmit has not been tried on the air.** The parameter model test checked that every row changes the signal; loopback tests: tx_loopback_tv 10/10, tx_loopback_rest 27/28 (Olivia 8 / 500 info).
- **Map badges**: the server side was tested with local tests only; badges appear once map.on-air.dev runs the 4.0 hub.
- **Plugin marketplace**: live, but community plugins are not reviewed. Install only what you trust.
- **Per-mode cards**: some 4.0 helpers are in the core with tests but not on a screen yet (docs/WIRING-4.0.md). The new strings are translated to German by machine only; other languages show English.
- **Screens built and tested on GitHub only**: the General receiver, Workspace, layout, settings, previews, tracker and waterfall were built and their logic tested on GitHub runners; they were not checked by screenshots on every platform.
- **#463, #435, #442 fixes** were checked on GitHub runners only: no phone, Windows desktop, CDR or DTMB signal was used. DRA sound of DTMB services and AVS+ pictures on the phone are still open.
- **Tests**: some tests on the Windows CI shards still have load-dependent timing flakes, as in 3.9.

### Next

- **ATSC 3.0 #484**: run the two recordings from the issue.
- **Android TV and USB radios**: the TV picture and sound panel (the phone's own decoders) and the Airspy / RTL-SDR USB additions are in this build but have not run on a phone, an emulator or a real radio; testing them is next. LimeSDR Mini, bladeRF, PlutoSDR, HydraSDR and RX888 over USB are not added.
- **Codecs folder**: one folder for user-installed voice codecs.
- **Spectran V6** support.
