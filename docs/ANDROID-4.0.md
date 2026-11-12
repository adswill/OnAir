# Android 4.0: TV picture and sound, more USB radios

Branch `feat40-android`. Two requests from the owner: watch TV on the phone, and drive the SDRs directly over USB.

**How this was tested.** No phone, no emulator and no radio were used (the laptop was on battery: no local builds). Everything
below was checked on GitHub only: the Kotlin JVM unit tests (`android.yml`, Kotlin check job), the native library build with the
NDK (`android.yml`, debug APK job), and the C++ USB tests (`quick.yml`). The CI results are at the end. Picture and sound on a real
phone, A/V sync by eye and ear, and every radio below on a real OTG port are **not verified**.

## 1. TV picture and sound

Before: the TV modes (DVB-T/T2, DVB-C, DVB-S/S2, ATSC, ATSC 3.0, ISDB-T, DTMB, ISDB-S) locked and showed their service tables, but
there was no picture and no sound, because FFmpeg is stubbed out of the Android build and OnAir must not ship patented codecs.

Now every TV mode has a **Watch** panel (first tab). OnAir ships no video codec and no Dolby or AAC codec: it demultiplexes the
transport stream and hands the elementary streams to the decoders the phone already has (Android `MediaCodec`). Which formats play
is decided by the phone (`MediaCodecList`), and the panel says so in plain words when it cannot.

### How it works

