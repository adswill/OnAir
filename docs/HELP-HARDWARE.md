# Help > Hardware

The window shows this computer, OnAir's minimum requirements and how the computer measures up, and every media codec, container and capture
input OnAir uses, each with a green tick (works) or a red cross (missing). **Copy to clipboard** copies the whole page as plain text, for a
bug report or a forum post. Code: `app/panels/hardware_ui.cpp` (window), `app/panels/hardware_logic.h` (lists, rules, text; tested in
`tests/app/test_hardware_logic.cpp`), `core/src/tv/media_probe.cpp` (FFmpeg lookups).

## Minimum requirements

| Item | Minimum | Recommended | Source |
|---|---|---|---|
| Operating system | Windows 10 64-bit, macOS 15, Linux 64-bit | Windows 11, macOS 15 or later, a current Linux | docs/INSTALL.md, the Windows installer |
| 64-bit | x86-64 or arm64 | | release packages |
| Processor threads | 2 | 4 or more | DVB-T2 / ATSC 3.0 decoding and HD video encoding |
| Processor instructions | x86: x86-64 baseline, SSE2 (AVX2 + FMA in a `DECT2_WIN_AVX2` build); arm64: NEON | AVX2 + FMA: the resamplers, FFT and ATSC / DTMB filters run their AVX2 + FMA copy | release baseline, `dect2/dsp/simd.h`, `app/main.cpp` |
| Memory | 4 GB | 8 GB or more | |
| Graphics | OpenGL 3.0 or OpenGL ES 3.0 (macOS: Metal) | OpenGL 3.2 or later | `app/main_window.cpp` start-up chain (#283, #470) |
| GPU computing | not needed | Vulkan, Direct3D 11 or Metal | faster TV error correction (Settings > Decoding) |

Memory counts from 90 % of the figure (a "4 GB" computer reports a little less). Green: meets the recommended. Amber: meets the minimum.
Red: below the minimum.

## Media support

The list is everything the code asks FFmpeg for by name or codec id (`avcodec_find_encoder_by_name`, `avcodec_find_decoder`,
`av_find_input_format`, `avformat_alloc_output_context2`), plus the camera input and the add-ons loaded at run time (AC-4, AVS+, DRA;
Help > Voice codecs). Rows for another operating system (VideoToolbox, Media Foundation, VA-API) are not shown.

- Decoders, muxers and demuxers: ticked when the linked FFmpeg has them.
- Video encoders: ticked only when a trial encode of a few black pictures works, the same trial as the digital video generator
  (`core/src/tv/videogen_params.cpp`). A hardware encoder in FFmpeg without the graphics card for it gets a red cross with the reason.
  The trial runs on a worker thread; the media list fills in when it is done.
- Rows marked optional may be missing: OnAir then uses another encoder, or the add-on is only needed in some countries.

## USB radio drivers

What a USB radio needs before OnAir can open it, in three groups, each line with a green tick or a red cross:

- Operating system. Windows: WinUSB (`winusb.dll`, part of Windows; each libusb radio gets it once with Zadig). Linux: the udev rules
  `60-onair-sdr.rules` (in `/etc`, `/usr/lib` or `/lib` `udev/rules.d`; radios open without root) and the blacklists
  `/etc/modprobe.d/onair-rtlsdr-blacklist.conf` (the kernel TV driver `dvb_usb_rtl28xxu`) and `onair-sdrplay-blacklist.conf` (`msi2500`),
  with a note when that kernel driver is loaded. macOS: nothing needed.
- Radio driver libraries: the "Radio drivers" group of `dect2/codec/optional_libs.h` (librtlsdr, libairspy, libairspyhf, fobos, libbladeRF,
  libhydrasdr, LimeSuite, libiio, UHD, the SDRplay API), as in Settings > Advanced > Optional libraries. Hamlib is left out (CAT, not USB).
  Each is optional: it is only needed for its own radio.
- Radios plugged in: every radio of `core/src/source/usb_diag.cpp` that is on the bus (`dect2::usbRadiosPresent()`, one libusb scan). On
  Windows each WinUSB radio is opened for a moment; `LIBUSB_ERROR_NOT_SUPPORTED` / `NOT_FOUND` means no WinUSB driver (an in-use radio, which
  gives `ACCESS`, is not flagged). The other problems are the hints of `usb_diag.h` (Linux permissions, a kernel driver that holds the radio,
  the SDRplay API, Aaronia RTSA, LimeSuite): a hint goes to the radio whose name it holds, and a hint without a radio (an AOR I/Q output, an
  Aaronia SPECTRAN) gets a line of its own. Matching: `hwinfo::usbRadioLines()` in `hardware_logic.h`.

The scan runs on the same worker thread as the media list. The section has its own **Copy to clipboard** button that copies only the
USB list (`hwinfo::usbReportText()`, the same text as the USB part of the full copy).
