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
- [Supported radios](#supported-radios)
- [Updates](#updates)
- [Good to know](#good-to-know)
- [Build from source](#build-from-source)
- [Support and community](#support-and-community)
- [License](#license)

## Install

Download the file for your system from the **[latest release](https://github.com/adswill/OnAir/releases/latest)**. There is nothing to compile.

| System | Download | How to install |
|---|---|---|
| **macOS** 15 or later | `OnAir-…-macos-arm64.dmg` (Apple silicon)<br>`OnAir-…-macos-x86_64.dmg` (Intel) | Open the `.dmg` and drag OnAir to Applications. The app is not signed with a developer certificate yet, so on the first launch right-click it and choose *Open*. Intel Macs need a CPU with AVX2 (2013 or newer). |
| **Windows** 10 / 11, 64-bit | `OnAir-…-windows-x64-setup.exe`<br>or `OnAir-…-windows-x64.zip` | Run the installer, or unpack the zip and start `OnAir.exe`. Windows may warn about an unknown publisher because the installer is not code-signed. Needs a CPU with AVX2 (Intel or AMD from about 2013 on). |
| **Linux** x86-64 and arm64 | `onair_…_amd64.deb` / `onair_…_arm64.deb`<br>or `onair-…-portable.tar.gz` | Debian / Ubuntu: `sudo apt install ./onair_<version>_<arch>.deb`, then start OnAir from the menu or with `onair`. The `.deb` carries its own video decoder, so it does not depend on your system's FFmpeg. Other distributions: unpack the portable archive and run `bin/onair`. |

**Windows USB drivers:** for a HackRF, RTL-SDR, Airspy or Airspy HF+, install the WinUSB driver once with [Zadig](https://zadig.akeo.ie). A LimeSDR Mini needs FTDI's FT60x driver (see the README inside the download).

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
| **Analog TV** | PAL, SECAM, NTSC | Picture and sound |
| **DRM** | Digital radio on long, medium and short wave | Audio and station information |
| **DMR** | Digital two-way radio | Talkgroups, IDs and data (no voice) |
| **ADS-B** | Aircraft transponders, 1090 MHz | Aircraft on a map, with altitude, speed and callsign |
| **ACARS** | Aircraft data link, VHF | Messages, and positions on a map |
| **Inmarsat Aero** | Aircraft satellite data link, L band | Messages, and positions on a map |
| **Inmarsat-C** | Maritime satellite broadcasts | Safety messages (SafetyNET) and system information |
| **Iridium** | Satellite phone network | Satellites on a map, pager messages, ring alerts |
| **AIS** | Ship transponders | Ships on a map |
| **Maritime** | NAVTEX, DSC, weather fax | Safety messages, distress and calling, fax pictures |
| **Radiosondes** | Weather balloons, 400 – 406 MHz | Balloon tracks, altitude and weather data |
| **GNSS** | GPS L1 | Satellites, position and time |
| **Mesh** | Meshtastic and MeshCore (LoRa) | Nodes, map and chat |

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

## Updates

OnAir checks GitHub for a newer version a few seconds after it starts (then at most once a day), downloads the package for your system in the background, verifies it against the SHA-256 that GitHub lists, and installs it when you close the program.

- The version button at the top right shows the state and has the settings: turn the check off, turn automatic install off (you then get a button instead), or leave out pre-releases.
- macOS replaces `OnAir.app`; Windows runs the installer quietly (Windows asks for permission); Linux replaces the portable folder, and a `.deb` install asks for your password through the system's installer.
- Only github.com is contacted, and nothing is sent but the usual request.

## Good to know

- Encrypted (scrambled) services cannot be decoded.
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
cmake --build build -j
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
- **[Pull requests](https://github.com/adswill/OnAir/pulls)** are welcome.

## License

OnAir is released under the **GNU General Public License v3.0 or later**, see [LICENSE](LICENSE). It includes [Dear ImGui](https://github.com/ocornut/imgui) and [miniaudio](https://github.com/mackron/miniaudio), each under its own permissive license, and uses [FFmpeg](https://ffmpeg.org), [GLFW](https://www.glfw.org), [libusb](https://libusb.info) and [libhackrf](https://github.com/greatscottgadgets/hackrf).
