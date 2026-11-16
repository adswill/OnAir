# OnAir 4.3

OnAir 4.3 adds no new receiver. It fixes the Android start crash of the first 4.2.0 APK (that APK was replaced on the release page), the Windows analog TV crash after 215 seconds, the ISM waterfall stall, the arrow keys that changed two digits, the digital video generator that sent a test tone instead of the video's sound, and a list of Android problems (IQ files, recordings, RSP1, FC0013, Chinese guide text, Airspy counters, xHE-AAC). It adds a manual notch filter, a live Google Earth track and a second NMEA output for the sonde, a reorganised Data feeds window, a new tuning marker, ISS SSTV on 437.550 MHz, DWD weather RTTY presets with the 85 Hz shift, a DVB subtitle track list, and an explanation when a voice mode is silent.

## Downloads

| System | File |
|---|---|
| macOS 15 or later, Apple silicon | `OnAir-4.3.0-macos-arm64.dmg` |
| macOS 15 or later, Intel | `OnAir-4.3.0-macos-x86_64.dmg` |
| Windows 10 / 11, 64-bit | `OnAir-4.3.0-windows-x64-setup.exe` or `OnAir-4.3.0-windows-x64.zip` |
| Linux x86-64 | `onair_4.3.0_amd64.deb`, `OnAir-4.3.0-linux-x86_64.AppImage` or `onair-4.3.0-linux-x86_64-portable.tar.gz` |
| Android 9 or later, arm64 | `OnAir-4.3.0-android.apk` |

**Measured** numbers come from OnAir's own generated test signals, not from over-the-air recordings, unless a line says so. Read **Known limits** at the end before relying on a new feature.

## Highlights

