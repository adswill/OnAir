# Competitor scorecard (3.0 campaign)

Written 2026-10-23 for the owner's goal: OnAir should be the best decoder in every family, "not only performance but
basically everything". Sources are public pages only: READMEs, project sites, specifications, forum posts. No competitor was built
or run for this file. The decoder agents (dec-*-030) build competitors in isolated scratch folders and add head-to-head numbers.

How to read it:

- **OnAir state** comes from the repo docs at `pub/integ-30` (README, docs/*.md, PATHWAY). Nothing was built or measured for this file.
- **Popularity / last release** are GitHub stars, the latest tagged release and the last push, read with `gh api` on 2026-10-23.
- **Published numbers** are rare. Almost no open-source decoder publishes a threshold in dB. The figures that do exist come from
  the standards (ideal or reference receiver) and from WSJT-X. Section 3 therefore uses the standard's figure wherever the
  competitors give none, and that is the honest bar: a decoder that reaches the standard's ideal Es/N0 beats every competitor
  that does not publish a number.
- **OnAir's biggest missing piece is not a decoder. It is the number:** OnAir has no per-mode threshold or lock-time table either
  (only scattered figures: DAB 4 dB AWGN, ISDB-S, packet, analog TV line lock 3 dB, DRM audit). Until the sens harness
  (sens-harness-030) publishes them, "best" cannot be shown. That is gap #1.

Branch names: dec-<family>-030 = the decoder agent of that family (for example dec-dvbs-030; the exact names are set by the
campaign lead), perf2-*-030 = the speed agents that are on `pub` (perf2-sat-tv-030, perf2-audio-bcast-030,
perf2-aviation-data-030, perf2-hf-030, perf2-voice-030, perf2-sat-misc-030), spectrum-sdrpp-030, autodetect-030, ui-clean-meter-030.

---

## 1. Per family

### 1.1 DVB-S / S2 / S2X (and QO-100 DATV)

| program | strengths | weaknesses | published numbers | OnAir gap / lead |
|---|---|---|---|---|
| **leandvb / leansdr** (GPL-3, Linux CLI; inside SDRangel's DATV demod; 168 stars, last push 2022) | Small, fast, the standard software DVB-S/S2 for amateur DATV. Runs in SDRangel and Portsdown. | Repository has been quiet since 2022. No S2X VL-SNR. CLI only, apart from SDRangel. The author's own note puts speed ahead of sensitivity. | No dB table published (pabr.org was down on 2026-10-23). | **Lead:** OnAir has VL-SNR, S2X, the TS analysis of the DecSAT parity work, a GUI and a player. **Gap:** a head-to-head at QPSK 1/4..8PSK 2/3 near threshold. |
| **gr-dvbs2rx** (GPL-3, GNU Radio, Linux/mac; 138 stars, v1.4.0 2023-12) | Full DVB-S2 receive chain: PL sync, LDPC with SIMD (xdsopl), a BCH 2.5x faster than before. Also transmits. Clean code. | DVB-S2 only (no DVB-S, no S2X). Needs GNU Radio. No player and no SI. | No Es/N0 or FER figures on its pages. | **Lead:** breadth, UX. **Gap:** its LDPC is a known reference, so compare LDPC iterations against waterfall. |
| **MiniTiouner + Longmynd / Ryde / Winterhill** (hardware STV0910 demod, BATC; GPL-3 software) | Hardware demod: near-ideal thresholds and lock in about 1 s. Winterhill: 4 receivers at once. Standard on QO-100. | Needs the Serit NIM board. Windows or Raspberry Pi. Limited MODCODs and frames (no VL-SNR). Forum reports say weak-signal lock is marginal around MER 7 and that auto mode locks worse than forced DVB-S. | Cliff -100 dBm at FEC 1/2 and -96 dBm at 7/8 at 1288 MHz (RigPix); QO-100 beacon around 8.5-8.9 dB MER on a 1 m dish (BATC forum). | **Gap:** time to lock (hardware takes about 1 s) and a "DVB-S forced" fast path. **Lead:** S2X, any SDR, multi-channel capture. |
| **TBS / DVB cards + tvheadend** (hardware tuners; tvheadend GPL-3, 3.5k stars) | Hardware demod, CAM support, EPG, recording server, many clients (Kodi, apps). | Needs hardware. No IQ and no constellation. Fixed MODCOD support set by the chip. | Chip datasheets: about QEF at spec Es/N0 + 0.5..1 dB. | **Gap:** tvheadend-class server (EPG grabber, scheduled recording, many clients); OnAir has a network tuner and recorder already. |
| **SatDump** (GPL-3, all OS, 2.35k stars, push 2026-10-22, last tag 1.2.2 2024-11) | Has DVB-S2 demod plus many satellite downlinks. Big community. | DVB-S2 is used for data downlinks (for example Fengyun), not for watching TV. | none for DVB-S2 | **Lead:** OnAir has a TV player, SI and EPG. |

### 1.2 DVB-T / T2, DVB-C, ATSC 1.0 / 3.0, ISDB-T / S, DTMB, analogue TV

| program | strengths | weaknesses | published numbers | OnAir gap / lead |
|---|---|---|---|---|
| **sdr_receiver_dvb_t2** (Oleg-Malyutin, GPL-3, Linux Qt; 81 stars, push 2026-03) | The only other open SDR DVB-T2 receiver. LDPC ported from gr-dvbs2rx. | Prototype: a few devices, one PLP, no player, no T2-Lite or MISO. Needs an i5-8600-class CPU. | none | **Lead:** OnAir has T2 + T2-Lite + MISO signalling, PLP selection, a player and EPG. |
| **gr-dtv (in GNU Radio 3.10)** (GPL-3) | DVB-T and ATSC 1.0 receive blocks, DVB-T2 transmit. Used as a reference. | Flowgraphs only. No DVB-T2 receive. Not a product. | none | **Lead** in every respect except as a reference implementation. |
| **Linux DVB tuners + tvheadend / VLC / Kodi** | Hardware demod at the specification figures, CI+ CAMs, mature EPG, recording, HbbTV (some clients). | Needs a tuner per standard. No analysis. | Chip QEF at about the figures of EN 300 744 Table A.1 + 1-2 dB. | **Gap:** EPG and recording polish, channel-list import (m3u, XMLTV export), HbbTV/teletext browser. |
| **libatsc3 / HDHomeRun** (ATSC 3.0) | HDHomeRun: commercial hardware with DRM support; libatsc3: an open ROUTE/MMT stack. | No open SDR ATSC 3.0 physical layer anywhere. | none | **Lead:** OnAir is the only open SDR ATSC 3.0 receiver. **Gap:** MMTP is not supported, and nothing is checked against a real broadcast. |
| **gr-isdbt** (Uruguay, GPL) / **dtmb-sdr** (ningzichun) | Open ISDB-T and DTMB receivers. | Research code, old or unmaintained. | none | **Lead:** OnAir has the player, three layers and 1-seg. **Gap:** real-signal validation, Japanese ARIB character set for names. |
| **Analog TV: SDRangel ATV demod, TVSharp, hacktv (TX)** | SDRangel shows PAL/NTSC pictures. | No SECAM colour, no teletext, no MAC. | none | **Lead:** OnAir decodes every system plus MAC and FPV. |

### 1.3 DAB / DAB+

| program | strengths | weaknesses | published numbers | OnAir gap / lead |
|---|---|---|---|---|
| **welle.io** (GPL-2+, all OS + Android; 754 stars, v2.7 2025-03) | Polished and simple. Android app. Decodes all programmes of an ensemble at once. | Slower development. No TII map, no Journaline, no SPI. | none | **Lead:** OnAir has the TII map, SPI/EPG and Journaline. |
| **Qt-DAB** (JvK, GPL-2, Win/Linux; 362 stars, 7.3.1.1 2026-10-05) | Most complete feature set: TII with an offline map, EPG/SPI, Journaline, MOT, scan variants, services in the background, ETI output, many SDRs including SDRconnect and spyServer. | Busy UI; Windows and Linux only for binaries. | none | **Parity** on data features. **Gap:** ETI output, spyServer/SDRconnect inputs, several services recorded at once. |
| **AbracaDABra** (KejPi, MIT, Win/mac/Linux/Android) | Modern UI, DL+, CatSLS, scheduled recording, every announcement type with switching, an unattended DX scan with CSV/JSON log, FMLIST upload, CLI + web UI. | Band III and mode I only. | none | **Gap:** scheduled recording, announcement switching UX, a DX scan log, FMLIST upload, DL+ display (OnAir has DL+ in 5 files; check the UI). |
| **DABlin / eti-cmdline / dab-cmdline** (GPL) | Headless ETI/EDI playback; DABlin catches up after a lock loss. | CLI | none | **Gap:** EDI/ETI input (playing a network-delivered ensemble). |
| **SDRangel DAB demod**, **QIRX** (Windows, closed) | QIRX: detailed TII/CIR analysis, a favourite of DXers. | Windows only (QIRX) | none | **Gap:** a channel-impulse-response view with TII markers. |

OnAir's own figure: no audio loss at AWGN 4 dB, and a fix for ±100 ppm clock at 4.5 dB (docs/DAB-AUDIT.md). No competitor publishes a comparable number.

### 1.4 DRM (DRM30 / DRM+)

| program | strengths | weaknesses | published numbers | OnAir gap / lead |
|---|---|---|---|---|
| **Dream** (GPL-2, SourceForge; 2.2 / 2.3.0 snapshot 2023) | The reference open DRM receiver: RSCI, MOT, EPG, Journaline (with libs), AM too. Basis of the KiwiSDR and OpenWebRX DRM decoders. | Practically unmaintained. Qt build pains. xHE-AAC support unclear or missing. | none from the project. | **Lead:** OnAir has xHE-AAC (USAC), DRM+ and the fade audit. **Gap:** RSCI output, a published threshold. |
| **KiwiSDR DRM extension / OpenWebRX DRM** (Dream based) | Thousands of public KiwiSDRs: the way most people hear DRM. | Same core as Dream. CPU-limited on a Beagle. | none | **Gap:** OnAir reads KiwiSDR (docs/kiwisdr.md), but this is untested against a live Kiwi. |
| **JvK drm-receiver** (GPL-2), **DRM Software Radio** (Fraunhofer, old, commercial) | | dated | none | |
| **Hardware: Gospell, Avion, Starwaves** | | | DRM Minimum Receiver Requirements: BER below 1e-4, test cases A/64QAM/0.78 .. D/64QAM/0.5 on channels 1-6. | **Numbers-to-beat:** the six MRR channel tests, run in OnAir's test_drm_impair. |

### 1.5 HD Radio (NRSC-5) and CDR

| program | strengths | weaknesses | published numbers | OnAir gap / lead |
|---|---|---|---|---|
| **nrsc5** (GPL-3, 971 stars, v3.2.0 2026-06) | The reference. FM + AM, programs 0-3, C and Python APIs, HDC packet dump. GUIs on top of it: nrsc5-gui, nrsc5-dui (album art, traffic and weather maps). | RTL-SDR only. CLI (GUIs are third-party). | none | OnAir ports nrsc5 (credited). **Lead:** any SDR, GUI, CDR as well. **Gap:** the traffic and weather map images (LOT/AAS) as the nrsc5-dui GUI shows them; check what OnAir's hdr_l2_aas shows. |
| **SDR# HD plug-ins / SDR++ community plug-ins** | | | none | |

### 1.6 FM / RDS, AM, C-QUAM

| program | strengths | weaknesses | published numbers | OnAir gap / lead |
|---|---|---|---|---|
| **redsea** (MIT, 443 stars, v1.3.1 2026-07) | Best open RDS decoder: RT+, ODA list, TMC, eRT, JSON output, real time on a Raspberry Pi 1, a noise plot in its wiki. | CLI. No audio. | Speed: 2564 groups/s on an M1 MacBook, 26 on a Pi 1 (needs 11.4). Sensitivity: plot only. | **Gap:** RT+ (no RadioText+ code in core/src/fm), eRT, RDS2 (none of these found), redsea-compatible JSON. |
| **SDR# / SDR++ / SDR Console / FM-DX Webserver** | FM-DX community tools: PI/PS logging, FMLIST, best weak-PS heuristics. | | none | **Gap:** a weak-signal PS/PI "best guess" display, FMLIST log. |
| **gr-rds** (GPL-3) | Reference | dated (2022 tag) | none | Lead |

### 1.7 ADS-B, Mode A/C, UAT, Mode S uplink, ASTERIX

| program | strengths | weaknesses | published numbers | OnAir gap / lead |
|---|---|---|---|---|
| **readsb + tar1090** (wiedehopf, GPL; 696 + 1939 stars, pushed 2026-10) | De-facto standard: CRC fix, speed checks, Mode A/C, UAT input, Beast/SBS/JSON, globe history, heatmaps, auto gain for RTL, feeds every aggregator, MLAT via mlat-client. | Linux only (mac partly). No GUI apart from the web map. | No message-rate table published; tuning notes only. | **Gap:** MLAT client, history/heatmap/replay, tar1090-class filters; a message-rate head-to-head on the same IQ. |
| **dump1090-fa / FlightAware** (GPL, 1149 stars) | 2.4 Msps phase-enhanced demodulation (a known message-rate gain), PiAware. | | none | **Gap:** measure messages/s at 2.0/2.4/8 Msps against it. |
| **antirez dump1090** (BSD-3, 2961 stars) | Origin | dated | none | |
| **dump978-fa** (UAT) | Standard UAT decoder | | none | **Lead:** OnAir does UAT + Mode S uplink + Mode A/C + ASTERIX in one app. |

Numbers-to-beat: RTCA DO-260B receiver MTL (-79 dBm class A1/A2, -84 dBm class A3); for software, messages per second on the same capture.

### 1.8 ACARS, VDL2, HFDL, Inmarsat Aero, airband, VOR/ILS

| program | strengths | weaknesses | published numbers | OnAir gap / lead |
|---|---|---|---|---|
| **acarsdec (f00b4r0 fork)** (GPL-2, v4.6 2026-06) | Any number of channels, file/UDP/MQTT output, airframes.io feeding. | CLI | none | **Gap:** MQTT output, airframes.io feeder. |
| **dumpvdl2** (GPL-3, v2.7.0 2026-08) | Many channels, ACARS reassembly, MIAM, ADS-C, CPDLC decoding, many outputs. | CLI | none | **Parity** expected; check CPDLC/ADS-C depth. |
| **dumphfdl** (GPL-3, v1.7.0 2025-11) | Every channel at once, system table, positions. | Its own README says "sensitivity and decoding accuracy needs improvement". | none | **Lead possible:** beat its CRC-error rate on the same capture. |
| **JAERO** (Inmarsat Aero / C-band) | The standard for Inmarsat Aero, ACARS over satellite, voice. | Windows/Qt, aging | none | **Gap:** C-channel and the 10.5k mode coverage; head-to-head counts. |
| **RTLSDR-Airband** (GPL-2, 960 stars, v6.2.0 2026-10-14) | Many AM/NFM channels from one dongle, streaming to Icecast/Broadcastify, very low CPU. | Headless | none | **Gap:** Icecast/Broadcastify streaming of airband channels. |

### 1.9 Maritime: AIS, VDES, NAVTEX, DSC, weather fax, marine VHF

| program | strengths | weaknesses | published numbers | OnAir gap / lead |
|---|---|---|---|---|
| **AIS-catcher** (GPL-3, 798 stars, v0.70 2026-06) | Several demodulation models run in parallel, a built-in web map with statistics, a community feed (aiscatcher.org), a places catalogue, Python binding, a dAISy hardware variant. | Its own disclaimer: a hobby project, not designed for reliability. | none published (the author's blog compares models). | **Gap:** a counts head-to-head, the multi-model trick, a community share. **Lead:** VDES and satellite AIS (OnAir docs/SATELLITE_AIS.md). |
| **rtl-ais** (2018) | Simple | dated | | Lead |
| **YaND / JNX / fldigi (NAVTEX)**, **YaDD (DSC)**, **fldigi/HamFax (WEFAX)** | Mature Windows tools | Windows, sound card only | none | Parity; OnAir decodes from IQ. |

Numbers-to-beat: IEC 61993-2 / ITU-R M.1371 AIS receiver sensitivity -107 dBm at 20 % PER.

### 1.10 Weather satellites (LRPT, HRPT/AHRPT, GOES HRIT, Elektro-L, APT)

| program | strengths | weaknesses | published numbers | OnAir gap / lead |
|---|---|---|---|---|
| **SatDump** (GPL-3, 2.35k stars) | The leader by far: hundreds of pipelines (Meteor, MetOp, FY-3, NOAA, GOES, GK-2A, Elektro, Himawari, Aqua/Terra...), calibrated instruments, projections, map overlays, composites, auto-tracking scheduler, rotator control. | The last tag is 2024-11 (nightlies since). Heavy. | none in dB | **Gap (large):** instrument products (AVHRR, MSU-MR calibrated, MHS, IASI), projections and composites, more satellites (FY-3, GK-2A, Himawari), APT. |
| **goestools** (BSD-2, 425 stars, 2024) | GOES HRIT/EMWIN products, CLI | unmaintained | none | parity |
| **MeteorDemod, meteor_demod, noaa-apt** | Small, focused tools | old | none | parity or lead |

### 1.11 Satellite communications and beacons (Iridium, Inmarsat-C, Orbcomm, CubeSat, Argos, SARSAT)

| program | strengths | weaknesses | published numbers | OnAir gap / lead |
|---|---|---|---|---|
| **gr-iridium + iridium-toolkit** (muccc) | The reference: burst detection, every frame type, ACARS over Iridium, pager, SBD. | GNU Radio, CLI | none | OnAir writes iridium-toolkit bits. **Gap:** in-app parsing of the frames (ring alert, SBD, ACARS) that the toolkit does. |
| **Scytale-C / stdc decoders** (Inmarsat-C EGC) | EGC / SafetyNET messages | Windows | none | parity |
| **SatNOGS client / gr-satellites** (daniestevez) | gr-satellites decodes about 100 cubesats with per-satellite telemetry parsers. | GNU Radio | none | **Gap:** per-satellite telemetry parsers (gr-satellites' list). |

### 1.12 GNSS, SBAS, DGPS, eLoran

| program | strengths | weaknesses | published numbers | OnAir gap / lead |
|---|---|---|---|---|
| **GNSS-SDR** (GPL-3, 2.31k stars, v0.0.22 2026-10-04) | GPS L1/L2C/L5, Galileo E1/E5a/E5b/E6, GLONASS, BeiDou, PVT, RINEX, RTCM, OSNMA. Research-grade. | Config files, no GUI. | Documents acquisition/tracking tests; typical SDR: acquisition about 35 dB-Hz, tracking about 25-28 dB-Hz. | **Gap:** RINEX/RTCM out, the L5/E5 bands, OSNMA. OnAir has GLONASS/Galileo/BeiDou code but the README says "GPS L1": reconcile. |
| **RTKLIB** (BSD-2) | Positioning engine (RTK, PPP) | needs raw measurements | | **Gap:** a raw-measurement output for RTKLIB. |

### 1.13 Digital voice and trunking (DMR, P25, NXDN, dPMR, D-STAR, YSF, M17, TETRA, TETRAPOL, MPT1327, LTR, EDACS)

| program | strengths | weaknesses | published numbers | OnAir gap / lead |
|---|---|---|---|---|
| **SDRTrunk** (GPL-3, Java, 2.21k stars, v0.6.1 2024-12, 454 open issues) | The best trunking UI: P25 phase 1 and 2, DMR Tier III / Capacity+ / Connect+ / Hytera XPT, NXDN, LTR, MPT1327, Passport; many tuners; streaming to Broadcastify, OpenMHz and Rdio Scanner; alias lists. | Heavy on CPU and RAM. Long release gaps. | none | **Gap:** DMR Tier III is not validated (docs/trunking.md), no Capacity+/Connect+/XPT/NXDN trunking, no streaming outputs or alias lists. |
| **DSD-FME** (lwvmobile, 398 stars, 2026-07) | The widest decoder set: DMR, dPMR, NXDN, P25 P1/P2, D-STAR, YSF, EDACS, ProVoice, M17, X2-TDMA; trunking through rigctl or rtl; decryption with user keys. | CLI (ncurses) | none | Parity on protocols; voice codecs are a user install in OnAir (by policy). |
| **OP25 (boatbod)** | P25 P1/P2 trunking, multi-receiver, a web UI, FLL, a noise squelch in dB, lock-time work. | Linux and GNU Radio | none | **Gap:** OP25 publishes lock-time improvements; measure P25 time-to-first-voice. |
| **Unitrunker** (Windows, closed) | Classic control-channel tracker | Windows, stalled | | |
| **TETRA: osmo-tetra + telive, sdrpp-tetra-demodulator** | telive: the standard TETRA monitor | The sdrpp plug-in "needs about 20 dB" to sync. | about 20 dB sync (sdrpp-tetra README) | **Lead:** OnAir can beat 20 dB clearly. Measure TETRA sync SNR. |

### 1.14 Pagers (POCSAG, FLEX, Multitone)

| program | strengths | weaknesses | published numbers | OnAir gap / lead |
|---|---|---|---|---|
| **multimon-ng** (GPL-2, 1144 stars, 1.6.1 2026-09) | Standard; also EAS, DTMF, ZVEI, X10, CW, AFSK. | Audio input, one channel, no FLEX 6400 in older versions. | none | **Lead:** OnAir decodes the whole span at once. |
| **PDW** (Windows, old) | FLEX/POCSAG/ERMES, filters | old | | **Gap:** ERMES (not in OnAir). |
| **SDRangel pager demod**, **OpenWebRX+** | | | | |

### 1.15 Cellular and railway

| program | strengths | weaknesses | published numbers | OnAir gap / lead |
|---|---|---|---|---|
| **LTE-Cell-Scanner** (AGPL), **srsRAN 4G/Project** (AGPL) | srsRAN: full stack, cell search. | Not a monitor GUI | none | **Lead:** OnAir has a GUI with LTE, NR SSB, NB-IoT and a map. |
| **gr-gsm** (1.5k stars) | GSM BCCH/CCCH, the standard | dated | | **Gap:** GSM (non-R) cell info; OnAir has GSM-R only. |
| **NR-Scope / 5GSniffer** (research) | NR SIB1 and DCI | research | | **Gap:** NR SIB1 (README: "not decoded yet"). |

### 1.16 IoT and ISM

| program | strengths | weaknesses | published numbers | OnAir gap / lead |
|---|---|---|---|---|
| **rtl_433** (GPL-2, 7.89k stars, 25.12) | About 380 device protocol entries, flex decoders, MQTT/InfluxDB/JSON, a huge community. | CLI | `-Y minsnr` tunable; no thresholds | **Gap (large):** OnAir has about 45 registered ISM decoders (16 added uncompiled on ism-315-030). Add MQTT/HA output. |
| **wmbusmeters / rtl-wmbus** | Meter drivers, Home Assistant | | | **Gap:** meter-driver depth, HA output. |
| **gr-lora_sdr** (EPFL, GPL-3, 1.02k stars) | Fully working LoRa PHY at low SNR (STO/CFO correction, soft decoding). | GNU Radio | Paper (arXiv 2002.08208) has the SNR curves. | **Numbers-to-beat:** Semtech demodulator limits, below. |
| **Meshtastic firmware** (8.47k stars) | Hardware nodes | | | **Lead:** OnAir follows every channel in an SDR capture. |
| **gr-ieee802-15-4 / -11, Ubertooth, ice9** | | | | parity |

### 1.17 Amateur weak signal (FT8, FT4, WSPR, JT65/JT9/JT4, Q65, FST4/W, MSK144, JS8)

| program | strengths | weaknesses | published numbers | OnAir gap / lead |
|---|---|---|---|---|
| **WSJT-X** (GPL-3) | The reference. AP decoding, multi-pass subtraction, deep search, the QSO engine, PSK Reporter. | Sound card plus rig, one band. | Table below (protocol S/N thresholds). | **Gap:** show decode counts on the same WAV sets (OnAir has AP code in 12 files). |
| **JTDX / MSHV / WSJT-X Improved** | JTDX: more decodes in crowded bands, by user reports; MSHV: multi-stream TX. | Windows-centric | none | **Gap:** a decode-count head-to-head on dense FT8 WAVs. |
| **ft8_lib** (MIT) | Small FT8 library | fewer decodes | | |
| **wsprd / WSPR daemon (wsprdaemon), KA9Q-radio** | Multi-band from one SDR | Linux | -31 dB (wsprd deep) | **Lead:** OnAir decodes multi-band (hfdig_multiband) with TX in one GUI. |
| **JS8Call** | Keyboard chat | quiet | -24 dB (normal) [JS8 docs] | parity |

### 1.18 Amateur keyboard, picture, voice, packet

| program | strengths | weaknesses | published numbers | OnAir gap / lead |
|---|---|---|---|---|
| **fldigi** | Every keyboard mode, macros, logging, a multi-signal PSK browser, NAVTEX, WEFAX. | Sound card | none | **Gap:** a signal browser (many PSK/RTTY decodes at once; OnAir CW has 14 kHz), macros. |
| **CW Skimmer** (VE3NEA, commercial) | Hundreds of CW signals, a callsign database, RBN | Windows, paid | | **Gap:** RBN-style skimmer output. |
| **MMSSTV / QSSTV / Robot36 (0BSD, Android)** | Mature SSTV, auto slant | | | parity |
| **EasyPal / QSSTV (HamDRM)** | | | | parity |
| **FreeDV GUI** (2.4.0 2026-08) | RADE (the neural vocoder mode), PSK Reporter | | | **Gap:** RADE (queued earlier). |
| **Dire Wolf** (GPL-2, 2.07k stars, 1.8.1 2025-11) | The best software TNC: multiple slicers and FX.25/IL2P; publishes WA8LMF track 2 decode counts. | | Track 2 counts in its docs (about 1000 frames) | **Gap:** FX.25, IL2P; OnAir's packet doc says it has not been run on the WA8LMF CD yet. |

### 1.19 HF utility (ALE, SITOR, ARDOP, NAVTEX, beacons, military ID)

| program | strengths | weaknesses | published numbers | OnAir gap / lead |
|---|---|---|---|---|
| **Wavecom W-Code / Code300 / Sorcerer / SigintOS** (commercial) | Hundreds of HF/VHF protocols, a classifier | expensive or closed | | **Gap:** breadth (STANAG 4285/4539/MIL-188-110, PACTOR detection, 3G ALE). OnAir only identifies the military waveforms. |
| **MultiPSK** (F6CTE, freeware) | An enormous mode list (ALE, SITOR, PACTOR-1, DGPS, ACARS, ...). | Windows, dated UI | | Parity on many; **gap** on PACTOR-1 and 3G ALE. |
| **PC-ALE / MARS-ALE** | 2G ALE | dated | | parity |

### 1.20 Weather and science (radiosondes, Horus, SAME, time signals, hydrogen line)

| program | strengths | weaknesses | published numbers | OnAir gap / lead |
|---|---|---|---|---|
| **radiosonde_auto_rx** (GPL-3, v1.9.0 2026-09) + **rs1729/RS** | Every common sonde type, SondeHub upload, auto scan, prediction. | Linux | none | **Gap:** SondeHub upload (check), and the rarer types (iMet-4, MRZ, MTS01, ...). |
| **horus-gui / horusdemodlib** | Horus 4FSK reference with SondeHub upload | | | parity; check SondeHub upload. |
| **dsame / multimon-ng EAS** | | | | parity |

### 1.21 Front ends, general receivers, spectrum

| program | strengths | weaknesses | published numbers | OnAir gap / lead |
|---|---|---|---|---|
| **SDR++** (GPL-3, 6.47k stars, nightly) | Fast, smooth waterfall, many VFOs, a server mode, plug-in modules (TETRA, DAB, Meteor, M17, ...), low CPU. | Few decoders built in. | | **Gap:** waterfall smoothness and zoom feel (spectrum-sdrpp-030). |
| **SDR# (Airspy, Windows, closed)** | The most used on Windows, a large plug-in ecosystem, polished DSP (IF noise reduction, co-channel canceller). | Windows only, closed | | **Gap:** co-channel canceller, IF noise reduction. |
| **SDRuno / SDRconnect** (SDRplay) | Tuned for RSP hardware | SDRplay only | | |
| **SDR Console V3** (Windows) | Multiple receivers, recordings, server | Windows | | |
| **GQRX / CubicSDR** | Simple, Linux/mac | few decoders; CubicSDR stalled (0.2.7 2022) | | Lead |
| **SDRangel** (GPL-3, 4.13k stars, v7.27.2 2026-08) | **The closest all-in-one rival:** DATV (leansdr), DAB, DRM, ADS-B, AIS, VOR/ILS, pager, APT, radiosonde, FT8, M17, DSD voice modes, packet, Meshtastic, MeshCore, a map, a satellite tracker and a star tracker; Android. | Complex UI. | none | **Lead:** OnAir has DVB-T2, ATSC, ISDB, the TV player and far more modes. **Gap:** SDRangel's map, satellite tracker and multi-device features as a single workspace. |
| **OpenWebRX+** (AGPL, 645 stars, 1.2.117 2026-06) | Multi-user web, background decoding (FT8, SSTV, fax, AIS, ADS-B, ACARS, VDL2, HFDL, pagers, ISM...), a map, bookmarks. | Web only; DRM/voice through add-ons. | | **Gap:** OnAir Web is built but "not tried with a real radio" (docs/ONAIR-WEB.md). |
| **KiwiSDR** | A public network of about 600+ receivers, TDoA, extensions | hardware | | OnAir can use Kiwis (docs/kiwisdr.md, untested live). |

---

## 2. Ranked gap list (40)

Effort: S = days, M = a week or two, L = more. Owner = the branch that should take it.

| # | gap | why | effort | owner |
|---|---|---|---|---|
| 1 | Publish a per-mode threshold and lock-time table (median/p90) from the sens harness, with competitor columns | No program publishes these: whoever publishes first, with method, owns "best". Without it every claim is unproven. | M | sens-harness-030, all dec-*-030 |
| 2 | DVB-S2: reach EN 302 307 Table 13 + ≤0.5 dB for every MODCOD and beat MiniTiouner's ~1 s lock | DATV users compare against the STV0910 hardware; the standard is the bar. | M | dec-dvbs-030 |
| 3 | DVB-T2/T: thresholds within 1 dB of the IG/NorDig figures, plus multi-PLP switching and T2-MI tested | The only other open T2 receiver is a prototype: make the lead measurable. | M | dec-dvbt-030 |
| 4 | Real-signal validation list: ATSC 3.0, ISDB-T, T2-Lite, VDL4, SGB, Argos, NFC (all "generated only") | "Works on generated signals" is the most common caveat in the README. Ask Discord for captures. | M | dec-* owners |
| 5 | ADS-B messages/s head-to-head against readsb and dump1090-fa (2.0/2.4/8 Msps), then close any gap | ADS-B is the most-used SDR mode. readsb is the bar. | M | dec-aviation-030, perf2-aviation-data-030 |
| 6 | ADS-B MLAT client plus a history/heatmap/replay map (tar1090 class) | Feeders expect MLAT. tar1090 is what everyone looks at. | L | dec-aviation-030 |
| 7 | Trunking: validate DMR Tier III, add Capacity+/Connect+/XPT and NXDN trunking, alias lists | SDRTrunk's main draw. OnAir has the follow logic but not the protocols. | L | dec-voice-030 |
| 8 | Streaming outputs: Broadcastify/Icecast/Rdio Scanner/OpenMHz for trunking and airband | SDRTrunk and RTLSDR-Airband users choose those tools for this. | M | dec-voice-030 |
| 9 | Weather satellites: SatDump-class products (calibrated AVHRR/MSU-MR, projections, map overlays, composites), plus FY-3, GK-2A, Himawari and NOAA APT | SatDump dominates. Picture quality is what users compare. | L | dec-wxsat-030 |
| 10 | ISM: grow from about 45 to 150+ rtl_433 protocols (compile and test the 16 uncompiled ones first), plus MQTT/Home Assistant output | rtl_433 has about 380 entries. Home-automation users need MQTT. | L | dec-ism-030 |
| 11 | RDS: RT+, eRT, RDS2, redsea-compatible JSON, a weak-signal PI/PS "best guess" | redsea and FM-DX tools beat OnAir on RDS depth. Cheap win. | S | dec-fm-030 |
| 12 | FT8/FT4: decode-count head-to-head against WSJT-X/JTDX on dense WAV sets (AP + subtraction passes) | Ham users judge by decodes per slot. | M | dec-hf-030, perf2-hf-030 |
| 13 | AIS: run several demodulation models in parallel (AIS-catcher trick) and publish counts against AIS-catcher | The AIS-catcher models are its edge. Counts are easy to compare. | M | dec-maritime-030 |
| 14 | DAB: scheduled recording, announcement switching, a DX scan log (CSV/JSON), FMLIST upload, DL+ display check | AbracaDABra and Qt-DAB UX parity. Everything else is already at parity or ahead. | M | dec-dab-030 |
| 15 | DAB: EDI/ETI input and ETI output | DABlin and Qt-DAB users feed or record ensembles over the network. | S | dec-dab-030 |
| 16 | DRM: run the six DRM MRR channel tests (BER below 1e-4) and publish the lowest S/N per test; add RSCI out | Turns the existing fade audit into the official yardstick. Dream has RSCI. | M | dec-drm-030 |
| 17 | GNSS: reconcile the README ("GPS L1") with the multi-constellation code; RINEX/raw output for RTKLIB; publish C/N0 acquisition/tracking thresholds | GNSS-SDR's strengths; position accuracy is what users compare. | M | dec-gnss-030 |
| 18 | LoRa: thresholds per SF within 1 dB of Semtech's demodulator limits | gr-lora_sdr already does it at low SNR. Meshtastic users compare range. | S | dec-lora-030 |
| 19 | Packet: run the WA8LMF track 1/2 test and add FX.25 and IL2P | Dire Wolf publishes counts. FX.25/IL2P are in current use. | S | dec-packet-030 |
| 20 | Spectrum/waterfall feel at SDR++ level (frame pacing, zoom, many VFOs) | The first thing a reviewer compares. | M | spectrum-sdrpp-030 |
| 21 | One-button mode auto-detect across the capture, with confidence | No competitor has it. It would be a headline lead. | L | autodetect-030 |
| 22 | Uniform signal meter (level, SNR, MER, BER, lock state) across every mode panel | Today it differs per mode. Hardware DVB tools always show MER/BER. | M | ui-clean-meter-030 |
| 23 | DVB: tvheadend-class EPG export (XMLTV), scheduled recording, m3u/HDHomeRun emulation for Plex/Kodi | People keep tvheadend for these. The network tuner exists already. | M | dec-dvbt-030 |
| 24 | HD Radio: traffic/weather map images and album art at nrsc5-dui level; more AM tests | The nrsc5 GUIs show them. OnAir already supports every SDR. | S | dec-hdradio-030 |
| 25 | TETRA: publish the sync SNR (sdrpp-tetra needs about 20 dB) and the neighbour/SDS decode depth against telive | An easy measurable win. | S | dec-voice-030 |
| 26 | P25: time-to-first-voice and lock time against OP25 | OP25 advertises lock-time work. | S | dec-voice-030 |
| 27 | HFDL: CRC-error rate below dumphfdl's on the same capture | dumphfdl itself says its sensitivity needs work: a clear lead to take. | S | dec-aviation-030 |
| 28 | ACARS/VDL2: MQTT and airframes.io feeder outputs; CPDLC/ADS-C depth check against dumpvdl2 | Feeding communities use acarshub and airframes. | S | dec-aviation-030 |
| 29 | Iridium: in-app parsing of ring alerts, SBD, ACARS and pager (iridium-toolkit parity) | Today the bits are exported only. | M | dec-satcom-030 |
| 30 | CubeSat: per-satellite telemetry parsers (gr-satellites list), SatNOGS upload | gr-satellites covers about 100 satellites. | M | dec-satcom-030 |
| 31 | OnAir Web: test with a real radio, many users, background decoders (OpenWebRX+ parity) | OpenWebRX+ owns multi-user web SDR. | M | (web owner) |
| 32 | General receiver: co-channel canceller and IF noise reduction (SDR#-class DSP) | AM/SSB listeners compare audio quality. | M | dec-general-030 |
| 33 | Keyboard modes: a multi-signal browser for PSK/RTTY (fldigi), CW skimmer/RBN output | fldigi and CW Skimmer are the bar for keyboard modes. | M | dec-hf-030 |
| 34 | GSM (non-R) BCCH cell info, and NR SIB1 | gr-gsm users; the README says NR SIB1 is "not decoded yet". | M | dec-cell-030 |
| 35 | Radiosondes: SondeHub upload check and the rarer sonde types against auto_rx | auto_rx users upload to SondeHub. | S | dec-sonde-030 |
| 36 | HF utility: 3G ALE, PACTOR-1 detection, STANAG 4285 demodulation (MultiPSK/Wavecom breadth) | The utility-DX crowd uses MultiPSK and Wavecom. | L | dec-hfutil-030 |
| 37 | Pagers: ERMES, plus multimon-ng extras (EAS, ZVEI already in the tone decoders) | PDW users | S | dec-pager-030 |
| 38 | Weather fax/NAVTEX/DSC: counts against YaND/JNX and fldigi on the same recordings | Cheap validation | S | dec-maritime-030 |
| 39 | Fix the README "What it does" table: duplicated rows (Broadcast radio ×3, TV ×2, Aviation ×2, Amateur ×3, IoT ×2, Weather ×2, Satellite ×2, Two-way ×2) with conflicting counts | It is the first table a visitor reads, and it contradicts itself. | S | (docs) |
| 40 | Release cadence and changelog per mode (competitor-style "what's new" pages) | Long silent gaps hurt SDRTrunk and SatDump. OnAir ships often: make it visible. | S | (release) |

---

## 3. Numbers to beat per mode

Standard figures are for an ideal or reference receiver. A software receiver within 0.5-1 dB of them is excellent. Competitor
figures are listed where any exist.

### DVB-S2: ideal Es/N0 at QEF (PER 1e-7, AWGN, 64800 frame; short frames +0.2..0.3 dB). Source: ETSI EN 302 307-1 Table 13, https://www.etsi.org/deliver/etsi_en/302300_302399/30230701/01.04.01_60/en_30230701v010401p.pdf

| MODCOD | dB | MODCOD | dB | MODCOD | dB |
|---|---|---|---|---|---|
| QPSK 1/4 | -2.35 | QPSK 3/4 | 4.03 | 8PSK 3/4 | 7.91 |
| QPSK 1/3 | -1.24 | QPSK 4/5 | 4.68 | 8PSK 5/6 | 9.35 |
| QPSK 2/5 | -0.30 | QPSK 5/6 | 5.18 | 8PSK 8/9 | 10.69 |
| QPSK 1/2 | 1.00 | QPSK 8/9 | 6.20 | 16APSK 2/3 | 8.97 |
| QPSK 3/5 | 2.23 | QPSK 9/10 | 6.42 | 16APSK 3/4 | 10.21 |
| QPSK 2/3 | 3.10 | 8PSK 3/5 | 5.50 | 32APSK 3/4 | 12.73 |
| | | 8PSK 2/3 | 6.62 | 32APSK 9/10 | 16.05 |

(The QPSK rows were checked against the PDF text on 2026-10-23. The 8PSK/APSK rows are the same table as commonly quoted.)
- DVB-S2X VL-SNR: down to about -10 dB Es/N0 (π/2-BPSK 1/5 with spreading), EN 302 307-2.
- DVB-S (EN 300 421): Eb/N0 after Viterbi for QEF after RS: 1/2 4.5, 2/3 5.0, 3/4 5.5, 5/6 6.0, 7/8 6.4 dB. https://www.etsi.org/deliver/etsi_en/300400_300499/300421/01.01.02_60/en_300421v010102p.pdf
- MiniTiouner Express: -100 dBm cliff at FEC 1/2 and -96 dBm at 7/8 (1288 MHz), https://rigpix.com/atv/minitiouner_express.htm; QO-100 beacon about 8.5-8.9 dB MER on a 1 m dish, https://forum.batc.org.uk/viewtopic.php?p=12600 ; lock in about 1 s (hardware).

### DVB-T: C/N for BER 2e-4 after Viterbi (QEF after RS), Gaussian channel. Source: ETSI EN 300 744 Annex A Table A.1, https://www.etsi.org/deliver/etsi_en/300700_300799/300744/01.06.02_60/en_300744v010602p.pdf

| | 1/2 | 2/3 | 3/4 | 5/6 | 7/8 |
|---|---|---|---|---|---|
| QPSK | 3.1 | 4.9 | 5.9 | 6.9 | 7.7 |
| 16-QAM | 8.8 | 11.1 | 12.5 | 13.5 | 13.9 |
| 64-QAM | 14.4 | 16.5 | 18.0 | 19.3 | 20.1 |

(Recalled from the standard, not re-extracted for this file: dec-dvbt-030 should confirm them from the PDF.)

### DVB-T2: the DVB-T2 Implementation Guidelines (ETSI TR 102 831) give the Gaussian-channel C/N per constellation and rate. QPSK uses the same LDPC as DVB-S2 (QPSK 1/2 is about 1 dB). Measured planning value: 256-QAM 2/3, 32k, PP7 extended = 20.0 dB minimum C/N (indicative, https://www.imda.gov.sg/-/media/imda/files/regulation-licensing-and-consultations/ict-standards/telecommunication-standards/imda-technical/mediabroadcast_gmbh_revised.pdf). Planning reference: ITU-R BT.2254. TR 102 831: https://www.etsi.org/deliver/etsi_tr/102800_102899/102831/ (the download failed here; dec-dvbt-030 should extract the table).

### ATSC 1.0: 15.2 dB C/N (8-VSB, AWGN TOV), ATSC A/54 recommended practice. ATSC 3.0, ISDB-T, DTMB: use the A/322 and ARIB B31 tables; no SDR competitor publishes any figure.

### DAB / DAB+: no competitor figure. OnAir's own: no audio loss at 4 dB AWGN and at ±100 ppm after the fix (docs/DAB-AUDIT.md). The bar to show: equal or better, with a welle.io / Qt-DAB head-to-head on the same IQ.

### DRM: DRM Minimum Receiver Requirements v2 (2015): lowest S/N for an average BER below 1e-4 on channel models 1-6 (A/64QAM/0.78/short, A/64QAM/0.71, B/64QAM/0.71/long, B/64QAM/0.6, C/16QAM/0.6, D/64QAM/0.5), https://www.drm.org/wp-content/uploads/2015/02/Minimum-Receiver-Requirements_v2-2015.pdf . Planning figures quoted from EBU Tech 3330: 64-QAM, 9 kHz, channel 1: 14.1 dB (protection level 0), 15.3 dB (level 1), https://www.stereonet.com/forums/topic/222431-transmission-coverage-variation-for-drm-depending-on-modulation-constellation/ . Fraunhofer receiver sensitivity: below -102 dBm (mode A, 64-QAM), below -108 dBm (mode B, 16-QAM), https://users.ece.utexas.edu/~gerstl/ee382v_f14/soc/drm/Fraunhofer_Software_Radio.pdf .

### FM RDS: redsea runs at 2564 groups/s on an M1 MacBook and 26/s on a Pi 1 (needs 11.4), https://github.com/windytan/redsea/wiki/Benchmark-results . Sensitivity: a plot of correct syndromes against subcarrier SNR, no numbers.

### ADS-B: RTCA DO-260B minimum trigger level -79 dBm (A1/A2) / -84 dBm (A3). Software bar: messages per second and the maximum range of readsb on the same capture (https://github.com/wiedehopf/readsb; no table published).

### AIS: receiver sensitivity -107 dBm at 20 % PER (IEC 61993-2, ITU-R M.1371). Software bar: message counts against AIS-catcher on the same IQ (https://github.com/jvde-github/AIS-catcher).

### WSJT family: S/N thresholds in 2500 Hz, WSJT-X 2.7 User Guide, protocol table, https://wsjt.sourceforge.io/wsjtx-doc/wsjtx-main-2.7.0.html

| mode | dB | mode | dB | mode | dB |
|---|---|---|---|---|---|
| FT8 | -21 | JT65A | -25 | FST4-60 | -28.1 |
| FT4 | -17.5 | JT9A | -26 | FST4-120 | -31.3 |
| WSPR | -31 | JT4A | -23 | FST4-300 | -35.3 |
| FST4W-120 | -32.8 | Q65-15A | -22.2 | FST4-900 | -40.2 |
| FST4W-300 | -36.8 | Q65-30A | -24.8 | FST4-1800 | -43.2 |
| FST4W-900 | -41.7 | Q65-60A | -27.6 | FST4-15 | -20.7 |
| FST4W-1800 | -44.8 | Q65-120A / 300A | -30.8 / -33.8 | FST4-30 | -24.2 |

JT9 B-H: -26 to -20; JT4 B-G: -22 to -17; JT65 B/C: -25 (same page). JS8Call normal: about -24 dB (JS8Call docs). The bar is
these thresholds plus WSJT-X's own decode count on the same WAV sets.

### LoRa (Semtech SX1276/77/78 datasheet, demodulator SNR limit): SF7 -7.5, SF8 -10, SF9 -12.5, SF10 -15, SF11 -17.5, SF12 -20 dB, https://www.semtech.com/products/wireless-rf/lora-connect/sx1276 . The gr-lora_sdr curves: https://arxiv.org/pdf/2002.08208 .

### GNSS: typical SDR acquisition about 35 dB-Hz and tracking about 25-28 dB-Hz C/N0 (GNSS-SDR docs, https://gnss-sdr.org/docs/ ); the bar is a cold-start time to fix and the position error on the same capture.

### TETRA: sdrpp-tetra-demodulator needs "~20 dB" of signal to sync, https://github.com/cropinghigh/sdrpp-tetra-demodulator . The reference sensitivity of EN 300 392-2 (clause 6) is the formal bar: dec-voice-030 should take it from the standard (it was not looked up here).

### APRS / AX.25 1200: Dire Wolf's WA8LMF TNC test CD track 2 decode counts (about 1000 frames for recent versions, from memory: check the count in its docs), https://github.com/wb2osz/direwolf/tree/master/doc .

### HFDL: dumphfdl's own note that its CRC-error rate is too high, https://github.com/szpajder/dumphfdl . The bar: more good PDUs per hour on the same capture.

### Popularity snapshot (stars / latest release / last push, 2026-10-23)

rtl_433 7.9k / 25.12 / 2026-10; Meshtastic firmware 8.5k; SDR++ 6.5k / nightly / 2026-10; GNU Radio 6.3k / 3.10.12; SDRangel
4.1k / 7.27.2 2026-08; srsRAN 4G 4.1k; GQRX 3.7k / 2.17.7; tvheadend 3.6k; antirez dump1090 3.0k; SatDump 2.35k / 1.2.2 2024-11;
GNSS-SDR 2.3k / 0.0.22 2026-10; CubicSDR 2.3k / 0.2.7 2022; SDRTrunk 2.2k / 0.6.1 2024-12; Dire Wolf 2.1k / 1.8.1; tar1090 1.9k;
gr-gsm 1.5k; OpenWebRX 1.3k (OpenWebRX+ 645 / 1.2.117); multimon-ng 1.1k / 1.6.1; dump1090-fa 1.1k; gr-lora_sdr 1.0k; nrsc5 971 /
3.2.0; RTLSDR-Airband 960 / 6.2.0; AIS-catcher 798 / 0.70; welle.io 754 / 2.7; readsb 696; noaa-apt 661; radiosonde_auto_rx 617 /
1.9.0; gr-iridium 494; OP25 489; redsea 443 / 1.3.1; goestools 425; DSD-FME 398 / 2026-07; Qt-DAB 362 / 7.3.1.1 2026-10;
FreeDV 331 / 2.4.0; dumpvdl2 259 / 2.7.0; gr-rds 252; dumphfdl 162 / 1.7.0; leansdr 168 (2022); DABlin 144 / 1.16.1;
gr-dvbs2rx 138 / 1.4.0 (2023); sdr_receiver_dvb_t2 81.

## 4. Where OnAir already leads (from the repo docs, before any measurement)

1. Breadth in one GUI: 144 modes plus plug-ins and the Mode Builder. SDRangel, the nearest rival, has roughly a quarter of the modes.
2. DVB-T2 (with T2-Lite and MISO signalling) as a full SDR receiver with a player. The only other one is a prototype.
3. ATSC 3.0 over SDR. No open competitor has a physical layer.
4. ISDB-T, ISDB-S, DTMB and DVB-C with the same player and channel scanner, plus a shared crowd-sourced channel list.
5. DVB-S2X VL-SNR plus DecSAT-parity TS analysis (DVB-RCS, CID, multicast audio) in one app. leandvb and gr-dvbs2rx have neither.
6. Several adjacent DVB channels from one wide capture. Hardware tuners need one tuner per channel.
7. DRM with xHE-AAC (USAC), DRM+ and the data services, plus a fade/clock audit. Dream lacks xHE-AAC and is unmaintained.
8. Every WSJT mode, with TX and a multi-band decoder inside a SDR app (SDRangel has FT8 only).
9. Whole-span, many-channel decoding in one process: pagers, APRS, VDL2, HFDL, dPMR, marine VHF. Most competitors need one instance per channel.
10. Niche modes nobody else has in a GUI: VDL Mode 4, Mode S uplink, UAT + FIS-B, SBAS, Argos, Sigfox, Wi-SUN, Wi-Fi HaLow, GOES DCS, SGB SARSAT, the TV sat beacon decoder.
11. Platforms: macOS native (HackRF Pro), Windows, Linux, Android, Raspberry Pi; any SoapySDR radio; a headless server and a remote API.