| Step | Where | What |
|---|---|---|
| Packets of one service | `android/native/src/jni_tv.cpp` | The engine's packet tap (`Engine::setPacketTap`, the same stream the desktop player gets) is cut down to the PIDs of the chosen service (picture, sound, PCR) into a 7.5 MB ring (~6 s of a 10 Mbit/s service). The app reads it with `TvNative.nativeTvRead`. Packets the app is too slow to take are counted ("lost before the player"). |
| Service list | `nativeTvServices` | The core's PAT/PMT/SDT/NIT/EIT snapshot (`Engine::tsSnapshot`): names, LCN, provider, now/next, streams with codec, language and scrambling. |
| PES | `tv/TsDemux.kt` | 188-byte packets to PES (13818-1 2.4.3.6): bounded and unbounded PES, adaptation-field stuffing, continuity counter (a lost packet marks the PES damaged, a duplicate is ignored), transport_error_indicator, resync on two sync bytes 188 apart, 33-bit PTS/DTS unwrapping. |
| Sound frames | `tv/AudioFrames.kt` | ADTS (header removed, AudioSpecificConfig built), LATM/LOAS (StreamMuxConfig parsed, payload copied bit-exact, HE-AAC explicit SBR/PS), AC-3 (frame size table per sampling rate), E-AC-3 (dependent substreams kept with their frame), MPEG audio layers I-III. Each frame gets the PTS of the PES it starts in, later ones follow by their duration. |
| Picture | `tv/VideoFrames.kt` | H.264 (SPS/PPS to csd-0/csd-1, picture size from the SPS with cropping, start on an IDR or an I slice for open-GOP broadcasts), HEVC (VPS+SPS+PPS, start on an IRAP picture, size from the SPS conformance window), MPEG-2 (sequence header, start on an I picture). A damaged picture stops the decoder feed until the next key frame. |
| Decoders | `tv/TvPlayer.kt` | MediaCodec in synchronous mode on one thread (`onair-tv`): the picture straight onto a SurfaceView, the sound through AudioTrack (mono, stereo, 4, 5.1 and 7.1 channels as the decoder reports them). A decoder that fails three times is switched off with a note instead of looping. |
| A/V sync | `tv/AvSync.kt` | The sound is the master clock (AudioTrack's play position mapped back through the PTS of each written block, so a gap in the sound does not shift the clock). A picture is scheduled at its time with `releaseOutputBuffer(index, ns)`, dropped when more than 80 ms late, held when more than 0.5 s early; a jump of more than 3 s (broken timestamps) is shown at once and counted. Without sound, a wall clock started at the first picture. If the sound runs more than 1.5 s behind the stream (clock drift between the broadcaster and the phone), frames are skipped to catch up. |
| Choice | `tv/TvServices.kt` | Services sorted by LCN, then id. The first clear TV service whose picture this phone can decode is started; then a clear TV service; then a radio service. Sound track: decodable first, then the phone's language (then English), then PMT order. Both can be changed by a tap; the choice is kept while the service exists. |
| MP2 | `tv/Mp2Decoder.kt` | MPEG audio layer II (most SD services in Europe) is rarely in a phone's MediaCodec. A small layer II decoder (MPEG-1 and the MPEG-2 half rates, stereo base layer) is in the app and is used only when the phone has none. Layer II is not covered by patents any more; the tables are the standard's values as FFmpeg keeps them (LGPL-2.1+, THIRD_PARTY.md). |

### What plays where (the codec-honesty notes)

| Format | Who decodes | On a typical phone |
|---|---|---|
| H.264 (AVC) video | the phone (MediaCodec `video/avc`) | every Android phone has a hardware decoder |
| HEVC (H.265) video | the phone (`video/hevc`) | most phones since about 2016; 10-bit HDR services depend on the phone |
| MPEG-2 video | the phone (`video/mpeg2`) | often missing (some vendors remove Android's software decoder). The panel says "This phone has no MPEG-2 video decoder, so there is no picture." The sound still plays. |
| AVS / AVS+ / AVS2 / AVS3 (China) | not handed to a decoder | Android has none: "no picture" with the reason |
| AAC, HE-AAC (ADTS or LATM) | the phone (`audio/mp4a-latm`) | every phone |
| AC-3, E-AC-3 (Dolby) | the phone (`audio/ac3`, `audio/eac3`) | only phones with a Dolby licence; otherwise "no sound" with the reason, and another track of the service is chosen if there is one |
| MPEG audio layer II (MP2) | the phone if it has `audio/mpeg-L2`, else OnAir's own layer II decoder | always plays |
| MP3 (layer III) | the phone (`audio/mpeg`) | every phone |
| AC-4 (ATSC 3.0), DRA (China) | not handed to a decoder | "no sound" with the reason |
| Scrambled (pay TV) | nothing | "scrambled (pay TV). OnAir does not descramble" |

The panel also shows the decoder names, picture size, sound format, counters (pictures shown, late or skipped, sound frames, sound
buffer, packets lost before the player, stream gaps) and a fixed line: OnAir ships no codec, the phone's own decoders do the work.

### Limits

- **Battery and CPU.** Demodulating wideband TV is the heaviest thing the app does. On the 3.7 emulator run DVB-T2, DVB-C, ATSC and
  ISDB-T used 1.3 to 2.7 cores of an Apple Silicon Mac, which is much faster than most phone cores, so **many phones will not keep
  up** (the picture then stutters and "lost before the player" or stream gaps climb). Hardware video decoding itself is cheap. Expect
  the phone to get warm and the battery to drain quickly; use a charger for long viewing. The mode list keeps its "heavy" warning.
- The picture and sound stop when you leave the Watch tab (TV in the background would drain the battery). The receiver keeps running.
- DVB-T2 with several PLPs: the service list is that of the PLP the receiver decodes (the desktop's PLP choice is not on the phone).
- No subtitles, teletext pages or EPG grid on the phone yet (the core decodes them; the panel shows now/next only).
- Interlaced SD: deinterlacing is whatever the phone's decoder and display do.
- Analogue TV and D2-MAC carry no transport stream: their Watch tab stays at "Waiting for the transport stream". The analogue picture is
  not on the phone yet (unchanged from 3.7).
- DMB video (DAB) and DVB-H/SH also feed the packet tap but are not offered on the Watch panel (it is shown for the TV group only).

## 2. USB radios

| Radio | USB id | Driver on the phone | State |
|---|---|---|---|
| RTL-SDR, all variants | **42 ids** (was 17): every entry of librtlsdr's `known_devices` list (osmocom rtl-sdr `src/librtlsdr.c`), e.g. Terratec DAB sticks 0ccd:00b4-00c6, KWorld/SVEON/ASUS 1b80:d39x, MSI 1d19:1104, MyGica 1f4d:d286 | the core's Rust RTL2832U driver over usbfs | Added ids: same driver, same chip; the plug-in dialog now appears for them too. |
| HackRF One, HackRF Pro, Jawbreaker, rad1o | 1d50:6089, 604b, cc15 | OnAir's libusb driver (`hackrf_usb.cpp`), descriptor wrapped | unchanged; the Pro shares the One's id and is told apart by its board id once opened |
| Airspy R2, Airspy Mini | 1d50:60a1 | libairspy `airspy_open_fd` | **fixed**: the APK build never included libairspy (android.yml did not set `AIRSPY_SRC`), so it was listed but "not available in this build". The workflow now builds it from a pinned airspyone_host commit (fc61ab6) that has `airspy_open_fd`; the v1.0.10 tag does not. |
| Airspy HF+, HF+ Discovery | 03eb:800c | libairspyhf `airspyhf_open_fd` | **fixed** the same way (airspyhf commit 24fe8ff; the 1.6.8 tag has no `airspyhf_open_fd`) |
| SDRplay | - | - | not possible: the SDRplay API is a closed binary service with no Android build |

The three tables (core `usbFdKindForId`, `UsbCatalog.kt`, `usb_device_filter.xml`) agree: `DeviceFilterTest` checks the filter against
the catalog, the new `UsbIdsAgreementTest` reads the core's table (now one `{vid, pid, kind}` table) and checks it against the catalog.

**What is verified about streaming through the descriptor path:** the RTL driver has differential tests against recorded register and
transfer sequences of the original librtlsdr (`rust/onair_usb/tests/golden`, `differential_against_original_librtlsdr`) and the URB
streaming tests; `test_usb_fd`, `test_usb_fd_open` (HackRF open-from-descriptor with a stub wrapper) and `test_usb_drops` ran on GitHub
(below). The Airspy libraries' descriptor path is the vendors' own code and was only compiled, never run: there is no Airspy test
double. No radio was streamed on a phone.

### Radios looked at and not added

| Radio | Why not (now) |
|---|---|
| LimeSDR Mini (0403:601f, FT601) | LimeSuite (Apache-2.0) opens radios by scanning the bus through libusb; it has no "open from descriptor" entry point, and a bus scan is not allowed on Android without root. Needs a LimeSuite patch (wrap the descriptor as libairspy does) and a LimeSuite Android build; could not be tested without the radio. |
| bladeRF (2cf0:5246, 2cf0:5250) | Same: libbladeRF (LGPL/MIT) has no descriptor entry point; its FX3 firmware/FPGA loading also needs a writable file path. Not added. |
| PlutoSDR over USB | The Pluto's USB network gadget needs the phone's kernel to bring up a USB Ethernet interface (vendor dependent), and OnAir's Pluto driver talks to it through libiio, which is not in the Android build (its network backend needs libxml2). A small IIOD (port 30431) client in the core would avoid libiio; not written in this round. |
| HydraSDR RFOne (38af:0001) | libhydrasdr (MIT) has `hydrasdr_open_fd`, so it is possible: the core's driver needs the descriptor path wired as for the Airspy and the APK build needs libhydrasdr. Left for later: its v1.1.4 release is days old and the build has many optional SIMD files to sort out for arm64. |
| RX888 MkII | USB 3 radio with a firmware upload that re-enumerates as another device (a second permission dialog); most phone ports are USB 2. Not added. |

## 3. Phone requirements

- Android 9 (API 28) or later, arm64 (as before). USB host (OTG) for radios; a powered hub or Y cable for HackRF and Airspy R2.
- TV: a phone fast enough to demodulate the channel (see the CPU note); hardware H.264/HEVC decoding (all current phones). Dolby
  sound only where the phone has the decoders.

## CI results

(filled in below by the runs of this branch)
