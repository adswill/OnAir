<p align="center"><img src="packaging/icons/onair_1024.png" width="140" alt="OnAir logo"></p>

<h1 align="center">OnAir</h1>

<p align="center"><b>Receive TV, radio, aircraft, ships, satellites, two-way radio, pagers, time signals and more: 83 kinds of signal with one software-defined radio.</b><br>
Free and open source, for macOS, Windows and Linux.</p>

<p align="center">
  <a href="https://github.com/adswill/OnAir/releases/latest"><img src="https://img.shields.io/github/v/release/adswill/OnAir?label=download&color=2ea44f" alt="Latest release"></a>
  <a href="https://github.com/adswill/OnAir/releases"><img src="https://img.shields.io/github/downloads/adswill/OnAir/total?color=blue" alt="Downloads"></a>
  <img src="https://img.shields.io/badge/platforms-macOS%20%7C%20Windows%20%7C%20Linux-lightgrey" alt="Platforms">
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-GPL--3.0-orange" alt="License: GPL-3.0"></a>
  <a href="https://on-air.dev"><img src="https://img.shields.io/badge/website-on--air.dev-1b232b" alt="Website"></a>
  <a href="https://discord.gg/Kky9c6atm"><img src="https://img.shields.io/badge/chat-Discord-5865F2?logo=discord&logoColor=white" alt="Discord"></a>
  <a href="https://ko-fi.com/adswill"><img src="https://img.shields.io/badge/%E2%9D%A4-Donate-FF5E5B" alt="Donate"></a>
</p>

<p align="center">
  <a href="https://on-air.dev"><b>Website</b></a> &nbsp;·&nbsp;
  <a href="https://github.com/adswill/OnAir/releases/latest"><b>Download</b></a> &nbsp;·&nbsp;
  <a href="https://github.com/adswill/OnAir/releases/download/v0.2.0/OnAir_promo_v0.2.0.mp4"><b>Overview video</b></a> &nbsp;·&nbsp;
  <a href="https://discord.gg/Kky9c6atm"><b>Discord</b></a> &nbsp;·&nbsp;
  <a href="https://files.on-air.dev"><b>Signal samples</b></a> &nbsp;·&nbsp;
  <a href="https://github.com/adswill/OnAir-channels"><b>Channel list</b></a>
</p>

---

OnAir turns a low-cost SDR (a HackRF, an RTL-SDR, an Airspy and many other radios) into a receiver for most of what is on the air: digital TV (DVB-T/T2, ATSC, ISDB-T, DTMB, DVB-S/S2, DVB-C), digital and analogue radio (DAB+, DRM, HD Radio, FM with RDS), aircraft and ships (ADS-B, ACARS, VDL2, HFDL, AIS), satellites (weather images from NOAA, Meteor, GOES and Elektro-L, Inmarsat, Iridium, Orbcomm, GNSS), two-way radio (DMR, D-STAR, NXDN, dPMR, P25, M17, TETRA, TETRAPOL), pagers, HF data modes (FT8, WSPR, RTTY, PSK, SSTV, weather fax, NAVTEX), IoT (LoRa, Meshtastic, Zigbee, Bluetooth LE, Wi-Fi), time stations, cell towers and more. Every mode is receive-only. Plug in an antenna, pick a mode, and watch what happens inside: the spectrum, the sync, the error correction and the decoded data.

It is built for people who want a receiver that is both easy to use and honest about what is happening inside it.

## Contents

