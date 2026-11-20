# OnAir 4.4

OnAir 4.4 adds Discord-requested features (DSC MMSI copy, an Inmarsat-C map, region modes, SYNOP weather reports, NXDN trunk grants, WiNRADiO G33DDC, an Iridium SBD view), a Tektronix RSA306 receiver, many more transmitter controls for DVB-T, DVB-T2 and DVB-C2, FM-ATV transmit, MIDI gain knobs, a loudness boost, a streamer mode, readable contrast and 24 px controls across the interface, ATSC 3.0 and GNSS fixes from Felbs, and the fixes for the bugs reported on Discord and GitHub since 4.3. Most of it was tested on generated signals only: read **Known limits** at the end.

## Downloads

| System | File |
|---|---|
| macOS 15 or later, Apple silicon | `OnAir-4.4.0-macos-arm64.dmg` |
| macOS 15 or later, Intel | `OnAir-4.4.0-macos-x86_64.dmg` |
| Windows 10 / 11, 64-bit | `OnAir-4.4.0-windows-x64-setup.exe` or `OnAir-4.4.0-windows-x64.zip` |
| Linux x86-64 | `onair_4.4.0_amd64.deb`, `OnAir-4.4.0-linux-x86_64.AppImage` or `onair-4.4.0-linux-x86_64-portable.tar.gz` |
| Linux arm64 | `onair_4.4.0_arm64.deb`, `OnAir-4.4.0-linux-aarch64.AppImage` or `onair-4.4.0-linux-aarch64-portable.tar.gz` |
| Android 9 or later, arm64 | `OnAir-4.4.0-android.apk` |

`SHA256SUMS` lists the checksum of every file. **Measured** numbers come from OnAir's own generated test signals, not from over-the-air recordings, unless a line says so.

## New features

