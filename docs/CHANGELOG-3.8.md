# OnAir 3.8

OnAir 3.8 adds 101 modes (292 in all), mostly satellite downlinks, HF utility links, radio-control and IoT protocols. It also adds one-click "ease" features across many modes, drag-to-tune on the spectrum, a Wireshark output, subscriber transmit for DMR, MeshCore transmit, a reworked satellite tracker, and many fixes from your reports.

**Measured** numbers come from OnAir's own generated test signals, not from over-the-air recordings, unless a line says so. Read **Known limits** at the end before relying on a new decoder.

## Highlights

- 101 new modes (191 in 3.7, 292 now). The list is in MODES.md, generated from the app's mode table.
- Drag on the spectrum to tune, like SDR++.
- Several modes at once on one capture; search everything heard across all modes; Wireshark output with one switch per mode.
- "My station": callsign, location, radios and antennas entered once and reused by the modes that need them.
- DMR subscriber transmit and MeshCore transmit (both need your Arm; neither has been tried on the air).
- Android: record IQ from the receiver menu, in-app update from the GitHub release or the mirror.

## New modes

- **Satellite X-band, L-band and S-band downlinks** (SatDump formats, GPL-3.0; the code was written for OnAir and the notices are in the files, THIRD_PARTY.md and the in-app credits): MSG raw data, HimawariCast, GeoNetCast, GOES GRB, GVAR, CDA and MDL, GK-2A UHRIT and CDAS, an S-band CCSDS table, JPSS HRD (NPP, NOAA-20, NOAA-21), EOS direct broadcast (Aqua, Aura, Terra), MetOp X-band dump, MetOp-SG DDB, Landsat 8/9 (X and S band), Proba S-band and Proba-V X-band, GCOM-W1, GCOM-C1, STEREO high rate, Oceansat-2 and -3, Saral, Kanopus-V and DMSP RTD. Details: docs/SAT-XBAND-3.8.md, docs/SATDUMP-TIER3-3.8.md.
- **Other satellites**: Geoscan, Lucky-7, Spino, Erminaz, Gonets, Tsikada / Parus, Thuraya GMR-1 channels, Tiantong signalling, TDRS, Tiangong / Shenzhou carriers, Starlink and OneWeb detectors, a UHF SATCOM skimmer, NavIC, "What satellite is this?", Space station and zombie satellites. Several of these are detectors or carrier finders (no public specification exists for the payload). Details: docs/SAT-MISC-3.8.md.
- **HF utility**: MIL-STD-188-110 serial and STANAG 4285 (with STANAG 5066 framing), ARQ-E, ARQ-N, AUTOSPEC, SPREAD, G-TOR, EFR teleswitch, Piccolo Mk VI, Coquelet-8 and HFGCS voice. Details: docs/HF-UTIL-3.8.md.
- **Ham science and weak-signal**: QRSS and DFCW grabbers, FSK441, ISCAT, PI4, Throb, SuperSID (VLF), a riometer, pulsar folding and weather radar detection. Details: docs/HAM-SCIENCE-3.8.md.
- **Aviation and road**: ADS-L (with FLARM from the same capture), FANET, OGN tracker, GBAS VDB, AAR end-of-train, ERMES paging, 802.11p / ITS-G5, DSRC road toll, C-V2X, LDACS and AeroMACS structure detectors, marine radar detection. Details: docs/ADSL-3.8.md, docs/AVIA-ROAD-3.8.md.
- **Radio control and links**: Reticulum (RNode), ExpressLRS, MAVLink on SiK radios, FrSky and FlySky receivers, wfb-ng / OpenHD, New Packet Radio (NPR-70), WINMOR, mioty and DECT NR+. Details: docs/MESH-RC-3.8.md, docs/LEFTOVERS-3.8.md.
- **Two-way radio and channels**: Motorola Type II (control channel and call following), TETRA DMO, MDC-1200, CB radio, FRS / GMRS, PMR446 analog, ham FM repeaters, a wireless microphone finder and an OpenSky identifier. Details: docs/VOICE-TRUNK-3.8.md, docs/CHANNELS-3.8.md.
- **Cellular and IoT**: UMTS cells (BCH, operator and cell identity), Bluetooth Classic discovery (access codes by LAP), KNX RF, HomeMatic BidCoS, EnOcean ERP1, DASH7. Details: docs/IOT-3.8.md.
- Existing modes grew: TETRAPOL (soft-decision decoding, messages, neighbours; tables and layouts from the public PAS), DRM, DVB-T2 (MISO, common PLPs, T2-Base / T2-Lite in mixed FEF signals, T2-MI), DVB-C annex B, ATSC (PSIP guide, ATSC-M/H), HD Radio traffic and radar maps, DAB packet mode and a census of every ensemble in a wide capture.

