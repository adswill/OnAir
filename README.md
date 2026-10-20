<p align="center"><img src="packaging/icons/onair_1024.png" width="140" alt="OnAir logo"></p>

<h1 align="center">OnAir</h1>

<p align="center"><b>Receive TV, radio, aircraft, ships, satellites, two-way radio, pagers, time signals and more: 120 modes with one software-defined radio.</b><br>
<p align="center"><b>Receive TV, radio, aircraft, ships, satellites, two-way radio, pagers, time signals and more: 113 modes with one software-defined radio.</b><br>
<p align="center"><b>Receive TV, radio, aircraft, ships, satellites, two-way radio, pagers, time signals and more: 142 modes with one software-defined radio.</b><br>
Free and open source, for macOS, Windows, Linux and Android.</p>

<p align="center">
  <a href="https://github.com/adswill/OnAir/releases/latest"><img src="https://img.shields.io/github/v/release/adswill/OnAir?label=download&color=2ea44f" alt="Latest release"></a>
  <a href="https://github.com/adswill/OnAir/releases"><img src="https://img.shields.io/github/downloads/adswill/OnAir/total?color=blue" alt="Downloads"></a>
  <a href="https://github.com/adswill/OnAir/stargazers"><img src="https://img.shields.io/github/stars/adswill/OnAir?style=flat&color=yellow" alt="Stars"></a>
  <a href="https://github.com/adswill/OnAir/actions/workflows/ci.yml"><img src="https://github.com/adswill/OnAir/actions/workflows/ci.yml/badge.svg" alt="CI"></a>
  <a href="https://app.codacy.com/gh/adswill/OnAir/dashboard?branch=0.2.7"><img src="https://img.shields.io/badge/code%20quality-A-brightgreen?logo=codacy" alt="Code quality: Codacy grade A"></a> <!-- swap for the dynamic badge: Codacy > Repository settings > General > Badges (Markdown) -->
  <img src="https://img.shields.io/badge/platforms-macOS%20%7C%20Windows%20%7C%20Linux%20%7C%20Android-lightgrey" alt="Platforms">
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-GPL--3.0-orange" alt="License: GPL-3.0"></a>
  <a href="https://on-air.dev"><img src="https://img.shields.io/badge/website-on--air.dev-1b232b" alt="Website"></a>
  <a href="https://discord.gg/Kky9c6atm"><img src="https://img.shields.io/badge/chat-Discord-5865F2?logo=discord&logoColor=white" alt="Discord"></a>
</p>

<p align="center">
  <a href="https://on-air.dev"><b>Website</b></a> &nbsp;·&nbsp;
  <a href="https://on-air.dev/docs/"><b>Docs</b></a> &nbsp;·&nbsp;
  <a href="https://github.com/adswill/OnAir/releases/latest"><b>Download</b></a> &nbsp;·&nbsp;
  <a href="https://github.com/adswill/OnAir/releases/download/v0.2.7/OnAir_promo_v0.2.7.mp4"><b>Overview video</b></a> &nbsp;·&nbsp;
  <a href="https://discord.gg/Kky9c6atm"><b>Discord</b></a> &nbsp;·&nbsp;
  <a href="https://files.on-air.dev"><b>Signal samples</b></a> &nbsp;·&nbsp;
  <a href="https://map.on-air.dev"><b>Live map</b></a> &nbsp;·&nbsp;
  <a href="https://github.com/adswill/OnAir-channels"><b>Channel list</b></a>
</p>

<p align="center"><a href="https://github.com/adswill/OnAir/releases/download/v0.2.7/OnAir_promo_v0.2.7.mp4"><img src="assets/promo-thumb-0.2.7.jpg" width="720" alt="OnAir 0.2.7 overview video"></a><br><sub>Watch the 0.2.7 overview video (2:46)</sub></p>

<p align="center"><a href="https://ko-fi.com/adswill"><img src="https://ko-fi.com/img/githubbutton_sm.svg" alt="Support OnAir on Ko-fi" height="36"></a></p>

---

<div align="center">

<table>
<tr>
<td align="center" width="33%"><a href="docs/img/main.jpg"><img src="docs/img/main.jpg" width="300" alt="Spectrum, waterfall and DVB-T constellations"></a><br><sub>Spectrum, waterfall and DVB-T constellations</sub></td>
<td align="center" width="33%"><a href="docs/img/adsb.jpg"><img src="docs/img/adsb.jpg" width="300" alt="ADS-B aircraft map"></a><br><sub>ADS-B aircraft map</sub></td>
<td align="center" width="33%"><a href="docs/img/dab.jpg"><img src="docs/img/dab.jpg" width="300" alt="DAB+ ensemble"></a><br><sub>DAB+ ensemble</sub></td>
</tr>
<tr>
<td align="center" width="33%"><a href="docs/img/fm.jpg"><img src="docs/img/fm.jpg" width="300" alt="FM with RDS and Listen"></a><br><sub>FM with RDS and Listen</sub></td>
<td align="center" width="33%"><a href="docs/img/ais.jpg"><img src="docs/img/ais.jpg" width="300" alt="AIS ships on the map"></a><br><sub>AIS ships on the map</sub></td>
<td align="center" width="33%"><a href="docs/img/touch.jpg"><img src="docs/img/touch.jpg" width="300" alt="Touch interface"></a><br><sub>Touch interface</sub></td>
</tr>
</table>

<sub>Screenshots use the built-in test signals and demo data, so no radio is shown. Click one to enlarge.</sub>

</div>

OnAir turns a low-cost SDR (a HackRF, an RTL-SDR, an Airspy and many other radios) into a receiver for most of what is on the air. Plug in an antenna, pick a mode, and watch what happens inside: the spectrum, the sync, the error correction and the decoded data. It is built for people who want a receiver that is both easy to use and honest about what is happening inside it.

