<p align="center"><img src="packaging/icons/onair_1024.png" width="140" alt="OnAir logo"></p>

<h1 align="center">OnAir</h1>

<p align="center"><b>Watch digital TV and listen to digital radio with a software-defined radio.</b><br>
Free and open source, for macOS, Windows and Linux.</p>

<p align="center">
  <a href="https://github.com/adswill/OnAir/releases/latest"><img src="https://img.shields.io/github/v/release/adswill/OnAir?label=download&color=2ea44f" alt="Latest release"></a>
  <a href="https://github.com/adswill/OnAir/releases"><img src="https://img.shields.io/github/downloads/adswill/OnAir/total?color=blue" alt="Downloads"></a>
  <img src="https://img.shields.io/badge/platforms-macOS%20%7C%20Windows%20%7C%20Linux-lightgrey" alt="Platforms">
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-GPL--3.0-orange" alt="License: GPL-3.0"></a>
  <a href="https://discord.gg/Kky9c6atm"><img src="https://img.shields.io/badge/chat-Discord-5865F2?logo=discord&logoColor=white" alt="Discord"></a>
  <a href="https://ko-fi.com/adswill"><img src="https://img.shields.io/badge/%E2%9D%A4-Donate-FF5E5B" alt="Donate"></a>
</p>

<p align="center">
  <a href="https://github.com/adswill/OnAir/releases/latest"><b>Download</b></a> &nbsp;·&nbsp;
  <a href="https://github.com/adswill/OnAir/releases/download/v0.2.0/OnAir_promo_v0.2.0.mp4"><b>Overview video</b></a> &nbsp;·&nbsp;
  <a href="https://discord.gg/Kky9c6atm"><b>Discord</b></a> &nbsp;·&nbsp;
  <a href="https://github.com/adswill/OnAir-channels"><b>Channel list</b></a>
</p>

---

OnAir turns a low-cost SDR (a HackRF, and many other radios) into a complete digital broadcast receiver. Plug in an antenna, pick a channel, and watch live TV or listen to digital radio, while seeing exactly what the receiver is doing: the spectrum, the signal quality, the constellations and the echoes in your reception.

It is built for people who want a receiver that is both easy to use and honest about what is happening inside it.

## Contents