## New features

- **Easier to start**: first Start scans and plays for FM, DAB and TV; Airband loads the nearest airport; Repeaters near me; automatic band by time and sun; clock correction for FT8, WSPR and JS8; the next-pass line and geostationary visibility per mode; "Whole span" for ALE, DRM and pagers on a wide radio; automatic settings for RTTY, PSK31 and keyboard modes (manual settings under Advanced). Details: docs/EASE-3.8.md, docs/EASE2-3.8.md, docs/AUTO-PARAMS-3.8.md.
- **Drag on the spectrum to tune** (docs/DRAGTUNE-3.8.md), several modes at once on one capture, a search over everything heard in all modes, "My station" (docs/STATION-3.8.md) and a Wireshark output with one switch per mode (docs/WIRESHARK-3.8.md).
- **Satellite tracker** reworked around a queue and a pass runner, with a dish Align view (level bar and peak hold). Details: docs/SATTRACK-3.8.md.
- **Transmit** (receive-first app; transmit stays behind your Arm): DMR subscriber transmit (voice, SMS, LRRP, Tier III request), MeshCore node screen with Join and chat, ISDB-T transmit mode, a video generator with several outputs, per-radio calibration and IQ correction, predicted output power in the live meter. Details: docs/DMR-TX-3.8.md, docs/MESHCORE-TX-3.8.md, docs/TX-CONTROL-3.8.md.
- **Decoder depth**: TV closed captions (CEA-608 and 708), ARIB STD-B24 text, TR 101 290 stream health; FM RDS TP / TA / PTY and EON rows, RDS2 subcarriers and SCA audio, BTSC stereo and SAP for NTSC sound; ADS-B trust score, ACAS lines and timeline; LoRaWAN and Zigbee / Wi-SUN / Z-Wave S0 / wM-Bus mode 5 decryption with keys you import (nothing is cracked); about 60 plain-text ACARS formats; Meshtastic waypoints and telemetry; MeshCore and Meshtastic list sizes; P25 and DMR trunking messages; AIS binary messages; FX.25 and IL2P on the 1200 baud demodulators; WSPR balloon telemetry; fldigi RSID detection; FIS-B NEXRAD and text from UAT.
- **Voice**: a Channels tab with Mute, Solo and Priority per talkgroup; a persistent TETRA talkgroup log with CSV and JSON export.
- **Optional libraries**: docs/OPTIONAL-LIBRARIES.md, a list in the app, and a codecs folder that radio driver DLLs are searched in too. A Discord button on every codec install screen.
- **Android**: IQ recording from the receiver menu and an in-app update (docs/ANDROID-IQ-3.8.md).

## Fixes

