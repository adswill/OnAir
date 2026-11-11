# OnAir 3.9

OnAir 3.9 adds 39 modes (333 rows in MODES.md, 294 in 3.8; 331 in the app's mode list, which has no row for Plugins or Mode Builder). It adds OnAir Web, a full receiver in the browser, plus Winlink and EchoLink clients, single-radio trunking, many smart features (insights, boards, alerts, uploads), better sensitivity and lower CPU in many decoders, and fixes from your reports.
## Downloads

| System | File |
|---|---|
| macOS 15 or later, Apple silicon | `OnAir-3.9.0-macos-arm64.dmg` |
| macOS 15 or later, Intel | `OnAir-3.9.0-macos-x86_64.dmg` |
| Windows 10 / 11, 64-bit | `OnAir-3.9.0-windows-x64-setup.exe` or `OnAir-3.9.0-windows-x64.zip` |
| Linux x86-64 | `onair_3.9.0_amd64.deb`, `OnAir-3.9.0-linux-x86_64.AppImage` or `onair-3.9.0-linux-x86_64-portable.tar.gz` |
| Linux arm64 | `onair_3.9.0_arm64.deb`, `OnAir-3.9.0-linux-aarch64.AppImage` or `onair-3.9.0-linux-aarch64-portable.tar.gz` |
| Android 9+, arm64 | `OnAir-3.9.0-android.apk` |
| Checksums | `SHA256SUMS` |

Install notes are in the README. Questions, recordings and reports: the Discord at https://discord.gg/ptxkzCuzR or GitHub issues.


**Measured** numbers come from OnAir's own generated test signals, not from over-the-air recordings, unless a line says so. Read **Known limits** at the end before relying on a new feature.


**Measured** numbers come from OnAir's own generated test signals, not from over-the-air recordings, unless a line says so. Read **Known limits** at the end before relying on a new feature.


**Measured** numbers come from OnAir's own generated test signals, not from over-the-air recordings, unless a line says so. Read **Known limits** at the end before relying on a new feature.

## Highlights

- 39 new modes: HF military and CIS data modes, ham science event monitors, DVB-SH, DVB-C2, ISDB-TSB, CubeSat framings, cellular broadcast cells and others. The list is in MODES.md, generated from the app's mode table.
- OnAir Web: every mode with a live panel in a browser, from the headless server (docs/WEB-3.9.md).
- Winlink email (packet, ARDOP, Telnet, or an ARDOP / VARA / Mercury modem program you run) and an EchoLink client. Both were tested against mock servers only.
- One radio for a trunked system: the control channel and several voice calls at once.
- Dolby Atmos detection with 5.1 / 7.1 output, and an experimental bitstream passthrough (not tested on real hardware).
- Sensitivity and CPU work in many decoders, with the figures below.

## New modes

All of these are receive only. Unless a line says otherwise they were tested on simulated signals only: no recording of these signals was available.

- **TV**: ISDB-TSB (narrow-band ISDB, one or three segments), DVB-SH (S-band mobile TV, OFDM and TDM forms), DVB-C2 (needs the cable network as input, not an antenna). Details: docs/BCAST-3.9.md, docs/DVBSH-3.9.md, docs/DVBC2-3.9.md.
- **Aviation**: VDL Mode 3 (burst receiver), ELT 121.5 MHz (swept-tone detection). Details: docs/AVIA-3.9.md, docs/W1A-3.9.md.
- **Satellites**: AX100 Mode 5, AX100 Mode 6, USP, NGHam, U482C and BEESAT (Mobitex) as their own entries (they were decoded before inside the CubeSat mode); Meteosat HRDCP data collection platforms; Fleet Broadcast (UHF SATCOM, framing and channel activity only, nothing is decoded). Details: docs/W1C-3.9.md, docs/SAT-DCS-XRIT-3.9.md, docs/FBR-3.9.md, docs/CUBESAT-3.9.md.
- **Two-way radio**: voice inversion scrambler (detection and descrambling). Details: docs/W1A-3.9.md.
- **Cellular**: CDMA2000 cells (pilot PN, SID / NID, system time) and TD-SCDMA cells (cell parameter from the SYNC-DL code; the broadcast channel is not decoded). Details: docs/CELL-3.9.md.
- **IoT and radio control**: Futaba S-FHSS and Spektrum DSM2 / DSMX link monitors (2.4 GHz, need a fast SDR). Details: docs/IOTRC-3.9.md.
- **Ham science and weak signal**: JT6M, Schumann resonance monitor (needs an ELF sensor input), whistler and tweek detector (needs VLF audio), Moon echo (EME) test, Radio Jove, e-CALLISTO style solar bursts and an aurora / Es / TEP beacon event log; CP16 (Chinese characters on 16 carriers, CRAC; request #474). Details: docs/MISC-3.9.md, docs/W1B-3.9.md, docs/CP16-3.9.md.
- **Lightning**: Lightning position (network): strikes located from several stations' arrival times. Details: docs/LIGHTNING-POSITION-3.9.md.
- **Amateur packet**: HF packet 300 Bd (AX.25 frames).
- **HF utility**: STANAG 4539, 3G-ALE, Link 11 CLEW (message detector and frame structure, no data), STANAG 4481 FSK, CIS-36-50 / 50-50 (BEE), CIS-14, CIS-12 (MS5). CIS-36 (MFSK), CIS OFDM HDR, CIS-128 and CIS-3000 are **identification only**. SITOR also reads ARQ links (SITOR-A / AMTOR) now. Details: docs/HF-MIL-3.9.md, docs/CIS-HF-3.9.md.
- Existing modes grew: Airband takes 225 - 400 MHz (military UHF); CubeSat telemetry decodes five more framings and sets itself up from the gr-satellites satellite table; DVB-T has the DVB-H layer (4K mode, MPE, time slicing, MPE-FEC) and writes GSE / MPE datagrams to pcap or UDP; DVB-S works down to 10 ksym/s; GSM-R reads GSM 850 / PCS 1900 and SI2bis neighbours; LTE-M MIB fields; Wireless M-Bus reads Wize and mode N at 2.4 kbit/s; Chayka chains are named in eLoran.

## New features

- **OnAir Web** (docs/WEB-3.9.md, docs/WEB-SCHEMA-3.9.md): the receiver in a browser, with a live panel for 290 of 295 rows, spectrum and waterfall with drag-to-tune and zoom, a mode picker, radio controls, recording, audio, settings in plain words, and an access layer (own network, login, or the relay). Panels are generated from one schema per mode. TV video and TETRA speech are not in the browser yet.
- **Winlink email** (docs/WINLINK-3.9.md): B2F sessions over OnAir's own packet modem (1200 or 9600 baud), its ARDOP modem, or Telnet to the CMS (off until you turn it on); inbox, outbox, compose, gateways, peer to peer. **External HF / VHF modems** (docs/HFMODEMS-3.9.md): ARDOP (ardopcf), VARA HF, VARA FM and Mercury over TCP. OnAir does not include, start or reimplement any of them; VARA is closed software with its own licence.
- **EchoLink client** (docs/ECHOLINK-3.9.md): Tools > EchoLink, with an RF link and a conference host. Uses the GSM 06.10 codec (a free specification) and EchoLink packet formats; credits are in THIRD_PARTY.md.
- **Single-radio trunking** (docs/TRUNK1-3.9.md): voice slots (4 by default) cut out of one wide capture, so Type II, EDACS and MPT 1327 follow the control channel and several calls at once; a call outside the capture shows the bandwidth it needs. P25 uses the same slices for the follower.
- **Dolby Atmos** (docs/ATMOS-3.9.md): the player shows the format of AC-3 / E-AC-3 tracks (Atmos flag, 5.1 or 7.1 bed) and AC-4 in ATSC 3.0 and DVB; Settings > Audio chooses stereo, Lt/Rt, 5.1, 7.1 or automatic. **Bitstream passthrough** (IEC 61937 to an HDMI or optical receiver) is experimental and off by default. OnAir does not render Atmos objects.
- **Lightning position**: several OnAir stations time one sferic and a server locates the strike (TDOA), with an error ellipse on the map. Roles: this station only, client, server.
- **Radiosondes**: upload to APRS-IS aggregators such as wettersonde.net and radiosondy.info (docs/SONDE-UPLOAD-3.9.md); flight track as KML and GPX; launch sites and landing map; sounding views and landing prediction before burst (docs/SONDE-TRACK-3.9.md, docs/SONDE-SITES-3.9.md).
- **TMC in plain words** (docs/TMC-PLAIN-3.9.md, #476): import your own TMC location files; messages show event, road, location, direction and extent; a CSV and text log.
- **HDMI / display emission finder** (docs/HDMI-EMISSION-3.9.md, #475): finds the pixel clock and line rate of a monitor or HDMI cable in the spectrum and the explorer.
- **DAB map** (docs/DABSITES-3.9.md): the map zooms, the shared transmitter list is chosen by the ensemble's country, unplaced rows say why, and qt-dab's own transmitter list can be read from your computer.
- **Smart features, round 4** (docs/R4-PER-MODE-3.9.md): APRS weather stations and messages; sensor names, Home Assistant devices and alerts, long history; ADS-B filter and search, overhead log, arrivals and departures board, theoretical range outline, plane-alert-db lists; AIS arrivals board; ADS-B and AIS history over days; airband clip per transmission; FM and DAB played list with artist, word and traffic alerts; FT8 / WSPR band hopping, "when is the band open", ham.decode alerts; Meshtastic channel links, mesh node watch, a receive-only LoRaWAN packet forwarder; weather station upload (Weather Underground, CWOP); Meteor pass report card; geostationary loops as GIF; SatNOGS DB forwarding; a records board; a daily digest; `image.done` event.
- **Smart features, round 5** (docs/SMART5-LIST-3.9.md and the SMART5-*-3.9.md pages): View > Insights with tabs for ham, voice, pagers, HF utility, aviation, marine and IoT; broadcast watchdogs (FM, AM, DAB, HD Radio, DRM), TV (fault or reception, per-carrier SNR), satellite, GNSS and radio astronomy engines. Everything runs on the figures the decoders already measure. **Simulated data only.**
- **Plugin Store** (docs/PLUGIN-MARKETPLACE-3.9.md, docs/plugins.md): View > Plugins > Store browses, installs, updates and removes plug-ins from a signed registry (Ed25519 signature on the index, SHA-256 per package, revocations). Nothing is fetched until you open it. The registry is live at https://adswill.github.io/onair-plugins/ (repository adswill/onair-plugins) and its public key is built into OnAir. **It is empty: no plug-ins are listed yet.**
- **Transmit**: DTMF keypad for analog voice, airband and QO-100, DMR slot sync to the repeater's timing (docs/TXFEAT-3.9.md). Transmit has still not been tried on the air.
- **Wide captures**: ISDB-S, FY-3 MPT, Elektro-L / Arktika-M RDAS, GEONETCAST and the X-band downlinks cost much less CPU on noise (docs/QA-SWEEP-3.9.md, docs/WIDECPU-3.9.md).

## Decoder improvements

All thresholds: lowest SNR at which at least 90 % of seeded trials decode (Es/N0 for satellite links), on generated signals, not recordings. CPU is seconds of CPU per second of signal. Machine load moves CPU figures by 10 to 20 %.

- **MetOp AHRPT** 4 to 2 dB (theory about 2); CPU 0.27 to 0.21. **Meteor LRPT** 5 to 2 dB (theory 2.5). (docs/SENSITIVITY-3.9-A.md, -D.md)
- **TETRA** coherent turbo detection and a 4 dB carrier search; the full receiver was 9 dB before (docs/TETRA-3.9.md). Also a first part of TEDS: identity and signalling only, no payload.
- **UAT 978**: 3.8 decoded no DO-282B test signal at all; now 8 dB. **FLARM** 8 to 4 dB. **ADS-L** 6 to 3 dB. (docs/SENSITIVITY-3.9-C.md)
- **ADS-B overlapping replies** are recovered by subtraction (docs/ADSB-REMOTEID-3.9.md); Remote ID CPU is lower.
- **NAVTEX / SITOR-B** 6 to 3 dB for short messages. **BPSK31** 10 to 9 dB (docs/SENSITIVITY-3.9-B.md).
- **MFSK16** -9 to -15 dB, **THOR16** -5 to -11 dB, **DominoEX 11** -7 to -12 dB; **ARQ-E** 3 to -2 dB, **G-TOR** 6 to -2 dB; ALE 2G now decodes with a 282 Hz tuning error (it decoded nothing before). **DVB-S** CPU 0.29 to 0.13. (docs/SENSITIVITY-3.9-D.md)
- **Wireless M-Bus** 17 to 11 dB. **HD Radio AM** 22 to 18 dB, lock 11 s to 7.5 s. **CW skimmer** character errors at 2 dB, machine-sent text, 13.2 % to 6.6 %. **GNSS** at 32 dB-Hz: 0 of 8 satellites tracked before, 3 of 8 now. (docs/SENSITIVITY-3.9-E.md)
- **ATSC 1.0 and ATSC 3.0** together: 0.97 to 0.83 cores on Apple silicon; on the x86 CI runner 1.80 - 2.64 to 1.10 - 1.72 cores. The goal of one core in total on the runner was **not** reached (docs/ATSC-PERF-3.9.md, docs/ATSC-X86-3.9.md).
- **COSPAS-SARSAT**: other modes' start-up lines no longer print when it starts; the search time spent on false bursts is capped (docs/SARSAT-CPU-3.9.md). ELT(DT) protocol 9 and location offsets follow C/S T.001 (#407).
- **All-modes QA sweep** (docs/QA-SWEEP-3.9.md): every mode runs from its test signal through the engine under six conditions. It found and fixed, among others, Remote ID and BLE on recordings (0 good packets before), DRM test signal freezes, and undefined behaviour in the DAB, DMR and DRM bridges. **Fuzzing** of every decoder (docs/FUZZ-3.9.md).

## Fixes

- Reports: #139 (touch start on small screens after an upgrade), #358 (Hamlib: the rig's sound card is found by name and chosen from a list), #407 (COSPAS-SARSAT ELT(DT)), #408 (TETRAPOL channel filter control in the Baseband filter row), #463 (Android: a new frequency forgets the old station's RDS).
- "Works, but" follow-ups (docs/WORKSBUT-A-3.9.md, docs/WORKSBUT-B-3.9.md): #410 (DVB-S/S2 and QO-100 service restart), #301 (General receiver centres on typed frequency and step arrows), #235 (ADS-B selection cleared when the aircraft is dropped), #239 (analog TV full-screen view takes new frames). #243, #338, #344 and #432 were already fixed earlier. #242 and the other feature requests in those pages are not built. #422 needs information from the reporter.
- Requests built: #474 (CP16), #475 (display emission finder), #476 (TMC in plain words); a German user's DAB map report and a radiosonde upload report.
- Also fixed: DRM test signal froze at 16-QAM with protection 2 or 3; DVB-S2 at 10 to 60 ksym/s; pagers and APRS on HF read -200 dBFS with Whole span off; ISDB-T timing-step phase sign; DVB-T2 P2 gather bounds; GVAR and X-band receivers on noise; several tests that depended on the C++ library's string hash.

## Contributors

Thanks to everyone who reported the issues named above. External pull requests since 3.8: #352 (wide voice first, by drfruct) and #353 (CI actions on Node 24, by peterdewitte). The full list is in CONTRIBUTORS.md.

OnAir's own code carries one attribution term under GPL section 7(b) (see ADDITIONAL-TERMS.md): keep the notices and the line "Based on OnAir by Kristijonas Adomaitis - https://on-air.dev" in a program that contains it. New third-party material of 3.9 is credited in THIRD_PARTY.md and in the in-app credits (wl2k-go and LZHUF, SvxLink echolib, the GSM 06.10 specification, qt-dab's TII list format, CP16+, gr-dsmx-rc, DIY-Multiprotocol-TX-Module and the public documents behind the HF and science modes).

## Known limits

Please read these before relying on a new feature.

- **Almost everything new was tested on simulated signals only.** If you have a recording of one, please share it. This holds for all the new modes (ISDB-TSB, DVB-SH, DVB-C2, VDL3, ELT 121.5, voice inversion, HRDCP, CDMA2000, TD-SCDMA, RC links, JT6M and the science monitors, lightning position, CP16, HF packet 300 Bd, STANAG 4539, 3G-ALE, Link 11 CLEW, STANAG 4481, the CIS modes), for the smart features of rounds 4 and 5, for the sensitivity figures above, and for TETRA coherent detection and TEDS.
- **Identification-only modes**: CIS-36 (MFSK), CIS OFDM HDR, CIS-128, CIS-3000, Link 11 CLEW (no data), Fleet Broadcast (framing only), TD-SCDMA (no broadcast channel), TETRA TEDS (identity and signalling only). Encrypted content is never decrypted.
- **Winlink, EchoLink and the external modems**: tested against mock gateways, servers and modems on 127.0.0.1 and simulated channels. Nothing has contacted a real gateway, the Winlink CMS, an EchoLink server, or a real ARDOP, VARA or Mercury program, and nothing was sent over the air.
- **Dolby passthrough** has not run on any real receiver. Atmos objects are not rendered; the laptop's own Atmos (Dolby Access) cannot be fed by OnAir.
- **Single-radio trunking** was tested on generated signals, not a real system. DMR Tier III trunking grants and NXDN Type C / D are not done; P25 in the app still uses its whole-span bank.
- **Lightning position**: hardware timestamps were never tried on a GPSDO radio; real sferics disperse with distance and only identical waveforms were tested.
- **OnAir Web**: no TV video or TETRA speech in the browser; only the test signal and 127.0.0.1 were used, not a real radio or the internet relay.
- **Radiosonde upload** follows the dxlAPRS / radiosonde_auto_rx format and was tested against a mock APRS-IS server.
- **ATSC CPU**: one core in total on the x86 CI runner was not reached.
- **Transmit has not been tried on the air** (DTMF, DMR slot sync, CP16 and earlier modes).
- **Tests**: some tests compare noisy results across platforms and can fail on a slow runner.
- **Plugin Store**: the registry is empty, and installing from it has not been tried with a real plug-in package.
- **Not built** from the round 4 list: flight routes, solar / K-index, trunk clip upload, WSPR neighbour comparison (they need internet services and an owner decision); Windy and AWEKAS uploads were not verified.
- **Carried over from 3.8**: transmit untried on the air, Android in-app update and IQ recording untried on a phone, DRM xHE-AAC #331 without a recording.
