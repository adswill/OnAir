# OnAir 4.5

OnAir 4.5 adds an airband scanner that needs no channel list, a video enhancer and a letterbox option for analog TV, many QO-100 improvements, sonde uploads that run only while a sonde is decoded, DRM sound on Android, and fixes for the bugs reported on Discord and GitHub since 4.4. Most of it was tested on generated signals only: read **Known limits** at the end.


## Downloads

| System | File |
|---|---|
| macOS 15 or later, Apple silicon | `OnAir-4.5.0-macos-arm64.dmg` |
| macOS 15 or later, Intel | `OnAir-4.5.0-macos-x86_64.dmg` |
| Windows 10 / 11, 64-bit | `OnAir-4.5.0-windows-x64-setup.exe` or `OnAir-4.5.0-windows-x64.zip` |
| Linux x86-64 | `onair_4.5.0_amd64.deb`, `OnAir-4.5.0-linux-x86_64.AppImage` or `onair-4.5.0-linux-x86_64-portable.tar.gz` |
| Linux arm64 | `onair_4.5.0_arm64.deb`, `OnAir-4.5.0-linux-aarch64.AppImage` or `onair-4.5.0-linux-aarch64-portable.tar.gz` |
| Android 9 or later, arm64 | `OnAir-4.5.0-android.apk` |

`SHA256SUMS` lists the checksum of every file. **Measured** numbers come from OnAir's own generated test signals, not from over-the-air recordings, unless a line says so.

## New features

- **Airband, automatic** (#587): no channel list and no airport import are needed any more. With Auto on (the default) OnAir opens every carrier it sees keying up on the 25 kHz and 8.33 kHz raster, closes idle ones after 30 s (busy ones after 120 s, at most 24 at a time), keeps 121.500 MHz (and 243.000 MHz) always open, and puts a carrier that stays on for 90 s (ATIS, VOLMET) last in the scan. A carrier you can see on the spectrum can be clicked to listen at once, without adding it to a list. With a radio that cannot see the whole band, OnAir hops across 118 to 137 MHz: it stays while a transmission is open, comes back often to busy parts and rarely to dead ones. What it heard is remembered for the next start. Your own channel list still works and comes first.
- **Analog TV, video enhancer**: a new "Enhance video" switch with Off, Light and Full. Light is a motion-adaptive noise reduction; Full adds comb-artefact repair, edge-preserving noise reduction, chroma edge repair and sharpening. Output can be as received, 720p or 1080p. It runs on its own thread; Advanced has sliders.
- **Analog TV, letterbox**: a video file in 16:9 is no longer squeezed vertically; it is sent undistorted, with an automatic letterbox option (wbus).
- **QO-100**: the published narrowband band plan is drawn on the spectrum as named, shaded segments and shown in each signal's tooltip (with a note when a signal is too wide for its segment); signal level is shown against the middle beacon, so sessions can be compared; one plain sentence above the lock row says what is going on; LNB presets have plain names; the DATV table shows Es/N0 and a quality word (no signal, weak, strong enough to try, locked, good); the LNB error is remembered per oscillator and used from the start until the beacons lock; a click on a signal in the list picks CW or USB and puts the dial in the right place.
- **Balloon sondes** (#526): the APRS-IS uploaders (wettersonde.net, radiosondy.info) and the SondeHub station record connect only while a sonde is being decoded and disconnect when it goes quiet, so the receiver is not shown as active in DAB, TV or other modes. The end-of-flight prediction ignores a wrong or failed answer and asks more often when the sonde is falling below 3000 m.
- **Motorola Type II**: the Custom band plan has three ranges (base, mid, high) and is remembered after a restart.
- **Record rules**: an automatic IQ clip now includes the seconds before the trigger when the Rewind is on (#629).

## Changes

- The tests of the QA sweep no longer pass a mode that only locks when the mode is supposed to decode (integ-45-syn4).
- The web app tells a DAB panel's user that the station list and DAB audio exist only in the desktop and Android apps (#585).
- Mesh views (map, chat, packets, telemetry) show only the protocol of the entry you are in (#566).
- Kyodo (JJC) weather fax, ended 2026-09-22, is removed from the station list (#565).

## Fixes

- **Android, DRM**: AAC services have sound; the decoder of the phone is used because the Android build has no FFmpeg (#516, #638). Choppy xHE-AAC at 17.7 MHz is not fixed.
- **Android, USB**: the USB radio choice is no longer greyed out on phones without the USB host flag (#582); a SoapyRemote radio or a manual SoapySDR string lists its antennas (#619); a crash when the Mirics USB transfer mode was changed (#503).
- **General receiver**: choosing a tuning step or clicking Auto notch no longer jumps to Weather Fax at 7.793 MHz (#595); station names on the spectrum can no longer be clicked through a window in front of them.
- **Video file sound** no longer restarts from 00:00 after 120 s (#642, wbus).
- **DMR**: LOCN position text in a data message is found (#335). **FM**: a narrowed channel filter now narrows its edges too, so a neighbour 100 kHz away no longer leaks in (#583). **ATSC 3.0**: a service change no longer touches a freed route (#484; the stuck-service reports need the reporter's recording).
- **STANAG 4285** can lock on a transmission that is already running; stray start lines of APRS, Packet and ROS no longer fill the log of other modes.
- The DAB transmitter map no longer takes a click that belongs to a window in front of it.

## Contributors

Pull requests merged since 4.4: #629 and #642 (OnAir).

OnAir's own code carries one attribution term under GPL section 7(b) (see ADDITIONAL-TERMS.md): keep the notices and the line "Based on OnAir by Kristijonas Adomaitis - https://on-air.dev" in a program that contains it. Third-party material is credited in THIRD_PARTY.md and in the in-app credits. The full list of contributors is in CONTRIBUTORS.md.

## Coming next (not in 4.5)

MQTT commands and integrations in the headless server, 17 more transmit modes that already have a generator, the ADS-B multi-message work, the remaining pager and sensitivity improvements, and the rest of the transmit control plan.

## Known limits

Please read these before relying on a new feature.

- **Almost everything new was tested on generated signals only**, and most of it was first built by the CI. The airband scanner has not been tried on a real airband; its thresholds are only simulated. The Android DRM sound and the Android USB changes were not tried on a phone.
- **DRM at 17.7 MHz** (audio plus data) still plays badly (#516, #332).
- **Not fixed**: shortwave RTTY and Wefax staying at "starting" (#506), Tetrapol traffic (#408, #333), the Windows 11 wizard buttons (#590), RSP1 and MSi2500 on Linux (#561), ATSC 3.0 channel 21 (#484). They need the reporters' logs or recordings.
- **STANAG 4285** on a running signal was only tried on a simulated one.
- **Chinese strings** are not reviewed by a native speaker.
- **Tests**: some tests on the Windows CI shards have load-dependent timing flakes, and two long-form tests (`hfutil_hdreq`, `loraraw_survey`) fail on Linux arm64 in the nightly full run.