- [Install](#install) (and the [development version](#development-version-nightly))
- [Quick start](#quick-start)
- [What it receives](#what-it-receives)
- [Features](#features)
- [Supported radios](#supported-radios) (and [network radios](#network-radios))
- [Remote-control API](#remote-control-api)
- [Updates](#updates)
- [Good to know](#good-to-know)
- [Build from source](#build-from-source)
- [Support and community](#support-and-community)
- [Supporting OnAir](#supporting-onair)
- [License](#license)

## Install

Download the file for your system from the **[latest release](https://github.com/adswill/OnAir/releases/latest)**. There is nothing to compile.

| System | Download | How to install |
|---|---|---|
| **macOS** 15 or later | `OnAir-…-macos-arm64.dmg` (Apple silicon)<br>`OnAir-…-macos-x86_64.dmg` (Intel) | Open the `.dmg` and drag OnAir to Applications. The app is not signed with a developer certificate yet, so on the first launch right-click it and choose *Open*. Intel Macs need a CPU with AVX2 (2013 or newer). |
| **Windows** 10 / 11, 64-bit | `OnAir-…-windows-x64-setup.exe`<br>or `OnAir-…-windows-x64.zip` | Run the installer, or unpack the zip and start `OnAir.exe`. Windows may warn about an unknown publisher because the installer is not code-signed. Needs a CPU with AVX2 (Intel or AMD from about 2013 on). |
| **Linux** x86-64 and arm64 | `onair_…_amd64.deb` / `onair_…_arm64.deb`<br>or `onair-…-portable.tar.gz` | Debian / Ubuntu: `sudo apt install ./onair_<version>_<arch>.deb`, then start OnAir from the menu or with `onair`. The `.deb` carries its own video decoder, so it does not depend on your system's FFmpeg. Other distributions: unpack the portable archive and run `bin/onair`. |
| **Docker** (a server without a screen, used from a browser) | `packaging/docker` in this repository | `docker compose up -d --build` in that folder, then open `https://<server>:3001`. See [packaging/docker/README.md](packaging/docker/README.md) for radios, graphics chips and settings. |

**SDRplay RSPs (RSP1, RSP1A, RSP1B, RSP2, RSPduo, RSPdx):** install the free **SDRplay API** from [sdrplay.com/api](https://www.sdrplay.com/api/) once, and restart OnAir. It is SDRplay's own driver and service, and its licence does not allow other programs to ship it, so OnAir cannot include it. Make sure the "SDRplay API Service" is running (it is started by the installer). An RSP-compatible radio that is not made by SDRplay (for example a Mirics MSi2500 "RSP1A clone") is not seen by the SDRplay API; use SoapySDR's `mirisdr` module for it instead. The RSPs deliver up to 10 MHz of bandwidth (10.66 Msps).

**Windows USB drivers:** for a HackRF, RTL-SDR, Airspy, Airspy HF+, Fobos SDR or RX888 MkII (its boot loader 04b4:00f3 and its running firmware 04b4:00f1 both need it), install the WinUSB driver once with [Zadig](https://zadig.akeo.ie). A LimeSDR Mini needs FTDI's FT60x driver (see the README inside the download). A LimeSDR-USB (Cypress FX3) needs the Cypress/LimeSuite USB driver (CyUSB3, the one LimeGUI installs), not WinUSB: do not replace it with Zadig. OnAir's own LimeSuite has no FX3 support, so for a LimeSDR-USB also install [PothosSDR](https://downloads.myriadrf.org/builds/PothosSDR/) or LimeSuite; OnAir finds its LimeSuite.dll by itself and the log (onair.log) says which copy saw the radio ("LimeSuite ...: N radio(s)").

### Development version ("nightly")

Fixes and new features land on the `main` branch first, before they go into a release. To try them early, [build OnAir from source](#build-from-source) from the latest `main`.

> [!WARNING]
> The development version is **not tested like a release**. It can contain bugs, unfinished features or changes that stop parts of the app (or the whole app) from working. Use the [latest release](https://github.com/adswill/OnAir/releases/latest) for everyday use, and if something breaks in a development build, please say so on [Discord](https://discord.gg/Kky9c6atm) or in [GitHub Issues](https://github.com/adswill/OnAir/issues), mentioning the commit you built.

## Quick start

1. Connect your radio and antenna, then start OnAir. Connected radios appear in the source menu.
2. Pick what you want to receive in the mode list on the left.
3. Enter a frequency, or open the **Scan** tab to find channels automatically.
4. Press **Start**. When the receiver locks, the channels appear on the right: click one to watch or listen.
5. If the picture breaks up, use **Auto-tune** for the gain (or **Optimise gain**, which sweeps the radio's whole gain range, measures the mode's SNR, or else the signal-to-noise of the spectrum around the channel, and keeps the lowest gain within 0.5 dB of the best that does not overload the ADC; **Undo gain** goes back) and watch the quality score while you move the antenna. Height and a clear view toward the transmitter matter more than anything else.

> **No radio yet?** Press **Tour**: a friendly little TV walks you through OnAir using a built-in demo signal, so you can explore everything before buying a radio.

## What it receives

| Standard | Where it is used | Content |
|---|---|---|
| **DVB-T2** (including T2-Lite) | Europe, Middle East, Africa, Asia, Australia | Digital TV |
| **DVB-T** | Same regions, older networks | Digital TV |
| **ATSC 1.0** | North America, South Korea | Digital TV |
| **ATSC 3.0** (NextGen TV), *experimental* | North America, South Korea | Digital TV |
| **ISDB-T**, *experimental* | Japan, Brazil and most of South America, Philippines | Digital TV and one-segment mobile TV |
| **DAB / DAB+** | Europe, Australia and more | Digital radio; Journaline text and menu pages (PAD or packet mode, ETSI TS 102 979) |
| **FM radio** (87.5 – 108 MHz) | Worldwide | Analogue radio: stereo and RDS (station name, radio text, programme type, traffic flags) |

The DVB standard is detected automatically.

### Also receives (experimental)

These receivers are built from the published specifications and checked end to end on simulated signals, but have had little or no testing on real transmissions yet. Reports and recordings on [Discord](https://discord.gg/Kky9c6atm) help a lot, and you can upload a recording to the [signal sample library](https://files.on-air.dev).

| Mode | What it is | What you see |
|---|---|---|
| **DVB-S / DVB-S2** | Satellite TV (through an LNB) | Channels and playback, all DVB-S2 MODCODs |
| **DTMB** | Digital TV in China, Hong Kong, Macau (8 MHz) and Cuba (6 MHz) | Channels and playback |
| **DVB-C** | Cable TV, EN 300 429 / J.83 annex A and C, 16 to 256-QAM (needs a cable feed) | Symbol rate and QAM found automatically, channels and playback |
| **Analog TV** | PAL, SECAM, NTSC; systems B/G, I, D/K, M, N, H, K', positive-modulation A (405 lines), C, E (819 lines), L, L'; NBTV and mechanical TV | Picture and sound |
| **MAC** | D2-MAC and D-MAC satellite TV (FM, 27 MHz; 20 Msps sees most of it) | Picture (4:3 / 16:9), channel name, digital sound; EuroCrypt detected, not descrambled |
| **DRM** | Digital radio on long, medium and short wave | Audio and station information; Journaline pages (packet mode data service) |
| **DMR** | Digital two-way radio | Talkgroups, IDs and data (no voice) |
| **NXDN** | Digital two-way radio, NXDN48 and NXDN96 | Call log with unit IDs and talkgroups, status and short data messages, site information, voice through your mbelib |
| **P25** | APCO Project 25 Phase 1 (TIA-102), C4FM, 12.5 kHz, conventional and trunked | Calls with talkgroups, unit IDs and emergency flags; trunking: WACN, system, NAC, RFSS / site, adjacent sites, channel identifiers, voice grants, talkgroup activity; voice through your mbelib. Receive only: encrypted calls are only reported as encrypted, nothing is decrypted. Follow the rules on receiving radio where you live |
| **dPMR / PMR446** | Digital two-way radio (ETSI TS 102 490), 6.25 kHz, PMR446 channels 1 to 16 | Call log with own and called IDs and colour codes, status and short messages, packet data, scan of all 16 PMR446 channels, voice through your mbelib |
| **ADS-B** | Aircraft transponders, 1090 MHz | Aircraft on a map, with altitude, speed and callsign; units switch between aviation (kt, ft, nm, ft/min) and metric (km/h or m/s, m, km, m/s) in the table, details, map labels and CSV export (SBS and feeds stay in aviation units) |
| **UAT** | Universal Access Transceiver, 978 MHz (DO-282) | UAT ADS-B position and identity reports, FIS-B weather text products and TIS-B traffic relay; aircraft table and FIS-B tab. Typical 2 Msps capture. Receive only |
| **Mode S uplink** | Radar and ACAS interrogations, 1030 MHz (needs 8 Msps) | Interrogator codes and rotation periods, aircraft and the registers asked for, TCAS coordination |
| **ACARS** | Aircraft data link, VHF | Messages, and positions on a map |
| **HFDL** | Aircraft data link on HF, 3 - 22 MHz | Squitters and ground stations, ACARS, positions on a map; every channel in the band at once |
| **CW** | Morse code on the HF bands | Every CW signal in a 14 kHz window at once: speed, SNR and text, callsigns highlighted |
| **Spectrum analyser** | Wide sweeps, 1 - 6000 MHz in about a second with a HackRF (its native sweep), stepped retuning on other radios | Live, peak-hold and averaged traces, persistence, waterfall, markers, EU band overlay, spur removal, CSV export with every bin at the chosen RBW (streamed to the file; the expected size is shown), automatic CSV saving every N seconds into a folder (timestamped specan_YYYYMMDD_HHMMSS.csv, optional limit on files kept) |
| **JS8Call** | Weak-signal keyboard messaging on HF (normal, fast, turbo) | Every frame in the passband, messages put together from their frames, heartbeats and CQs, relays, stations and their grids on a map |
| **HF ALE** | 2G automatic link establishment (MIL-STD-188-141 / FED-STD-1045) on HF | Who calls whom: soundings, individual, net and group calls, responses and links, AMD messages, LQA reports, with BER and SNR; the stations heard |
| **Spectrum explorer** | No decoder: a tool to look around | Spectrum and waterfall with markers, zoom and pan, region measurements (occupied bandwidth at -3 / -26 dB, channel power, SNR, symbol rate), LNB and converter offsets, range sweeps with signal detection, bookmarks |
| **Radio astronomy** | No decoder: the 21 cm hydrogen line (1420.4058 MHz) and the OH lines | Long integration of the spectrum (1 s to 1 h, FFT 1024 - 16384), on - off reference or frequency switching, polynomial baseline outside a line window, radial velocity axis (km/s), peak marker, drift-scan waterfall, CSV export, a short how-to (LNA, filter, horn or dish) |
| **Time signals** | DCF77 (77.5 kHz), MSF, WWVB (60 kHz), JJY (40 / 60 kHz) carrier keying; WWV / WWVH 100 Hz BCD subcarrier and ticks; CHU 300 bit/s FSK (A frames); **BPM** (China, Pucheng; 2.5 / 5 / 10 / 15 MHz, shares them with WWV so the station is chosen by hand): 1 kHz ticks and pulse-width coded seconds, UTC and UT1 minutes (frame layout assumed, see `time_proto.h`, not yet checked against a real BPM recording), and **BPM coded** (the 1 kHz burst code of the pure carrier slots, 2.5 / 5 MHz: every second frame names the date and time, DUT1 to 0.1 ms and the leap count; read from a real recording); **BPC** (China, 68.5 kHz, 20 s frames of 0.1 - 0.4 s carrier dips, UTC+8, needs an LF radio or upconverter) and **HLA** (Korea, 5 MHz, 100 Hz pulses, UTC+9). A recording of the demodulated audio and a slow recorder clock can be entered on the Receiver panel. LF needs a radio specified for it (SDRplay, Airspy HF+, RTL-SDR direct sampling); OnAir tries any frequency on any radio and notes when it is outside what the radio is specified for | Time, date, time zone, summer time and leap second flags, DUT1, the seconds tick and its jitter, the computer clock offset after the path delay from your position, level and SNR history; **Clock** tab: *Sync computer clock* (source GPS or time station, shows the measured offset, a **Sync now** button that sets the clock only after your click through the system's administrator prompt and refuses when the signal is not locked or the offset is over 10 minutes; a command to copy) and an optional **Act as NTP server (LAN)** switch (off by default, UDP 123 or 1123, stratum 1 "GPS" or stratum 2 from a time station, LAN clients only, rate limited, hobby accuracy) |
| **Ionospheric sounders** | No decoder: HF chirp ionosondes and oblique sounders sweeping 0 - 30 MHz (100, 125, 150, 200, 500 kHz/s) | Sweeps found by dechirping, their rate, period and start offset; an ionogram of group range against frequency built from every sweep through the captured span (a HackRF at 20 Msps sees 18 MHz of it), the band coverage of a narrow radio, a timeline of sweeps, PNG / CSV export |
| **Datawell Buoy HF Link** | Wave-buoy telemetry channels, 25.5 - 35.5 MHz (45 MHz on request), the listed channels as presets, 250 ksps | The 2FSK (HXV) format of the DWR-MkIII, DWR-G and WR-SG buoys, 81.92 bit/s, decoded from Datawell's published reference manual: displacements, wave height and period, water temperature, GPS position, directional spectrum, with the BCH parity repairing up to 2 bit errors per vector (the parity code was found from a real recording, not published). The 4FSK (HVA) format of the DWR4 is not public: tones and bit-stream measurements only. Checked on the sigidwiki sample recordings and on synthetic signals built from the documented layout. Receive only |
| **CODAR / HF ocean radar** | The ITU oceanographic radar bands: 4.4 - 4.65, 5.25, 12 - 14, 24.45 - 24.65 and 41.5 - 42.5 MHz, 250 ksps and up (2 Msps takes in 1.8 MHz; a wide radio sees a whole band, a narrower one steps through the band presets with the scan option) | Finds the FMCW sweeps of SeaSonde-class radars as moving lines in a 1 ms spectrogram and measures centre, sweep width, rate, direction (rising / falling), repetition and level; a table of the radars heard (first / last heard, SNR history), a chirp waterfall with the sweeps marked, "heard on band X at time Y" history (propagation only, no position is claimed), band presets and a scan. Radars that share a frequency overlap, so the sweep numbers give only a "possible" hint. The Morse call sign that some radars send is read from the keyed energy of the whole sweep width and is always labelled unverified: it follows the published description, not a recording. Receive only: these are broadcast radar signals and nothing is decrypted or transmitted |
| **Signal workbench** | No radio needed: a recording opened from a file: audio (WAV, MP3, FLAC, OGG, M4A: a real signal, or a stereo file as I/Q) or IQ (.cs8 .cu8 .cf32 .ci16 .cu16, SigMF, IQ WAV) | Waveform and spectrogram with cursors and a time and band selection; peaks, bandwidth (-3 dB, -20 dB, 99 %), carrier offset, SNR, symbol rate, modulation hints, power over time and bursts; AM, FM, SSB, CW, ASK/OOK, 2-FSK / 4-FSK, AFSK (Bell 202, 103 ...), BPSK / QPSK / 8PSK, MSK / GMSK demodulators with eye, constellation and the recovered bit stream (invert, differential, NRZI, Manchester, bit order, pattern search); listening and export |
| **NOAA Weather Radio SAME** | The seven NFM weather channels, 162.400 / .425 / .450 / .475 / .500 / .525 / .550 MHz (US and Canada), 1 Msps and up | SAME / EAS alert decoder: 520.83 baud AFSK (mark 2083.3 Hz, space 1562.5 Hz), the header repeats voted bit by bit and every field checked; originator, event with its plain-English name (the full EAS event table), locations with the partial-county direction (NW / N / NE / W / C / E / SW / S / SE), state and province names, valid-for and issue time, sender call sign, how many of the three repeats agreed, the 1050 Hz attention tone, alert history, a state-level map (approximate) and the raw messages. County names only from a county CSV you load (none are built in, see `docs/same.md`). Receive only: the text of an alert is a public broadcast. Checked on synthetic signals only (no real recording tried yet) |
| **ANT+ sensors** | 2457 MHz (ANT+), or any ANT channel 2400 - 2524 MHz, 4 Msps (a radio that reaches 2.4 GHz) | Heart rate straps and watches, bike speed / cadence / power sensors, footpods and environment sensors: device number and type, the data pages (heart rate with beat count and R-R interval, plotted over time; power, cadence, speed, temperature), manufacturer, serial and battery. Receive only: the open broadcasts a watch hears, nothing is sent or paired |
| **DECT cordless phones** | Carriers 1881.792 - 1897.344 MHz (EU), 5 Msps or more (a radio that reaches 1.9 GHz) | Base stations from their beacons: RFPI with the manufacturer code and system number, level, slots used, system-information tails, a burst log with base and handset bursts. Activity and signalling only: calls are encrypted, the voice is not decoded and nothing is sent |
| **Multitone Paging** | Multitone Electronics (UK) pagers, hospitals and industrial sites, 25 - 470 MHz, narrowband FM / FFSK. Detector only: the format is not public | Per transmission: carrier, deviation, steady tones and spectral lines of the audio; for FSK data the measured baud rate, the bit stream in hex and the 32-bit words that pass the POCSAG BCH(31,21) check, POCSAG sync flagged. Audio FFSK found in real recordings (tones f / 2f, 585 baud, the 0x605A frame word); the address and message fields are not worked out, so everything is unverified and no message text is shown; receive only, pager messages are private (see `docs/multitone-research.md`) |
| **M17** | Open digital voice, 2 m / 70 cm | Callsigns, voice (codec2 comes with OnAir), text messages, positions on a map |
| **D-STAR** | Amateur digital voice, 2 m / 70 cm | Call log with callsigns and repeaters, slow data messages, D-PRS / GPS positions on a map, voice through your mbelib |
| **PSK31** | Keyboard chat on the HF bands (BPSK31 / 63, QPSK31 / 63) | Every signal in the 3 kHz passband at once: a chat log per signal, the picked one in big text, callsigns highlighted |
| **Airband** | AM voice, 118 - 137 MHz | Several 25 and 8.33 kHz channels at once: squelch, scan, priority, activity log |
| **VDL Mode 2** | Aircraft data link, 136.7 - 137 MHz | Every VDL2 channel at once: ACARS, X.25 / CLNP, ADS-C, aircraft and ground stations, positions on a map |
| **VDL Mode 4**, *experimental* | STDMA GFSK data link, 118 - 137 MHz | Synchronization and DLS bursts (ISO/IEC 13239 framing, CPR positions from sync bursts). Synthetic test signal and spec-based decoder only; no live captures validated yet. Receive only |
| **VOR** | Navigation beacons, 108 - 118 MHz | The radial (conventional and Doppler VOR), Morse identification, voice; a position from two stations on the map |
| **ILS** | Approach guidance, 75 / 108 - 112 / 329 - 335 MHz | Localizer and glide slope needles (DDM), marker beacons, identification |
| **Inmarsat Aero** | Aircraft satellite data link, L band | Messages, and positions on a map |
| **Inmarsat-C** | Maritime satellite broadcasts | Safety messages (SafetyNET) and system information |
| **Iridium** | Satellite phone network | Satellites on a map, pager messages, ring alerts |
| **Orbcomm** | Satellite data downlinks, 137.2 to 137.8 MHz (4800 bit/s SDPSK) | Two downlinks at once with Doppler tracking, satellites on the map from their ephemeris, packet log by type |
| **QO-100** | Es'hail-2, the geostationary amateur satellite: downlink 10489.5 - 10500 MHz through a Ku LNB (IF 739.5 - 750 MHz with 9750 MHz; 9739, 10000, 10600 or any LO) | Beacon lock: the CW beacons measure the LNB's frequency error and drift (kHz, ppm, history) and correct every frequency shown; the narrowband transponder as a high-resolution spectrum and waterfall with SSB listening (USB, 2.7 kHz); the wideband transponder's DATV carriers with their symbol rates and a hand-over to DVB-S/S2 |
| **FLARM** | Glider and light-aircraft collision-avoidance broadcasts on 868.2 and 868.4 MHz (both from one radio on 868.3 MHz, at least 1 Msps, so an RTL-SDR works), 2-FSK 100 kchip/s Manchester | The old protocol is decoded (device ID, aircraft type, altitude, speed, track, climb, level, distance and bearing) in a table and on the live map; the newer protocol is only counted ("FLARM (new protocol)"), without positions. Receive only, as the Open Glider Network does: positions are what the aircraft broadcast openly. The no-track and stealth flags are respected: such aircraft (and anonymous addresses) are never shown, logged, saved or exported, only counted as "privacy-flagged aircraft". Needs the receiver position (shared with ADS-B) and the computer clock set right (the packet key changes every 64 s) |
| **CubeSat telemetry** | Amateur satellites on 2 m / 70 cm (145.8 - 146 and 435 - 438 MHz): AX.25 at 1200 baud AFSK and 9600 / 4800 baud G3RUH, GOMspace AX100 Mode 5 (ASM + Golay + Reed-Solomon, GMSK 9600 / 4800) and Mode 6 (Reed-Solomon in HDLC), CCSDS Reed-Solomon (255,223) frames, all at once | A frame log with callsigns or CSP addresses, hex and ASCII dumps, Reed-Solomon corrections, satellite names from an editable table (NORAD number, frequency, framing, callsign), KISS out to SatNOGS-style tools through the data feeds; the pass planner starts it at AOS with Doppler correction. The AX100 convolutional code and AO-40 FEC are not decoded |
| **Globalstar** | Globalstar L-band downlink, 1610 - 1626.5 MHz, 13 CDMA channels of 1.23 MHz (receive only) | Activity monitor only: per-channel power, SNR, occupancy and Doppler (spectral centroid), a Doppler track against time with the pass planner's Globalstar satellites drawn over it, an event log. The downlink is spread and encrypted: nothing is decoded or decryptable |
| **HF beacons** | A propagation monitor: which NCDXF/IARU International Beacon Project beacons (18 beacons on 14.100, 18.110, 21.150, 24.930, 28.200 MHz in 10 s slots), Long Dashes (Australia) and Russian single-letter beacons are heard from here, by band, with SNR, distance, bearing, a great-circle map and a 24 hour history. Identity from the keying and the published schedule, not just a carrier; an editable beacon list (JSON in the data folder); steps through bands when the capture is narrower than the list; click a beacon to tune and listen. HF band, 0.5 - 30 MHz |
| **AIS** | Ship transponders | Ships on a map |
| **VDES** | VHF Data Exchange System (ITU-R M.2092): ASM on 161.975 / 162.025 MHz, VDE-TER on the 100 kHz channels at 157.2 and 161.8 MHz | ASM frames decoded (GMSK 9.6 kbit/s, AIS framing, CRC-16 checked, payload as hex). VDE-TER bursts detected and logged with bandwidth (25 / 50 / 100 kHz), level and a modulation class (pi/4-QPSK, 8PSK, 16QAM); their content is not decoded |
| **Maritime** | NAVTEX, DSC, weather fax | Safety messages, distress and calling, fax pictures |
| **Radiosondes** | Weather balloons, 400 – 406 MHz | Balloon tracks, altitude and weather data |
| **ISM sensors** | 433.92 / 868 / 915 MHz devices (the rtl_433 idea) | Weather stations, tyre pressure sensors, remotes and energy / water meters (wireless M-Bus header), and Z-Wave R1 / R2 / R3 (ITU-T G.9959: Home IDs, nodes, command classes; passive receive only, S0 and S2 traffic stays encrypted and is never decrypted): a device table with history, a Z-Wave view grouped by Home ID, a live log, unknown signals with their pulses |
| **Cell towers** | LTE downlink broadcast (MIB, SIB1 - SIB5) | Operator, band, EARFCN, PCI, eNB / sector, TAC, neighbours, band scan; receive only |
| **5G NR cells** | NR SS/PBCH blocks on the sync raster (GSCN) of n1, n3, n7, n8, n20, n28, n38, n40, n41, n77 and n78, 15 / 30 kHz SSBs, from 3.84 / 7.68 Msps (20 Msps searches several raster points per step) | PCI (PSS / SSS), SSB index and the beams heard, SS-RSRP / RSRQ / SINR, carrier offset, and the MIB from the polar-coded PBCH: SFN, common subcarrier spacing, k_SSB, DMRS type A position, CORESET#0 and search space 0 of SIB1, barred, intra-frequency reselection. Receive only, public broadcast; SIB1 (operator, TAC, cell id) is not decoded yet, so the tower map stays empty |
| **NB-IoT cells** | LTE narrowband IoT carriers (180 kHz: in-band, guard band or standalone), found on the 100 kHz raster of the LTE bands that carry NB-IoT (20, 8, 3, 1, 28, 5, 2, 4, 12, 13, 17, 18, 19, 25, 26, 66, 71, 85); 2 Msps, even an RTL-SDR; one second per block, band presets and scan | PCI (NPSS / NSSS), operating mode, NRS antenna ports and system frame number from the NPBCH (MIB-NB), and from SIB1-NB the operator (MCC / MNC), tracking area, cell identity (eNB and sector), band and barring; level, SNR, carrier and clock offset, a table of the cells with their signal level, results saved between runs. Receive only, public broadcast: paging, NPDCCH, data and device identities are never decoded. Receiving the broadcast channels of cellular networks is legal in many places but not everywhere, and decoding anything addressed to a phone or device is not allowed in most: check the rules where you live |
| **GSM-R** | Railway GSM broadcast (BCCH, 921 - 925 MHz) | Base stations: ARFCN, BSIC, cell identity, location area, operator code, channel lists; band scan; receive only |
| **TETRA** | TETRA base station broadcast (ETSI EN 300 392-2: BSCH, BNCH, AACH, MCCH), 25 kHz channels | Network code, colour code, location area, carriers and duplex spacing, services, neighbour cells, whether encryption is used; scan over a span; receive only, nothing decrypted |
| **TETRAPOL** | TETRAPOL control channel (PAS 0001-2), GMSK 8 kbit/s, 10 / 12.5 kHz (not TETRA) | D_SYSTEM_INFO: country, system, cell, location area, channel, scramble; receive only, no voice, nothing decrypted |
| **Bluetooth LE** | Advertising on channels 37, 38 and 39 (2402 / 2426 / 2480 MHz), one at a time or hopping, 4 Msps | What a scanner app such as nRF Connect shows: a device table with address type, name, level history, vendor, services, iBeacon, Eddystone and exposure notification beacons, and a packet log. Receive only: it never connects, pairs or transmits and does not follow connections; most phones use random addresses that change every few minutes for privacy |
| **Zigbee / 802.15.4** | 2.4 GHz channels 11 - 26 (O-QPSK, 2 Mchip/s, 250 kbit/s), one channel at a time or scanning all, 4 Msps (needs a fast radio: not an RTL-SDR) | Networks (PAN ID, extended PAN ID, coordinator, devices, level), a device list with the vendor from the extended address, a live frame log (MAC, Zigbee NWK and clear APS headers, beacons, association and other MAC commands, FCS checked) and a frames-per-second plot. Receive only: encrypted payloads are not decrypted (the network key is not obtainable here), only frames sent in the clear are shown in full. |
| **LoRa (raw)** | Plain point-to-point LoRa (not LoRaWAN, not a mesh) on 433, 868 and 915 MHz (and any frequency you type; 137 MHz to 2.5 GHz is what a typical radio reaches), SF5 - SF12, 125 / 250 / 500 kHz, from 1 Msps (an RTL-SDR works) | Every frame as bytes (hex and text) with SNR, level, carrier offset and time. You set the spreading factor, bandwidth, header mode (explicit, or implicit with coding rate, CRC and length), sync word and frequency (EU868, US915, 433 presets), or leave spreading factor and bandwidth on Auto and every combination is searched. Receive only: nothing is sent, decrypted or interpreted, the payload is just the bytes. |
| **Wi-Fi networks** | 2.4 GHz access point beacons on channels 1 - 13 (802.11b DSSS, 1 Mbit/s DBPSK, Barker-11), one channel at a time or hopping, 20 Msps | What a phone's Wi-Fi list shows: SSID (hidden networks as hidden), BSSID with the vendor, channel, security (open, WEP, WPA, WPA2, WPA3, enterprise), supported rates, beacon interval, level history and last seen, and a beacon log. Passive: only beacons and probe responses are read; data frames are counted, never decoded; it never transmits, probes or joins. Access points that beacon only at OFDM rates (802.11g / n only) are not heard |
| **Wi-Fi HaLow (802.11ah)** | Sub-GHz S1G channels of 1, 2 and 4 MHz: 863 - 868 MHz (EU) and 902 - 928 MHz (US), one channel at a time or scanning the region's channel list, 4 Msps (1 and 2 MHz channels also from 2.4 Msps, an RTL-SDR) | A table of the networks around from their S1G beacons and probe responses: SSID (full, or resolved from the compressed SSID), BSSID and vendor, S1G channel and frequency, bandwidth, MCS, the security type the network announces (WPA2-PSK, WPA3-SAE, OWE, open: information only), beacon interval and level, plus a live frame log and a frames-per-second plot. The preamble (short training field) is detected at every width, the SIG field is read and checked, and BPSK 1/2 frames (how beacons are sent) are decoded and FCS-checked; frames at faster rates are only counted. **Legality: receive only.** OnAir never transmits, probes, associates or joins a network, and it does not decrypt anything or try keys; it reads what a network broadcasts openly, as a phone's Wi-Fi list does. Receiving is legal in most places but the rules differ: check them where you live. Written from the published standard and tested only on its own synthetic signal, no live 802.11ah recording yet. |
| **Wi-SUN FAN** | Smart meter and street light mesh networks (IEEE 802.15.4g SUN FSK, 50 / 100 / 150 kbit/s): the band plans of every Wi-SUN region (470 MHz to 2.4 GHz: ANZ, North America, Mexico, Brazil, Japan, Korea, South-East Asia, Europe, India, China, worldwide), picked by the frequency or by name; a whole band at once from 16 Msps for 915 - 928 MHz (the nodes hop over every channel), a few channels from 2 Msps | Networks (PAN ID, network name, border router, PAN size, schedule, PHY modes), nodes (EUI-64, vendor, role, routing cost, frame types, channels heard on, unicast and broadcast schedules, EAPOL joins and their authenticator), links with the levels enhanced acks report, a frame log with the Wi-SUN IEs and a hop chart. Receive only; PAN configurations and data stay encrypted |
| **General receiver** | Anywhere the radio tunes, 2 Msps | NFM, WFM (mono), AM, USB, LSB and CW (BFO pitch) on a channel clicked on the spectrum, with draggable filter edges and width presets, squelch (level or automatic against the noise beside the channel), AGC, de-emphasis, S-meter, frequency steps and audio recording to WAV |
| **DJI drones** | Receive only, 2.4 and 5.8 GHz, at 16 Msps or more (HackRF at 20 Msps, PlutoSDR, bladeRF, USRP): wideband OFDM burst trains are classified by shape as DJI OcuSync-like, Lightbridge-like or unknown wideband OFDM, with a confidence (a guess from the width, burst period, duty cycle and cyclic-prefix correlation, never a certain identification); **DJI DroneID** (the OcuSync 2 broadcast of the Mini 2, Mavic Air 2, Mavic 2 and others: 10 MHz OFDM, Zadoff-Chu roots 600 and 147, LTE turbo code, CRC-24; format from the NDSS 2023 paper "Drone Security and the Mysterious Case of DJI's DroneID" and the MIT-licensed notes of proto17/dji_droneid) is decoded to the serial number, position, home point and pilot position as sent in the clear. The encrypted video link is not decoded. DJI's standard Remote ID (Wi-Fi / Bluetooth) is the Drone Remote ID mode. |
| **Drone Remote ID** | ASTM F3411 / ASD-STAN EN 4709-002 Open Drone ID on 2.4 GHz: Bluetooth LE legacy advertising (service data 0xFFFA) and Wi-Fi beacons (vendor element FA:0B:BC), both at once from one 20 Msps band at 2432 MHz | A drone table (serial number or session ID, UA type, position, altitude and height, speed and track, operator position and ID, Self-ID, last seen) and a map with each drone linked to its operator. Receive only: Remote ID is a public broadcast meant for anyone to read; nothing is transmitted. BLE 5 long range and Wi-Fi NAN are not heard |
| **Meteor-M LRPT** | Meteor-M N2-3 / N2-4 weather pictures on 137.100 / 137.900 MHz: OQPSK 72k (differential), QPSK 72k or the interleaved OQPSK 80k mode, Viterbi, Reed-Solomon, CCSDS packets, MSU-MR JPEG-like strips | The picture grows line by line during the pass: channels 1 - 6 and a colour composite (123 by day, 125 at night), Save PNG; View > Passes starts it at AOS and follows the Doppler. Checked only against the built-in test signal so far: no off-air recording yet |
| **GOES HRIT** | GOES-16 / 18 / 19 HRIT on 1694.1 MHz: BPSK 927 ksym/s, K = 7 Viterbi, Reed-Solomon (255,223) x 4, CCSDS VCDU / M_PDU / CP_PDU, LRIT / HRIT files with Rice (CCSDS 121), plain and JPEG images | A product list (full disk, mesoscale, CONUS and relayed images, EMWIN text, admin messages) with an image viewer that grows as the lines arrive, EMWIN ZIP files unpacked, everything saved to a folder. Needs a dish (about 1 m, or a grid antenna) with an LNA and filter at the feed |
| **Elektro-L** | Elektro-L N3 / N4 / N5 HRIT and LRIT on 1691 MHz (BPSK, Viterbi, Reed-Solomon, CCSDS xRIT at 665.4 k / 1 M / 128 k / 64 ksym/s) and GGAK space-weather telemetry on 1693 MHz (5 ksym/s PCM/PM) | A product list and image viewer for HRIT / LRIT (same xRIT path as GOES) and a GGAK frame log. Receive only. Needs a dish with an LNA at the feed |
| **HRPT / AHRPT** | MetOp-B / -C AHRPT on 1701.3 MHz (QPSK 2.33 Msym/s, K = 7 Viterbi, Reed-Solomon (255,223) x 4, CCSDS packets, AVHRR lines), FY-3 AHRPT (QPSK, frames counted only), retired NOAA HRPT (BPSK, Manchester, 665.4 kbit/s) | The AVHRR picture grows line by line (colour or one of five channels), saved as PNG. Needs a dish (about 1 m or more, following the satellite) with an LNA at the feed and a fast radio (4 Msps or more; not an RTL-SDR). NOAA-15, 18 and 19 are retired |
| **GNSS** | GPS L1 | Satellites, position and time; **Clock** tab: UTC and GPS time, leap seconds (and a pending one), the computer clock offset against the satellites (accuracy limited by USB / SDR latency, 5 - 30 ms), level, C/N0 and offset history, the same *Sync computer clock* section and optional NTP server as the Time signals mode |
| **Mesh** | Meshtastic and MeshCore (LoRa) | Nodes, map and chat |
| **LoRaWAN** | EU868 (868.1 / 868.3 / 868.5 MHz and 867.x) or US915, SF7 - SF12 at once | A device table (DevAddr or DevEUI, network from the NetID, last heard, level, SNR, frame counter, SF) and a frame log with filters: message type, FCtrl, FCnt, MAC commands, port, encrypted length, MIC, join requests; only the clear headers, payloads stay encrypted |

Experimental (the receivers exist and pass the synthetic, loopback and impairment tests in `tests/`, but they have had little or no checking against live broadcasts): **HD Radio** (FM and AM, `hdr_*`), **CDR** (China Digital Radio, `cdr_*`), **Pagers** (POCSAG, FLEX), **APRS / Packet** (AX.25) and **HF digital** (RTTY, SSTV, FreeDV).

<details>
<summary><b>Iridium bursts for iridium-toolkit</b></summary>

In the Iridium screen, **Save for iridium-toolkit** writes every demodulated burst (downlink, uplink and simplex, unique word included) as the `RAW:` lines of gr-iridium's `iridium-extractor`, by default to `output.bits` in the recordings folder. From the command line: `onair-cli --standard iridium --iridium-bits output.bits --secs 600`. Then run the toolkit ([muccc/iridium-toolkit](https://github.com/muccc/iridium-toolkit)) on it as usual:

```
iridium-parser.py output.bits > output.parsed
```

OnAir writes the bits only and never decodes voice (the AMBE codec is patented). Extracting voice from `output.parsed` with the toolkit's voice tools is up to you and happens outside OnAir.
</details>

### Signal workbench

The Signal workbench (mode list, GENERAL & TOOLS) takes a recording apart without a radio, in the spirit of Inspectrum, SigDigger and Universal Radio Hacker. Short walkthrough:

1. **Open** a file with Open... (or drop it on the window). Audio files (WAV, MP3, FLAC, OGG, M4A) are decoded with the FFmpeg libraries OnAir already uses and treated as a real signal; a stereo file can be read as I/Q with "stereo is I/Q" (left = I, right = Q). IQ files are read from disk as they are, so 100 MB and larger files open at once: .cs8 .cu8 .cf32 .ci16 .cu16 need the sample rate (a name like `capture_2Msps.cs8` gives it) and optionally the centre frequency; SigMF and IQ WAV files describe themselves. "Demo signal" makes a recording to try the tools on.
2. **Look.** An overview of the whole file, the waveform of what is in view and a large spectrogram (FFT size, window, overlap, colour map and range are set above it). Mouse wheel zooms time, Ctrl + wheel zooms frequency, right-drag pans, a click puts the cursor (time, frequency and level are read out), a drag selects a time range and a band (Shift + drag: time only; the boxes above the plots take exact values), double click clears. Only what is in view is computed, so the screen stays fast on long files.
3. **Measure.** On the Measure tab press Measure selection: the strongest peaks (frequency, level, width), bandwidth at -3 dB, at -20 dB and the 99 % occupied band, the carrier offset, SNR, a symbol-rate estimate from the lines of the envelope and the frequency steps, the signal identifier's modulation guess with candidates, the power over time and the bursts (start, length, level; click one to select it).
4. **Demodulate.** On the Demodulate tab choose the demodulator and, for digital ones, the baud rate (Auto-detect finds it from the selection), then Demodulate selection: the band you selected is moved to baseband, filtered and demodulated. The result shows the instantaneous frequency, phase and magnitude, the eye diagram, the constellation (PSK) or the histogram of the soft symbols (FSK, ASK) and the recovered bits. Carrier recovery, a Gardner timing loop, the matched-filter roll-off, differential coding, AFSK tones and FM-first are options. Listen plays the audio of the demodulator (OnAir's audio output, with the app's volume and mute).
5. **Read the bits.** The Bits tab applies invert, differential decode, NRZI, Manchester (IEEE 802.3 and G.E. Thomas), byte reversal and byte alignment, shows the stream as hex, binary or ASCII, searches a pattern (also inverted, with some wrong bits allowed) and copies or saves the bits as text or binary.
6. **Export.** The Export tab saves the selection as .wav, .cf32 or SigMF (a selected band is written at baseband and a lower rate), the demodulated audio as .wav, the bits as .txt or .bin and the peak table as CSV.

`wbtool` (built with the tests) does the same on the command line: `wbtool gen out.cf32 --kind qpsk --rate 250000 --baud 12500` makes a test recording with known bits, `wbtool analyze file --rate R`, `wbtool demod file --kind fsk2 --baud 9600 --band f0 f1`.

### Cell tower map (OpenCelliD key)

Cell towers broadcast who they are (operator, area code, cell id) but not where they stand. The Cells mode can look each decoded LTE cell up in [OpenCelliD](https://opencellid.org), an open, crowd-sourced tower database, and put it on the map:

1. Sign up (free) at https://opencellid.org and confirm your e-mail.
2. Log in and open your account page (My Account / API access); copy your API token.
3. In OnAir, open Cells → Map and paste it into **OpenCelliD API key** (the "?" next to it repeats these steps).

Lookups need internet and run in the background at most once a second; only the cell identity (MCC, MNC, TAC, cell id) and your key are sent. Answers are cached on disk, so the map also works offline afterwards. A CSV downloaded from OpenCelliD still works instead (Load tower file). Tower data © OpenCelliD, CC BY-SA 4.0.

### TETRA network information

TETRAPOL is a different air interface (GMSK 8 kbit/s, PAS 0001-2), listed as its own mode: it shows D_SYSTEM_INFO from the control channel. It does not decode voice.

The TETRA mode lists the TETRA base stations it hears, with what each one broadcasts in the clear to every terminal: the network code (MCC, MNC), colour code, location area, the main carrier, uplink and duplex spacing, the services the cell offers, its neighbour cells, the network time, and whether the network uses air interface encryption. The same information is shown by open-source tools such as osmo-tetra. **Scan** steps the radio over a span (390 - 395 MHz, 410 - 430, 450 - 470 or your own) and lists every carrier found.

It is receive only and standards based. It does not decrypt anything, does not transmit, does not join or disturb any network, and does not decode or play voice, short data messages or subscriber identities: encrypted signalling is only counted and reported as encrypted, and traffic timeslots are skipped. You are responsible for following your country's rules on receiving radio signals.

### LoRaWAN monitor

The LoRaWAN entry (in the Mesh family) shows what every LoRaWAN gateway in range receives in the clear: the message type (join request, (un)confirmed uplink or downlink, proprietary), the DevAddr with the network its NetID belongs to (The Things Network, Helium, Orange, ...), the FCtrl flags, the frame counter, the MAC commands of LoRaWAN 1.0 FOpts, the port, the length of the encrypted payload and the MIC; for join requests the JoinEUI, the DevEUI (with its maker when the OUI is in a short table) and the DevNonce. All spreading factors of every channel in the capture are searched at once.

It is receive only. The application payloads and join accepts are encrypted with the devices' own keys and stay encrypted: OnAir never decrypts them, never checks a MIC and never transmits. These are other people's devices; you are responsible for following your country's rules on receiving radio signals.

### LoRa (raw) monitor

Shows plain LoRa frames between two radios (SX127x / SX126x style point-to-point links, sensors, Arduino and ESP32 projects): the frame as hex and text, spreading factor, bandwidth, coding rate, CRC result, SNR, level and carrier offset. Set the parameters of the link, or choose Auto for the spreading factor and bandwidth to search them all at once (more CPU). Implicit-header links carry no header, so their length, coding rate and CRC must be set by hand.

It is receive only. It does not transmit, does not decrypt, and does not interpret payloads (use the LoRaWAN or Mesh modes for those protocols). These are other people's devices and may carry private data: you are responsible for following your country's rules on receiving radio signals and on what you may do with what you receive.

### Channel scanner and shared channel list

The TV channel scanner covers DVB, ATSC, ATSC 3.0, ISDB-T and DTMB. Scan results can be shared with other users through a public, crowd-sourced channel list, picked by country and city. You can also browse it on GitHub: **[adswill/OnAir-channels](https://github.com/adswill/OnAir-channels)**.

### Notes on the newer TV standards

<details>
<summary><b>T2-Lite</b></summary>

T2-Lite (the 1/3 and 2/5 code rates, short FEC frames, 1.7 MHz channels) is decoded like any other DVB-T2 signal. It is checked end to end on simulated signals from QPSK to 256-QAM, and the error correction sits where theory says it should; it has not been tried on a real T2-Lite broadcast. A T2-Lite signal that shares a channel with a normal T2 one (in its future-extension frames) is not decoded: the normal T2 part keeps working.
</details>

<details>
<summary><b>ATSC 3.0</b> (experimental)</summary>

The whole receiver chain is built from the published specifications and works on simulated signals (carrier offset, noise and echoes included): synchronisation, error correction, link layer, ROUTE and playback of HEVC video with AAC audio. It has not yet been checked against a real broadcast, so details such as the scrambler or interleaver may need fixing once a real recording is available. Services that use MMTP or AC-4 audio are not supported yet, and it needs a radio that can sample at 6.5 Msps or faster. If you have an ATSC 3.0 recording or can try it on air, please tell us on Discord.
</details>

<details>
<summary><b>ISDB-T</b> (experimental)</summary>

ISDB-T follows the ARIB STD-B31 specification (all three modes, all guard intervals, DQPSK, QPSK, 16QAM and 64QAM, the 13 segments in up to three layers including the one-segment layer, TMCC, the time and frequency interleavers, Viterbi and Reed-Solomon) and decodes simulated signals exactly, with carrier offset, clock error, noise and echoes, at 6 to 20 Msps. It has not been checked against a real broadcast yet, so details may need adjusting once a real recording is available. Channel names written in the Japanese character set are not converted yet. The channel scanner covers the 6 MHz raster of Japan and Brazil.

To try it without a signal, `isdbtgen --out sample.cs8` (built with the tools) makes a recording that carries the built-in test programme: open it as an IQ recording, 10 Msps, CS8, with ISDB-T selected. A recording from a real transmitter would help a lot; please share one on Discord.
</details>

## Features

**Watch and listen**
- **Live player** with picture, sound, subtitles, teletext, programme guide and multiple audio tracks.
- **Automatic repair** of short signal dropouts, so weak reception stays watchable.

**See your signal**
- Spectrum and waterfall, every constellation, signal-to-noise and error figures.
- A single **quality score**, and an **echo (multipath) detector**.
- **DAB transmitter map:** OnAir reads the transmitter identification (TII) that DAB networks send and lists the transmitters you receive with their relative strength. With a transmitter list (`dab-transmitters.csv` in OnAir's data folder: `eid,main,sub,lat,lon,name,country,power_kw`) they appear on a map with distance and bearing.

**Get the best reception**
- A **channel scanner**, **automatic gain tuning**, and a **direction finder** that helps you aim the antenna.
- **Your radio, set up properly:** every antenna input is its own entry in the source list, and **Radio settings** offers what the radio has: frequency correction (ppm), notch filters, gain modes, direct sampling for HF and more.
- **Up-converters and LNBs:** pick Ham It Up, SpyVerter, a QO-100 / Bullseye TCXO / Ku LNB or a custom LO (with ppm and spectrum inversion) in **Radio settings**; the whole app then shows and takes the frequency at the antenna (7.074 MHz, 10489.55 MHz) while the radio is tuned to the IF, with I/Q swapped when the converter mirrors the spectrum.
- **DC spike handling:** leave the radio's centre spike alone, remove it, or keep it off the channel by tuning just beside it. Optional **IQ imbalance correction**.

**Share it**
- A built-in **network tuner** streams your channels to VLC, phones, Plex or Jellyfin over your home network, as a plain stream or as HLS for browsers and phones.
- On a Mac, **cast** the playing service to an Apple TV or AirPlay TV.
- **Record** to a file, or send the stream out over UDP.

**Comfortable to use**
- **Mouse wheel on any slider:** hover a volume, gain, squelch or bandwidth slider and scroll to change it by one step (Shift = fine, Ctrl = coarse); the window does not scroll meanwhile.
- **Logbook export:** "Save results..." writes CSV or JSON, an ADIF logbook (call, grid, band, mode, RST / SNR, UTC) for the ham modes, and KML or GPX for anything with positions (ADS-B, AIS, APRS, FLARM, sondes); pick the format and "since start" or "all".
- **Light mode** (View → Light) next to the dark palettes; the layout adapts to small windows and 125 % / 150 % display scaling.
- **Fast:** error correction runs on the GPU where available, and with optimised vector code on the CPU everywhere else.

## Supported radios

OnAir supports the **HackRF One and HackRF Pro** natively, and drives these radios directly too (marked "experimental" in the radio list):

| Radio | Wide TV channels (6 – 8 MHz) | DAB, FM and narrow channels | Notes |
|---|:---:|:---:|---|
| **HackRF One / Pro** | ✅ | ✅ | The radio OnAir is developed and tested with |
| **BladeRF**, **LimeSDR** | ✅ | ✅ | |
| **USRP** | ✅ | ✅ | On Windows, install UHD from [Ettus](https://files.ettus.com) (with its firmware images) |
| **PlutoSDR** | ⚠️ | ✅ | Over its USB cable it streams about 4 Msps, too little for a 6 – 8 MHz channel. Library not included on macOS yet |
| **Airspy R2**, **SDRplay RSP** | Should work | ✅ | SDRplay: install the SDRplay API from [sdrplay.com](https://www.sdrplay.com) first (RSP1, RSP1A, RSP1B, RSP2, RSPduo, RSPdx, RSPdx-R2) |
| **RTL-SDR** | ❌ | ✅ | |
| **Airspy HF+** | ❌ | Narrow channels only | Specified for HF and 60 – 260 MHz |
| **Fobos SDR** (agile firmware) | ✅ | ✅ | Contributed, untested here. Specified for 50 MHz – 6 GHz on the RF input, HF on the HF1 / HF2 inputs. Library not included: put RigExpert's `fobos_sdr` library next to OnAir, see [DEVICES.md](DEVICES.md) |
| **RX888 MkII** | ✅ | ✅ | Experimental, written from the open SDDC protocol and never run on a real radio. USB 3 required. Firmware image not included: put `SDDC_FX3.img` (from the SDDC_Driver Releases page) in OnAir's codecs folder, see [DEVICES.md](DEVICES.md) |
| **RFNM** | ✅ | ✅ | Contributed. Only in builds made with [librfnm](https://github.com/rfnm/librfnm) installed (not the downloads yet): librfnm has a C++ interface only, so it is linked when OnAir is built, see [DEVICES.md](DEVICES.md). One entry per receive channel of the daughterboards |
| **Anything else** | depends | depends | Through [SoapySDR](https://github.com/pothosware/SoapySDR), once its SoapySDR module is installed |

- The libraries these radios need are included in the downloads, except as noted above.
- A TV channel needs a radio that samples fast enough: roughly 1 million samples per second per MHz of channel width. If a radio cannot reach the rate or frequency a mode needs, OnAir says so next to the source.
- **Any frequency.** OnAir never limits the frequency you enter, in any mode and on any radio. The ranges listed for a radio are what it is *specified* for: outside them OnAir still asks the driver and shows a note ("the driver may not reach this frequency or the signal may be weak or absent"). An RTL-SDR picks the way down below 24 MHz by itself (an RTL-SDR Blog V4 or V4 Lite upconverts HF in librtlsdr (osmocom rtl-sdr v2.0.3, bundled with OnAir), any other dongle samples its Q input directly), so you can go to 9 MHz and get whatever the hardware gives. Only what a driver itself refuses stays an error; see [DEVICES.md](DEVICES.md).
- The radios other than the HackRF have had less real-world testing. Notes for specific radios are in **[DEVICES.md](DEVICES.md)**.
- **Sound card** (the Radio list: **Sound card: …**, including Default input): WebSDR, a virtual audio cable, or a handheld radio's speaker/line into the computer, for HF decoders (FT8, RTTY, CW, SSB) at the capture rate (usually 48 kHz). Four mappings: I/Q stereo, USB audio, LSB audio, mono real. The frequency field is the *dial* the decoder should assume; nothing is tuned. This is not a replacement for an SDR when you need megahertz of bandwidth. Unverified against a real WebSDR.

### Network radios

Radios on another computer or on the network are added from the source list: pick **Add network radio…**, choose the type, type its address and press **Connect**. OnAir tries the connection for a few seconds and says why when it gets no answer; the radios added are remembered and listed with the others (remove them in the same window).

| Type | Address | Port |
|---|---|---|
| **PlutoSDR** by IP | `192.168.2.1` (its USB network), or its address on your network | libiio's default |
| **rtl_tcp** (an RTL-SDR shared by `rtl_tcp -a 0.0.0.0`) | the computer running `rtl_tcp` | 1234 |
| **SoapyRemote** (any SoapySDR radio shared by `SoapySDRServer --bind`) | the computer running the server | 55132 |
| **SpyServer** (Airspy's server, as SDR# and SDR++ use it) | the computer running SpyServer | 5555 |
| **Hermes Lite 2** | found by itself on your networks (or type its address) | 1024 |
| **Red Pitaya** (SDR transceiver app, HPSDR protocol 1) | found by itself on your networks (or type its address) | 1024 |

- A Pluto on a real network (not its USB cable) is offered its full sample rate; how much of it arrives depends on the network.
- rtl_tcp carries 8-bit samples at up to about 2.5 Msps; when the connection drops, OnAir connects again and counts the samples that were missed.
- SoapyRemote needs a build with SoapySDR and the SoapyRemote module on both computers. OnAir asks the server which radios it has and lists each one (sound cards are hidden); if it picks the wrong one or lists none, type the radio's driver (for example `mirisdr`) in "Remote driver".
- **SpyServer** (Airspy's server, as SDR# and SDR++ use it) is spoken to the public protocol description (2.0.1700): OnAir asks for the full IQ stream (8-bit or 16-bit), takes the sample-rate list from the server's decimation stages, and controls frequency and gain. Do not confuse its port 5555 with the SoapyRemote port 55132. It has only been tested against a fake server written from that description, not against a real SpyServer; tell us if yours misbehaves.
- When a network radio drops, OnAir reconnects by itself, and the status bar shows how many samples were lost (the underruns). Servers are reached on the address you type; nothing is opened to the network by default.
- **Share my radio** (View > Data feeds): OnAir becomes an rtl_tcp server for the radio it is running, so a second OnAir (Add network radio, rtl_tcp), SDR#, SDR++ or any rtl_tcp client can listen to the same signal. It sends 8-bit IQ at the rate the radio runs at. Only this computer can connect until you tick **Let other computers connect (LAN)**, and clients can only change the frequency and gain when **Allow remote control** is ticked. The connected clients are listed.
- **Data feeds** (View > Data feeds), all off until you turn one on: ADS-B SBS-1 and Beast (TCP), AIS NMEA (UDP), APRS-IS and KISS (TCP), ACARS and VDL2 JSON (UDP), GNSS NMEA RMC/GGA (UDP), Bluetooth LE advertisement JSON (UDP), LoRa / LoRaWAN / Meshtastic JSON (UDP). One host and port per UDP feed. The TCP servers stay on this computer unless **Let other computers connect (LAN)** is ticked.
- Choosing **Hermes Lite 2** searches this computer's networks for one (openHPSDR discovery) and fills in its address; **Search** looks again. It receives up to 38.4 MHz at 48 to 384 ksps, so only HF and narrow modes fit; the N2ADR filter board's band filters are switched with the frequency (a setting of the radio turns that off).
- **Red Pitaya** is the same openHPSDR protocol 1 client, for a board running Pavel Demin's HPSDR-compatible SDR transceiver (the STEMlab's "SDR transceiver" app): one receiver at 48 to 384 ksps, tuning up to 61.44 MHz, no gain control. It was written from that project's server source and tested only against a fake server, not a real board: the sample rate accuracy, level scaling, frequency correction and whether the spectrum is mirrored (setting "Swap I and Q") are unverified. The board id in its discovery reply (1, Hermes) is shared with other Hermes-protocol radios.

## Remote-control API

Scripts and other programs can control OnAir over HTTP and WebSocket. It is **off by default**: turn it on in **View > Data feeds > Remote control API** (RX 1). It listens on `127.0.0.1` (port 8765) only; **Let other computers connect** opens it to the network and then needs a token (at least 16 characters, made for you; the app shows it and can copy it). Programs on the same computer need no token; every other address must send `Authorization: Bearer <token>` (or `?token=<token>`, which ends up in logs: prefer the header). A web page cannot drive it: no CORS headers are ever sent, a POST must be `Content-Type: application/json`, a request with a foreign `Origin` or (on loopback) a non-loopback `Host` is refused. Requests are limited to an 8 KB header block and a 4 KB body, 16 connections and 8 WebSockets. Plain HTTP, no TLS: on a LAN anyone who can sniff the traffic sees the token.

| Endpoint | What it does |
|---|---|
| `GET /api/v1/status` | running, receiver, device, family and mode, frequency (Hz), sample rate (Hz), gain (dB), AGC, gain range |
| `GET /api/v1/devices` | the radios and sources: index, name, kind, board, serial, rate / gain / frequency limits, `selected` |
| `GET /api/v1/modes` | the modes: `family` (and `sub` for the protocol entries of HF digital, mesh, pagers, ...) |
| `GET /api/v1/telemetry` | the active mode's report: `standard`, `state` (2 = locked), `locked`, `snrDb` (SNR / MER / Es/N0 as the mode gives it, else `null`), `cfoHz`, FEC `blocksOk` / `blocksBad`, `quality`, and `details` for DVB-T2, DVB-T, ATSC, DAB, FM, DVB-S/S2, DTMB and DVB-C; the other modes carry the common fields only |
| `GET /api/v1/spectrum?bins=N` | the latest spectrum line: `dbfs` (at most N values, the strongest bin of each group), `centerHz`, `sampleRateHz`, `rmsDbfs`, `peak`, `clipFraction` |
| `POST /api/v1/start` | `{"device": 2}` (optional: the index from `/devices`) starts the receiver |
| `POST /api/v1/stop` | stops it |
| `POST /api/v1/mode` | `{"family": 5}` or `{"family": 25, "sub": "ft8"}` (stopped receiver only) |
| `POST /api/v1/frequency` | `{"hz": 1090000000}` or `{"mhz": 474}` |
| `POST /api/v1/samplerate` | `{"hz": 2400000}` or `{"msps": 2.4}`; 0 = automatic |
| `POST /api/v1/gain` | `{"db": 33}`: a radio's overall gain (a HackRF: the total of its three stages); turns the AGC off |
| `GET /api/v1/ws` | WebSocket (RFC 6455, text frames, read-only): pushes `{"type":"telemetry","data":{...}}` whenever a new report is available (at most 10 a second). Send `{"spectrum":true,"bins":512}` to also get `{"type":"spectrum","data":{...}}` (at most 5 a second) and `{"telemetry":false}` to stop the telemetry |

Replies are JSON; a refused or failed request gives `{"error":"..."}` with status 400 (bad input, or the radio refused), 401 (token), 403 (Origin / Host), 404, 405, 411, 413, 415, 431, 503 (busy) or 501. Changes are made on the program's main thread within a frame, and a request fails with 400 instead of hanging if the window is not responding.

```
curl -s http://127.0.0.1:8765/api/v1/telemetry
curl -s -X POST -H 'Content-Type: application/json' -d '{"mhz": 474}' http://127.0.0.1:8765/api/v1/frequency
curl -s -H "Authorization: Bearer $TOKEN" http://192.168.1.20:8765/api/v1/status
```

The server is `core/src/remote_api.cpp` (sockets only, with its own SHA-1 and base64 for the handshake); `tests/test_remote_api.cpp` runs a socket client against it, and against the real engine on a generated DVB-T2 signal.

## Updates

OnAir checks GitHub for a newer version a few seconds after it starts (then at most once a day), downloads the package for your system in the background, verifies it against the SHA-256 that GitHub lists, and installs it when you close the program.

- The version button at the top right shows the state and has the settings: turn the check off, turn automatic install off (you then get a button instead), or leave out pre-releases.
- macOS replaces `OnAir.app`; Windows runs the installer quietly (Windows asks for permission); Linux replaces the portable folder, and a `.deb` install asks for your password through the system's installer.
- Only github.com is contacted, and nothing is sent but the usual request.

## Discord status

Off by default. **View > Show in Discord** shows on your Discord profile that you are using OnAir, the mode and (optionally, **Show the frequency**) the frequency. It needs the Discord desktop app running, talks only to it on your computer, and sends nothing else anywhere.

The official builds ship with the Application ID of the OnAir Discord application. To use your own (a fork): create an application named OnAir at https://discord.com/developers/applications, copy its Application ID into `kDiscordAppId` in `app/discord_rpc.cpp` (or set the environment variable `ONAIR_DISCORD_APP_ID`), and upload the icon (`assets/onair.svg` rendered to PNG) as a Rich Presence art asset named `onair`.

## Good to know

- Encrypted (scrambled) services cannot be decoded.
- codec2 (FreeDV and M17 speech) comes with OnAir as a separate shared library, nothing to install. Some other voice and audio codecs are not shipped (AMBE, HDC and DRA+ are patented). **[docs/CODECS.md](docs/CODECS.md)** says how to install the ones you can, and what to use for the others. The **?** button next to the sound of each such mode says the same.
- **Saving DRA sound.** OnAir cannot decode DRA, but it can save it. In the CDR *Radio* tab, and in the *Player* section of the TV tab for a DTMB service whose sound is DRA (or any service with MPEG audio), press **Save stream**. The raw audio elementary stream goes to `audio-streams/<name>_<date>_<time>.dra` (`.mp2` for MPEG audio) in OnAir's data folder, and the path is shown under the button. Press **Stop saving** (or pick another service) to close the file. Play it with an FFmpeg that has DRA support; user YZYMC reported a build of FFmpeg 7.1.2 with a DRA patch. The commands, assuming such a build:
  ```sh
  ffplay stream.dra                 # listen
  ffmpeg -i stream.dra stream.wav   # convert to WAV
  ```
  If FFmpeg does not recognise the file, name the format: `ffplay -f dra stream.dra`. A normal FFmpeg cannot read `.dra`; `.mp2` files play in any FFmpeg (`ffplay stream.mp2`).
- Reception depends heavily on your antenna and location. A weak or echo-filled signal is the usual cause of a broken picture; the quality score and echo detector are there to help you improve it.
- Receiving broadcasts is legal in most places, but laws differ. Check the rules where you live.

## Build from source

<details>
<summary>Build instructions</summary>

You need a C++20 compiler, CMake 3.20 or newer, pkg-config, FFmpeg, libhackrf, and GLFW for the app. SoapySDR is optional.

```sh
# macOS
brew install cmake pkg-config hackrf soapysdr libusb ffmpeg glfw
# Debian / Ubuntu
sudo apt install build-essential cmake pkg-config libhackrf-dev libsoapysdr-dev libusb-1.0-0-dev \
  libglfw3-dev libavformat-dev libavcodec-dev libavutil-dev libswscale-dev libswresample-dev

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DDECT2_BUILD_APP=ON
cmake --build build -j "$(getconf _NPROCESSORS_ONLN)"   # one job per core (a bare -j starts hundreds and can run out of memory)
ctest --test-dir build          # run the test suite
```

**Tuning for your own CPU.** The release programs are built for the baseline 64-bit PC, so they start on any computer; the hot loops pick AVX2 at run time when the processor has it. A binary you build yourself can be lighter on the processor, because the compiler may use everything your CPU offers. These are the real switches in `CMakeLists.txt`:

- `-DDECT2_WIN_AVX2=ON` (x86-64 on Windows and Intel Macs): build the whole receiver for AVX2 + FMA. The program then refuses to start on older CPUs, so use it only on your own machine.
- `-DCMAKE_CXX_FLAGS="-march=native"` (any GCC or Clang build): CMake has no OnAir-specific option for this, but it does not override `CMAKE_CXX_FLAGS`, so the standard variable works. The result runs only on CPUs like yours; it is not what the release uses.
- `-DDECT2_PORTABLE=ON` (macOS, plain C++ instead of Accelerate) and `-DDECT2_NO_SIMD=ON` go the other way: they are for unusual CPUs, and slower.

Configure with a clean `build` folder after changing these. `-DCMAKE_BUILD_TYPE=Release` is already the default (`-O3`).

The app is `build/dect2`. You can also work without a radio: record an IQ file with your radio's tools and open it in OnAir through the *IQ recording file* source, or run the command-line receiver on it:

```sh
build/dect2cli --file capture.cs8 --rate 10 --format cs8 --play <service id>
```

SigMF recordings (IQEngine, public captures) and two-channel WAV IQ files open the same way: choose the `.sigmf-meta` (or `.sigmf-data`) or the `.wav`, and OnAir reads the sample rate, centre frequency, sample format (cf32, ci16, cu16, ci8, cu8, little or big endian) and start offset from the file, so it plays in any mode; `dect2cli --file x.sigmf-meta` does the same, and `--rate`, `--format` and `--freq` still override.

**Windows** (10 or 11, 64-bit), step by step:

1. Install [MSYS2](https://www.msys2.org) with its default settings.
2. Open **MSYS2 UCRT64** from the Start menu. Use the UCRT64 shell, not MINGW64: MINGW64 has no HackRF and SoapySDR packages.
3. Update MSYS2. If the window closes, open UCRT64 again and run the command a second time:
   ```sh
   pacman -Syu
   ```
4. Install the compiler, the build tools and the libraries:
   ```sh
   pacman -S --needed git zip mingw-w64-ucrt-x86_64-{gcc,cmake,ninja,pkgconf,ffmpeg,glfw,hackrf,soapysdr,libiconv}
   ```
   Optional: `mingw-w64-ucrt-x86_64-nsis` for the setup .exe. (codec2 for FreeDV and M17 is built from `third_party/codec2` with OnAir and goes into the zip.)
5. Get the source:
   ```sh
   git clone https://github.com/adswill/OnAir.git
   cd OnAir
   ```
6. Build:
   ```sh
   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DDECT2_BUILD_APP=ON
   cmake --build build
   ```
7. Run it from the same shell, so Windows finds the DLLs: `./build/dect2.exe`. Run the tests with `ctest --test-dir build`.
8. To get a folder that runs on any PC, with all DLLs next to `OnAir.exe`, run `tools/package/make_windows.sh`. The result is a portable zip in `build-windows/`. If NSIS is installed, you also get a setup .exe.

To use a HackRF, RTL-SDR or Airspy, install the WinUSB driver once with [Zadig](https://zadig.akeo.ie). From macOS or Linux, `tools/package/make_windows_cross.sh` builds the same Windows package with a cross-compiler.

Useful options (all in `CMakeLists.txt`): `DECT2_UI_BACKEND` (Metal or OpenGL3), `DECT2_WITH_SOAPY`, `DECT2_WITH_RFNM` (the RFNM, when librfnm is installed: [DEVICES.md](DEVICES.md)), and `DECT2_PORTABLE` / `DECT2_NO_SIMD` for plain C++ code paths on unusual CPUs.

**macOS app and disk image:** run `tools/package/make_dmg.sh`. It targets macOS 15.0 by default and uses the same minimum version in the executable and the app metadata; override it with `MACOSX_DEPLOYMENT_TARGET` if needed. Every bundled library must also support that version: setting a lower deployment target does not rebuild Homebrew libraries, and packaging stops if any executable or library requires a newer macOS. Build releases on macOS 15 with compatible Homebrew dependencies, as in the release workflow, or rebuild the dependencies for the intended target.

**Project layout**

| Folder | Contents |
|---|---|
| `core/` | The receiver library: demodulators, error correction, transport stream, player, network tuner |
| `app/` | The graphical application |
| `tools/` | Command-line receiver and scanner, plus the packaging scripts |
| `tests/` | Unit tests and benchmarks |
| `packaging/` | Linux and Windows installer resources |
</details>

## Support and community

- **[Discord](https://discord.gg/Kky9c6atm)**: support, feature requests, questions, and sharing what you receive.
- **[GitHub Issues](https://github.com/adswill/OnAir/issues)**: bug reports.
  Attach an IQ recording of the problem: press **Record IQ** in the Source panel (or run `onair-cli --record auto --secs 10`); files go to the `recordings` folder of the app data folder, and **Show recording** opens it. With **SigMF** ticked (the default) every recording also gets a `.sigmf-meta` file (sample rate, data type, centre frequency, time) and the same data under a `.sigmf-data` name (a hard link, no second copy), so it opens directly in [IQEngine](https://iqengine.org) and other SigMF tools.
- **[Signal sample library](https://files.on-air.dev)**: IQ and audio recordings of real signals, shared by users and tagged by mode (for example `#dvb-t2`). Upload your own: every file is checked and reviewed before it appears, and recordings of modes marked experimental are the most useful.
- **[Pull requests](https://github.com/adswill/OnAir/pulls)** are welcome.

## Supporting OnAir

I'm a bit embarrassed to ask this, but here goes. OnAir is free and always will be. I build it in my spare time, along with the radios, antennas and test equipment it needs. If it's useful to you and you'd like to help, a small tip on **[Ko-fi](https://ko-fi.com/adswill)** really does help and keeps new modes coming. If you'd rather not, that's completely fine: bug reports, recordings and kind words help just as much. Thank you!

## License

OnAir is released under the **GNU General Public License v3.0 or later**, see [LICENSE](LICENSE). It includes [Dear ImGui](https://github.com/ocornut/imgui) and [miniaudio](https://github.com/mackron/miniaudio), each under its own permissive license. It ships [codec2](https://github.com/drowe67/codec2) (LGPL-2.1, `third_party/codec2`) as a separate shared library that it loads at run time, so you can replace it with your own build (see [docs/CODECS.md](docs/CODECS.md)). It uses [FFmpeg](https://ffmpeg.org), [GLFW](https://www.glfw.org), [libusb](https://libusb.info) and [libhackrf](https://github.com/greatscottgadgets/hackrf).
