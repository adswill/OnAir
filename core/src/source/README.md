# core/src/source: Radios, drivers and capture files

Sources for this topic. One source_<radio>.cpp per supported radio, the native USB drivers (hackrf_usb, rx888_usb, mirics_usb), Hamlib rig control, the antenna rotator, IQ file playback and recording, gain control and the frequency converter.

Mode folders (core/src/<mode>/) hold one decoder each; this folder holds code shared by all of them. See ARCHITECTURE.md.

## Airspy Mini at 10 Msps (opt-in)

An Airspy Mini lists 6 and 3 Msps (an R2 lists 10 and 2.5). The Mini's radio settings have "10 Msps (unofficial)" (key `mini10`, off by
default); when on, `source_airspy.cpp` adds 10 Msps to the rate it may pick and asks libairspy for it. The firmware decides: a refusal is
reported with libairspy's error code and name and the Mini falls back to the fastest listed rate. With the setting off nothing changes.
Tests: `tests/source/test_native.cpp` (fake libairspy: rate choice with and without the opt-in, refused rate, refused listed rate).


`source_limesdr.cpp` drives LimeSDR boards through libLimeSuite (loaded at run time); `lime_board.h` holds the board facts it uses (name from the device string, Mini rate list, input by frequency, gain mapping, error hints) as plain functions that tests/source/test_limesdr_mini.cpp checks.

## SDRplay RSP1 / Mirics MSi2500 + MSi001 (own driver, issue #503, untested on hardware)

`mirics_usb.cpp` drives the MSi2500 bridge over libusb like `hackrf_usb.cpp` (board `mirics`, kind Native): from the app's descriptor on
Android, by bus scan elsewhere. All register maths is in `mirics_regs.cpp` (`dect2/source/mirics.h`): the 24-bit register write (request
0x41), the ADC clock (registers 3 and 4), the four sample formats (register 7) and their unpacking from 1024-byte blocks with the sample
counter in the header (lost blocks are counted), the MSi001 synthesiser and band plan (register 9, band switch on register 8, RSP1 and
generic words), the filter and the LNA / mixer / IF gain split. Facts from libmirisdr (GPL-2.0-or-later, credited); the kernel drivers
(GPL-2.0-only) were not used. An RSP1 that the SDRplay API lists hides this driver's entry. Tests: `tests/source/test_mirics.cpp`.

## SoapySDR over the network (SoapyRemote, issue #84)

A radio plugged into another computer is used through SoapySDR's own remote protocol: run `SoapySDRServer --bind` there, then add a
**SoapyRemote** network radio (host, port 55132, optionally the remote radio's driver or `driver=x,serial=y`). `core/src/net/netradio.cpp` builds
`driver=remote,remote=tcp://host:port,remote:timeout=5000000[,remote:driver=...]` and the normal Soapy source (`source_soapy.cpp`) opens it, so
gain, rates and retuning work as for a local radio. Errors say what to do: no SoapyRemote client module (`soapyRemoteModuleAvailable`, checks
SoapySDR's registered drivers; the text is chosen per platform by `soapyRemoteModuleHelpFor`: Windows and macOS packages bundle the module, so there it says the installation is damaged; Linux says `apt install soapysdr-module-remote`), server unreachable (the 3 s probe), server up but no radio listed. A build
without SoapySDR keeps the type but says so. Discovery is by typing the host (avahi is not used: it is unavailable on Windows).
Tests: `tests/net/test_netradio.cpp` (arguments, offers, help, module-missing probe) and `tests/source/test_soapy.cpp` (fake driver).

## Sound-card IF / AF input (issue #137)

`source_audio.cpp` captures stereo audio (`AudioIn`: miniaudio / CoreAudio at `audio_rate`, 8-192 kHz) and `audio_if.cpp` (`audioIfProcess`) makes IQ:
gain, DC removal, channel (`audio_chan`), real -> analytic (Hilbert) for the real modes, shift by `-audio_if_hz` so the IF centre lands on 0 Hz,
then sideband inversion (LSB or `audio_invert`). The dial is not touched: the user sets it to the radio frequency of the IF centre. Tutorial: with an
IC-7300 in USB with its 12 kHz IF on the USB audio, pick the sound card, mode Real (mono), IF centre 12000 Hz, dial 14.074 MHz, and FT8 decodes as
from an SDR (the radio's own filters and AGC do the front end). A serial of `file:<wav>` plays a WAV as the sound card (tests). Level for a meter:
`audioSourceLevel()`. Tests: `tests/audio/test_audio_if.cpp`, `tests/audio/test_audio_source.cpp`.
