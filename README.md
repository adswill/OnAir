<p align="center"><img src="packaging/icons/onair_1024.png" width="140" alt="OnAir logo"></p>

<h1 align="center">OnAir</h1>

<p align="center"><b>Watch digital TV and listen to digital radio with a software-defined radio.</b><br>
Free and open source. macOS, Windows and Linux.</p>

<p align="center"><a href="https://github.com/adswill/OnAir/releases/download/v0.1.0/OnAir_promo_v0.1.0.mp4"><b>Watch the overview video</b></a> &nbsp;·&nbsp; <a href="https://discord.gg/Kky9c6atm"><b>Join the Discord</b></a> for support and feature requests</p>

---

OnAir turns a low-cost SDR (a HackRF, and many other radios) into a complete digital broadcast receiver. Plug in an antenna, pick a channel, and watch live TV or listen to digital radio, while seeing exactly what the receiver is doing: the spectrum, the signal quality, the constellations, the echoes in your reception.

It is built for people who want a receiver that is both easy to use and honest about what is happening inside it.

## What it receives

| Standard | Where it is used | Content |
|---|---|---|
| **DVB-T2** (including T2-Lite) | Europe, Middle East, Africa, Asia, Australia | Digital TV |
| **DVB-T** | Same regions, older networks | Digital TV |
| **ATSC 1.0** | North America, South Korea | Digital TV |
| **ATSC 3.0** (NextGen TV), *experimental* | North America, South Korea | Digital TV |
| **ISDB-T**, *experimental* | Japan, Brazil and most of South America, Philippines | Digital TV and one-segment mobile TV |
| **DAB / DAB+** | Europe, Australia and more | Digital radio |
| **FM radio** (87.5 - 108 MHz) | Worldwide | Analogue radio: stereo, and RDS (station name, radio text, programme type, traffic flags) |

The DVB standard is detected automatically, and one switch at the top of the window selects the family you want (DVB, ATSC, ATSC 3.0, ISDB-T, DAB or FM).

**T2-Lite** (the 1/3 and 2/5 code rates, short FEC frames, 1.7 MHz channels) is decoded like any other DVB-T2 signal. It is checked end to end on simulated signals from QPSK to 256-QAM, and the error correction sits where theory says it should; it has not been tried on a real T2-Lite broadcast. A T2-Lite signal that shares a channel with a normal T2 one (in its future-extension frames) is not decoded: the normal T2 part keeps working.

**ATSC 3.0 is experimental.** The whole receiver chain is built from the published specifications and works on simulated signals (carrier offset, noise and echoes included): synchronisation, error correction, link layer, ROUTE and playback of HEVC video with AAC audio. It has not yet been checked against a real broadcast, so details such as the scrambler or interleaver may need fixing once a real recording is available. Services that use MMTP or AC-4 audio are not supported yet, and it needs a radio that can sample at 6.5 Msps or faster. If you have an ATSC 3.0 recording or can try it on air, please tell us on Discord.

**ISDB-T is experimental too.** It follows the ARIB STD-B31 specification (all three modes, all guard intervals, DQPSK, QPSK, 16QAM and 64QAM, the 13 segments in up to three layers including the one-segment layer, TMCC, the time and frequency interleavers, Viterbi and Reed-Solomon) and decodes simulated signals exactly, with carrier offset, clock error, noise and echoes, at 6 to 20 Msps. It has not been checked against a real broadcast yet, so details may need adjusting once a real recording is available. Channel names written in the Japanese character set are not converted yet. The channel scanner covers the 6 MHz raster of Japan and Brazil. To try it without a signal, `isdbtgen --out sample.cs8` (built with the tools) makes a recording that carries the built-in test programme: open it as an IQ recording, 10 Msps, CS8, with ISDB-T selected. A recording from a real transmitter would help a lot; please share one on Discord.

## Updates

OnAir checks GitHub for a newer version a few seconds after it starts (and then at most once a day), downloads the package for your system in the background, checks it against the SHA-256 that GitHub lists for the file, and puts it in place when you close the program. The button at the top right (`v0.1.5`) shows the state and has the settings: turn the check off, turn the automatic install off (you then get a button instead), or leave out pre-releases. On macOS it replaces `OnAir.app`, on Windows it runs the installer quietly (Windows asks for permission), on Linux it replaces the portable folder; a `.deb` install asks for your password through the system's installer. Only github.com is contacted, and nothing is sent but the usual request.

## Highlights

- **Live player.** Picture, sound, subtitles, teletext, programme guide and multiple audio tracks, with automatic repair of short signal dropouts so weak reception stays watchable.
- **See your signal.** Spectrum and waterfall, every constellation, signal-to-noise and error figures, a single quality score, and an echo (multipath) detector.
- **Find the best reception.** A channel scanner, automatic gain tuning, and a direction finder that helps you aim the antenna.
- **Share it.** A built-in network tuner streams your channels to VLC, phones, Plex or Jellyfin over your home network, as a plain stream or as HLS for browsers and phones. On a Mac you can cast the playing service to an Apple TV or AirPlay TV. You can also record to a file or send the stream out over UDP.
- **Try it without hardware.** The first-start tour and the **Tour** button play a built-in demo signal, so you can explore everything before buying a radio.
- **Fast.** Error correction runs on the GPU where available, and with optimised vector code on the CPU everywhere else.