- **DSC**: a "copy MMSI" button on the from, to and distress lines of a call (Discord).
- **Inmarsat-C**: EGC messages on a clickable map and a "Clear stations" button (Discord).
- **Region modes**: "Only my region" in the mode picker hides the modes not used in the country of your location (Discord).
- **RTTY 50 baud**: SYNOP and SHIP weather reports are decoded under the text, with a copy button (Discord).
- **Frequency database**: import of EiBi, HFCC and Aoki schedules with all fields (time, days, language, target, power, remarks) (Discord).
- **NXDN trunking**: the VCALL_ASSGN message of a Type-D control channel is decoded and becomes a trunk grant (the bit layout is not confirmed on a real system).
- **Analog TV**: a noise reduction control (Automatic, Off, Light, Medium, Strong), so a still picture is drawn without the slow build-up (Discord).
- **WiNRADiO G33DDC** receiver through G33DDCAPI.dll (Windows only; no radio was available, see Known limits), and an **Iridium SBD** view.
- **Tektronix RSA306** receiver through the RSA API (#557, datosg).
- **Decoding threads** in Settings > Advanced (#372).
- **Marine VHF**: the channel that is playing is named on the spectrum at its own frequency (#597).
- **Android in Simplified Chinese** for all interface strings (#559).
- **MIDI knobs** for HackRF LNA, VGA and overall gain, receive only; raw ALSA on Linux, WinMM on Windows, CoreMIDI on macOS (#543).
- **FM-ATV transmit** (23 cm, 625 lines, constant envelope, 8 MHz peak-to-peak deviation) in the analog TV test signal and the TX panel (#584).
- **Transmitter, DVB-T**: 4K (DVB-H), hierarchy with a low-priority code rate and a second test programme, cell ID, in-depth interleaver, time slicing and MPE-FEC bits. **DVB-T2**: PLP ID, T2 version 1.1.1 / 1.2.1 / 1.3.1, L1-post scrambling and the 1/3 and 2/5 code rates. **DVB-C2** is now on the transmit list, with rows for modulation and code rate, guard interval, data slice type, time interleaving, narrowband notch, short FEC frames and a second PLP.
- **Loudness boost** after the volume control (Settings > Audio), default +9 dB and up to +24 dB, with a soft limiter, for audio that stays too quiet at full volume.
- **Streamer mode** and a better channel list (UI audit).
- **Whole span on by default** for every mode family that offers it (pagers, DMR, P25, APRS, VDL2, HFDL, HF digital, CW, PSK and the other wide families). A saved choice per family still wins.
- **Voice scout tools** for analogue and digital voice discovery (#594, m6c7l).
- **GNSS carrier smoothing** of the pseudoranges, Hatch filter (#552, Felbs).
- **Remote API**: `GET /api/v1/metrics` returns Prometheus / OpenMetrics text (running, frequency, rate, gain, dropped samples, load, decoded messages).
- **Alt+letter** opens the menu bar menus (Alt+F File, Alt+O Mode, because Alt+M is mute) (#596), and "Add notch here" is in the VFO band's right-click menu (#568).

## Changes

- Dim text is lifted to a contrast of 4.5:1 and control outlines get their own colour at 3:1, in every palette; controls, rows and headers are 24 px high at scale 1 (UI audit).
- The AIS view says "vessels" instead of "ships" (#589); mouse wheel action labels follow the interface conventions (#562); the transcript window is easier to use (#563) (m6c7l).
- Row time-averaging of the waterfall is off by default; Settings > Display has "Waterfall time smoothing (seconds)", 0 to 2.
- The sources and tests compile with clang as well as GCC (#550, Peter De Witte).

## Fixes

- **TETRA**: a slot whose call set-up was not heard is shown as "encryption state unknown, muted" instead of "encrypted". New option "Play calls of unknown state" (off by default).
- **Analog TV, video file**: the static FFmpeg of the Linux and macOS builds lacked most video decoders and demuxers. VP8, VP9, MPEG-4, MJPEG, images, Matroska, AVI, Ogg and WAV were added, and a failed picture message names the codec. AV1 is still not supported.
- **ATSC 3.0** (Felbs): the channel estimate from the pilot comb was 1.18 times too large, and the LDM core layer with it (#556); a hopeless batch of Type B LDPC words no longer keeps the receiver waiting (#573); a live receiver that falls behind drops whole frames and goes on (#575); broken ALP packets and late components no longer stop the transport stream output for good (#576).
- A TS recording closed while the paced queue is behind keeps its packets in order (#551).
- The arrow keys on a frequency readout no longer also tune and step (#547); Clear empties the whole-span receivers too, and MeshCore has a Clear button (#579); no double free when a channel change aborts an Internet TV stream (#564); the Weather Fax Chinese label for "clear picture" (#591).
- The macOS and Windows build and test fixes of the release: the Windows charset converter used by Android's Big5 and GBK text, the G33DDC radio description, and the Full CI disk-image job.

## Contributors

Pull requests merged since 4.3: #548, #550 Peter De Witte (peterdewitte); #557 datosg; #562, #563, #589, #594 m6c7l; #551, #552, #556, #573, #575, #576 Felbs.

OnAir's own code carries one attribution term under GPL section 7(b) (see ADDITIONAL-TERMS.md): keep the notices and the line "Based on OnAir by Kristijonas Adomaitis - https://on-air.dev" in a program that contains it. Third-party material is credited in THIRD_PARTY.md and in the in-app credits. The full list of contributors is in CONTRIBUTORS.md.

## Coming next (not in 4.4)

MQTT commands and integrations in the headless server, 17 more transmit modes that already have a generator, the ADS-B multi-message work, the remaining pager and sensitivity improvements, and the rest of the transmit control plan.

## Known limits

Please read these before relying on a new feature.

- **Almost everything new was tested on generated signals only**, and some code was first built by the CI. Not tested on hardware: the WiNRADiO G33DDC driver (no radio was available), the Tektronix RSA306, HackRF over MIDI, FM-ATV on air, the DVB-T, DVB-T2 and DVB-C2 transmitter options with a real receiver, the NXDN trunk grant on a real system, the Android Chinese strings (machine-written, they need a native review).
- **FM-ATV** has no sound subcarrier and no CCIR 405 pre-emphasis yet.
- **AV1 video files** are still not supported in the analog TV video file input.
- **Tests**: some tests on the Windows CI shards have load-dependent timing flakes, and two long-form tests (`hfutil_hdreq`, `loraraw_survey`) fail on Linux arm64 in the nightly full run.
