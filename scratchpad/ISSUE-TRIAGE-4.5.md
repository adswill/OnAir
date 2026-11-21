# Issue triage for 4.5 (open issues of adswill/OnAir at v4.4.0)

Classes: A bug with locatable cause, B fixed in a release, C reporter confirmed fixed, D feature request, E question / duplicate / fix PR open / not actionable, F needs hardware or more info.
Evidence is from the issue text, docs/CHANGELOG-*.md and a quick code look at v4.4.0; "unverified" means I did not reproduce.

## Closed by this triage (12)

| # | title | class | evidence | action |
|---|---|---|---|---|
| 394 | VDL2 aircraft type and distance | C | f0cyf thanked for Clear button in 4.2, works | closed |
| 488 | Audio problem (monitor audio) | C | Ugo300 "working" after 4.1 | closed |
| 331 | DRM audio stuttering | C | Cook8ing: DRM sound perfect in 4.3 | closed |
| 470 | Pi 5 llvmpipe | C | reporter fixed own compositor, GPU used | closed (ATSC video on Pi CPU-bound left for a new issue) |
| 597 | Marine VHF active channel on spectrum | B | CHANGELOG-4.4 (#597) | closed, 4.4 |
| 584 | FM-ATV transmit | B | CHANGELOG-4.4 (#584) | closed, 4.4 |
| 559 | Chinese Android | B | CHANGELOG-4.4 (#559) | closed, 4.4 |
| 372 | Thread count setting | B | CHANGELOG-4.4 (#372) | closed, 4.4 |
| 425 | Infinite wheel zoom | B | app/gfx/plots.cpp minSpan clamp (kFreqSpanMinMhz), 3.7 | closed, 3.7 |
| 430 | IC-R8600 .spt "write outside FX2 RAM" | B | core/src/source/icom_r8600.cpp recordsProblem allows CPUCS writes, commit 11c4aacd7 (3.8.0) | closed, 3.8.0 |
| 496 | Web server play does nothing | B | CHANGELOG-4.1 #496 (web_listener_start) | closed, 4.1 |
| 279 | Codecs in non-default folder | B | CHANGELOG-3.0 #279 | closed, 3.0.0 |

## A: bugs (priority order, P = work package)

| # | title | priority | likely cause / files | fix idea | P |
|---|---|---|---|---|---|
| 503 | RSP1 Android/Windows: cannot restart after first stream; CRASH when USB transfer mode Isochronous -> Bulk | 1 crash | core/src/source/mirics_usb.cpp (useBulk, iso/bulk transfer set-up and teardown on re-open); bug report zip attached on issue | free/re-claim interface and alt setting on stop; do not switch transfer type while transfers are live | 2 |
| 516 | DRM: no audio on Android (AAC and xHE-AAC); 17.700 MHz (audio+data) plays badly on desktop and Android | 1 no audio | android/docs/AUDIT-modes.md says DRM audio is an FFmpeg stub on Android; core/src/drm/*; android/.../tv/AudioFrames.kt (MediaCodec path used for TV AAC) | route DRM AAC/xHE-AAC frames through MediaCodec `audio/mp4a-latm`; 17.7 MHz sample (#332 comment) shows decode cuts out when a data service is present | 1 |
| 332 | DRM 17.700 MHz hardly any sound (sample attached 0.25 Msps) | 2 | same sample as #516; core/src/drm audio super-frame handling with data services | reproduce with attached zip, check audio service selection / multi-service MSC | 1 |
| 506 | HF Service > Shortwave > RTTY stays "nothing decoded yet" while Amateur > RTTY works; same for Wefax | 2 | app/shortwave/shortwave_ui.cpp (decoder pane, rttyBaud/rttyShift, passband/sideband feed) vs app/hfdig/hfdig_rtty_ui.cpp; 4.2 Auto-shift fix did not help (4.3 report) (unverified) | compare audio/IQ path feeding the shortwave pane with the ham RTTY path (sideband, centre offset) | 3 |
| 408 / 333 | Tetrapol: only "Scramble", no traffic info, no Ch since 3.0 | 2 | core/src/tetrapol, app tetrapol table; reporter sent a recording (working channel 393.4125, data 393.3875 MHz) | decode channel / traffic from the recording; may be a hidden mode (reporter's own suspicion) | 3 |
| 561 | RSP1 / MSi2500 native not working on Linux (device found, no signal) | 3 | core/src/source/mirics_usb.cpp, mirics_regs.cpp ("untested on hardware"); log in issue | get libusb log from reporter; check tuner/ADC init for MSi2500 | 2 |
| 619 | Antenna ports not listed for SoapyRemote / manual SoapySDR strings | 3 | core/src/source/source_soapy.cpp ~793-830: antenna entries only built from gRanges (filled after the radio was started once) and only in the enumeration loop; manual strings and network radios never get them | fill antennas on first open and for manual/network entries; add selector in app/ui/toolbar_source.cpp | 2 |
| 335 | DMR LRRP (other frequency in whole span) / LOCN | 3 | core/src/dmr data header handling; LOCN not written; whole-span LRRP not shown on map | LOCN decode needs a raw hex line; map hook for whole-span receivers | 3 |
| 484 | ATSC 3.0 ch 21 never gets signalling / routes; service switching sticks | 3 | core/src/atsc3 (Atsc3Receiver service selection, LLS/SLT waiting) | needs IQ; selection reset on service change | 3 |
| 435 | Android: RTL2832U+FC0013 shown as R820T; Airspy R2 / RTL not selectable as USB radio | 3 | android/.../MainActivity.kt, ui/Permissions.kt (USB device filter / permission), core/src/source/source_native.cpp tuner naming | check device-filter xml and tuner detection log | 2 |
| 582 | Android (Honor 70): "USB direct" greyed out with RSP1 | 3 | same Android USB host path as #435 (Permissions.kt, UsbManager enumeration) | log which USB check fails | 2 |
| 595 | Auto Notch click switches mode to Weather Fax and jumps to 7.793 MHz | 3 | button in app/panels/audio_control_ui.cpp:354 (or similar id clash); 7.7931 MHz is JMH wefax in data/frequencies/marine.tsv, so some auto-detect / "go to station" action fires (unverified) | find which widget shares the id / handler; repro in SSB 7.084 | 4 |
| 590 | 4.3 setup wizard buttons do not react to clicks on Windows 11 (hover works) | 3 | app/ui/wizard.cpp, app/main_start.cpp (modal / input capture on Windows) (unverified) | needs repro, check modal popup input focus | 4 |
| 566 | Meshtastic / MeshCore data mixed in tabs; Clear button | 4 | app/mesh/* views; 4.4 added Clear for MeshCore (#579) but views are not filtered per protocol | filter tabs by protocol; Clear on all tabs | 4 |
| 565 | Kyodo (JJC) fax bulletins discontinued 2026-09-22 | 4 data | core/src/marine/marine_wefax_stations.cpp:32 (JJC), data/frequencies/*.tsv | remove or mark JJC as discontinued | 4 |
| 585 | OnAir Web: DAB+ scan / play does nothing (screen reader user) | 4 | server/web/* has DAB only as a signal block (docs/WEB-3.9.md); no web DAB service list or audio | at least tell the user DAB audio is not supported in the web app, or add it | 4 |
| 583 | FM filter skirts too wide: need 100 kHz at 100 kHz spacing, steeper filter | 4 | channel filter in core/src/dsp (FM VFO filter order / window) | offer filter shape or higher-order option | 3 |

Not assigned to a package (other worker): **#587 Airband without channels** (handled by another worker).

## Work packages (independent, no shared files)

1. **DRM / Android audio**: #516, #332 (17.7 MHz). Files: core/src/drm/*, android/app/src/main/java/io/github/adswill/onair/tv/*, android native audio glue.
2. **Radio sources and Android USB**: #503 (crash first), #561, #619, #435, #582. Files: core/src/source/mirics_*.cpp, source_soapy.cpp, source_native.cpp, app/ui/toolbar_source.cpp, android/.../MainActivity.kt + ui/Permissions.kt.
3. **Decoders**: #506, #408/#333, #335, #484, #583. Files: app/shortwave + core/src/hfdig (RTTY), core/src/tetrapol, core/src/dmr, core/src/atsc3, FM channel filter in core/src/dsp.
4. **App UI, web, data**: #595, #590, #566, #565, #585. Files: app/panels/audio_control_ui.cpp, app/ui/wizard.cpp, app/mesh/*, data/frequencies + core/src/marine/marine_wefax_stations.cpp, server/web/*.

## All other open issues

| # | title | class | evidence | action |
|---|---|---|---|---|
| 640 | Android spectrum zoom +/- buttons | D | android/.../* has no zoom buttons (pinch only) | none |
| 639 | Android: show OK/BAD counters after lock SNR | D | not in Android UI | none |
| 638 | Android DRM AAC playback | A-dup | same as #516 (package 1) | none |
| 625 | China CMMB support (patch attached) | D | no CMMB in MODES.md; patch + snapshot provided by reporter | none |
| 614 | Marine VHF duplex coast reception | F | docs/marine-vhf.md says both frequencies followed; asked for bug-report zip, none yet | none |
| 593 | Web SDRs as input (KiwiSDR/UberSDR/OpenWebRX) | D | KiwiSDR already in app; UberSDR / OpenWebRX not | none |
| 592 | Interface looks labyrinthine | E | opinion, UI is being redone (Tauri) | none |
| 587 | Airband listening without channels | A | other worker | none |
| 583 | see A | A | | |
| 581 | DMR RC4 key says encrypted; 20 s start in 4.3 | F | core/src/voicekeys/voice_crypto.cpp has RC4; need recording and key type; start delay likely antivirus scan | none |
| 577 | Android DTMB: AVS+, AC-3, subtitles, MHEG-5; EPG; tuning step; LDPC colours | D | codecs not shipped (docs/ANDROID-4.0.md: only phone-native codecs); rest UI requests | none |
| 570 | ATSC 3.0 dead PLP costs full decode | E | fix PR #574 (Felbs) open | merge PR |
| 567 | Export/import Meshtastic/MeshCore channel keys | D | app/mesh/mesh_settings_ui.cpp only has add/remove, keys saved by saveKeys() | none |
| 554 | Soundcard in SDR server (patch) | D | patch attached | review patch |
| 549 | RSPdx on Android | E | answered: SDRplay API has no Android build; use rsp_tcp | none |
| 543 | MIDI LNA/VGA/Bias-T | B (partial) | 4.4: MIDI knobs for HackRF LNA, VGA, gain (receive only); Bias-T toggle and LED feedback not done | not closed (partial) |
| 537 | Docs: TETRA DMO | E | empty issue body | none |
| 536 | Defender false positive (unsigned zip) | E | explained, SHA256SUMS | none |
| 526 | Remove SondeHub upload | B-pending | adswill promised to switch it off; check it is off in 4.5 | verify in code before closing |
| 522 | NESDR drops to weak in ADS-B | B-pending | auto-gain wait fix in 4.2, no reply | wait |
| 519 | AM sound weird/metallic | F | 4.1 NR change; new comment still "metallic", no station given | none |
| 508 | Inmarsat all-carriers positions / Inmarsat-C lock | F | 4.2 changes, no reply | wait |
| 503 | see A (feature part: native RSP1 on Android done; OpenRSP suggestion) | A | | |
| 498 | DPRK DVB-T2 / Malaysia T2-MI | B-pending | 4.1 plays inverted PIDs and T2-MI; reporter wants load-TS-file player | ask |
| 475 | HDMI emission detection | D (partial) | 3.9 works, reporter asks for resolution/refresh in result (timing 4.32 MHz shown) | none |
| 469 | DVB-T no video, PIDs | F | asked for IQ, none | none |
| 462 | Meteor scatter suggestions | D | listed for 3.8, not confirmed | none |
| 459 | RSP1 audio / FM dropouts | F | first problem solved by reporter (2nd output); dropouts on RTL-TCP/USB, bug report zip attached | none |
| 458 | TX encoding params | D | split into #490/#492 | none |
| 457 | Meshcore key "not valid" | E | key has 31 hex digits | none |
| 455 | 150 nodes / 100 messages limit | D | planned for 3.8, not confirmed | none |
| 454 | Multi-program TX mux | B-pending | 3.7 pads the mux, no reply | wait |
| 441 | 5G Broadcast | D | no promise | none |
| 435 | see A | A | | |
| 422 | Server tab closes | B-pending | 3.7 own sharing panel, no confirmation | wait |
| 419 | SDRconnect headless api_version | F | waiting for traffic log after 2026-11-18 | none |
| 418 | FM scan stops at 18% | B-pending | 3.7 fix, no reply | wait |
| 412 | DMR RC4 5-byte key | F | needs IQ recording | none |
| 405 | Web-888 | F | use KiwiSDR type; reporter must test | none |
| 404 | False FGB decodes | B-pending | stricter check in 3.7 | wait |
| 398 | Pluto dual RX/TX | D | needs iio_info | none |
| 397 | Real SGB recordings | E | recordings to add as tests | none |
| 390 | WeFax stop tone, realign editor | D/A | 8000 line limit fixed 3.7; stop tone + editor open | none |
| 386 | LoRa on RTL-SDR sample jumps | F | fixes in 3.5, reporter measuring | none |
| 379 | More LoRa BW, MRZ-N1, Qi | D | planned | none |
| 375 | Chinese translation (desktop) | D | PR #558 open (BG2GGF) | review PR |
| 367 | Pluto low rates via network | F | tested by friend pending | none |
| 338 | Keep frequency across satellite modes | B-pending | 3.7 fix, no confirmation | wait |
| 337 | RTL-SDR V4L no reception | B-pending | 3.7 packaging fix, no reply | wait |
| 333 | see #408 | A | | |
| 328 | Multiple RTL-SDRs | F | needs two dongles with same serial | none |
| 321 | SpyServer tuner range | B-pending | 3.7 fix, no reply | wait |
| 316 | Pluto CMA errors | B-pending | 3.2/3.7 | wait |
| 313 | ISDB-T layer B | F | needs better capture | none |
| 299 / 266 / 247 | DRM xHE-AAC, Tianqi LoRa freq, DRA in DTMB | B-likely | xHE-AAC reported good in 4.3 by others (#331, #516 desktop 6030 kHz); Tianqi 400.265 and DRA still open | ask reporters |
| 277 | Analog TV TX slow | F | likely Pluto USB bandwidth | none |
| 257 | KiwiSDR "stopped sending samples" | F | still failing on 3.8.0 per log | needs log analysis |
| 255 | RWM decoder calibrated time | D | decoder in 3.0, time display planned | none |
| 254 | DTMB choppy | F | needs IQ | none |
| 252 | DMR audio | E | needs mbelib in codecs folder | none |
| 251 | Elad Duo | D | sound card route documented, no native driver | none |
| 246 | Android waterfall not refreshing | B-pending | 3.7 Android work | wait |
| 245 | Radio driver offer | E | maintainer waiting for details | none |
| 242 | Detachable windows | D | not in 3.7 | none |
| 233 | Per-stage gain sliders (usrp, airspy, rtl) | D | open | none |
| 228 | SoapyRemote hang on macOS | B-pending | 3.7 20 s timeout | wait |
| 219 | RSP1 click tuning offset | B-pending | 0.2.9/3.7 | wait |
| 216 | Pactor 2-4 | D | partial | none |
| 214 | GTH6/CF6 sondes, baseband out | D | planned | none |
| 211 | SELCAL | B-pending | own mode exists, no version certain | wait |
| 210 | DAB+ RTL-SDR v3 drops on mac | F | still USB underruns in 3.7 | none |
| 201 | Android freeze on start (rtl_tcp) | B-pending | 3.2/3.7 | wait |
| 619 | see A | A | | |
| 565 | see A | A | | |

## Counts

A about 17 (incl. #587 other worker and #638 duplicate), B 12 closed + about 16 pending reporter reply, C 4 closed, D about 28, E about 9, F about 22 (rounded, one primary class per issue).
