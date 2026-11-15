# OnAir 4.2

OnAir 4.2 adds 1 new receiver, Kenwood FleetSync (receive only). It fixes Auto gain falling to the minimum after a strong burst, adds one slider per gain stage, makes the SPECTRAN V6 gain checked against the device (with the V6 ECO fixes from datosg's pull request #538), adds a new radiosonde type (GTH), a TETRA name book, a pager scan, live UDP / RTP input for the TV transmitters, one station location for the whole program, a Help > Hardware page and a one-window Report a problem with a privacy notice, and a long list of fixes from your reports. SondeHub upload is switched off in the app until SondeHub has validated OnAir's decoders (#526).

## Downloads

| System | File |
|---|---|
| macOS 15 or later, Apple silicon | `OnAir-4.2.0-macos-arm64.dmg` |
| macOS 15 or later, Intel | `OnAir-4.2.0-macos-x86_64.dmg` |
| Windows 10 / 11, 64-bit | `OnAir-4.2.0-windows-x64-setup.exe` or `OnAir-4.2.0-windows-x64.zip` |
| Linux x86-64 | `onair_4.2.0_amd64.deb`, `OnAir-4.2.0-linux-x86_64.AppImage` or `onair-4.2.0-linux-x86_64-portable.tar.gz` |
| Android 9 or later, arm64 | `OnAir-4.2.0-android.apk` |

**Measured** numbers come from OnAir's own generated test signals, not from over-the-air recordings, unless a line says so. Read **Known limits** at the end before relying on a new feature.

## Highlights

- New receiver: Kenwood FleetSync (fleet and unit number of each burst; receive only).
- Auto gain no longer drops to the minimum and stays there after a strong burst (RTL-SDR, Airspy and other radios with a gain index), and ADS-B and other pulsed modes no longer hold the gain low for 10 minutes after a close aircraft (#522). New tests cover both.
- Per-stage gain sliders (Airspy LNA / mixer / VGA, the RTL-SDR E4000 IF stages, USRP gain elements) under Gain > Advanced (#233).
- SPECTRAN V6: the gain is read back after every set, and the V6 ECO fixes found on a real device by datosg (#538).
- New radiosonde type GTH (Huayuntianyi HT03G / GTH3 and Changwang GTH6); own receiver height; NMEA output of the selected sonde.
- TETRA name book with a "Speaking now" line; pager scan "Find pagers"; HFDL ground-station table corrected.
- Live UDP / RTP transport-stream input for the digital TV transmit modes.
- One station location in Settings for the whole program (#453).
- Help > Hardware page (peterdewitte, #528) and a one-window Report a problem with a privacy notice before anything is saved (peterdewitte, #523).
- Android: SDR++ server as a network radio (#473), every plugged-in USB radio can be selected (#435), an "update available" banner (#183), xHE-AAC DRM sound through the phone's decoder (#516), a touch interface for narrow screens (#139).
- Fixes from your reports (#534, #522, #521, #517, #516, #514, #508, #507, #506, #533, #526, #249, #223, #214, #216, #239 and more).

## New mode

- **Kenwood FleetSync** (family 347): every FM carrier of the span is followed and the 1200 bit/s FFSK burst of FleetSync and FleetSync II is decoded (error correction in FleetSync II): fleet and unit number, the carrier it came on, a log and the raw words (docs/fleetsync.md). Receive only. Message type, status text and position are not decoded and there is no map. Format facts are from the public multimon-ng and GopherTrunk descriptions (credited in THIRD_PARTY.md). Simulated signals only.

## New features

- **Gain**: one slider per stage the driver lets you set by hand, with the step table, what each stage does and "Reset to overall gain". A stage set by hand turns Auto gain off and is remembered with the radio (#233).
- **SPECTRAN V6 / V6 ECO**: the gain slider (the reference level) is checked against what the device took and a refused or clipped value is reported in the start and retune message and the log; the gain control is written as Manual at every start; the slider and Auto gain have a radio profile of their own. A retune no longer waits behind the receive thread. From datosg's V6 ECO measurements (#538): wait until the device is ready, start again below 8 MHz, receive below the device's lowest centre frequency, reset between receiving and transmitting, a start without samples is cured by a reset, the ECO takes a new rate while it streams, the transmit span is never below 20 MHz (docs/SPECTRAN-V6.md, "Measured on a V6 ECO").
- **Radiosondes**: the new type GTH reads position, speed, time and, when the fields look plausible, temperature, humidity and pressure (2400 baud, 109-byte frame, CRC-16; layout from the public rs1729 decoder, GPL-3, credited). "Own height (m)" in the me / prediction panel goes into the SondeHub station record and the APRS-IS station beacon; the same value is used by the ADS-B position. A new optional "Send the selected sonde as NMEA" sends $GPGGA and $GPRMC once a second to a serial port, or TCP or UDP host:port (off by default; nothing is sent while the sonde has no position) (#214).
- **SARSAT**: "Save every burst as a baseband file" writes each beacon-like burst, decoded or not, as a 2-channel float WAV at 32 kHz with the carrier at zero, to the sarsat folder of the data directory; the whole-capture search has a channel cap (1, 2, 4, 8 or all 90 kHz finders) for slow computers (#214, #223).
- **Pagers**: "Scan the span for pagers" in the pager screens (wide captures) lists each carrier with the speed that decoded most pages (POCSAG 512 / 1200 / 2400, FLEX 1600 / 3200 / 6400) and its page count; click to tune (#269).
- **TV transmit**: a live MPEG-TS stream from UDP (`udp://@:1234`, a multicast group, or RTP) instead of a file, for OBS, ffmpeg or VLC; a jitter buffer with null-packet stuffing keeps the modulator fed and counters are shown in the transmit panel. DVB-T, T2, S, S2 (also QO-100), C, ATSC, ISDB-T and DTMB (docs/TS-UDP-IN.md).
- **TETRA name book**: a Names tab for your own names of terminals (ISSI) and talkgroups (GSSI), with notes, network, colour code and site. Names show in the slot lines, the Messages, Talkgroups and Map views, and a "Speaking now" line shows who is on the air in calls that are not encrypted. Import and export of a CSV (docs/TETRA-NAMES.md). The names stay in your data folder and are never shipped. The SDR# TTT file format is not supported.
- **HFDL**: the header, the aircraft list and the receiver panel show how many aircraft have a position.
- **Location**: Settings > Location holds one station position (latitude, longitude, height, locator, name). PSK Reporter, WSPRnet and Winlink follow it; a screen can still keep its own locator. Older saved positions are folded in once at start (#453).
- **Analog TV and FPV**: Brightness and Contrast sliders with a Reset button, applied to the picture on the screen only (#239).
- **Spectrum and waterfall wheel**: a setting for what the wheel does (m6c7l, #535), "Tune by 10 steps" as a wheel choice and a custom tuning step.
- **Maps**: a Clear button on the HFDL, VDL2 and ACARS maps; map colours in Settings > Network and updates (the default look is unchanged).
- **Presets**: 11 m / CB buttons for SSTV (27.700 MHz USB), JS8Call (27.245 MHz), ROS (27.635 MHz USB) and packet (27.235 MHz FM); the buttons only warn about local rules. LTE operator names Erillisverkot (Finland) 24445 and 24446.
- **Help > Hardware** (peterdewitte, #528): this computer, requirements, media support and USB radio drivers (docs/HELP-HARDWARE.md).
- **Report a problem** (peterdewitte, #523): the debug dump and the troubleshooter are in one window, and a privacy notice is shown before the dump or screenshot is saved.
- **DTMB / DVB text**: on Android TV the provider name from the SDT shows when the multiplex has no network name; the sound-track list shows every audio stream and says "not supported on this phone" when no decoder plays one; DVB subtitle, teletext and MHEG-5 / data-carousel streams are listed (the phone does not draw them yet). Unprefixed UTF-8 under a Chinese or Korean profile is read as UTF-8, and the GB 2312 default switches to Big5 when the time offset says Hong Kong, Macau or Taiwan (#517).
- **Data services**: the TV data sub-tab is named "Data services" and says that files are shown and interactive pages are not run (docs/MHEG-REDBUTTON.md lists what is not implemented).
- **Pack downloads** (such as the DAB transmitter lists) retry twice and say why one failed, with the address (#249).
- **Smart checks** follow Insights > Advanced > Analyse decodes; Clear findings also resets the FM watchdog.
- **Elad FDM-DUO**: there is no public IQ protocol, so no native driver. Its CAT port is found as a Hamlib rig (FDM-DUO model, then TS-480); IQ stays on its USB sound card (DEVICES.md has the recipe) (#251).
- **PACTOR**: the PACTOR-I screen names a PACTOR-II or PACTOR-III signal (carriers, speed level). It is not decoded: the coding is not public (docs/PACTOR-RESEARCH.md) (#216).
- **Simplified Chinese**: a complete, reviewed catalogue (thalorex, #531, #532).

## Changes

- **SondeHub upload is off in the app** (a note in the panel; saved settings are ignored) until SondeHub has validated OnAir's decoders (#526). The other radiosonde uploads (APRS-IS sites) are unchanged. Developers can use `ONAIR_SONDEHUB=1` and `ONAIR_SONDEHUB_LOG=<file>` to produce validation logs. The RS41 time is sent as the sonde's own GPS time (docs/SONDE-UPLOAD-3.9.md).
- **HFDL ground-station table** compared with the UDXF "Digital aero" list (version 51) and the dumphfdl system table (version 52). Frequencies added: Molokai, Shannon, Johannesburg, Barrow, Muan, Agana. Corrected: Muan, Krasnoyarsk, Molokai, Barrow. Removed: Canarias 5589 (in neither list). A station can now hold 20 frequencies (it was 8; Barrow has 19), and the band table gained a 3.0 MHz entry and wider 3.455 and 4.67 MHz entries.
- **Inmarsat**: the Aero P channels are ground to aircraft, so an aircraft's own position reports are not heard from the ground; the mode text and docs say so.
- **Codec setup** names the exact file, the folders searched and how to get the plug-in when a row is missing, and reports a near-miss file name (#533).
- **Weather fax**: the frequency button of the Next broadcasts list is always whole.

## Fixes

- **#522, Auto gain**: a strong signal that keyed up cut the gain by up to 30 dB; the weak wanted signal then vanished under the ADC noise, the loop read that as "the signal went away" and waited 10 minutes at the minimum, again on every burst. A cut that leaves the wanted channel invisible is now treated as an out-of-band overload and the gain climbs back at once. For ADS-B and other pulsed modes, the counters read zero after a cut for a very close aircraft; the idle wait is no longer used for them. New tests: `autogain_collapse` (nine radio models, normal and keying signals) and a 1-hour test with a strong aircraft every 4 minutes on the RTL-SDR and HackRF models.
- **#534** FT8, FT4, WSPR and the other sideband-only HF digital entries fell to narrow FM on 6, 4 and 2 m; Auto now means upper sideband for them at every dial.
- **#507** The DRM panel has an SW 49 m (5.9 to 6.2 MHz) band button.
- **DRM channel filter** (reported by email): with the filter at 10 kHz the spectrum still drew a 20 kHz box. The box now shows 10 or 20 kHz as chosen, or, with the filter off, the occupancy the receiver found.
- **#514** A click or short drag on the bandwidth slider that ended over the spectrum could tune the radio to the frequency under the mouse. The spectrum now tunes only when the press began on it. Found by reading the code and the report log; not reproduced with a radio.
- **#506** Shortwave RTTY Auto also finds 200 and 450 Hz shifts (weather RTTY such as DDK9 is 50 baud / 450 Hz), and a half-fixed baud and shift pair no longer falls back to 45.45 baud / 170 Hz silently. The reporter's fixed-setting case is not explained.
- **#508** Inmarsat all-carriers: opening or closing one window no longer restarts the receivers of the other windows (it dropped Aero locks, half-received multi-block messages and the Inmarsat-C frame search on a busy band). An Inmarsat-C receiver is also started for carriers 0.9 to 1.5 kHz wide. **#533** The codec-missing message is clearer (see Changes).
- **#521** The ADS-B map full screen works in the Simple view (peterdewitte, #525).
- **#517** Android DTMB provider names, track lists and Big5 for Hong Kong (see New features).
- **#516** DRM xHE-AAC sound on Android through the phone's own decoder (see Known limits).
- **#435** Android: the radio screen lists every plugged-in USB radio and can select it; generic RTL-SDR sticks are named by their chip family, not always R820T.
- **#249** DAB TII and other pack downloads say why they failed instead of "error 22".
- **#526** SondeHub: see Changes.
- **Weather fax**: Stations table fix (see Changes).
- **Smart checks** hide when analysis is off.

## Contributors

Pull requests merged since 4.1:

- **#538** SPECTRAN V6 / V6 ECO fixes from a real V6 ECO: datosg.
- **#523** Report a problem (privacy notice before saving), **#528** Help > Hardware, **#525** ADS-B map full screen in the Simple view: peterdewitte.
- **#535** a setting for the spectrum and waterfall wheel: m6c7l.
- **#531**, **#532** Simplified Chinese catalogue: thalorex.

OnAir's own code carries one attribution term under GPL section 7(b) (see ADDITIONAL-TERMS.md): keep the notices and the line "Based on OnAir by Kristijonas Adomaitis - https://on-air.dev" in a program that contains it. Third-party material is credited in THIRD_PARTY.md and in the in-app credits. The full list of contributors is in CONTRIBUTORS.md.

Thanks to everyone who reported the issues named above, to the Discord user who asked for FleetSync and the HFDL table check, and to the reporter of the DRM channel box.

## Known limits

Please read these before relying on a new feature.

- **Almost everything new was tested on simulated signals only**: Kenwood FleetSync, the GTH sonde frames (no recording of a GTH sonde was available; the GTH6 PTU fields of the 2026 batch are not confirmed), the pager scan, SARSAT export, RTTY shifts, Inmarsat position decoding, the HFDL table (checked against two published lists, not on the air), the TS-UDP input (loopback sockets) and the Auto gain tests.
- **Kenwood FleetSync**: message type, status text and position are not decoded and there is no map. The CF-06-AH sonde frame is not decoded.
- **Not tested on hardware**: the SPECTRAN V6 beyond datosg's V6 ECO (the full-size V6 and the gain read-back run against a fake RTSA API only), and the Auto gain fix on a real radio.
- **Android code is compiled by the CI only; nothing was tried on a phone**: the update banner, the USB radio list, the SDR++ server dialog (and its mDNS discovery), the DTMB provider and track lists, and the touch layout. **xHE-AAC DRM sound** (#516) needs an xHE-AAC decoder in the phone (Android 9 or later) and has not been run with a real decoder: buffer timing, output format and which phones accept the configuration are untested. Without a decoder the DRM Audio card says so.
- **NMEA output of a sonde** was checked against a file and a UDP socket on localhost, not a real COM port. SondeHub upload is off in the app.
- **TETRA name book**: the screen was only syntax-checked; the SDR# TTT file format is not supported yet (a sample is needed).
- **One station location, Brightness / Contrast, touch layout**: the screens were only syntax-checked; their logic tests had not yet run on CI when written.
- **DRM channel box** was checked by test and code reading, not on a live DRM signal.
- **PACTOR-II / -III** are named, not decoded. **Data services**: interactive MHEG-5 pages are not run, and Hong Kong DTMB weather and stock pages are not verified (no real stream).
- **Elad FDM-DUO**: no native IQ driver; CAT through Hamlib and IQ through the sound card only.
- **#506** The exact cause of the reporter's "starting" line with fixed settings is not confirmed. **#514** was not reproduced with a radio.
- **Detachable windows are not part of 4.2.**
- **Tests**: some tests on the Windows CI shards still have load-dependent timing flakes, as before.