- The 4.2.0 Android APK aborted at launch on every phone (#544). It was replaced on the 4.2.0 release page with a fixed build, and 4.3 contains the fix.
- Windows: analog TV (and the other picture decoders using the same code) no longer crash after 215 seconds at 10 Msps (#520).
- ISM: the waterfall no longer stalls while bursts are decoded (#542).
- Manual notch filters set from the spectrum.
- Sonde: a live Google Earth track and a second NMEA output for the predicted course; a reorganised Data feeds window.
- ISS SSTV on 437.550 MHz with Doppler follow (#540), DWD weather RTTY presets and the 85 Hz shift (#539).
- DVB subtitle track list with language names (desktop lists and shows; Android lists only).
- Voice modes (DMR, P25, NXDN) say why nothing is heard.
- Android: IQ files can be opened, recordings are saved to Downloads by themselves, RSP1 works after the first stream, FC0013 tuners work, Chinese guide text is readable.
- Fixes from your reports (#520, #542, #544, #545, #546, #547, #503, #516, #517 and more).

## New features

- **Notch filters**: "Add notch here" in the spectrum menu and a Notches list in the Auto gain options. A bank of narrow band-stops (fixed in RF, or following the tuning) runs in front of the spectrum, the waterfall and the decoders. With an empty list nothing changes.
- **Sonde, Google Earth**: a live KML file (`onair-sonde-live.kml`, rewritten every 5 seconds) with the path drawn down to the ground, the burst point, the current position and the dashed predicted course. A small NetworkLink KML makes Google Earth reload it.
- **Sonde, second NMEA output**: sends the predicted rest of the flight (or the aim point) as GGA and RMC, one point per N seconds of flight every M seconds. The sonde map pointer now reads in km.
- **Data feeds window**: five places instead of one long list: Upload to websites, Send to programs, Share my radio, My station (callsign, location, own height) and Serial / NMEA output, each with status chips and a settings expander. The sonde "me / prediction" popup links to them.
- **Tuning marker**: a new marker for the tuned signal in the spectrum, drawn in the theme's accent colour: a slim centre line, a label tab that opens on hover or drag (mode and width, and the offset from the dial while dragging), a soft passband with edge lines, and grab handles on the movable edges. Hit testing, dragging and filter limits are unchanged. It is used by the general receiver and by the Simple tuner style of the other modes.
- **USB / LSB / CW filter width**: up to 20 kHz (it stopped at 6 kHz for USB and LSB and at 3 kHz for CW). Defaults are unchanged.
- **ISS SSTV (UHF)**: a preset on 437.550 MHz with Doppler follow (#540).
- **Weather RTTY**: the 85 Hz shift is added to the RTTY shift list (saved choices keep working) and there are DWD weather RTTY presets (#539). Auto RTTY does not search 85 Hz (see Known limits).
- **VHF DSC from a sound card or a WAV file**: the Marine receiver now takes the audio as already demodulated and slices the 1300 / 2100 Hz tones at any sound-card rate and level. Test recordings from the blackwellj/vhf-dsc project (MIT) are credited.
- **Voice modes, "No audio: why"**: DMR, P25 and NXDN calls that decode but are silent now show one line with the reason (a talkgroup hold from an earlier click, volume at 0, a sound device that failed to open, or a codec plug-in returning only silence) and a button that fixes it. A sound output that failed to open once is now retried.
- **Signal doctor**: no "raise the gain" advice when a strong signal (more than 20 dB over the floor) is in the passband, a new setting "Signal doctor opens by itself" (Settings > Decoding, on by default) and a pause of 60 seconds after you change the gain by hand. The doctor still opens when you ask.
- **VDL2 ground stations**: a station that reports the airports it covers is checked against OnAir's airport table (OurAirports, public domain); a decoded position more than 600 km from that airport is replaced by the airport's and marked in the map tip. Unknown stations are unchanged.
- **DVB subtitle track list**: tracks with language names, the subtitle type (and hearing-impaired naming), an Off entry and a remembered language. Desktop: named tracks in the subtitle menu and the subtitles are shown. Android: the tracks are listed, Off and the preferred language are kept; the phone does not draw subtitles yet and says "not supported yet".
- **Android**: IQ files picked in the file chooser can be opened (#546); finished recordings are copied to Downloads/OnAir by themselves (an option, on by default); the Airspy driver turns on packed 12-bit transfers on a phone and counts the samples libairspy drops (shown in the status line and as source-side loss); Big5, GBK and GB 18030 guide text for Hong Kong and mainland DTMB (#517).
- **Layout**: the waterfall stays inside a short panel, a lost waterfall texture is rebuilt, and Reset layout also resets the spectrum bar and its fold.

## Changes

- **Marine audio path**: the audio-as-demodulated path is used only when VHF DSC is the only Marine decoder; NAVTEX, weather fax and MF/HF DSC from a sound card keep their IQ (IF) behaviour.
- **Build without SIMD** (Peter De Witte, #548): the "AVX2 paths in use" line is guarded the same way as every other caller, so OnAir compiles with MSVC and clang-cl and with `-DDECT2_NO_SIMD=ON`.
- **Credits**: an entry for the RTL-SDR tuner drivers (FC0013) and for the VHF DSC test recordings.

## Fixes

- **#544, Android start crash**: the release APK aborted at launch on every device. The code shrinker renamed the methods of the xHE-AAC decoder that the native code looks up by name. Keep rules were added. The 4.2.0 APK on the release page was replaced with a fixed build (same version number, signed with the same certificate; install it over the old one).
- **#520, Windows ATV crash**: on Windows a `long` has 32 bits, so after 2^31 samples (215 s at 10 Msps, 107 s at 20 Msps) the picture decoder cut the line out of the signal at a wrong place and the receiver thread crashed. The picture, SECAM and sync code now use 64-bit sample positions, and a line that would reach outside its samples is skipped. The test `test_atv_gap` runs below, across and above 2^31 and with gaps (clean under AddressSanitizer).
- **#542, ISM waterfall stall**: decoding a burst (and the Sigfox receiver) ran on the thread that also feeds the waterfall, one call taking 100 to 240 ms. Bursts now go to a worker thread; for a radio the feed never waits, for a file it waits so nothing is lost. New test `ism_stall`.
- **#547, arrow keys**: every frequency readout (main, transmit panel) shared the keyboard digit and each took the same arrow key, so one press changed two digits. The keyboard digit now belongs to one readout.
- **#545, video generator**: with a video file as the input the digital video generator kept sending the 1 kHz test tone (only RTMP switched to the stream's sound, and the Sound row is under Advanced). A video file or RTMP now sends its own sound, picked when the input is chosen or a video is dropped.
- **#546, Android IQ files**: the file source reopened `/proc/self/fd/N`, which Android refuses for many providers ("Permission denied"); it now duplicates the open descriptor. Recordings are saved to Downloads by themselves (see New features). The FC0013 tuner (Fitipower, found in some RTL dongles) was recognised but refused (error -95); it is now driven by the Rust RTL-SDR driver, translated from librtlsdr's tuner driver.
- **#503, RSP1 on Android**: after the first stream the RSP1 stayed on its streaming alternate setting, so the next stream (mode switch, pause, relaunch) saw no samples until a replug. The interface is parked on setting 0 and the endpoint is cleared before every stream.
- **#517, Android DTMB**: guide text in Big5 and GBK came out as raw bytes (Bionic's iconv has none of them); a platform converter now runs first. Airspy dropped-sample counters were added (see New features).
- **#516, xHE-AAC on Android**: the decoder never drops output now, restarts on a codec error and logs its counters, which cured choppy sound. The AC-3 "needs a decoder" label is clear about what is missing. (#517 asked for the label.)
- **Waterfall vanishing until restart** (reported on Discord): fixed by rebuilding a lost view texture and keeping the waterfall inside a short panel.
- **Signal doctor** telling you to raise the gain with a strong signal present, and **voice modes silent without a reason**: see New features.
- **VDL2** ground stations with a damaged coordinate: see New features (reported by email).
- **NMEA, KML and Data feeds** work from the new Data feeds window.

## Contributors

Pull requests merged since 4.2:

- **#548** the build without SIMD (AVX2 line guard): Peter De Witte (peterdewitte).

OnAir's own code carries one attribution term under GPL section 7(b) (see ADDITIONAL-TERMS.md): keep the notices and the line "Based on OnAir by Kristijonas Adomaitis - https://on-air.dev" in a program that contains it. Third-party material is credited in THIRD_PARTY.md and in the in-app credits. The full list of contributors is in CONTRIBUTORS.md.

Thanks to everyone who reported the issues named above, to the Discord users who reported the vanishing waterfall, the silent voice calls and VHF DSC from a sound card, and to the reporter of the VDL2 ground station positions.

## Known limits

Please read these before relying on a new feature.

- **Almost everything new was tested on simulated signals only**: the notch filters, the sonde KML and NMEA outputs, the VHF DSC audio path (a synthetic call and the credited test recordings), the voice "No audio" reasons, the ISM and ATV tests and the VDL2 correction.
- **Not tested on hardware**: the FC0013 driver, the RSP1 reopen fix, the Android IQ file and auto-save, the Android EPG character sets and Airspy packing, xHE-AAC robustness, the Android subtitle list, VHF DSC from a real sound card, the 20 kHz USB filter in the GUI, the notch filter and the live sonde KML / NMEA in the GUI and in Google Earth, the DMR / P25 / NXDN audio reasons, the tuning marker animation, 85 Hz RTTY on a real signal and the ISS UHF preset.
- **Android has no subtitle renderer**: the subtitle tracks are listed and chosen, but nothing is drawn ("not supported yet").
- **AC-3 on Android** needs the phone's own decoder; without one the label says so.
- **Auto RTTY does not search 85 Hz**: the shift list has it, but Auto still searches the first five shifts only. Choose 85 Hz by hand.
- **MIDI and Hamlib IF output are not in 4.3.**
- **Detachable windows are not part of 4.3.**
- **Tests**: some tests on the Windows CI shards still have load-dependent timing flakes, as before.
