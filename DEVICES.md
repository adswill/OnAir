# Any frequency, on every radio

OnAir does not limit the frequency you can enter, in any mode and on any radio. The numbers in this file (and in the radio list) are
the range a radio is **specified** for, not a limit OnAir enforces: the frequency is passed to the driver as you typed it. Outside the
specified range you get a note in plain words ("this radio is specified for X to Y MHz; the driver may not reach this frequency or the
signal may be weak or absent") and the radio keeps running. What stays an error is only what the hardware or its driver itself refuses
(its error text is shown), and the few cases where the call cannot carry the number: RTL-SDR, rtl_tcp and Airspy take the frequency as a
32-bit value in Hz (at most 4.29 GHz), and a negative frequency is turned into 0.

RTL-SDR below 24 MHz picks the way down by itself (direct sampling "Auto"): the RTL-SDR Blog V4 and V4 Lite use the upconverter in the
Blog library (OnAir never switches direct sampling on there), every other dongle samples its Q input directly (an RTL-SDR Blog V3 has its
HF input on Q; a stock dongle has no HF input, so the signal may be weak or absent unless it is modified). The radio settings can still
pick the I input or switch direct sampling off. Over rtl_tcp "Auto" sends nothing and the server's own library decides.

# Airspy

Tested with an Airspy R2 on Linux (ATSC).

```sh
# debian / ubuntu
sudo apt install libairspy0 airspy
sudo usermod -aG plugdev $USER   # log out and back in
# replug the radio, then check it shows up
airspy_info
```

- R2 runs at 10 Msps for TV, Mini not tried yet
- gain is the linearity gain, 0 to 21
- 10 Msps needs a decent CPU, a 2-core laptop couldn't keep up with live ATSC

# SDRplay RSPduo

OnAir talks directly to the [SDRplay Hardware API](https://sdrplay.com/hardware-api/), which supplies IQ samples. Install API 3.x
(tested with 3.15), then restart OnAir. **SDRconnect is a separate application**: installing it alone does not install the Hardware API
OnAir needs. SDRconnect's [WebSocket API](https://sdrplay.com/websocket-api/) is not used by this driver. Close other programs using the
RSPduo before starting it in OnAir.

The radio list offers **Tuner 1** (SMA), **Tuner 2** (SMA), and **Tuner 1 Hi-Z**. Pick the socket with the antenna. Both SMA inputs cover
1 kHz to 2 GHz; Hi-Z covers 1 kHz to 30 MHz. The gain slider uses the appropriate LNA table for that input. Antenna power is offered
only on Tuner 2; leave it off unless the connected equipment needs it.

- Single-tuner mode, one input at a time. Simultaneous dual-tuner operation, diversity, and master/slave sharing with another application
  are not implemented. Multiple OnAir receivers cannot claim the same physical RSPduo at once.
- Output sample rates: 62.5 kS/s to 10 MS/s, using the API's decimation below 2 MS/s. A rate change stops and restarts the stream.
  API 3.15 rejects 10.66 MS/s on the tested device; requests above 10 MS/s are capped with a note about the actual rate.
- Frequency, gain, PPM correction, and RF/MW and DAB notch filters can change while receiving. Failed starts and rate-change restarts
  return the API error instead of reporting a running receiver with no samples.

Hardware validation on Windows x64 with API 3.15 (2026-10-17): all three inputs streamed across 62.5 kS/s, 250 kS/s, 1, 2, 4, 8 and
10 MS/s, including a 10.66 MS/s request capped to 10 MS/s. Frequency/gain sweeps covered 1 kHz to 2 GHz on SMA and 1 kHz to 30 MHz on
Hi-Z, plus PPM/notch changes and repeated stop/start cycles. No ring-buffer drops or nonfinite samples occurred in those checks.
Only Tuner 1 SMA had an antenna. In the Windows app it received FM at 100.2 MHz with carrier lock and RDS decoding (TRT), including
changes between 2, 4, 8 and 10 MS/s. These checks establish streaming and reception, not calibrated sensitivity or RF accuracy.
The 120-second FM run produced 121 seconds of 48 kHz stereo WAV audio (the station was mono), with carrier lock in all 78 telemetry
reports, 1,359 decoded RDS groups, no reported sample drops, and no clipped PCM samples. Live GUI broadcast audio was also confirmed
by listening after explicitly selecting Tuner 1. Physical USB unplug/replug recovered automatically in the running GUI, with broadcast
audio returning and carrier/RDS lock restored.

Build with `-DDECT2_BUILD_TESTS=ON`, then run `ctest --test-dir <build> -R '^sdrplay$' --output-on-failure` for the hardware-independent
regression. It covers all inputs, tuner-B-only parameters/callbacks, Hi-Z gain states, bias-tee routing, low/high rates, failed Init,
failed rate restarts, and reconnecting with settings changed while unplugged.

The Windows and Linux GUI builds passed. Targeted SDRplay, FM, rate-choice and setup-help regressions passed on both platforms;
Linux's no-bias native test also passed. The broader Linux native test has four failures involving Airspy HF+ and RTL-SDR; the same
four failures were reproduced with the original driver and test sources at commit `6d4fc539`.

The opt-in hardware tool uses OnAir's production source and keeps antenna power off:

```sh
sdrplay_probe --port 0 --freq 100200000 --gain 40 --secs 3 --matrix
sdrplay_probe --port 1 --freq 100200000 --gain 40 --secs 3 --matrix
sdrplay_probe --port 2 --freq 7100000 --gain 40 --secs 3 --matrix
dect2cli --standard fm --freq 100.2 --gain 40 --secs 120 --wav rspduo-fm.wav
```

Frequencies are examples: use a local station for the FM reception test. The matrix also exercises maximum gain and can report
overloads from strong local signals. Bias-tee switching is checked with the fake API; its physical output voltage was not measured.
macOS and Linux hardware reception have not been tested on this device.

To launch the GUI directly on a particular input, pass its exact source-list name (replace the serial with yours):

```sh
OnAir --device "SDRplay RSPduo YOUR_SERIAL Tuner 1 (native, experimental)" --fm --freq 100.2
```

`--device` starts that source and leaves the receiver stopped if it is absent. Bare `--autostart` is a developer option that starts
the synthetic test signal; its FM test tones are not a received station, even when a broadcast frequency is displayed.

# Fobos SDR (RigExpert)

Not tested on a radio yet: the driver was written against RigExpert's library source and checked with a stand-in for the library
(`tests/native_fakes.cpp`). Reports are welcome.

OnAir drives the Fobos SDR through RigExpert's **agile** library, [libfobos-sdr-agile](https://github.com/rigexpert/libfobos-sdr-agile)
(`fobos_sdr.dll`, `libfobos_sdr.so`). That library only sees radios that run the agile firmware: hardware rev. 4 has it from the factory,
rev. 2 and 3 need the firmware from the library's Releases page. A Fobos SDR with the stock firmware is not listed.

The library is not shipped with OnAir:

- Windows: copy the 64-bit `fobos_sdr.dll` next to `OnAir.exe` (it uses the `libusb-1.0.dll` that is already there). The radio needs the
  WinUSB driver, as for RigExpert's own programs.
- Linux: build and install libfobos-sdr-agile (`cmake`, `make`, `sudo make install`, which also installs its udev rules), then
  `sudo ldconfig`.

`onair.log` (Windows) or the terminal says which library was loaded, how many radios it lists, and the hardware and firmware revision
of the one that was opened.

- RF input: specified for 50 MHz to 6 GHz, at 8, 10, 12.5, 16, 20, 25, 32, 40 or 50 Msps (rev. 4: also 58 and 64); OnAir takes the smallest rate that is
  fast enough for the mode
- gain: one slider from 0 to 95 dB. The LNA has three steps (0, +16, +33 dB) and follows the slider; the rest is the VGA in 2 dB steps.
  The radio settings can fix the LNA step
- HF1 / HF2 inputs: below 50 MHz the radio samples one of them directly (radio settings: HF1, HF2 or off). The converter then runs at
  its fastest rate and OnAir shifts, filters and decimates to the rate the mode asks for; about 70 % of that rate is clean. The range
  ends at half the converter's rate (25 MHz at 50 Msps, 32 MHz on rev. 4), so 25 to 50 MHz is covered by neither input (OnAir still asks for it, with a note). The gain slider
  does nothing here: the HF inputs have no amplifier
- the fast converter rates need a USB 3 port; on HF, a lower "HF converter rate" in the radio settings takes less of the computer
- an external 10 MHz reference can be chosen in the radio settings
- SoapySDR: a SoapyFobosSDR module built with Visual Studio (PothosSDR) cannot be loaded by OnAir's Windows build, which is built with
  MinGW; the native driver needs no SoapySDR

# RFNM

Contributed; tested on Windows only. The driver uses [librfnm](https://github.com/rfnm/librfnm), modelled on the RFNM client in XMC_SDR.

librfnm has a C++ interface only, so OnAir cannot load it at run time as it does the other radio libraries: it is linked when OnAir is
built, and only when CMake finds it through pkg-config (`librfnm.pc`). The downloads do not include it yet. Without librfnm the build is
the same as before; `-DDECT2_WITH_RFNM=OFF` leaves it out even when it is installed. CMake says which applies ("librfnm ...: RFNM enabled"
or "librfnm not found").

Building librfnm (it needs spdlog and libusb-1.0, and pkg-config):

- Windows, in the MSYS2 **UCRT64** shell (build it with the same compiler as OnAir: a librfnm built with Visual Studio cannot be linked
  into the MinGW build):
  ```sh
  pacman -S --needed mingw-w64-ucrt-x86_64-{spdlog,libusb}
  git clone https://github.com/rfnm/librfnm.git
  cmake -S librfnm -B librfnm/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/ucrt64 -DBUILD_RFNM_UTILS=OFF
  cmake --build librfnm/build && cmake --install librfnm/build
  ```
  If the compiler stops at `'memcpy' is not a member of 'std'` in `src/api/rx_stream.cpp`, add `#include <cstring>` at its top (newer
  GCC no longer brings it in through other headers). Without `-DCMAKE_INSTALL_PREFIX` librfnm installs to `C:/Program Files/RFNM`; then
  add its `lib/pkgconfig` folder to `PKG_CONFIG_PATH` before running CMake for OnAir. `librfnm.dll` must be next to `OnAir.exe` or on the
  PATH (`-DDECT2_COPY_RUNTIME_DLLS=ON` copies it into the build folder). The radio needs the WinUSB driver on USB.
- Linux / macOS: install spdlog and libusb-1.0 from the package manager, then build and install librfnm with CMake (on Linux it also
  installs its udev rules), and `sudo ldconfig`.

The radio list has one entry per receive channel: `RFNM <board> CH<n> <serial>`, with the slot when both slots are fitted. A radio
that librfnm finds on USB and on the network is listed once, on USB. `onair.log`
(Windows) or the terminal says how many radios librfnm lists, and for the channel that was opened its frequency and gain range.

- sample rates: 7.68, 15.36, 30.72, 61.44 and 122.88 Msps; OnAir takes the smallest one that is fast enough for the mode. A new rate
  restarts the stream
- frequency and gain: the channel's own ranges, as the board reports them once the radio has been opened (until then the list shows a
  guess). A frequency outside the channel's range is clamped to it, with a note: librfnm refuses it otherwise
- RF input: radio settings, "Automatic" (the board's preferred input) or one SMA input; an input the channel does not have falls back
  to the automatic one
- the low-pass filter follows the sample rate (or the baseband filter setting); boards without an RFIC (Breakout, XMC_RFFE) ignore it
- DC offset is removed by librfnm's stream
- not yet: channels that carry a real signal (the XMC_RFFE's HF input) are not listed, the Breakout's second channel comes with I and Q
  swapped, and there is no transmit

# RX888 MkII (Cypress FX3, USB 3)

**Experimental: written from the open SDDC protocol ([SDDC_Driver](https://github.com/renardspark/SDDC_Driver), MIT) and tested only against a
simulated FX3. It has never run on a real radio; please report what you see.**

OnAir talks to the radio over USB itself (no vendor library). HF (direct sampling, 64 or 128 Msps ADC clock) and VHF/UHF (R828D tuner, up
to about 1.75 GHz) are covered; the sample rate list is 0.25 to 64 Msps.

- The radio has no program of its own: the FX3 needs `SDDC_FX3.img`. Official OnAir packages ship it in the `codecs` folder (or, on Linux,
  `lib/onair/`). OnAir loads it into the radio's **RAM** when needed; **nothing is flashed**. A hand-built copy may need the file from the
  SDDC_Driver Releases page in the codecs folder, or `ONAIR_RX888_FIRMWARE` pointing at it.
- It must be on a USB 3 port (the stream is up to 256 MB/s). On Windows install WinUSB with Zadig for both `04b4:00f3` (boot loader) and
  `04b4:00f1` (running); on Linux the udev rule for `04b4:00f1` / `00f3` is needed.
- **Windows quick start (what most users need):** in [Zadig](https://zadig.akeo.ie), turn on **List All Devices**, install **WinUSB** for
  `04B4:00F3` and again for `04B4:00F1` (`RX888mk2`), **unplug and replug** the cable on a USB 3 port, then press **refresh** in OnAir's
  radio list. If the Cypress driver from the official RX-888 / HDSDR installer still owns the device, repeat Zadig on `00F1` and replug.
- A LimeSDR-USB shares `04b4:00f1`; OnAir tells them apart by the product name ("RX888mk2").
- Settings: ADC clock (auto, 64, 128 MHz or exact), input (auto, HF, VHF), HF attenuator, dither, randomiser, PGA, bias tee.

# SDR++ server

Tested with SDR++ 1.2 running `sdrpp --server` on a NanoPi R5S with an Airspy R2 (rates, gain, gain modes, bias tee, retuning, a dropped
connection), and against a fake server in the test suite (`test_sdrpp`).

Add it under **Add network radio...** (or **Add a network radio...** in the setup wizard): type **SDR++ server**, the computer's address,
port 5259. OnAir reads the server's source menu, so the radio list shows the server's source, its sample rates and its gain range before
the radio is started.

- **Any SDR++ source.** SDR++ has no protocol command for the sample rate or the gain: they live in the source module's menu, which the
  server sends to its client as a list of widgets. OnAir picks the sample rate combo (the one whose entries read as rates), the gain
  slider (an Airspy's linear or sensitive gain, an RTL-SDR's gain, a PlutoSDR's, ...), or, when the source has only stage sliders (LNA,
  mixer, VGA: an Airspy in Free mode, a HackRF), moves them together. OnAir's gain slider then runs over the server's own scale.
- **The server's other controls** are under **Radio settings**, named "SDR++: ...": gain mode, bias tee, AGC, offset tuning, antenna, the
  device and the server's source. Untouched, OnAir leaves them as the server has them; a value set in OnAir is set again whenever the radio
  starts. **These are changes to the server**: SDR++ keeps them after OnAir disconnects, and another client sees them.
  **Leave the gain to the server** stops OnAir moving the gain slider.
- **Sample rate**: the slowest entry of the server's list at or above what the mode needs. SDR++ only takes a new rate (or a new device or
  source) while it is not streaming, so OnAir stops the stream, changes it and starts again; the samples in between are dropped.
- **Wire format**: 16-bit (the default), 8-bit (half the network) or float. Compression (zstd) is switched off: OnAir does not read it, and
  noise-like IQ hardly compresses.
- **Retuning** never waits for the network: the samples are dropped until the server has answered the change, and two of SDR++'s buffers
  after that, so the decoder does not see samples from before it.
- **One client at a time.** While SDR++'s own client (or another OnAir) is connected the server turns OnAir away; the message says so, and
  OnAir tries again. When the server drops the connection, OnAir reconnects by itself and counts the samples lost.
- **A link slower than the stream** (Wi-Fi, a VPN, powerline): SDR++ numbers nothing, so OnAir compares what arrives with the rate the
  server announced. Below 90 % the difference is counted as lost samples (the status bar shows them) and the log says what the link
  carries; use a lower rate or the 8-bit format. 2.5 Msps needs about 10 MB/s in 16-bit, 10 Msps about 40 MB/s. On a link that cannot carry
  the stream, SDR++ queues samples inside itself, so a change can take a second or two to show.

# Transmit (the RX / TX switch)

A slider at the top left of the interface (new and classic) switches between **RX** (the normal receiver) and **TX** (the Transmit panel in the main area). The
transmitter is independent of the receivers, like in SDRangel: it has its **own radio choice** (any radio that can transmit) and its own mode, so a receiver
keeps receiving while you transmit. Example: a HackRF Pro transmits while an RTL-SDR v4 is a receiver and decodes it. Switching the view starts and stops nothing.

A radio is never used by both a receiver and the transmitter unless it is genuinely full duplex (`DeviceInfo::txFullDuplex`; none is today). The claim is
made through `device_claims.h` (the transmitter holds its radio as `kTxClaimOwner`): a radio a receiver has open is marked "in use by RX n" in the TX radio list
and cannot be started, with a sentence saying it cannot receive and transmit at the same time; the receivers' radio lists mark the transmitter's radio in the
same way. Nothing is stopped silently.

Transmitting is off until you press the big **Start transmitting** button: it never starts by itself, never restarts after a crash or when the program starts
(the settings do not remember "transmitting"; the program opens in RX view), and it stops when you stop it, the radio fails or you quit. The frequency is
pre-filled from the receiver but is yours to edit. Transmitting needs a licence or permission where you are; the first time, a notice asks you to confirm that
you understand (one button, once per install). OnAir limits neither the frequency nor the power: what you type goes to the radio's driver as typed, and a
refusal by the driver is shown in its own words. The gain slider is the radio's own TX gain range (lowest by default). Connect an antenna, an attenuator or a dummy load.
**ADS-B** shows a standing warning: it simulates aircraft and must only go into a dummy load or a shielded setup.

**HackRF Pro:** it is covered by the HackRF USB path (`UsbHackrfTx`). libhackrf's own host library identifies the HackRF One and the Pro (board id 5, "Praline") by the
same USB id 1d50:6089 (the code lists it in `kPids`, and `boardName(5)` reads "HackRF Pro", 0.1 MHz to 6 GHz), and the transmit protocol is the same vendor requests
(transceiver mode 2, TX VGA) with the samples on bulk endpoint 0x02. That is read from the vendor's sources, not tried: no HackRF Pro was available.

**Radios** (`DeviceInfo::canTransmit`; the code is `TxSink` in `core/include/dect2/source.h`):

| Radio | Path | Notes |
|---|---|---|
| HackRF One / Pro (OnAir's own USB driver) | `UsbHackrfTx` | 2 to 20 Msps, TX VGA 0 to 47 dB, the +14 dB amplifier is a switch |
| PlutoSDR (libiio) | `PlutoTx` | 2.1 Msps and up, TX attenuation -89.75 to 0 dB |
| Any SoapySDR radio with a TX channel (LimeSDR, bladeRF, USRP, ...) | `SoapyTx` (`writeStream`) | known after the radio has been opened once, or by its driver name |

Every other radio (RTL-SDR, Airspy, RX888, SDRplay, Fobos, ... receive-only; the native LimeSDR / bladeRF / USRP drivers are receive-only, use their SoapySDR module to transmit) reports `canTransmit = false`.
No radio is marked full duplex here, so the transmitter and a receiver always need two different radios (a radio is shared only through the claim above).

**Modes that can transmit** (the table is `txModes()` in `core/src/tx.cpp`; deny-list, not allow-list). Every engine mode that has a generator is offered except pointless / jammer / impersonation cases. No frequency limit is applied here. Empty payload fields fall back to the demo signal; the Transmit panel has a real payload UI on every row (callsign, grid, text, IDs, file, microphone as that mode fits).

**Voice.** FM broadcast: live microphone or WAV/MP3 into a stereo multiplex with RDS (PI, PS, RT), deviation and 50/75 µs pre-emphasis, and a mic-level meter. NFM, airband AM, analog-TV sound and amateur SSB (when offered) take the same audio path. M17 and FreeDV encode live speech with Codec 2 when `libcodec2` is loaded. DMR, P25, NXDN, dPMR and D-STAR cannot encode: OnAir loads mbelib for decode only; the panel says so, IDs still go on the air, and a 1 kHz tone / silence / WAV is still offered. The microphone is never opened until **START TRANSMITTING** (`AudioIn`: CoreAudio on macOS, miniaudio elsewhere).

**Carrier tune.** Every offered mode has a hold button that sends an unmodulated carrier (I = 1, Q = 0 at DC) at the current frequency and gain, independent of the payload, for setting drive / SWR / dummy-load level. Not saved as transmitting.

| Mode | Generator | Round trip |
|---|---|---|
| DVB-T2 | T2 generator with a real PLP and the test transport stream | decodes, programme found |
| DVB-T | `dvbt::Generator` with the demo programme | decodes, programme found |
| ATSC 1.0 | `atsc::Generator` with the demo programme | decodes, programme found |
| DAB / DAB+ | `makeDabSynth` | decodes |
| ISDB-T | `makeModeSynth(6)` | decodes, programme found |
| DVB-S / S2 / S2X | `makeDvbsSynth` | S2 and S decode, programme found |
| DTMB | `makeDtmbSynth` | decodes, programme found |
| ADS-B | `makeAdsbSynth` (simulated aircraft: test content, not real ones) | decodes |

**Not offered**, with the reason shown in the interface: the cellular modes (the generators model a network cell for testing), GNSS (spoofing), Bluetooth / Wi-Fi / HaLow / ANT+ / Remote ID (would impersonate those networks), ISM sensors and Datawell (receive-only harvesters), the spectrum analyser and every analysis / general-listening / workbench mode, time-signal clocks (spoofing), and Multitone paging (not the unpublished real format). Pagers (POCSAG 512/1200/2400 and FLEX) are offered.

# Sound card (audio IQ)

Listed as **Sound card: Default input** and each capture device. Uses the same `AudioIn` path as transmit (CoreAudio on macOS, miniaudio elsewhere). The microphone is not opened until Start. Sample rate is the capture rate (typically 48 kHz); the frequency you type is the dial for the decoder. IQ mappings: stereo I/Q, USB (analytic), LSB (conjugate), mono real. No gain or bias-tee. For WebSDR / VAC / a radio speaker tap — not a wideband SDR. **Unverified against a real WebSDR.**

**Tested and not tested.** The loop-back tests, the HackRF sample conversion, the PlutoSDR path against a fake libiio and the SoapySDR path against a fake driver
all run in the test suite (`test_tx`). Nothing here has been transmitted by a real radio: the three real TX paths are built from the radios' documented
interfaces and have never been run on hardware.