## Install

Download the installer for your system from the **[Releases](../../releases)** page. There is nothing to compile.

**macOS** (macOS 15 or later, Apple silicon and Intel): open the `.dmg` for your Mac (`arm64` for Apple silicon, `x86_64` for Intel) and drag OnAir to Applications. The app is not yet signed with a developer certificate, so on the first launch right-click it and choose *Open*.

**Windows** (64-bit): run the `OnAir-…-setup.exe` installer (or unpack the `.zip` and start `OnAir.exe`). For a HackRF, RTL-SDR or Airspy, install the WinUSB driver once with [Zadig](https://zadig.akeo.ie). Windows may warn about an unknown publisher because the installer is not code-signed.

**Linux:** the `.deb` installs on any current Debian or Ubuntu (it carries its own video decoder, so it does not depend on the version of FFmpeg your system has): `sudo apt install ./onair_<version>_<arch>.deb`, then start OnAir from the menu or with `onair`. For other distributions there is a `-portable.tar.gz` with everything inside: unpack it and run `bin/onair`. x86-64 and arm64 packages are provided.

## Getting started

1. Connect your radio and antenna, then start OnAir. It lists connected radios in the source menu.
2. Enter a frequency, or open the **Scan** tab to find channels automatically.
3. Press **Start**. When the receiver locks, the channels appear on the right. Click one to watch.
4. If the picture breaks up, use **Auto-tune** for the gain and watch the quality score while you move the antenna. Height and a clear view toward the transmitter matter more than anything else.

New to all this? Press **Tour** and a friendly little TV will walk you through it.

## Supported radios

OnAir supports the **HackRF One and HackRF Pro** natively, and drives **RTL-SDR, Airspy, BladeRF, LimeSDR, PlutoSDR and USRP** radios directly too (marked "experimental" in the radio list). The libraries those radios need are included in the downloads, so there is nothing else to install, with these exceptions: on macOS the PlutoSDR library is not included yet, and on Windows the LimeSDR and USRP libraries are not (install the manufacturer's software for those). On Windows a radio also needs its USB driver once, see the install notes above. Every other radio, such as the SDRplay RSP, works through [SoapySDR](https://github.com/pothosware/SoapySDR) once its SoapySDR module is installed. Radios that are found appear in the source list next to the HackRF, and the gain control becomes one overall gain slider.

A TV channel needs a radio that can sample fast enough, roughly 1 million samples per second per MHz of channel width:

| Radio class | Wide TV channels (6-8 MHz) | DAB and FM radio, narrow channels |
|---|---|---|
| HackRF, PlutoSDR, BladeRF, LimeSDR, USRP | Yes | Yes |
| Airspy R2, SDRplay | Should work | Yes |
| RTL-SDR | No | Yes |

The HackRF is the radio the project is developed and tested with; the others have had less real-world testing. On Windows, RTL-SDR and Airspy radios are included in the installer.

Notes for specific radios are in [DEVICES.md](DEVICES.md).

## Good to know

- Encrypted (scrambled) services cannot be decoded.
- Reception depends heavily on your antenna and location. A weak or echo-filled signal is the usual cause of a broken picture, and OnAir's quality score and echo detector are there to help you improve it.
- Receiving broadcasts is legal in most places, but laws differ. Check the rules where you live.

## Build from source

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

Useful options: `DECT2_UI_BACKEND` (Metal or OpenGL3), `DECT2_WITH_SOAPY`, and `DECT2_PORTABLE` / `DECT2_NO_SIMD` for plain C++ code paths on unusual CPUs.

To build a self-contained macOS app and disk image, run `tools/package/make_dmg.sh`.
It targets macOS 15.0 by default and uses the same minimum version in the executable
and the app metadata. Override it with `MACOSX_DEPLOYMENT_TARGET` if needed.
Every bundled library must also support that version: setting a lower deployment
target does not rebuild Homebrew libraries. Packaging stops if any executable or
library requires a newer macOS. Build releases on macOS 15 with compatible
Homebrew dependencies, as in the release workflow, or rebuild the dependencies
for the intended target.

## Support and community

The [OnAir Discord server](https://discord.gg/Kky9c6atm) is the place for **support**, **feature requests**, questions, and sharing what you receive. Bugs can also go in [GitHub Issues](https://github.com/adswill/OnAir/issues).

## Project layout

| Folder | Contents |
|---|---|
| `core/` | The receiver library: demodulators, error correction, transport stream, player, network tuner |
| `app/` | The graphical application |
| `tools/` | Command-line receiver and scanner, plus the packaging scripts |
| `tests/` | Unit tests and benchmarks |
| `packaging/` | Linux and Windows installer resources |

## License

OnAir is released under the **GNU General Public License v3.0 or later**, see [LICENSE](LICENSE). It includes [Dear ImGui](https://github.com/ocornut/imgui) and [miniaudio](https://github.com/mackron/miniaudio), each under its own permissive license, and uses [FFmpeg](https://ffmpeg.org), [GLFW](https://www.glfw.org), [libusb](https://libusb.info) and [libhackrf](https://github.com/greatscottgadgets/hackrf).
