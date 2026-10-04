<p align="center"><img src="packaging/icons/onair_1024.png" width="140" alt="OnAir logo"></p>

<h1 align="center">OnAir</h1>

<p align="center"><b>Watch digital TV and listen to digital radio with a software-defined radio.</b><br>
Free and open source. macOS, Windows and Linux.</p>

<p align="center"><a href="https://github.com/adswill/OnAir/releases/download/v0.1.0/OnAir_promo_v0.1.0.mp4"><b>Watch the overview video</b></a></p>

---

OnAir turns a low-cost SDR (a HackRF, and many other radios) into a complete digital broadcast receiver. Plug in an antenna, pick a channel, and watch live TV or listen to digital radio, while seeing exactly what the receiver is doing: the spectrum, the signal quality, the constellations, the echoes in your reception.

It is built for people who want a receiver that is both easy to use and honest about what is happening inside it.

## What it receives

| Standard | Where it is used | Content |
|---|---|---|
| **DVB-T2** | Europe, Middle East, Africa, Asia, Australia | Digital TV |
| **DVB-T** | Same regions, older networks | Digital TV |
| **ATSC 1.0** | North America, South Korea | Digital TV |
| **DAB / DAB+** | Europe, Australia and more | Digital radio |

The standard is detected automatically, and one switch at the top of the window selects the family you want.

## Highlights

- **Live player.** Picture, sound, subtitles, teletext, programme guide and multiple audio tracks, with automatic repair of short signal dropouts so weak reception stays watchable.
- **See your signal.** Spectrum and waterfall, every constellation, signal-to-noise and error figures, a single quality score, and an echo (multipath) detector.
- **Find the best reception.** A channel scanner, automatic gain tuning, and a direction finder that helps you aim the antenna.
- **Share it.** A built-in network tuner streams your channels to VLC, phones, Plex or Jellyfin over your home network. You can also record to a file or send the stream out over UDP.
- **Try it without hardware.** The first-start tour and the **Tour** button play a built-in demo signal, so you can explore everything before buying a radio.
- **Fast.** Error correction runs on the GPU where available, and with optimised vector code on the CPU everywhere else.

## Install

Download the installer for your system from the **[Releases](../../releases)** page. There is nothing to compile.

**macOS** (Apple silicon): open the `.dmg` and drag OnAir to Applications. The app is not yet signed with a developer certificate, so on the first launch right-click it and choose *Open*.

**Windows** (64-bit): run the `OnAir-…-setup.exe` installer (or unpack the `.zip` and start `OnAir.exe`). For a HackRF, RTL-SDR or Airspy, install the WinUSB driver once with [Zadig](https://zadig.akeo.ie). Windows may warn about an unknown publisher because the installer is not code-signed.

**Linux** (Debian and Ubuntu): install the `.deb` with `sudo apt install ./onair_<version>_<arch>.deb` and start OnAir from the menu or with `onair`. Both x86-64 and arm64 packages are provided, plus a `.tar.gz`.

## Getting started

1. Connect your radio and antenna, then start OnAir. It lists connected radios in the source menu.
2. Enter a frequency, or open the **Scan** tab to find channels automatically.
3. Press **Start**. When the receiver locks, the channels appear on the right. Click one to watch.
4. If the picture breaks up, use **Auto-tune** for the gain and watch the quality score while you move the antenna. Height and a clear view toward the transmitter matter more than anything else.

New to all this? Press **Tour** and a friendly little TV will walk you through it.

## Supported radios

OnAir supports the **HackRF One and HackRF Pro** natively. **RTL-SDR, Airspy, BladeRF, LimeSDR, PlutoSDR and USRP** radios are driven directly too (marked "experimental" in the radio list) as soon as the manufacturer's driver library is installed, and nothing else has to be set up. Every other radio, such as the SDRplay RSP, works through [SoapySDR](https://github.com/pothosware/SoapySDR) once its SoapySDR module is installed. Radios that are found appear in the source list next to the HackRF, and the gain control becomes one overall gain slider.

A TV channel needs a radio that can sample fast enough, roughly 1 million samples per second per MHz of channel width:

| Radio class | Wide TV channels (6-8 MHz) | DAB radio, narrow channels |
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

## Project layout

| Folder | Contents |
|---|---|
| `core/` | The receiver library: demodulators, error correction, transport stream, player, network tuner |
| `app/` | The graphical application |
| `tools/` | Command-line receiver and scanner, plus the packaging scripts |
| `tests/` | Unit tests and benchmarks |
| `packaging/` | Linux and Windows installer resources |

## License

OnAir is released under the **GNU General Public License v3.0 or later**, see [LICENSE](LICENSE). It includes [Dear ImGui](https://github.com/ocornut/imgui), [ImPlot](https://github.com/epezent/implot) and [miniaudio](https://github.com/mackron/miniaudio), each under its own permissive license, and uses [FFmpeg](https://ffmpeg.org), [GLFW](https://www.glfw.org) and [libhackrf](https://github.com/greatscottgadgets/hackrf).