- Reports fixed: #139 and #424 (top bar buttons overlapped the mode selector), #169 (Lime driver hint on Windows), #225 (Hellschreiber: click the signal to receive, clean keying, USB transmit), #335 (DMR LRRP positions behind a Motorola compressed header), #357 and #430 (Icom R8600 firmware check), #358 (Hamlib rigs tune to the dial; macOS asks for the microphone), #373 (transmit fall-back is NFM), #421 (a picked More tab shows its own name), #440 (an installed Aero AMBE plug-in is registered), #468 (the Windows codec helper links the static mbelib-neo), #471 (audio files longer than 10 minutes play to the end), #472 (the installer update keeps user-installed DLLs).
- Features from requests: #368 (ADS-B label size and contents; Meteor 124 colour composite), #375 (Chinese texts for the TV player and teletext), #427 (332 Turkish texts, machine translated and marked unreviewed), #431, #432, #436, #438, #443, #449, #453, #455, #464 and #466 (see New features and the per-feature docs).
- Also fixed: GVAR infrared block written past its buffer, plot order during a middle-button pan, DAB transmitter submission uses the wizard's country, the codec-missing badge in the mode browser, ADS-B no longer reads coarse TIS-B as ADS-B, DME opens at 1104 MHz, annex B frame sync is re-acquired after lock, meteor trails dropped from the satellite echo list, extcap stops on SIGTERM, several Windows and GCC build errors.

## Contributors

Thanks to everyone who reported the issues named above: @richt314, @trebor-56, @BI4PYM, @djnutmac, @ArieGroeneveld, @djs007, @G8HUIGW, @BG2GGF, @perseus68-github, @ptmy53, @scanflight, @DickvanNoort, @xxj737, @moricef, @Voyager-III, @Saga24, @ibomdv, @Totoche95, @jk1rcc, @ddjridley, @YanO1420 and @G0MJW. No external pull requests were merged since 3.7. The full list is in CONTRIBUTORS.md, which is generated from tools/contributors.tsv.

OnAir's own code carries one attribution term under GPL section 7(b) (see ADDITIONAL-TERMS.md): keep the notices and the line "Based on OnAir by Kristijonas Adomaitis - https://on-air.dev" in a program that contains it.

## Known limits

Please read these before relying on a new feature.

- **Most new decoders were tested on simulated signals only.** If you have a recording of one of these, please share it. This holds for: all the new satellite X-band, L-band and S-band modes (no public recordings were found; GOES GRB has never run on a real recording), the other-satellite detectors, the HF utility modes, the ham science modes (QRSS, DFCW, FSK441, ISCAT, PI4, Throb, SuperSID, riometer, pulsar folding), ADS-L, FANET, OGN, GBAS, ERMES, 802.11p, DSRC and the structure detectors, Reticulum, ExpressLRS, MAVLink, FrSky, FlySky, wfb-ng, NPR-70, WINMOR, mioty, DECT NR+, Bluetooth Classic, KNX RF, HomeMatic, EnOcean, DASH7, UMTS cells, Motorola Type II, TETRA DMO, the channel-voice modes (CB, FRS / GMRS, PMR446, MDC-1200), the DVB-T2 MISO and sub-sliced PLP work, ATSC PSIP, DRM SDC schedules, FIS-B, FX.25 and IL2P, and the "ease" features.
- **Validated on real recordings, in part**: **TETRAPOL** (a real recording from issue #408: 3092 control-channel frames without a bad one, traffic channels and encrypted group calls; VHF coding, clear calls and many message types are simulated only), **AAR end-of-train** (3 frames of a public recording, plus simulated frames) and the ADS-B signal-library excerpt.
- **Transmit has not been tried on the air.** DMR subscriber transmit, MeshCore transmit, ISDB-T transmit and the video generator outputs are checked against OnAir's own receivers on simulated signals.
- **Android**: the in-app update and IQ recording have not yet been tried on a phone. The 3.7 Android limits (emulator only, wide TV modes too heavy for most phones) still apply.
- **TETRAPOL**: "Follow clear calls" assumes a regular channel raster; on the network of issue #408 the channel numbers do not follow one.
- **Carried over from 3.7**: Inmarsat BGAN and the Iridium silent-WAV fix are not validated on real signals, auto gain was tested on a simulated front end, and Arktika-M GGAK is experimental.
- **DRM xHE-AAC (#331, #332)**: concealed xHE-AAC frames are now played; on the #332 sample the missing frames went from 212 to 63 of 776. #331 (32 kHz mono, "refuses this stream") has no recording, so it is not known whether this covers it.