- [Install](#install) (and the [development version](#development-version-nightly))
- [Quick start](#quick-start)
- [What it receives](#what-it-receives)
- [Features](#features)
- [Supported radios](#supported-radios) (and [network radios](#network-radios))
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

**Windows USB drivers:** for a HackRF, RTL-SDR, Airspy or Airspy HF+, install the WinUSB driver once with [Zadig](https://zadig.akeo.ie). A LimeSDR Mini needs FTDI's FT60x driver (see the README inside the download). A LimeSDR-USB (Cypress FX3) needs the Cypress/LimeSuite USB driver (CyUSB3, the one LimeGUI installs), not WinUSB: do not replace it with Zadig. OnAir's own LimeSuite has no FX3 support, so for a LimeSDR-USB also install [PothosSDR](https://downloads.myriadrf.org/builds/PothosSDR/) or LimeSuite; OnAir finds its LimeSuite.dll by itself and the log (onair.log) says which copy saw the radio ("LimeSuite ...: N radio(s)").

### Development version ("nightly")

Fixes and new features land on the `main` branch first, before they go into a release. To try them early, [build OnAir from source](#build-from-source) from the latest `main`.

> [!WARNING]
> The development version is **not tested like a release**. It can contain bugs, unfinished features or changes that stop parts of the app (or the whole app) from working. Use the [latest release](https://github.com/adswill/OnAir/releases/latest) for everyday use, and if something breaks in a development build, please say so on [Discord](https://discord.gg/Kky9c6atm) or in [GitHub Issues](https://github.com/adswill/OnAir/issues), mentioning the commit you built.

## Quick start

1. Connect your radio and antenna, then start OnAir. Connected radios appear in the source menu.
2. Pick what you want to receive in the mode list on the left.
3. Enter a frequency, or open the **Scan** tab to find channels automatically.
4. Press **Start**. When the receiver locks, the channels appear on the right: click one to watch or listen.
5. If the picture breaks up, use **Auto-tune** for the gain and watch the quality score while you move the antenna. Height and a clear view toward the transmitter matter more than anything else.

> **No radio yet?** Press **Tour**: a friendly little TV walks you through OnAir using a built-in demo signal, so you can explore everything before buying a radio.

## What it receives

| Standard | Where it is used | Content |
|---|---|---|
| **DVB-T2** (including T2-Lite) | Europe, Middle East, Africa, Asia, Australia | Digital TV |
| **DVB-T** | Same regions, older networks | Digital TV |
| **ATSC 1.0** | North America, South Korea | Digital TV |
| **ATSC 3.0** (NextGen TV), *experimental* | North America, South Korea | Digital TV |
| **ISDB-T**, *experimental* | Japan, Brazil and most of South America, Philippines | Digital TV and one-segment mobile TV |
| **DAB / DAB+** | Europe, Australia and more | Digital radio |
| **FM radio** (87.5 – 108 MHz) | Worldwide | Analogue radio: stereo and RDS (station name, radio text, programme type, traffic flags) |

The DVB standard is detected automatically.

### Also receives (experimental)

These receivers are built from the published specifications and checked end to end on simulated signals, but have had little or no testing on real transmissions yet. Reports and recordings on [Discord](https://discord.gg/Kky9c6atm) help a lot.

| Mode | What it is | What you see |
|---|---|---|
| **DVB-S / DVB-S2** | Satellite TV (through an LNB) | Channels and playback, all DVB-S2 MODCODs |
| **DTMB** | Digital TV in China, Hong Kong, Macau (8 MHz) and Cuba (6 MHz) | Channels and playback |
| **DVB-C** | Cable TV, EN 300 429 / J.83 annex A and C, 16 to 256-QAM (needs a cable feed) | Symbol rate and QAM found automatically, channels and playback |
| **Analog TV** | PAL, SECAM, NTSC; systems B/G, I, D/K, M, N, H, K', positive-modulation A (405 lines), C, E (819 lines), L, L'; NBTV and mechanical TV | Picture and sound |
| **MAC** | D2-MAC and D-MAC satellite TV (FM, 27 MHz; 20 Msps sees most of it) | Picture (4:3 / 16:9), channel name, digital sound; EuroCrypt detected, not descrambled |
| **DRM** | Digital radio on long, medium and short wave | Audio and station information |
| **DMR** | Digital two-way radio | Talkgroups, IDs and data (no voice) |
| **NXDN** | Digital two-way radio, NXDN48 and NXDN96 | Call log with unit IDs and talkgroups, status and short data messages, site information, voice through your mbelib |
| **P25** | APCO Project 25 Phase 1 (TIA-102), C4FM, 12.5 kHz, conventional and trunked | Calls with talkgroups, unit IDs and emergency flags; trunking: WACN, system, NAC, RFSS / site, adjacent sites, channel identifiers, voice grants, talkgroup activity; voice through your mbelib. Receive only: encrypted calls are only reported as encrypted, nothing is decrypted. Follow the rules on receiving radio where you live |
| **dPMR / PMR446** | Digital two-way radio (ETSI TS 102 490), 6.25 kHz, PMR446 channels 1 to 16 | Call log with own and called IDs and colour codes, status and short messages, packet data, scan of all 16 PMR446 channels, voice through your mbelib |
| **ADS-B** | Aircraft transponders, 1090 MHz | Aircraft on a map, with altitude, speed and callsign |
| **Mode S uplink** | Radar and ACAS interrogations, 1030 MHz (needs 8 Msps) | Interrogator codes and rotation periods, aircraft and the registers asked for, TCAS coordination |
| **ACARS** | Aircraft data link, VHF | Messages, and positions on a map |
| **HFDL** | Aircraft data link on HF, 3 - 22 MHz | Squitters and ground stations, ACARS, positions on a map; every channel in the band at once |
| **CW** | Morse code on the HF bands | Every CW signal in a 14 kHz window at once: speed, SNR and text, callsigns highlighted |
| **Spectrum analyser** | Wide sweeps, 1 - 6000 MHz in about a second with a HackRF (its native sweep), stepped retuning on other radios | Live, peak-hold and averaged traces, persistence, waterfall, markers, EU band overlay, spur removal, CSV export |
| **JS8Call** | Weak-signal keyboard messaging on HF (normal, fast, turbo) | Every frame in the passband, messages put together from their frames, heartbeats and CQs, relays, stations and their grids on a map |
| **HF ALE** | 2G automatic link establishment (MIL-STD-188-141 / FED-STD-1045) on HF | Who calls whom: soundings, individual, net and group calls, responses and links, AMD messages, LQA reports, with BER and SNR; the stations heard |
| **Spectrum explorer** | No decoder: a tool to look around | Spectrum and waterfall with markers, zoom and pan, region measurements (occupied bandwidth at -3 / -26 dB, channel power, SNR, symbol rate), LNB and converter offsets, range sweeps with signal detection, bookmarks |
| **Radio astronomy** | No decoder: the 21 cm hydrogen line (1420.4058 MHz) and the OH lines | Long integration of the spectrum (1 s to 1 h, FFT 1024 - 16384), on - off reference or frequency switching, polynomial baseline outside a line window, radial velocity axis (km/s), peak marker, drift-scan waterfall, CSV export, a short how-to (LNA, filter, horn or dish) |
| **Time signals** | DCF77 (77.5 kHz), MSF, WWVB (60 kHz), JJY (40 / 60 kHz) carrier keying; WWV / WWVH 100 Hz BCD subcarrier and ticks; CHU 300 bit/s FSK (A frames). LF needs a radio that tunes below 1 MHz (SDRplay, Airspy HF+, RTL-SDR direct sampling) | Time, date, time zone, summer time and leap second flags, DUT1, the seconds tick and its jitter, the computer clock offset after the path delay from your position, level and SNR history |
| **Ionospheric sounders** | No decoder: HF chirp ionosondes and oblique sounders sweeping 0 - 30 MHz (100, 125, 150, 200, 500 kHz/s) | Sweeps found by dechirping, their rate, period and start offset; an ionogram of group range against frequency built from every sweep through the captured span (a HackRF at 20 Msps sees 18 MHz of it), the band coverage of a narrow radio, a timeline of sweeps, PNG / CSV export |
| **M17** | Open digital voice, 2 m / 70 cm | Callsigns, voice through your libcodec2, text messages, positions on a map |
| **D-STAR** | Amateur digital voice, 2 m / 70 cm | Call log with callsigns and repeaters, slow data messages, D-PRS / GPS positions on a map, voice through your mbelib |
| **PSK31** | Keyboard chat on the HF bands (BPSK31 / 63, QPSK31 / 63) | Every signal in the 3 kHz passband at once: a chat log per signal, the picked one in big text, callsigns highlighted |
| **Airband** | AM voice, 118 - 137 MHz | Several 25 and 8.33 kHz channels at once: squelch, scan, priority, activity log |
| **VDL Mode 2** | Aircraft data link, 136.7 - 137 MHz | Every VDL2 channel at once: ACARS, X.25 / CLNP, ADS-C, aircraft and ground stations, positions on a map |
| **VOR** | Navigation beacons, 108 - 118 MHz | The radial (conventional and Doppler VOR), Morse identification, voice; a position from two stations on the map |
| **ILS** | Approach guidance, 75 / 108 - 112 / 329 - 335 MHz | Localizer and glide slope needles (DDM), marker beacons, identification |
| **Inmarsat Aero** | Aircraft satellite data link, L band | Messages, and positions on a map |
| **Inmarsat-C** | Maritime satellite broadcasts | Safety messages (SafetyNET) and system information |
| **Iridium** | Satellite phone network | Satellites on a map, pager messages, ring alerts |
| **Orbcomm** | Satellite data downlinks, 137.2 to 137.8 MHz (4800 bit/s SDPSK) | Two downlinks at once with Doppler tracking, satellites on the map from their ephemeris, packet log by type |
| **QO-100** | Es'hail-2, the geostationary amateur satellite: downlink 10489.5 - 10500 MHz through a Ku LNB (IF 739.5 - 750 MHz with 9750 MHz; 9739, 10000, 10600 or any LO) | Beacon lock: the CW beacons measure the LNB's frequency error and drift (kHz, ppm, history) and correct every frequency shown; the narrowband transponder as a high-resolution spectrum and waterfall with SSB listening (USB, 2.7 kHz); the wideband transponder's DATV carriers with their symbol rates and a hand-over to DVB-S/S2 |
| **CubeSat telemetry** | Amateur satellites on 2 m / 70 cm (145.8 - 146 and 435 - 438 MHz): AX.25 at 1200 baud AFSK and 9600 / 4800 baud G3RUH, GOMspace AX100 Mode 5 (ASM + Golay + Reed-Solomon, GMSK 9600 / 4800) and Mode 6 (Reed-Solomon in HDLC), CCSDS Reed-Solomon (255,223) frames, all at once | A frame log with callsigns or CSP addresses, hex and ASCII dumps, Reed-Solomon corrections, satellite names from an editable table (NORAD number, frequency, framing, callsign), KISS out to SatNOGS-style tools through the data feeds; the pass planner starts it at AOS with Doppler correction. The AX100 convolutional code and AO-40 FEC are not decoded |
| **AIS** | Ship transponders | Ships on a map |
| **Maritime** | NAVTEX, DSC, weather fax | Safety messages, distress and calling, fax pictures |
| **Radiosondes** | Weather balloons, 400 – 406 MHz | Balloon tracks, altitude and weather data |
| **ISM sensors** | 433.92 / 868 / 915 MHz devices (the rtl_433 idea) | Weather stations, tyre pressure sensors, remotes and energy / water meters (wireless M-Bus header), and Z-Wave R1 / R2 / R3 (ITU-T G.9959: Home IDs, nodes, command classes; passive receive only, S0 and S2 traffic stays encrypted and is never decrypted): a device table with history, a Z-Wave view grouped by Home ID, a live log, unknown signals with their pulses |
| **Cell towers** | LTE downlink broadcast (MIB, SIB1 - SIB5) | Operator, band, EARFCN, PCI, eNB / sector, TAC, neighbours, band scan; receive only |
| **5G NR cells** | NR SS/PBCH blocks on the sync raster (GSCN) of n1, n3, n7, n8, n20, n28, n38, n40, n41, n77 and n78, 15 / 30 kHz SSBs, from 3.84 / 7.68 Msps (20 Msps searches several raster points per step) | PCI (PSS / SSS), SSB index and the beams heard, SS-RSRP / RSRQ / SINR, carrier offset, and the MIB from the polar-coded PBCH: SFN, common subcarrier spacing, k_SSB, DMRS type A position, CORESET#0 and search space 0 of SIB1, barred, intra-frequency reselection. Receive only, public broadcast; SIB1 (operator, TAC, cell id) is not decoded yet, so the tower map stays empty |
| **GSM-R** | Railway GSM broadcast (BCCH, 921 - 925 MHz) | Base stations: ARFCN, BSIC, cell identity, location area, operator code, channel lists; band scan; receive only |
| **TETRA** | TETRA base station broadcast (ETSI EN 300 392-2: BSCH, BNCH, AACH, MCCH), 25 kHz channels | Network code, colour code, location area, carriers and duplex spacing, services, neighbour cells, whether encryption is used; scan over a span; receive only, nothing decrypted |
| **Bluetooth LE** | Advertising on channels 37, 38 and 39 (2402 / 2426 / 2480 MHz), one at a time or hopping, 4 Msps | What a scanner app such as nRF Connect shows: a device table with address type, name, level history, vendor, services, iBeacon, Eddystone and exposure notification beacons, and a packet log. Receive only: it never connects, pairs or transmits and does not follow connections; most phones use random addresses that change every few minutes for privacy |
| **Wi-Fi networks** | 2.4 GHz access point beacons on channels 1 - 13 (802.11b DSSS, 1 Mbit/s DBPSK, Barker-11), one channel at a time or hopping, 20 Msps | What a phone's Wi-Fi list shows: SSID (hidden networks as hidden), BSSID with the vendor, channel, security (open, WEP, WPA, WPA2, WPA3, enterprise), supported rates, beacon interval, level history and last seen, and a beacon log. Passive: only beacons and probe responses are read; data frames are counted, never decoded; it never transmits, probes or joins. Access points that beacon only at OFDM rates (802.11g / n only) are not heard |
| **General receiver** | Anywhere the radio tunes, 2 Msps | NFM, WFM (mono), AM, USB, LSB and CW (BFO pitch) on a channel clicked on the spectrum, with draggable filter edges and width presets, squelch (level or automatic against the noise beside the channel), AGC, de-emphasis, S-meter, frequency steps and audio recording to WAV |
| **Drone Remote ID** | ASTM F3411 / ASD-STAN EN 4709-002 Open Drone ID on 2.4 GHz: Bluetooth LE legacy advertising (service data 0xFFFA) and Wi-Fi beacons (vendor element FA:0B:BC), both at once from one 20 Msps band at 2432 MHz | A drone table (serial number or session ID, UA type, position, altitude and height, speed and track, operator position and ID, Self-ID, last seen) and a map with each drone linked to its operator. Receive only: Remote ID is a public broadcast meant for anyone to read; nothing is transmitted. BLE 5 long range and Wi-Fi NAN are not heard |
| **Meteor-M LRPT** | Meteor-M N2-3 / N2-4 weather pictures on 137.100 / 137.900 MHz: OQPSK 72k (differential), QPSK 72k or the interleaved OQPSK 80k mode, Viterbi, Reed-Solomon, CCSDS packets, MSU-MR JPEG-like strips | The picture grows line by line during the pass: channels 1 - 6 and a colour composite (123 by day, 125 at night), Save PNG; View > Passes starts it at AOS and follows the Doppler. Checked only against the built-in test signal so far: no off-air recording yet |
| **GOES HRIT** | GOES-16 / 18 / 19 HRIT on 1694.1 MHz: BPSK 927 ksym/s, K = 7 Viterbi, Reed-Solomon (255,223) x 4, CCSDS VCDU / M_PDU / CP_PDU, LRIT / HRIT files with Rice (CCSDS 121), plain and JPEG images | A product list (full disk, mesoscale, CONUS and relayed images, EMWIN text, admin messages) with an image viewer that grows as the lines arrive, EMWIN ZIP files unpacked, everything saved to a folder. Needs a dish (about 1 m, or a grid antenna) with an LNA and filter at the feed |
| **GNSS** | GPS L1 | Satellites, position and time |
| **Mesh** | Meshtastic and MeshCore (LoRa) | Nodes, map and chat |
| **LoRaWAN** | EU868 (868.1 / 868.3 / 868.5 MHz and 867.x) or US915, SF7 - SF12 at once | A device table (DevAddr or DevEUI, network from the NetID, last heard, level, SNR, frame counter, SF) and a frame log with filters: message type, FCtrl, FCnt, MAC commands, port, encrypted length, MIC, join requests; only the clear headers, payloads stay encrypted |

In development (the mode is in the app, but nothing is decoded yet): **HD Radio**, **CDR** (China Digital Radio), **Pagers** (POCSAG, FLEX), **APRS / Packet** (AX.25) and **HF digital** (RTTY, SSTV, FreeDV).

<details>
<summary><b>Iridium bursts for iridium-toolkit</b></summary>

In the Iridium screen, **Save for iridium-toolkit** writes every demodulated burst (downlink, uplink and simplex, unique word included) as the `RAW:` lines of gr-iridium's `iridium-extractor`, by default to `output.bits` in the recordings folder. From the command line: `onair-cli --standard iridium --iridium-bits output.bits --secs 600`. Then run the toolkit ([muccc/iridium-toolkit](https://github.com/muccc/iridium-toolkit)) on it as usual:

```
iridium-parser.py output.bits > output.parsed
```

OnAir writes the bits only and never decodes voice (the AMBE codec is patented). Extracting voice from `output.parsed` with the toolkit's voice tools is up to you and happens outside OnAir.
</details>

### Cell tower map (OpenCelliD key)

Cell towers broadcast who they are (operator, area code, cell id) but not where they stand. The Cells mode can look each decoded LTE cell up in [OpenCelliD](https://opencellid.org), an open, crowd-sourced tower database, and put it on the map:

1. Sign up (free) at https://opencellid.org and confirm your e-mail.
2. Log in and open your account page (My Account / API access); copy your API token.
3. In OnAir, open Cells → Map and paste it into **OpenCelliD API key** (the "?" next to it repeats these steps).

Lookups need internet and run in the background at most once a second; only the cell identity (MCC, MNC, TAC, cell id) and your key are sent. Answers are cached on disk, so the map also works offline afterwards. A CSV downloaded from OpenCelliD still works instead (Load tower file). Tower data © OpenCelliD, CC BY-SA 4.0.

### TETRA network information

The TETRA mode lists the TETRA base stations it hears, with what each one broadcasts in the clear to every terminal: the network code (MCC, MNC), colour code, location area, the main carrier, uplink and duplex spacing, the services the cell offers, its neighbour cells, the network time, and whether the network uses air interface encryption. The same information is shown by open-source tools such as osmo-tetra. **Scan** steps the radio over a span (390 - 395 MHz, 410 - 430, 450 - 470 or your own) and lists every carrier found.

It is receive only and standards based. It does not decrypt anything, does not transmit, does not join or disturb any network, and does not decode or play voice, short data messages or subscriber identities: encrypted signalling is only counted and reported as encrypted, and traffic timeslots are skipped. You are responsible for following your country's rules on receiving radio signals.

### LoRaWAN monitor

The LoRaWAN entry (in the Mesh family) shows what every LoRaWAN gateway in range receives in the clear: the message type (join request, (un)confirmed uplink or downlink, proprietary), the DevAddr with the network its NetID belongs to (The Things Network, Helium, Orange, ...), the FCtrl flags, the frame counter, the MAC commands of LoRaWAN 1.0 FOpts, the port, the length of the encrypted payload and the MIC; for join requests the JoinEUI, the DevEUI (with its maker when the OUI is in a short table) and the DevNonce. All spreading factors of every channel in the capture are searched at once.

It is receive only. The application payloads and join accepts are encrypted with the devices' own keys and stay encrypted: OnAir never decrypts them, never checks a MIC and never transmits. These are other people's devices; you are responsible for following your country's rules on receiving radio signals.

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
- **DC spike handling:** leave the radio's centre spike alone, remove it, or keep it off the channel by tuning just beside it. Optional **IQ imbalance correction**.

**Share it**
- A built-in **network tuner** streams your channels to VLC, phones, Plex or Jellyfin over your home network, as a plain stream or as HLS for browsers and phones.
- On a Mac, **cast** the playing service to an Apple TV or AirPlay TV.
- **Record** to a file, or send the stream out over UDP.

**Comfortable to use**
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
| **Airspy HF+** | ❌ | Narrow channels only | HF and 60 – 260 MHz |
| **Anything else** | depends | depends | Through [SoapySDR](https://github.com/pothosware/SoapySDR), once its SoapySDR module is installed |

- The libraries these radios need are included in the downloads, except as noted above.
- A TV channel needs a radio that samples fast enough: roughly 1 million samples per second per MHz of channel width. If a radio cannot reach the rate or frequency a mode needs, OnAir says so next to the source.
- The radios other than the HackRF have had less real-world testing. Notes for specific radios are in **[DEVICES.md](DEVICES.md)**.

### Network radios

Radios on another computer or on the network are added from the source list: pick **Add network radio…**, choose the type, type its address and press **Connect**. OnAir tries the connection for a few seconds and says why when it gets no answer; the radios added are remembered and listed with the others (remove them in the same window).

| Type | Address | Port |
|---|---|---|
| **PlutoSDR** by IP | `192.168.2.1` (its USB network), or its address on your network | libiio's default |
| **rtl_tcp** (an RTL-SDR shared by `rtl_tcp -a 0.0.0.0`) | the computer running `rtl_tcp` | 1234 |
| **SoapyRemote** (any SoapySDR radio shared by `SoapySDRServer --bind`) | the computer running the server | 55132 |
| **Hermes Lite 2** | found by itself on your networks (or type its address) | 1024 |

- A Pluto on a real network (not its USB cable) is offered its full sample rate; how much of it arrives depends on the network.
- rtl_tcp carries 8-bit samples at up to about 2.5 Msps; when the connection drops, OnAir connects again and counts the samples that were missed.
- SoapyRemote needs a build with SoapySDR and the SoapyRemote module on both computers.
- Choosing **Hermes Lite 2** searches this computer's networks for one (openHPSDR discovery) and fills in its address; **Search** looks again. It receives up to 38.4 MHz at 48 to 384 ksps, so only HF and narrow modes fit; the N2ADR filter board's band filters are switched with the frequency (a setting of the radio turns that off).

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
- Some voice and audio codecs are not shipped with OnAir (codec2 for FreeDV; AMBE, HDC and DRA+ are patented). **[docs/CODECS.md](docs/CODECS.md)** says how to install the ones you can, and what to use for the others. The **?** button next to the sound of each such mode says the same.
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

The app is `build/dect2`. You can also work without a radio: record an IQ file with your radio's tools and open it in OnAir through the *IQ recording file* source, or run the command-line receiver on it:

```sh
build/dect2cli --file capture.cs8 --rate 10 --format cs8 --play <service id>
```

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
   Optional: `mingw-w64-ucrt-x86_64-codec2` for FreeDV sound, and `mingw-w64-ucrt-x86_64-nsis` for the setup .exe.
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

The build needs a CPU with AVX2. To use a HackRF, RTL-SDR or Airspy, install the WinUSB driver once with [Zadig](https://zadig.akeo.ie). From macOS or Linux, `tools/package/make_windows_cross.sh` builds the same Windows package with a cross-compiler.

Useful options: `DECT2_UI_BACKEND` (Metal or OpenGL3), `DECT2_WITH_SOAPY`, and `DECT2_PORTABLE` / `DECT2_NO_SIMD` for plain C++ code paths on unusual CPUs.

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
  Attach an IQ recording of the problem: press **Record IQ** in the Source panel (or run `onair-cli --record auto --secs 10`); files go to the `recordings` folder of the app data folder, and **Show recording** opens it.
- **[Pull requests](https://github.com/adswill/OnAir/pulls)** are welcome.

## Supporting OnAir

I'm a bit embarrassed to ask this, but here goes. OnAir is free and always will be. I build it in my spare time, along with the radios, antennas and test equipment it needs. If it's useful to you and you'd like to help, a small tip on **[Ko-fi](https://ko-fi.com/adswill)** really does help and keeps new modes coming. If you'd rather not, that's completely fine: bug reports, recordings and kind words help just as much. Thank you!

## License

OnAir is released under the **GNU General Public License v3.0 or later**, see [LICENSE](LICENSE). It includes [Dear ImGui](https://github.com/ocornut/imgui) and [miniaudio](https://github.com/mackron/miniaudio), each under its own permissive license, and uses [FFmpeg](https://ffmpeg.org), [GLFW](https://www.glfw.org), [libusb](https://libusb.info) and [libhackrf](https://github.com/greatscottgadgets/hackrf).