OnAir is a receiver first. It can also send test signals through a transmit-capable radio into a dummy load or a shielded setup (off by default, behind a licence notice, see [Transmit](#transmit)).

> [!TIP]
> New here? Press **Tour** in the app: a little TV walks you through OnAir with a built-in demo signal, so you can explore everything before you buy a radio.

## Contents

- [What it does](#what-it-does)
- [What is new in 0.2.8](#what-is-new-in-028) · [0.2.7](#what-is-new-in-027) · [0.2.6](#what-is-new-in-026)
- [Install](#install) · [Android app](#android-app) · [Development version](#development-version-nightly)
- [Quick start](#quick-start)
- [Modes](#modes) · [Tools and notes](#tools-and-notes)
- [Features](#features)
- [Supported radios](#supported-radios) · [Network radios](#network-radios)
- [Transmit](#transmit)
- [Remote-control API](#remote-control-api)
- [Updates](#updates) · [Discord status](#discord-status)
- [Good to know](#good-to-know) · [Troubleshooting](#troubleshooting)
- [Build from source](#build-from-source)
- [Support and community](#support-and-community) · [Credits](#credits) · [Contributors](#contributors) · [License](#license)

## What it does

| Family | Modes | What you get |
|---|:---:|---|
| **TV** | 10 | DVB-T/T2, DVB-S/S2/S2X, DVB-C, ATSC 1.0 and 3.0, ISDB-T, DTMB, analogue TV and MAC, analog FPV video (5.8 / 1.2 GHz), with a live player (picture, sound, subtitles, teletext, programme guide), recording, pause and live buffer, and 2-3 adjacent channels from one wide capture |
| **Broadcast radio** | 6 | DAB+ (with a programme guide), DRM (with its data services), HD Radio, CDR, AM broadcast (medium wave and shortwave), AM stereo (C-QUAM), and FM with RDS, a Mono switch for weak stations and a saved-station library |
| **TV** | 11 | DVB-T/T2, DVB-S/S2/S2X, TV sat beacon decoder, DVB-C, ATSC 1.0 and 3.0, ISDB-T, DTMB, analogue TV and MAC, analog FPV video (5.8 / 1.2 GHz), with a live player (picture, sound, subtitles, teletext, programme guide), recording, pause and live buffer, and 2-3 adjacent channels from one wide capture |
| **Broadcast radio** | 5 | DAB+, DRM (with its data services), HD Radio, CDR, and FM with RDS, a Mono switch for weak stations and a saved-station library |
| **Aviation** | 14 | ADS-B with a live map, UAT, ACARS, VDL2 and VDL4, HFDL, airband, VOR / ILS / NDB, FLARM, drones |
| **Broadcast radio** | 5 | DAB+, DRM (with its data services), HD Radio, CDR, and FM with RDS, a Mono switch for weak stations and a saved-station library |
| **Aviation** | 15 | ADS-B with a live map, Mode A/C squawks and altitudes, UAT, ACARS, VDL2 and VDL4, HFDL, airband, VOR / ILS / NDB, FLARM, drones |
| **Maritime** | 5 | AIS, VDES, NAVTEX, DSC, weather fax |
| **Satellite and navigation** | 12 | Weather pictures from Meteor, GOES, Elektro-L and MetOp, Iridium, Inmarsat, Orbcomm, QO-100, CubeSat telemetry, GNSS, COSPAS-SARSAT 406 MHz distress beacons |
| **Two-way radio** | 7 | DMR, P25 (with a trunking follower), NXDN, dPMR, TETRA, TETRAPOL, tone decoders (CTCSS, DCS, DTMF, selcall) |
| **Satellite and navigation** | 13 | Weather pictures from Meteor, GOES, Elektro-L and MetOp, Iridium, Inmarsat, Orbcomm, QO-100, CubeSat telemetry, GNSS, COSPAS-SARSAT 406 MHz distress beacons, GOES / Meteosat DCS platform reports |
| **Two-way radio** | 6 | DMR, P25, NXDN, dPMR, D-STAR, M17, TETRA, TETRAPOL |
| **Cellular and rail** | 7 | LTE, 5G NR and NB-IoT cells, GSM-R, LBJ, KLW, TDCS |
| **Pagers** | 3 | POCSAG, FLEX, Multitone |
| **IoT and sensors** | 14 | ISM sensors, LoRaWAN, Meshtastic, MeshCore, Bluetooth LE, Zigbee, Wi-Fi, Wi-SUN, Wireless M-Bus, Z-Wave, DECT, ANT+ |
| **Amateur** | 30 | FT8, FT4, WSPR, JS8Call, RTTY, PSK, MT63, FSQ, IFKP, SSTV, FreeDV, APRS, LoRa APRS, MeshCom and more |
| **Weather and science** | 8 | Radiosondes, HRPT, SAME alerts, time signals, e-Czas, ionospheric sounders, radio astronomy, meteor scatter |
| **Amateur** | 29 | FT8, FT4, WSPR, JS8Call, RTTY, PSK, MT63, ARDOP, SSTV, FreeDV, APRS, LoRa APRS, MeshCom and more |
| **IoT and sensors** | 15 | ISM sensors, LoRaWAN, Meshtastic, MeshCore, Bluetooth LE, BLE tracker detector, Zigbee, Wi-Fi, Wi-SUN, Wireless M-Bus, Z-Wave, DECT, ANT+ |
| **Amateur** | 28 | FT8, FT4, WSPR, JS8Call, RTTY, PSK, MT63, SSTV, FreeDV, APRS, LoRa APRS, MeshCom and more |
| **Weather and science** | 7 | Radiosondes, SAME alerts, time signals, ionospheric sounders, radio astronomy, meteor scatter |
| **HF utility** | 4 | ALE, beacons, Datawell buoys, CODAR |
| **Tools** | 4 | General receiver, spectrum analyser, spectrum explorer, signal workbench; also a gain optimiser, a direction finder, a pass planner and a remote-control API |

**Many radios:** HackRF, RTL-SDR, Airspy, SDRplay, BladeRF, LimeSDR, USRP, Pluto, Fobos, RX888, RFNM, network radios, sound cards and Hamlib rigs. The mode counts are the rows of the mode list in the app. Details per mode are under [Modes](#modes).

## What is new in 0.2.8

<details open>
<summary><b>Show the 0.2.8 changes</b></summary>

- **22 new modes** (142 in all): the **KA family** (KAAL, KAPP, KAFT, KAVB, KAIT, KAWS, see below), eLoran / Loran-C, MPT1327, LTR and EDACS trunking control channels, Yaesu Fusion, a military HF waveform identifier (identify only), ARDOP, GOES DCS, Mode A/C, a BLE tracker detector, AM stereo (C-QUAM), DGPS beacons, SITOR, DME, FSQ and IFKP. Several are marked *in development* in the app and here: they have only been tested on generated signals.
- **Voice you install yourself:** TETRA voice (unencrypted calls only) and Yaesu Fusion voice through a user-installed codec plug-in, for example one built from [mbelib-neo](https://github.com/arancormonk/mbelib-neo). Nothing patented is bundled; see [docs/CODECS.md](docs/CODECS.md).
- **Easier to use:** a command palette (Ctrl+K), a get-me-listening wizard, four guided tours, workspaces with crash restore, a recording library with re-analyse, a Report a problem dialog with a local diagnostics bundle, a typography choice (Current, Clean, Compact) and a larger-touch-targets option.
- **Receiver and display:** the demod mode shown and settable in the FFT panel, with a name search in the mode list (#212); waterfall export as PNG frames and a timelapse; a Solar and Jupiter burst monitor; FM tone control and a warm preset; C-QUAM stereo; VHF DSC channel 70; a toolbar button that says Memory; contact lines between stations on the FT8 and JS8 maps.
- **Satellite dishes:** a dish alignment assistant (beacon level meter and look angles), LNB calibration from a beacon, a rotator north-offset calibration, a LEO Doppler monitor (detect only) and a SatNOGS transmitter browser (in development).
- **Radios:** DXpatrol SDR (RTL2832U based, HF direct sampling, #220); PlutoSDR no longer contacts network Plutos you did not add (#215); the Linux RX888 start and a failed LimeSDR probe now name the device (see the release notes).
- **Fixes:** GOES-19 HRIT differentially coded (NRZ-M) frames decode, HD Radio long station names and per-service logos, HaLow 2 MHz sub-channel scan and vendor elements (#192, #167), LTE / NR scan filters (#164), ADS-B altitude colours and display options, spectrum analyser resolution, DVB-T2 HEVC software fallback (#203), Android USB (#201), Meteor-M Doppler retune (#199), a DAB crash (#210), CDR audio recovery (#138, #121), a mesh clock that keeps running through a receiver reset (PR #217, thanks @ffishman).
- **DVB-S / S2:** carrier survey, symbol dump and DVB-RCS / DVB-CID work from the DecSAT code by Arclamp (GPL-3.0-or-later), in development.
- The full list, with issue numbers and known limits, is in the release notes.

### The KA family (own modes, in development)

KAAL, KAPP, KAFT, KAVB, KAIT and KAWS are digital modes designed from scratch by Kristijonas Adomaitis for amateur radio and built into OnAir. Every transmission carries the sender's callsign, nothing is encrypted, and they use their own unpatented codecs. **All figures below were measured in simulation (generated signals with simulated noise); no KA mode has been on the air yet.**

| Mode | Job | Width | Notes |
|---|---|---|---|
| KAAL | voice, beacon, station ID | about 10 kHz (8 ksym/s, 16-QAM) | own 12 / 16 kbit/s voice codec live; 24 / 32 kbit/s live at 16 ksym/s; the callsign is also hidden in a sweep signature |
| KAPP | short messages and small data | 100 kHz | nine profiles, BPSK to 1024-QAM, selective-repeat bursts |
| KAFT | file transfer | 200 / 500 / 1000 kHz | ten profiles, hash-checked files |
| KAVB | video | 1 to 2 MHz | physical layer and container only; no video codec yet (in development) |
| KAIT | still pictures | under 200 kHz | a picture in a 10 s burst, sent in layers |
| KAWS | HF weak-signal chat and beacons | 2.4 kHz | chirp and OFDM profiles in UTC slots |

KAAL, KAPP and KAFT can write an IQ file you can open in SDR++ or OnAir without a radio. KAPP and KAFT only listen: they send no acknowledgement.

</details>

## What is new in 0.2.7

<details>
<summary><b>Show the 0.2.7 changes</b></summary>

- **About twenty new modes** (120 in all): Wireless M-Bus, tone decoders, COSPAS-SARSAT 406 MHz, MT63, Z-Wave, meteor scatter, analog FPV video, Wi-Fi HaLow, a P25 trunking follower, e-Czas, a working LBJ decoder, and KiwiSDR as a network radio.
- **New tools:** [band survey](docs/band-survey.md), a [message database](docs/messages.md) with full-text search, [watchlists](docs/watchlists.md) with ntfy / Telegram / webhook alerts, a [time machine](docs/time-machine.md) (a rolling memory of raw IQ), a pass scheduler, a [Doppler orbit fit](docs/doppler-orbit.md), [self-calibration](docs/self-calibration.md), [station health](docs/STATION_HEALTH.md), an [interference hunter](docs/rfi-hunter.md), HF propagation and GNSS navigation views, a [hydrogen line toolkit](docs/hydrogen-line.md), [passive radar](docs/passive-radar.md), an [RF camera](docs/rf-camera.md) and a [protocol lab](docs/protocol-lab.md).
- **TV and DAB:** a [TV recorder](docs/tv-recorder.md) (programme guide grid, scheduled recording, timeshift, subtitle export) and a DAB programme guide with XMLTV export.
- **Plugin SDK** ([docs/plugins.md](docs/plugins.md)): add a mode or action as a plugin file without rebuilding. **Headless server** ([docs/headless-server.md](docs/headless-server.md)): `onair-server` runs OnAir without a window, controlled from a browser.
- **Audio and signals:** an [audio cleanup chain](docs/audio-cleanup.md), [auto gain](docs/auto-gain.md) with per-device tables, live translation to English ([docs/TRANSCRIBE.md](docs/TRANSCRIBE.md)), [live rooms](docs/live-rooms.md), auto-labelled spectrum, a calibrated signal identifier, a weather picture studio, [offline data packs](docs/PACKS.md) and [sound card input](docs/SOUND-CARD.md) for narrowband modes.
- **Interface:** a [touch interface](#features), full-screen maps, an airband airport search with an On the air tab, a redesigned Transmit panel, a new logo.
- **Android:** direct USB for RTL-SDR radios. The APK is signed with a new permanent key, so **uninstall 0.2.6 first**; later updates install normally.
- **Fixes:** DVB-T2 16K extended pilot table (the 642 MHz capture decodes), TETRA no longer calls clear cells encrypted, HFDL strong bursts, DRA+ bit rate, mode switch while running, rtl_tcp greeting wait, Airspy HF+ late callbacks, accented paths on Windows.
- **Under the hood:** the C++ protocol layers were replaced by Rust; **Rust is now required to build** (see [Build from source](#build-from-source)).
- Most new modes and tools have been tested on synthetic signals only; the full list is in the release notes.
</details>

## What is new in 0.2.6

<details>
<summary><b>Show the 0.2.6 changes</b></summary>

- **[Android app](#android-app):** the same decoder core with a phone interface (receive only).
- **Hamlib rig control and rotators:** drive CAT radios such as an Icom IC-R8600 or IC-7300 (frequency and mode follow in both directions), and aim an antenna rotator.
- **A real-time map of positions** (aircraft, ships, APRS, mesh nodes, radiosondes and more), with offline map files, a receiver-range plot on the ADS-B and AIS maps, and the public [map.on-air.dev](https://map.on-air.dev).
- **DRM data services:** slide show, broadcast website, programme guide (EPG), TPEG and Journaline. DAB and DRM share the same MOT object handling.
- **Whole-span decoding** for pagers (POCSAG, FLEX) and APRS / packet: every channel in the captured span at once.
- **TV:** teletext pages and DVB subtitles, recording with pause and live buffer, multi-channel DVB-T/T2 from one wide capture, user-set channel logos, DVB-S2X VL-SNR frames, DTMB LDPC on the GPU.
- **ADS-B:** feeds from other receivers, offline registrations and countries, range statistics, metric vertical speed.
- **FM:** a Mono switch for weak stations, a saved-station library, a scan with full RDS.
- **Scroll to tune:** the wheel (or a trackpad) steps the frequency field digit by digit.
- **Share my radio:** an rtl_tcp server for the radio OnAir is running.
- **More radios:** Fobos SDR, RX888 MkII (native USB 3 driver), RFNM, Red Pitaya, Pluto+ Ethernet, SpyServer, SDR++ server, Hermes Lite 2.
- **Transmit** of test signals into a dummy load, with the modes offered listed in [docs/TRANSMIT.md](docs/TRANSMIT.md).
- **New modes** since 0.2.5: Tone decoders (CTCSS, DCS, DTMF, selcall), Wi-SUN FAN, e-Czas (Polish time service on 225 kHz), UAT 978 MHz, VDL Mode 4, TETRAPOL, Elektro-L, LBJ (Chinese railway warnings), COSPAS-SARSAT 406 MHz beacons, Wireless M-Bus, Z-Wave, MT63, FSQ, IFKP, meteor scatter, DECT, ANT+ sensors, NB-IoT and 5G NR cells, MAC, DVB-C, MeshCom, Q65 and the other WSJT modes, and more (120 modes in all).
- **Interface:** Light and High contrast themes, an accent colour, colour maps and scroll-back for the spectrum, a General receiver with up to 4 channels and MP3 / FLAC recording, a one-button **Record audio** in the Source panel (next to Record IQ) that saves the sound you hear in any mode to WAV, FLAC or MP3 (optionally only while there is sound, one file per transmission), **Monitor audio** (Source panel): hear the tuned channel of any mode, also the decoders that have no sound of their own, as USB / LSB / narrow FM / AM, on a chosen output device, a second device (a virtual audio cable for fldigi, WSJT-X, multimon-ng) or a local TCP stream (off by default, 127.0.0.1 only unless you allow other computers; see docs/AUDIO.md), a "Save bug report" zip, translations (German, French, Spanish, Chinese, Lithuanian, Hungarian, Japanese, Polish, Dutch, Portuguese, Italian, Russian).
- **Docker image** that shows the OnAir GUI in a browser, published with each release.
- **mbelib is no longer included** (patented AMBE/IMBE vocoders): see [Good to know](#good-to-know).
</details>

## Install

Download the file for your system from the **[latest release](https://github.com/adswill/OnAir/releases/latest)**. There is nothing to compile.

| System | Download | How to install |
|---|---|---|
| **macOS** 15 or later | `OnAir-…-macos-arm64.dmg` (Apple silicon)<br>`OnAir-…-macos-x86_64.dmg` (Intel) | Open the `.dmg` and drag OnAir to Applications. The app is not signed with a developer certificate yet, so on the first launch right-click it and choose *Open*. Intel Macs need a CPU with AVX2 (2013 or newer). |
| **Windows** 10 / 11, 64-bit | `OnAir-…-windows-x64-setup.exe`<br>or `OnAir-…-windows-x64.zip` | Run the installer, or unpack the zip and start `OnAir.exe`. Windows may warn about an unknown publisher because the installer is not code-signed. Needs a CPU with AVX2 (Intel or AMD from about 2013 on). |
| **Linux** x86-64 and arm64 | `onair_…_amd64.deb` / `onair_…_arm64.deb`<br>or `onair-…-portable.tar.gz` | Debian / Ubuntu: `sudo apt install ./onair_<version>_<arch>.deb`, then start OnAir from the menu or with `onair`. The `.deb` carries its own video decoder, so it does not depend on your system's FFmpeg. Other distributions: unpack the portable archive and run `bin/onair`. |
| **Android** 9+, arm64 | `OnAir-…-android.apk` | Sideload the APK, see [Android app](#android-app). |
| **Docker** (a server without a screen, used from a browser) | `packaging/docker` in this repository | `docker compose up -d --build` in that folder, then open `https://<server>:3001`. See [packaging/docker/README.md](packaging/docker/README.md) for radios, graphics chips and settings. |

> [!NOTE]
> **SDRplay RSPs (RSP1, RSP1A, RSP1B, RSP2, RSPduo, RSPdx):** install the free **[SDRplay Hardware API](https://sdrplay.com/hardware-api/)** once, separately from SDRconnect, and restart OnAir. OnAir loads SDRplay's own library and service; it does not bundle them. Make sure the "SDRplay API Service" is running (the installer starts it). For the RSPduo, select the input with the antenna: **Tuner 1**, **Tuner 2**, or **Tuner 1 Hi-Z**. OnAir uses one tuner at a time, at 62.5 kS/s to 10 MS/s; see [tested features and limits](DEVICES.md#sdrplay-rspduo). An RSP-compatible radio not made by SDRplay (for example a Mirics MSi2500 "RSP1A clone") is not seen by the SDRplay API; use SoapySDR's `mirisdr` module instead.

> [!IMPORTANT]
> **Windows USB drivers:** for a HackRF, RTL-SDR, Airspy, Airspy HF+, Fobos SDR or RX888 MkII (its boot loader 04b4:00f3 and its running firmware 04b4:00f1 both need it), install the WinUSB driver once with [Zadig](https://zadig.akeo.ie). A LimeSDR Mini needs FTDI's FT60x driver (see the README inside the download). A LimeSDR-USB (Cypress FX3) needs the Cypress/LimeSuite USB driver (CyUSB3, the one LimeGUI installs), not WinUSB: do not replace it with Zadig. OnAir's own LimeSuite has no FX3 support, so for a LimeSDR-USB also install [PothosSDR](https://downloads.myriadrf.org/builds/PothosSDR/) or LimeSuite; OnAir finds its LimeSuite.dll by itself and the log (onair.log) says which copy saw the radio ("LimeSuite ...: N radio(s)").

### Android app

<details>
<summary><b>The Android app: sources, limits, how to install</b></summary>

 is the desktop decoder core (the same C++ code, built as `libonair.so`) with a phone interface in Kotlin Compose: spectrum and waterfall, the mode list with search and groups, a results tab with the decoded data of the selected mode, and a log.

- **Receive only.** The app never transmits. Its only permission is INTERNET (for rtl_tcp and SpyServer, and for map tiles).
- **Sources:** an RTL-SDR on the USB port (0.2.7), `rtl_tcp` (an RTL-SDR served over the network, or on the phone itself), SpyServer, an IQ file (cs8, cu8, cf32, ci16, cu16, with its sample rate) and a built-in test signal for the selected mode that needs no radio.
- **Results:** per-mode results (cards and tables), an OpenStreetMap map (tiles are only fetched after you agree), and FM with RDS.
- **Use it:** pick a source on the Receiver tab, a mode on the Mode tab, press play. Tap the spectrum to tune, drag to move, pinch to zoom (double tap resets).
- **Install:** download `OnAir-…-android.apk` from the [release](https://github.com/adswill/OnAir/releases/latest), copy it to the phone, open it and allow installs from unknown sources for the app you opened it with. The APK is signed with the project's own key, not a Play Store key; updating later needs the same key.
- **Limits:** arm64-v8a only, Android 9 (API 28) or newer. Direct USB works for RTL-SDR radios (new in 0.2.7); HackRF and Airspy USB are not supported yet, so use rtl_tcp or SpyServer for those. No vocoder voice (DMR, P25, NXDN, D-STAR, TETRA), no TV video or sound, and no images yet (the satellite pictures and the SSTV / fax pictures). Wide modes (DVB, ATSC) need a fast phone.
</details>

### Development version ("nightly")

Fixes and new features land on the `main` branch first, before they go into a release. To try them early, [build OnAir from source](#build-from-source) from the latest `main`.

> [!WARNING]
> The development version is **not tested like a release**. It can contain bugs, unfinished features or changes that stop parts of the app (or the whole app) from working. Use the [latest release](https://github.com/adswill/OnAir/releases/latest) for everyday use, and if something breaks in a development build, please say so on [Discord](https://discord.gg/Kky9c6atm) or in [GitHub Issues](https://github.com/adswill/OnAir/issues), mentioning the commit you built.

## Quick start

1. Connect your radio and antenna, then start OnAir. Connected radios appear in the source menu.
2. Pick what you want to receive in the mode list.
3. Enter a frequency, or open the **Scan** tab to find channels automatically.
4. Press **Start**. When the receiver locks, the channels appear on the right: click one to watch or listen.
5. If the picture breaks up, use **Auto-tune** for the gain, or **Optimise gain** (it sweeps the radio's whole gain range, measures the mode's SNR, or else the signal-to-noise of the spectrum around the channel, and keeps the lowest gain within 0.5 dB of the best that does not overload the ADC; **Undo gain** goes back), and watch the quality score while you move the antenna. Height and a clear view toward the transmitter matter more than anything else.

No radio? Open an IQ recording through the *IQ recording file* source (cs8, cu8, cf32, ci16, cu16, SigMF and two-channel WAV IQ files: the sample rate, centre frequency and format are read from SigMF and WAV files), or press **Tour**. Real recordings are in the [signal sample library](https://files.on-air.dev).

> [!TIP]
> Height and a clear view toward the transmitter matter more than anything else for TV. Watch the quality score while you move the antenna.

## Modes

142 modes, grouped as in the app (the list in the app also has a Plugins entry for managing add-ons). Every receiver is built from the published specifications and tested on generated signals (carrier offsets, noise, echoes, clock errors) and, where recordings exist, on real captures. Reports and recordings from your own antenna help most: send them on [Discord](https://discord.gg/Kky9c6atm) or upload them to the [signal sample library](https://files.on-air.dev). Where a mode has a specific caveat it is said below. **Receive only** means nothing is transmitted, decrypted or joined.

<details>
<summary><b>All 120 modes, by family</b></summary>
119 modes, grouped as in the app. Every receiver is built from the published specifications and tested on generated signals (carrier offsets, noise, echoes, clock errors) and, where recordings exist, on real captures. Reports and recordings from your own antenna help most: send them on [Discord](https://discord.gg/Kky9c6atm) or upload them to the [signal sample library](https://files.on-air.dev). Where a mode has a specific caveat it is said below. **Receive only** means nothing is transmitted, decrypted or joined.

<details>
<summary><b>All 119 modes, by family</b></summary>
<summary><b>All 142 modes, by family</b></summary>

### TV

- **DVB-T2 / DVB-T** (detected automatically; T2-Lite included): Europe, Middle East, Africa, Asia, Australia.
- **ATSC 1.0** (North America, South Korea) and **ATSC 3.0** NextGen TV (ROUTE, HEVC video with AAC audio, optional AC-4 audio through libavcodec when installed; needs 6.5 Msps or more; MMTP services are not supported).
- **ISDB-T** (Japan, Brazil and most of South America, Philippines; 13 segments, up to three layers, one-segment mobile TV).
- **DVB-S / S2 / S2X** satellite TV through an LNB, all MODCODs, VL-SNR frames, polarisation warning.
- **TV sat beacon decoder** (the narrow carrier of a geostationary beacon, found anywhere in the capture and tracked: level, C/N0, offset, drift; the reference carrier; phase-modulated telemetry recovered as bits and frames with the frame length learned from the repetition; CSV logging; a decoded view of every word of the frames with its confidence, the satellite estimated from the frequency, CSV and JSON export; [details](docs/satbeacon.md)).
- **DTMB** (China, Hong Kong, Macau 8 MHz, Cuba 6 MHz).
- **DVB-C** cable (EN 300 429 / J.83 annex A and C, 16 to 256-QAM, symbol rate and QAM found automatically; needs a cable feed).
- **Analog TV:** PAL, SECAM, NTSC; systems B/G, I, D/K, M, N, H, K', 405-line A, C, E (819 lines), L, L'; NBTV and mechanical TV. Picture and sound.
- **FPV video (analog):** drone video links on the 5.8 GHz bands A, B, E, F, R (Raceband), L and the 1.2 GHz plan: every channel with its level, a scan that tunes to the strongest transmitter, the live picture (PAL or NTSC, frequency modulated) with its 6.0 / 6.5 MHz sound, snapshots and recording; the on-screen display is part of the picture, not decoded; needs 20 Msps ([details](docs/fpv.md)).
- **MAC:** D2-MAC and D-MAC satellite TV (FM, 27 MHz; 20 Msps sees most of it); picture, channel name, digital sound; EuroCrypt is detected, not descrambled.

### Broadcast radio

- **DAB / DAB+** with a transmitter map from the TII, Journaline and MOT slide show.
- **FM** (87.5 - 108 MHz): stereo and RDS (station name, radio text, programme type, traffic flags), band scan, saved stations, Mono switch.
- **AM stereo (C-QUAM)** on medium wave: the 25 Hz pilot found, left and right from the phase of the carrier, a stereo / mono indicator; plain AM stations play as mono; the older Kahn-Hazeltine, Harris and Magnavox systems are not decoded ([details](docs/cquam.md)).
- **DRM** (DRM30 and DRM+) on long, medium and short wave: audio, station information and the data services (slide show, website, EPG, TPEG, Journaline).
- **AM broadcast** (medium wave 531 - 1602 kHz and the shortwave broadcast bands, in the general receiver with MW and SW presets).
- **HD Radio** (NRSC-5, FM and AM) and **CDR** (China Digital Radio). HDC and DRA audio are patented or of unclear status: see [Good to know](#good-to-know). HD Radio and CDR sound play only with a codec add-on you build yourself ([docs/CODECS.md](docs/CODECS.md)); the stations, services and data work without it.

### Aviation

- **ADS-B** (1090 MHz): aircraft on a map with altitude, speed and callsign; aviation or metric units; feeds from other receivers; offline registrations and countries; range statistics.
- **Mode A/C** (1090 MHz secondary-radar replies): squawk and altitude of aircraft that answer radar, the ident pulse, emergency squawks, matched to the Mode S aircraft of the same capture on the ADS-B map; [details](docs/modeac.md).
- **UAT** (978 MHz, DO-282): ADS-B reports, FIS-B weather products, TIS-B traffic relay. **Mode S uplink** (1030 MHz, needs 8 Msps): interrogator codes, TCAS coordination.
- **ACARS**, **VDL Mode 2** (every channel at once), **VDL Mode 4** (STDMA; spec-based, no live captures validated yet) and **HFDL** (every channel in the band at once), with positions on a map.
- **Airband** (AM voice, several 25 and 8.33 kHz channels at once, scan, priority), **VOR**, **ILS** and **NDB**.
- **FLARM** (868.2 / 868.4 MHz): the old protocol is decoded; the newer one is only counted. Aircraft with the no-track or stealth flag are never shown, logged or exported.
- **DJI drones** (2.4 and 5.8 GHz, 16 Msps or more): OcuSync-like bursts are classified by shape (a guess, never an identification); DroneID is decoded. **Drone Remote ID** (ASTM F3411, BLE and Wi-Fi). Receive only.

### Maritime

- **AIS** ship positions, **VDES** (ASM decoded; VDE-TER only detected), **NAVTEX**, **DSC** and **Weather fax**.

### Satellite and navigation

- **GNSS** (GPS L1; satellites, position, time and a computer-clock check).
- **Weather pictures:** **Meteor-M LRPT** (137.1 / 137.9 MHz, Doppler tracking, channels and colour composite), **GOES HRIT** (1694.1 MHz), **Elektro-L** HRIT / LRIT and GGAK, **HRPT / AHRPT** (MetOp, FY-3, retired NOAA). The L-band ones need a dish with an LNA at the feed.
- **Inmarsat-C**, **Inmarsat Aero**, **Iridium** (bursts can be saved for iridium-toolkit), **Orbcomm**, **Globalstar** (activity monitor only; L-band 1610-1626.5 MHz terminal uplink, spread and encrypted; the S-band satellite downlink is not tuned), **QO-100** (beacon lock, narrowband and wideband transponders, DATV hand-over to DVB-S/S2) and **CubeSat telemetry** (AX.25 1200 / 9600 baud, AX100 modes, CCSDS Reed-Solomon, SatNOGS-style KISS out).

### Two-way radio

- **Tone decoders** (CTCSS, DCS, DTMF, five-tone, SELCAL and two-tone pages; [details](docs/tones.md)).
- **DMR**, **NXDN** (NXDN48 / 96), **P25** (Phase 1, conventional and trunked), **dPMR / PMR446** (all 16 channels scanned), **D-STAR** (also in the amateur group) and **M17**: call logs with IDs and talkgroups, data and short messages. P25 encrypted calls are only reported as encrypted. Voice for the AMBE / IMBE modes needs mbelib you install yourself, see [Good to know](#good-to-know).
- **TETRA** (base station broadcast: network and colour code, location area, carriers, services, neighbours, whether encryption is used; scan over a span) and **TETRAPOL** (D_SYSTEM_INFO). Receive only, no voice, nothing decrypted.

### Cellular and rail

- **Cell towers** (LTE MIB / SIB1 - SIB5; towers on a map with an [OpenCelliD](#tools-and-notes) key), **5G NR cells** (SS/PBCH, PCI, RSRP, MIB; SIB1 is not decoded yet), **NB-IoT cells**, **GSM-R** (921 - 925 MHz) and **LBJ** (Chinese railway train warnings at 821.2375 MHz: train number, speed, kilometre post, direction; see [docs/lbj.md](docs/lbj.md)). Public broadcast information only.
- **KLW** end-of-train data (default 866.2375 MHz) and **TDCS** train dispatch data (default 457.550 MHz): 1200 bit/s FFSK with BCH and CRC checks, locomotive and tail IDs, pressure / commands or train number, speed, mileage and consist fields. LBJ also decodes the fixed-position extension, WGS-84 coordinates and time broadcasts. [Details and test coverage](docs/railway.md).

### Pagers

- **POCSAG** (512 - 2400 bd) and **FLEX** (1600 - 6400), whole span at once.
- **Multitone Paging**: a detector only (the format is not public); no message text is shown. Pager messages are private; receive only.

### IoT and sensors

- **ISM sensors** (433.92 / 868 / 915 MHz: weather stations, tyre pressure, meters, remotes, Z-Wave; S0 and S2 stay encrypted).
- **Wireless M-Bus** (EN 13757-4 utility meters: S1, T1, C1 and R2 on 868 MHz, N1 on 169 MHz; manufacturer, ID, device type and the readings of unencrypted frames, encrypted payloads stay encrypted; [details](docs/wmbus.md)).
- **LoRaWAN** (EU868 / US915, all spreading factors; clear headers only, payloads stay encrypted), **Meshtastic** and **MeshCore** (nodes, map, chat, telemetry, exports), **LoRa (raw)** (plain point-to-point LoRa; hex and text frames, no decryption).
- **Bluetooth LE** (advertising, never connects), **Zigbee / 802.15.4** (needs a fast radio), **Wi-Fi networks** (2.4 GHz beacons, passive), **Wi-Fi HaLow** (802.11ah sub-GHz beacons), **Z-Wave** (G.9959 R1 / R2 / R3, encrypted payloads stay encrypted), **Wi-SUN FAN** (smart meter and street light mesh, all regional band plans), **ANT+ sensors** and **DECT cordless phones** (base stations from beacons; calls are encrypted, no voice; Europe 1880-1900 MHz, US/Canada DECT 6.0 1920-1930 MHz, Brazil, Latin America and the US 900 MHz ISM band, see docs/DECT.md).

### Weather and science

- **Radiosondes** (400 - 406 MHz and LMS6-1680; tracks and landing prediction), **NOAA Weather Radio SAME** (EAS alert decoder).
- **Time signals** (DCF77, MSF, WWVB, JJY, WWV / WWVH, BPM, BPC and more; the computer clock can be synced), **e-Czas** (Poland's official time on 225 kHz long wave, from a long-wave capable radio or an upconverter; [how to receive it](docs/eczas.md)) and **Ionospheric sounders** (ionograms from chirp sounders).
- **Radio astronomy** (21 cm hydrogen line and OH lines).
- **GOES / Meteosat DCS** (river gauges, weather stations, tide gauges and buoys reporting on the 401.7 - 402.1 MHz uplink: every carrier in the capture followed, 100 / 300 / 1200 bps with NRZ or Manchester coding, the BCH-checked platform address, the channel and the ASCII or pseudo-binary sensor data, CSV logging; [details](docs/goes-dcs.md)).
- **Meteor scatter** (pings of meteor trails reflecting a distant beacon such as GRAVES on 143.050 MHz: start, duration, peak level, Doppler, rates per hour and day with the showers marked, CSV logging; [details](docs/mscatter.md)).
- **BLE tracker detector** (Apple Find My accessories such as AirTag, Tile, Chipolo, Samsung SmartTag and Google Find My Device tags recognised from their advertisements; how long and how close each stays, and an alert through the watchlists for one that follows you; receive only, identifiers kept for a short window and never saved; [details](docs/bletrk.md)).

### Amateur: digital

- **FSQ** and **IFKP** keyboard chat (33 tones).
- **ARDOP** and **SITOR** are listed under HF utility.
- **WSJT family:** FT8, FT4, FT2, WSPR (with an optional spot upload to wsprnet.org), JT65, JT9, Q65, FST4 / FST4W, MSK144, and **JS8Call**.
- **MT63** (500 / 1000 / 2000 Hz, short and long interleave found automatically, 64 carriers with Walsh-Hadamard coded characters; [details](docs/mt63.md)).
- **ARDOP** (the Winlink HF modem, monitor only: frame types, connect requests, IDs and pings with call signs, ACK / NAK quality and the text of 4FSK data frames; PSK and QAM frames are listed by header; [details](docs/ardop.md)).
- **Keyboard modes:** PSK31 / 63, RTTY, CW (every signal in a 14 kHz window), Olivia, Contestia, MFSK, THOR, DominoEX, Hellschreiber. Picture modes: **SSTV** and **HamDRM** (EasyPal).

### Amateur: voice and packet

- **Yaesu Fusion** (C4FM, in development): callsigns, call mode and slow data; voice only through a codec plug-in you install.
- **FreeDV** (codec2 included), **M17**, **D-STAR**, **APRS (AFSK)** / AX.25 packet (whole span at once), **LoRa APRS** and **MeshCom**.

### HF utility

- **HF waveform ID** (identify only): estimates the baud rate, tone spacing and repetition of a military or government HF signal and ranks candidate waveforms; nothing is decoded.
- **SITOR** (FEC-A and FEC-B, in development) and **ARDOP** (Winlink HF frames, monitor only).
- **HF ALE** (2G calls, soundings, LQA), **HF beacons** (NCDXF / IARU propagation monitor), **Datawell Buoy HF Link** (wave-buoy telemetry, HXV format decoded; the 4FSK HVA format is not public) and **CODAR / HF ocean radar** (sweeps measured; propagation only, no positions).

### Kris custom modes (KA family, in development)

- **KAAL** voice and beacon (10 kHz, receive and transmit), **KAPP** packets (100 kHz), **KAFT** file transfer (200 kHz to 1 MHz), **KAVB** video (1 to 2 MHz, spec and physical layer only), **KAIT** pictures in 10 s bursts and **KAWS** HF weak-signal chat. Measured in simulation only, never on the air.

### More 0.2.8 modes

- **eLoran / Loran-C** (100 kHz, in development), **MPT1327**, **LTR** and **EDACS** trunking control channels (in development; control data only, no voice), **GOES DCS** (data collection platforms, 401.7 - 402.1 MHz), **Mode A/C** (squawk and altitude), **BLE tracker detector** (AirTag, Tile and others; warns about one that follows you), **AM stereo (C-QUAM)**, **DGPS beacon** and **DME** (in development).

### General and tools

- **General receiver:** NFM, WFM, AM, USB, LSB and CW anywhere the radio tunes, up to 4 channels from one capture, filters, squelch, recording to WAV (one file per transmission), FLAC and MP3.
- **Spectrum analyser** (1 - 6000 MHz sweeps; CSV export), **Spectrum explorer** (markers, measurements, bookmarks) and **Signal workbench** (analyse and demodulate a recording from a file, no radio needed).
</details>

## Tools and notes

<details>
<summary><b>Signal workbench</b></summary>

Takes a recording apart without a radio, in the spirit of Inspectrum, SigDigger and Universal Radio Hacker.

1. **Open** a file (or drop it on the window). Audio files (WAV, MP3, FLAC, OGG, M4A) are treated as a real signal; a stereo file can be read as I/Q ("stereo is I/Q"). IQ files (.cs8 .cu8 .cf32 .ci16 .cu16) are read from disk as they are, so 100 MB and larger files open at once; they need the sample rate (a name like `capture_2Msps.cs8` gives it). SigMF and IQ WAV files describe themselves. "Demo signal" makes a recording to try the tools on.
2. **Look** at the waveform and a large spectrogram. Wheel zooms time, Ctrl + wheel zooms frequency, right-drag pans, a click puts the cursor, a drag selects a time range and a band.
3. **Measure:** peaks, bandwidth (-3 dB, -20 dB, 99 %), carrier offset, SNR, symbol rate, modulation guess, power over time and bursts.
4. **Demodulate:** AM, FM, SSB, CW, ASK/OOK, 2-FSK / 4-FSK, AFSK, BPSK / QPSK / 8PSK, MSK / GMSK, with eye diagram, constellation and the recovered bits; Listen plays the audio.
5. **Read the bits:** invert, differential, NRZI, Manchester, byte reversal and alignment, hex / binary / ASCII, pattern search.
6. **Export** the selection as .wav, .cf32 or SigMF, the audio as .wav, the bits as .txt or .bin, the peaks as CSV.

`wbtool` (built with the tests) does the same on the command line: `wbtool gen out.cf32 --kind qpsk --rate 250000 --baud 12500`, `wbtool analyze file --rate R`, `wbtool demod file --kind fsk2 --baud 9600 --band f0 f1`.
</details>

<details>
<summary><b>Cell tower map (OpenCelliD key)</b></summary>

Cell towers broadcast who they are but not where they stand. The Cells mode can look each decoded LTE cell up in [OpenCelliD](https://opencellid.org), an open, crowd-sourced tower database, and put it on the map:

1. Sign up (free) at https://opencellid.org and confirm your e-mail.
2. Log in and copy your API token from your account page.
3. In OnAir, open Cells → Map and paste it into **OpenCelliD API key**.

Lookups need internet and run at most once a second; only the cell identity (MCC, MNC, TAC, cell id) and your key are sent. Answers are cached on disk, so the map also works offline afterwards. A CSV downloaded from OpenCelliD works too (Load tower file). Tower data © OpenCelliD, CC BY-SA 4.0.
</details>

<details>
<summary><b>Iridium bursts for iridium-toolkit</b></summary>

In the Iridium screen, **Save for iridium-toolkit** writes every demodulated burst as the `RAW:` lines of gr-iridium's `iridium-extractor` (by default `output.bits` in the recordings folder). From the command line: `onair-cli --standard iridium --iridium-bits output.bits --secs 600`. Then run [muccc/iridium-toolkit](https://github.com/muccc/iridium-toolkit):

```
iridium-parser.py output.bits > output.parsed
```

OnAir writes the bits only and never decodes voice (the AMBE codec is patented).
</details>

<details>
<summary><b>TV standards: what has been checked</b></summary>

- **T2-Lite** (1/3 and 2/5 code rates, short FEC frames, 1.7 MHz channels) is decoded like any other DVB-T2 signal and checked on simulated signals; not tried on a real T2-Lite broadcast. A T2-Lite signal sharing a channel with a normal T2 one (in its future-extension frames) is not decoded.
- **ATSC 3.0** is built from the specifications and works on simulated signals (synchronisation, error correction, link layer, ROUTE, HEVC with AAC); details may need fixing against a real broadcast. If you have a recording, please tell us on Discord.
- **ISDB-T** follows ARIB STD-B31 (all modes and guard intervals, up to three layers, TMCC, interleavers, Viterbi, Reed-Solomon) and decodes simulated signals exactly; not checked against a real broadcast. Channel names in the Japanese character set are not converted yet. To try it without a signal, `isdbtgen --out sample.cs8` (built with the tools) makes a recording: open it as an IQ recording, 10 Msps, CS8, with ISDB-T selected.
</details>

**Channel scanner and shared channel list.** The TV channel scanner covers DVB, ATSC, ATSC 3.0, ISDB-T and DTMB. Scan results can be shared through a public, crowd-sourced channel list, picked by country and city; browse it on GitHub: **[adswill/OnAir-channels](https://github.com/adswill/OnAir-channels)**.

**Receive only, other people's signals.** The TETRA, LoRaWAN, LoRa, cell, Bluetooth, Wi-Fi, Zigbee and pager modes read only what is sent in the clear: nothing is decrypted, joined or disturbed. You are responsible for following your country's rules on receiving radio signals.

## Features

**Watch and listen**
- **Live player** with picture, sound, subtitles, teletext, programme guide and multiple audio tracks; record a service to a file, pause and live buffer.
- **Automatic repair** of short signal dropouts, so weak reception stays watchable.

**See your signal**
- Spectrum and waterfall (colour maps, pause and scroll-back, averaging, max hold, peak markers), every constellation, signal-to-noise and error figures.
- A single **quality score**, and an **echo (multipath) detector**.
- **Neural noise remover** for speech in the General receiver (NFM, WFM, AM) and Airband: RNNoise ([BSD-3-Clause](third_party/rnnoise/COPYING)) in the Listen chain, off by default, with a strength slider. See [docs/noise-remover.md](docs/noise-remover.md).
- **DAB transmitter map:** OnAir reads the transmitter identification (TII) that DAB networks send and lists the transmitters heard.

**Tablets and touch screens**
- **Touch interface** (View side panel > Touch interface, or `--touch`): 48 px targets, a bottom dock with Start / Stop, tuning, gain, volume and tuner, a numeric keypad for the frequency (MHz, kHz, GHz), a full-screen mode picker in the mode list's colours, bottom-sheet dialogs, drag and fling scrolling, swipe between tabs, long press for the context menu, portrait and landscape.

**Get the best reception**
- **Memories:** keep the frequencies you use with their mode, sample rate, gains and the mode's options (`Ctrl+M`, or the **M** menu): search, favourites, groups, one-click recall on any receiver, import and export as CSV / JSON (also plain `frequency,name` lists and SDR++ bookmarks), and a scan of the memories you flag.
- A **channel scanner**, **automatic gain tuning**, a **gain optimiser**, and a **direction finder** that helps you aim the antenna; a **pass planner** for satellites.
- **Your radio, set up properly:** every antenna input is its own entry in the source list, with per-radio settings.
- **Up-converters and LNBs:** Ham It Up, Ham It Down (1500 MHz), SpyVerter, QO-100 / Bullseye TCXO / Ku LNB or a custom LO.
- **DC spike handling:** leave the radio's centre spike alone, remove it, or keep it off the channel by tuning.

**Share it**
- **[map.on-air.dev](https://map.on-air.dev)** is the public live map of aircraft, ships, APRS, mesh nodes and more (asked for at start, off until you agree).
- A built-in **network tuner** streams your channels to VLC, phones, Plex or Jellyfin over your home network, and **Share my radio** serves your radio as rtl_tcp.
- On a Mac, **cast** the playing service to an Apple TV or AirPlay TV. **Record** to a file, or send the stream out over UDP.

**Comfortable to use**
- **Scroll to tune** and the **mouse wheel on any slider** (volume, gain, squelch, bandwidth).
- **Logbook export:** "Save results..." writes CSV or JSON, an ADIF logbook and more.
- **Light**, **High contrast** and dark themes with your own accent colour; the layout adapts to small windows and display scaling.
- **Fast:** error correction runs on the GPU where available, and with optimised vector code on the CPU everywhere.

## Supported radios

OnAir supports the **HackRF One and HackRF Pro** natively, and drives these radios directly too:

| Radio | Wide TV channels (6 – 8 MHz) | DAB, FM and narrow channels | Notes |
|---|:---:|:---:|---|
| **HackRF One / Pro** | ✅ | ✅ | The radio OnAir is developed and tested with |
| **BladeRF**, **LimeSDR** (USB, **Mini**, Mini 2) | ✅ | ✅ | LimeSDR-USB: external reference clock (10-40 MHz on CLK IN) in the radio settings; any SoapySDR radio that lists clock or time sources gets the same choice. LimeSDR Mini: 10 MHz - 3.5 GHz, up to 30.72 Msps (USB 3 above about 15), listed by name; Windows needs FTDI's FT60x driver, and `LimeUtil --update` if the gateware is old. See [DEVICES.md](DEVICES.md) |
| **USRP** | ✅ | ✅ | Reference clock (internal, external 10 MHz, GPSDO) and time source (PPS) are picked in the radio settings. On Windows, install UHD from [Ettus](https://files.ettus.com) (with its firmware images) |
| **PlutoSDR** | ⚠️ | ✅ | Over its USB cable it streams about 4 Msps, too little for a 6 – 8 MHz channel. Library not included on macOS yet: install libiio 0.26 (Analog Devices' macOS package, or Homebrew) |
| **Airspy R2**, **SDRplay RSP** | Should work | ✅ | SDRplay: install the [Hardware API](https://sdrplay.com/hardware-api/) first (RSP1, RSP1A, RSP1B, RSP2, RSPdx, RSPdx-R2) |
| **SDRplay RSPduo** | Streaming tested at 10 MS/s; TV decoding untested | FM and RDS tested on Windows | Select Tuner 1 SMA, Tuner 2 SMA, or Tuner 1 Hi-Z; one input at a time. [Setup and validation](DEVICES.md#sdrplay-rspduo) |
| **RTL-SDR** | ❌ | ✅ | An RTL-SDR Blog V4 works for HF on macOS too |
| **Airspy HF+** | ❌ | Narrow channels only | Specified for HF and 60 – 260 MHz |
| **Fobos SDR** (agile firmware) | ✅ | ✅ | Contributed, untested here. Specified for 50 MHz – 6 GHz on the RF input, HF on the HF1 / HF2 inputs. Library not included: put RigExpert's `fobos_sdr` library next to OnAir, see [DEVICES.md](DEVICES.md) |
| **RX888 MkII** | ✅ | ✅ | Native SDDC/USB driver (FX3 firmware, 16-bit ADC), still being hardened on real hardware. USB 3 required. Packages include `SDDC_FX3.img` (RAM load only); see [DEVICES.md](DEVICES.md) |
| **RFNM** | ✅ | ✅ | Contributed. Only in builds made with [librfnm](https://github.com/rfnm/librfnm) installed (not the downloads yet), see [DEVICES.md](DEVICES.md). One entry per receive channel of the daughterboards |
| **Anything else** | depends | depends | Through [SoapySDR](https://github.com/pothosware/SoapySDR), once its SoapySDR module is installed |

- The libraries these radios need are included in the downloads, except as noted above.
- A TV channel needs a radio that samples fast enough: roughly 1 million samples per second per MHz of channel width. If a radio cannot reach the rate or frequency a mode needs, OnAir says so next to the source.
- **Any frequency.** OnAir never limits the frequency you enter, in any mode and on any radio. The ranges listed for a radio are what it is *specified* for: outside them OnAir still asks the driver and shows a note. An RTL-SDR picks the way down below 24 MHz by itself (an RTL-SDR Blog V4 upconverts HF in its library, any other dongle samples its Q input directly). Only what a driver itself refuses stays an error.
- The radios other than the HackRF have had less real-world testing. Notes for specific radios are in **[DEVICES.md](DEVICES.md)**.

<details>
<summary><b>Sound card and Hamlib rig sources</b></summary>

- **Sound card** (the radio list: **Sound card: …**, including Default input): WebSDR, a virtual audio cable, or a handheld radio's speaker/line into the computer, for HF decoders (FT8, RTTY, CW, SSB) at the capture rate (usually 48 kHz). Four mappings: I/Q stereo, USB audio, LSB audio, mono real. The frequency field is the *dial* the decoder should assume; nothing is tuned. Unverified against a real WebSDR.
- **Hamlib rig** (source list: **Add Hamlib rig…**): a CAT-controlled radio such as an Icom IC-R8600 or IC-7300. Search the model by name or type its Hamlib number, pick the serial port OnAir finds (or `host:port` for a `rigctld` on the network), and choose a sound card for the audio or IQ. The frequency and mode follow in both directions. If the rig is unplugged or `rigctld` stops, OnAir says so and reconnects by itself. Receiving never keys the transmitter. **Rotators** are driven the same way.
</details>

### Network radios

Add them from the source list: **Add network radio…**, choose the type, type its address and press **Connect**. OnAir tries for a few seconds and says why when it gets no answer; added radios are remembered.

<details>
<summary><b>Network radio types, ports and notes</b></summary>

| Type | Address | Port |
|---|---|---|
| **PlutoSDR** by IP (USB network or LAN, with a Link setting Auto / USB / Ethernet on a Pluto+; needs libiio 0.23 – 0.26 installed) | `192.168.2.1`, or its address on your network | libiio's default |
| **rtl_tcp** (an RTL-SDR shared by `rtl_tcp -a 0.0.0.0`) | the computer running `rtl_tcp` | 1234 |
| **SoapyRemote** (any SoapySDR radio shared by `SoapySDRServer --bind`) | the computer running the server | 55132 |
| **SpyServer** (Airspy's server, as SDR# and SDR++ use it) | the computer running SpyServer | 5555 |
| **SDR++ server** (SDR++ run as `sdrpp --server`, any source SDR++ has) | the computer running it | 5259 |
| **KiwiSDR** (HF receiver on the network, 12 kHz IQ; experimental, see [docs/kiwisdr.md](docs/kiwisdr.md)) | the receiver (or **Find KiwiSDR** for the public list) | 8073 |
| **SDRplay nRSP-ST** (through SDRconnect's WebSocket API; experimental, see [docs/nrsp-st.md](docs/nrsp-st.md)) | the computer running SDRconnect, with its WebSocket server on | 5454 |
| **Hermes Lite 2** | found by itself on your networks (or type its address) | 1024 |
| **Red Pitaya** (SDR transceiver app, HPSDR protocol 1) | found by itself on your networks (or type its address) | 1024 |

- rtl_tcp carries 8-bit samples at up to about 2.5 Msps (more with the Auto server radio setting). SoapyRemote needs SoapySDR with the SoapyRemote module on both computers; if it picks the wrong radio or lists none, type the radio's driver (for example `mirisdr`) in "Remote driver". Do not confuse SpyServer's port 5555 with SoapyRemote's 55132.
- **SpyServer** is spoken from the public protocol description (2.0.1700): the full IQ stream (8-bit or 16-bit), sample rates from the server's decimation stages, frequency and gain control. Tell us if your server misbehaves.
- **SDR++ server**: OnAir sets the sample rate and the gain through the server's own source menu, so it works with whatever radio SDR++ drives there, and that source's other controls (gain mode, bias tee, AGC, antenna, the server's source) show up under **Radio settings** as "SDR++: ...". Those are changes to the server: SDR++ keeps them after OnAir disconnects. The server takes one client at a time. See [DEVICES.md](DEVICES.md).
- **Hermes Lite 2** receives up to 38.4 MHz at 48 to 384 ksps, so only HF and narrow modes fit; the N2ADR filter board's band filters follow the frequency. **Red Pitaya** runs Pavel Demin's HPSDR-compatible SDR transceiver app: 48 to 384 ksps, tuning up to 61.44 MHz, no gain control; tested only against a fake server, so sample rate accuracy, level scaling and mirroring (setting "Swap I and Q") are unverified.
- **Several receivers (slices) of one Hermes Lite 2 or Red Pitaya**: the radio list has one entry per receiver ("Hermes Lite 2 RX1 ...", "Hermes Lite 2 RX2 ..."; as many as the HL2's gateware says it has, three on a Red Pitaya), so each OnAir receiver tab can take its own, on its own frequency and mode. They share the radio's one stream, so they share its sample rate (the first one opened sets it), its gain and the filter board (the filter for the highest frequency in use). Opening or closing a receiver above the others restarts the stream for a fraction of a second. One program at a time can stream from the radio.
- When a network radio drops, OnAir reconnects by itself, and the status bar shows the samples lost. Nothing is opened to the network by default.
- **Share my radio** (View > Data feeds): OnAir becomes an rtl_tcp server for the radio it is running, so a second OnAir, SDR#, SDR++ or any rtl_tcp client can listen to the same signal (8-bit IQ at the radio's rate). Only this computer can connect until you tick **Let other computers connect (LAN)**; clients can change the frequency and gain only when **Allow remote control** is ticked.
- **Data feeds** (View > Data feeds), all off until you turn one on: ADS-B SBS-1 and Beast (TCP), AIS NMEA (UDP), APRS-IS and KISS (TCP), ACARS and VDL2 JSON (UDP), GNSS NMEA RMC/GGA (UDP), Bluetooth LE JSON (UDP), LoRa / LoRaWAN / Meshtastic JSON (UDP). The TCP servers stay on this computer unless **Let other computers connect (LAN)** is ticked.
</details>

## Transmit

OnAir is a receiver first, but it can also send the signal of a mode through a radio that can transmit (a HackRF, for example) into a dummy load or a shielded setup, to test receivers.

- **Needs a radio that can transmit.** A receive-only radio (RTL-SDR, Airspy and so on) cannot be used.
- **A one-time notice** appears the first time you start a transmission and has to be accepted with "I understand".
- **No frequency or power limits are applied by OnAir.** The frequency and the gain you enter go to the radio as typed. You are responsible for staying within the law where you are: transmitting needs a licence on most frequencies, and a radio without a filter or an antenna can disturb others.
- **A guided panel.** Switch RX / TX at the top left: a status header with one Start / Stop button (Space starts and stops, Esc stops), then three steps: **1 Signal** (a searchable, grouped list), **2 Settings** (radio, frequency with the radio's range as a hint, output level, the signal's own options, and a **Test (dry run, no RF)** switch that runs the generator into nothing and shows the spectrum) and **3 Preview and send** (live spectrum, peak / RMS meters, clipping lamp, sample rate, occupied bandwidth, **STOP ALL**). It can pop out into its own window. A soft limiter keeps the generated signal inside the converter's range; it is not a power limit.
- **Every mode with a signal generator can be sent.** OnAir refuses no mode; only a mode without a generator says "this mode has no transmit generator".

The full list of modes offered is in [docs/TRANSMIT.md](docs/TRANSMIT.md).

## Remote-control API

<details>
<summary><b>Endpoints and examples</b></summary>


Scripts and other programs can control OnAir over HTTP and WebSocket. It is **off by default**: turn it on in **View > Data feeds > Remote control API**. It listens on `127.0.0.1` (port 8765) only; **Let other computers connect** opens it to the network and then needs a token (at least 16 characters, made for you; the app shows it and can copy it). Programs on the same computer need no token; every other address must send `Authorization: Bearer <token>` (or `?token=<token>`, which ends up in logs: prefer the header). A web page cannot drive it: no CORS headers are sent, a POST must be `Content-Type: application/json`, and a request with a foreign `Origin` is refused.

| Endpoint | What it does |
|---|---|
| `GET /api/v1/status` | running, receiver, device, family and mode, frequency, sample rate, gain, AGC, gain range |
| `GET /api/v1/devices` | the radios and sources: index, name, kind, board, serial, limits, `selected` |
| `GET /api/v1/modes` | the modes: `family` (and `sub` for the protocol entries) |
| `GET /api/v1/telemetry` | the active mode's report: `standard`, `state` (2 = locked), `locked`, `snrDb`, `cfoHz`, FEC `blocksOk` / `blocksBad`, `quality`, and `details` for the TV and radio standards |
| `GET /api/v1/spectrum?bins=N` | the latest spectrum line: `dbfs`, `centerHz`, `sampleRateHz`, `rmsDbfs`, `peak`, `clipFraction` |
| `POST /api/v1/start` / `stop` | start (optionally `{"device": 2}`) or stop the receiver |
| `POST /api/v1/mode` | `{"family": 5}` or `{"family": 25, "sub": "ft8"}` (stopped receiver only) |
| `POST /api/v1/frequency` | `{"hz": 1090000000}` or `{"mhz": 474}` |
| `POST /api/v1/samplerate` | `{"hz": 2400000}` or `{"msps": 2.4}`; 0 = automatic |
| `POST /api/v1/gain` | `{"db": 33}`; turns the AGC off |
| `GET /api/v1/ws` | WebSocket (read-only): pushes telemetry (at most 10 a second); send `{"spectrum":true,"bins":512}` for spectrum lines (at most 5 a second) |

Replies are JSON; a refused or failed request gives `{"error":"..."}` with a 4xx / 5xx status. Changes are made on the program's main thread within a frame.

```
curl -s http://127.0.0.1:8765/api/v1/telemetry
curl -s -X POST -H 'Content-Type: application/json' -d '{"mhz": 474}' http://127.0.0.1:8765/api/v1/frequency
curl -s -H "Authorization: Bearer $TOKEN" http://192.168.1.20:8765/api/v1/status
```

The server is `core/src/net/remote_api.cpp`; `tests/net/test_remote_api.cpp` runs a socket client against it, and against the real engine on a generated DVB-T2 signal.
</details>

## Updates

OnAir checks GitHub for a newer version a few seconds after it starts (then at most once a day), downloads the package for your system in the background, verifies it against the SHA-256 that GitHub lists, and installs it when you close the program.

- The version button at the top right shows the state and has the settings: turn the check off, turn automatic install off (you then get a button instead), or leave out pre-releases.
- macOS replaces `OnAir.app`; Windows runs the installer quietly (Windows asks for permission); Linux replaces the portable folder, and a `.deb` install asks for your password through the system's installer.
- Only github.com is contacted, and nothing is sent but the usual request. If GitHub is blocked or very slow where you are (for example in China), see **[docs/UPDATE-MIRROR.md](docs/UPDATE-MIRROR.md)**: an optional, off-by-default mirror for the update check and the download, with the same checksum checks.

## Discord status

Off by default. **View > Show in Discord** shows on your Discord profile that you are using OnAir, the mode and (optionally, **Show the frequency**) the frequency. It needs the Discord desktop app running, talks only to it on your computer, and sends nothing else anywhere. To use your own application in a fork: set the environment variable `ONAIR_DISCORD_APP_ID` (or `kDiscordAppId` in `app/platform/discord_rpc.cpp`) and upload the icon as a Rich Presence art asset named `onair`.

## Good to know

- Encrypted (scrambled) services cannot be decoded.
> [!NOTE]
> **mbelib is not included from 0.2.6.** DMR, P25, NXDN, dPMR and D-STAR voice is silent until you install mbelib yourself, because the AMBE / IMBE vocoders are patented. Calls, IDs and data work without it. Put the library in the codecs folder (the **?** next to the sound has an **Open codecs folder** button), then press **Look again**. Steps: **[docs/CODECS.md](docs/CODECS.md)**.
>
> **HD Radio sound is not included.** HDC is a proprietary codec of Xperi, so OnAir shows the stations, program data and pictures but is silent until you build the HDC-patched FAAD2 (`libfaad_hdc`) yourself from the open-source nrsc5 patch and put it in the codecs folder, then press **Look again** (the **?** next to the sound). Steps and the legal note: **[docs/CODECS.md](docs/CODECS.md#hd-radio-sound-hdc-build-it-yourself)**.

- codec2 (FreeDV and M17 speech) comes with OnAir as a separate shared library, nothing to install. Some other voice and audio codecs are not shipped (AMBE and HDC are patented, DRA+ is of unclear status). **[docs/CODECS.md](docs/CODECS.md)** says how to install the ones you can, and what to use for the others; the **?** button next to the sound of each such mode says the same.
- **Saving DRA sound.** OnAir does not include a DRA decoder (CDR sound needs the optional add-on in [docs/CODECS.md](docs/CODECS.md#dra-cdr-sound), TV DRA needs a patched FFmpeg), but it can save the raw sound. In the CDR *Radio* tab, and in the *Player* section of the TV tab for a DTMB service whose sound is DRA (or any service with MPEG audio), press **Save stream**. The raw audio goes to `audio-streams/<name>_<date>_<time>.dra` (`.mp2` for MPEG audio) in OnAir's data folder; press **Stop saving** to close the file. Play it with an FFmpeg that has DRA support (`ffplay stream.dra`, or `ffplay -f dra stream.dra` if it is not recognised); a normal FFmpeg cannot read `.dra`, while `.mp2` plays anywhere.
- Reception depends heavily on your antenna and location. A weak or echo-filled signal is the usual cause of a broken picture; the quality score and echo detector help you improve it.
- Receiving broadcasts is legal in most places, but laws differ. Check the rules where you live.

## Troubleshooting

<details>
<summary><b>Common problems and where to look</b></summary>

- **The radio is not listed.** Windows: install the WinUSB driver once with [Zadig](https://zadig.akeo.ie), see [Install](#install). SDRplay RSPs need the SDRplay API installed first. Notes per radio are in [DEVICES.md](DEVICES.md).
- **The picture breaks up.** Use **Auto-tune** or **Optimise gain**, and move the antenna while you watch the quality score, see [Quick start](#quick-start). A weak or echo-filled signal is the usual cause.
- **macOS will not open the app.** It is not signed with a developer certificate yet: right-click it and choose *Open* on the first launch.
- **No voice in DMR, P25, NXDN, dPMR or D-STAR.** Install mbelib yourself, see [Good to know](#good-to-know) and [docs/CODECS.md](docs/CODECS.md).
- **No radio at all.** Open an IQ recording through the *IQ recording file* source, or press **Tour**.
- **Still stuck.** Press **Save bug report** (one zip with diagnostics, log and crash report) and send it on [Discord](https://discord.gg/Kky9c6atm) or in [GitHub Issues](https://github.com/adswill/OnAir/issues).
</details>

## Build from source

<details>
<summary>Build instructions</summary>

You need a C++20 compiler, CMake 3.20 or newer, **Rust (cargo and rustc 1.70 or newer; required since 0.2.7, the protocol layers are Rust)**, pkg-config, FFmpeg, libhackrf, and GLFW for the app. SoapySDR is optional. For a cross build install the Rust target first (`rustup target add <triple>`); CMake names it when it is missing.

```sh
# macOS
brew install cmake rust pkg-config hackrf soapysdr libusb ffmpeg glfw
# Debian / Ubuntu
sudo apt install build-essential cmake cargo pkg-config libhackrf-dev libsoapysdr-dev libusb-1.0-0-dev \
  libglfw3-dev libavformat-dev libavcodec-dev libavutil-dev libswscale-dev libswresample-dev

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DDECT2_BUILD_APP=ON
cmake --build build -j "$(getconf _NPROCESSORS_ONLN)"   # one job per core (a bare -j starts hundreds and can run out of memory)
ctest --test-dir build          # run the test suite
```

The app is `build/dect2`. Without a radio, open an IQ recording through the *IQ recording file* source, or run the command-line receiver on it:

```sh
build/dect2cli --file capture.cs8 --rate 10 --format cs8 --play <service id>
```

SigMF (`.sigmf-meta` / `.sigmf-data`) and two-channel WAV IQ files open the same way and describe their own sample rate, centre frequency, format and start offset; `--rate`, `--format` and `--freq` still override.

**Tuning for your own CPU.** Release programs are built for the baseline 64-bit PC (the hot loops pick AVX2 at run time). A binary you build yourself can use everything your CPU offers:

- `-DDECT2_WIN_AVX2=ON` (x86-64 on Windows and Intel Macs): the whole receiver for AVX2 + FMA; refuses to start on older CPUs.
- `-DCMAKE_CXX_FLAGS="-march=native"` (GCC or Clang): runs only on CPUs like yours.
- `-DDECT2_PORTABLE=ON` (macOS, plain C++ instead of Accelerate) and `-DDECT2_NO_SIMD=ON` go the other way, for unusual CPUs, and are slower.

Configure with a clean `build` folder after changing these. Other options in `CMakeLists.txt`: `DECT2_UI_BACKEND` (Metal or OpenGL3), `DECT2_WITH_SOAPY`, `DECT2_WITH_RFNM` (when librfnm is installed: [DEVICES.md](DEVICES.md)).

**Windows** (10 or 11, 64-bit), step by step:

1. Install [MSYS2](https://www.msys2.org) with its default settings, and open **MSYS2 UCRT64** (not MINGW64: it has no HackRF and SoapySDR packages).
2. Update MSYS2 with `pacman -Syu` (if the window closes, open UCRT64 again and run it a second time).
3. Install the tools and libraries:
   ```sh
   pacman -S --needed git zip mingw-w64-ucrt-x86_64-{gcc,cmake,ninja,pkgconf,ffmpeg,glfw,hackrf,soapysdr,libiconv}
   ```
   Optional: `mingw-w64-ucrt-x86_64-nsis` for the setup .exe. (codec2 is built from `third_party/codec2` with OnAir and goes into the zip.)
4. Get the source and build:
   ```sh
   git clone https://github.com/adswill/OnAir.git
   cd OnAir
   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DDECT2_BUILD_APP=ON
   cmake --build build
   ```
5. Run it from the same shell, so Windows finds the DLLs: `./build/dect2.exe`. Tests: `ctest --test-dir build`.
6. For a folder that runs on any PC, with all DLLs next to `OnAir.exe`, run `tools/package/make_windows.sh`: a portable zip in `build-windows/`, plus a setup .exe if NSIS is installed. From macOS or Linux, `tools/package/make_windows_cross.sh` builds the same package with a cross-compiler.

To use a HackRF, RTL-SDR or Airspy, install the WinUSB driver once with [Zadig](https://zadig.akeo.ie).

**macOS app and disk image:** run `tools/package/make_dmg.sh`. It targets macOS 15.0 by default (override with `MACOSX_DEPLOYMENT_TARGET`); every bundled library must support that version, and packaging stops if any executable or library requires a newer macOS.

**Project layout**

| Folder | Contents |
|---|---|
| `core/` | The receiver library: demodulators, error correction, transport stream, player, network tuner |
| `app/` | The graphical application |
| `tools/` | Command-line receiver and scanner, plus the packaging scripts |
| `tests/` | Unit tests and benchmarks |
| `packaging/` | Linux and Windows installer resources, Docker |
</details>

## Support and community

- **[Docs](https://on-air.dev/docs/)**: the wiki, with a page per mode. The [GitHub wiki](https://github.com/adswill/OnAir/wiki) is the same text.
- **[Discord](https://discord.gg/Kky9c6atm)**: support, feature requests, questions, and sharing what you receive.
- **[GitHub Issues](https://github.com/adswill/OnAir/issues)**: bug reports. Attach an IQ recording of the problem: press **Record IQ** in the Source panel (or run `onair-cli --record auto --secs 10`); files go to the `recordings` folder of the app data folder, and **Show recording** opens it. With **SigMF** ticked (the default) every recording also gets a `.sigmf-meta` file, so it opens directly in [IQEngine](https://iqengine.org). **Save bug report** makes one zip with diagnostics, log and crash report.
- **[Signal sample library](https://files.on-air.dev)**: IQ and audio recordings of real signals, shared by users and tagged by mode (for example `#dvb-t2`). Every upload is checked and reviewed before it appears; recordings of the less-tested modes are the most useful.
- **[Pull requests](https://github.com/adswill/OnAir/pulls)** are welcome.

### Buy me a coffee

I'm a bit embarrassed to ask this, but here goes. OnAir is free and always will be. I build it in my spare time, along with the radios, antennas and test equipment it needs. If it's useful to you and you'd like to help, a small tip on **[Ko-fi](https://ko-fi.com/adswill)** really does help and keeps new modes coming. If you'd rather not, that's completely fine: bug reports, recordings and kind words help just as much. Thank you!

<p align="center"><a href="https://ko-fi.com/adswill"><img src="https://ko-fi.com/img/githubbutton_sm.svg" alt="Support OnAir on Ko-fi" height="36"></a></p>

## License

OnAir is released under the **GNU General Public License v3.0 or later**, see [LICENSE](LICENSE).

### Logo

OnAir name and logo © 2026 Kristijonas Adomaitis. All rights reserved; the logo is not covered by the GPL licence of the source code. Designed by Cipta Karsa Studio as work for hire, all rights assigned to the owner (see [assets/branding](assets/branding/README.md)).

## Credits

OnAir stands on the work of many people. Full list with authors, licences and what each licence asks of us: **[THIRD_PARTY.md](THIRD_PARTY.md)** (also in the program: Help, Credits and licences). The main ones:

- **Code in the program:** [Dear ImGui](https://github.com/ocornut/imgui) (Omar Cornut), [miniaudio](https://github.com/mackron/miniaudio) (David Reid), [codec2](https://github.com/drowe67/codec2) (David Rowe, LGPL-2.1, a separate library loaded at run time, see [docs/CODECS.md](docs/CODECS.md)), `dump978` UAT code (Oliver Jowett), the DRA decoder (maliwen2015), the RX888 firmware (Oscar Steila, SDDC_Driver).
- **Decoders written from or ported from open-source receivers:** [nrsc5](https://github.com/theori-io/nrsc5) (HD Radio), [etisnoop](https://github.com/Opendigitalradio/etisnoop) (DAB), [WSJT-X](https://sourceforge.net/projects/wsjt/), JS8Call and fldigi (HF digital modes), [rtl_433](https://github.com/merbanan/rtl_433), [gr-dvbt2](https://github.com/drmpeg/gr-dvbt2), [gr-lora_sdr](https://github.com/tapparelj/gr-lora_sdr), [e-Czas](https://github.com/sp5wwp/e-Czas), [JAERO](https://github.com/jontio/JAERO), acarsdec, libacars, dumphfdl, dumpvdl2, iridium-toolkit, multimon-ng, Meshtastic, MeshCore, MeshCom.
- **Libraries it uses:** [FFmpeg](https://ffmpeg.org), [GLFW](https://www.glfw.org), [libusb](https://libusb.info), [libhackrf](https://github.com/greatscottgadgets/hackrf), [Hamlib](https://hamlib.github.io), SoapySDR.
- **Data:** map tiles and data (c) [OpenStreetMap](https://www.openstreetmap.org/copyright) contributors, aircraft data from adsbdb, aircraft photos from Planespotters.net with credit, satellite elements from CelesTrak.

- **0.2.8 additions:** the DVB-S / DecSAT work uses code by **Arclamp** (GPL-3.0-or-later); the mesh clock fix is PR #217 by [@ffishman](https://github.com/ffishman); the recommended source for the TETRA and Yaesu Fusion voice plug-ins is [mbelib-neo](https://github.com/arancormonk/mbelib-neo) by arancormonk (GPL-2.0-or-later, not bundled); the NRSC-5 patch for HD Radio sound is from nrsc5 with [FAAD2](https://github.com/knik0/faad2). The KA modes are Kristijonas Adomaitis's own designs.

If your work is used and not credited correctly, please open an issue.

## Contributors

Thank you to everyone who sent fixes, reported bugs, shared recordings, translated or suggested ideas. The full list, with what each person did and for which mode, is in **[CONTRIBUTORS.md](CONTRIBUTORS.md)** (also in the program: Help, Credits, tab Contributors, and in each mode's tooltip). Code contributors: [@0xVANISHED](https://github.com/0xVANISHED), [@A2ureIm4la](https://github.com/A2ureIm4la), [@andriux26](https://github.com/andriux26), [@drelectro](https://github.com/drelectro), [@F5OEO](https://github.com/F5OEO), [@ffishman](https://github.com/ffishman), [@modemuser](https://github.com/modemuser), [@MUSTARDTIGERFPV](https://github.com/MUSTARDTIGERFPV), [@PU2SWX](https://github.com/PU2SWX), [@sbuchweitz](https://github.com/sbuchweitz), [@WispAyr](https://github.com/WispAyr) and [@YZYMC](https://github.com/YZYMC). Want your name changed or removed? Tell us on Discord or open an issue.

---

Business enquiries, press and anything that does not belong in an issue: **kristijonasvr@gmail.com**

