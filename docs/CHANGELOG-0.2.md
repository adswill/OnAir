# Changelog 0.2.6 to 0.2.8

Moved from the README. 0.2.8 added the KA family: see [KA-FAMILY.md](KA-FAMILY.md). From 3.0 on each release has its own `CHANGELOG-<version>.md`.

<details open>
<summary><b>Show the 0.2.8 changes</b></summary>

- **22 new modes** (142 in all): the **KA family** (KAAL, KAPP, KAFT, KAVB, KAIT, KAWS, see below), eLoran / Loran-C, MPT1327, LTR and EDACS trunking control channels, Yaesu Fusion, a military HF waveform identifier (identify only), ARDOP, GOES DCS, Mode A/C, a BLE tracker detector, AM stereo (C-QUAM), DGPS beacons, SITOR, DME, FSQ and IFKP. Several are marked *in development* in the app and here: they have only been tested on generated signals.
- **Voice you install yourself:** TETRA voice (unencrypted calls only) and Yaesu Fusion voice through a user-installed codec plug-in, for example one built from [mbelib-neo](https://github.com/arancormonk/mbelib-neo). Nothing patented is bundled; see [docs/CODECS.md](CODECS.md).
- **Easier to use:** a command palette (Ctrl+K), a get-me-listening wizard, four guided tours, workspaces with crash restore, a recording library with re-analyse, a Report a problem dialog with a local diagnostics bundle, a typography choice (Current, Clean, Compact) and a larger-touch-targets option.
- **Receiver and display:** the demod mode shown and settable in the FFT panel, with a name search in the mode list (#212); waterfall export as PNG frames and a timelapse; a Solar and Jupiter burst monitor; FM tone control and a warm preset; C-QUAM stereo; VHF DSC channel 70; a toolbar button that says Memory; contact lines between stations on the FT8 and JS8 maps.
- **Satellite dishes:** a dish alignment assistant (beacon level meter and look angles), LNB calibration from a beacon, a rotator north-offset calibration, a LEO Doppler monitor (detect only) and a SatNOGS transmitter browser (in development).
- **Radios:** DXpatrol SDR (RTL2832U based, HF direct sampling, #220); PlutoSDR no longer contacts network Plutos you did not add (#215); the Linux RX888 start and a failed LimeSDR probe now name the device (see the release notes).
- **Fixes:** GOES-19 HRIT differentially coded (NRZ-M) frames decode, HD Radio long station names and per-service logos, HaLow 2 MHz sub-channel scan and vendor elements (#192, #167), LTE / NR scan filters (#164), ADS-B altitude colours and display options, spectrum analyser resolution, DVB-T2 HEVC software fallback (#203), Android USB (#201), Meteor-M Doppler retune (#199), a DAB crash (#210), CDR audio recovery (#138, #121), a mesh clock that keeps running through a receiver reset (PR #217, thanks @ffishman).
- **DVB-S / S2:** carrier survey, symbol dump and DVB-RCS / DVB-CID work, in development.
- The full list, with issue numbers and known limits, is in the release notes.

</details>

## What is new in 0.2.7

<details>
<summary><b>Show the 0.2.7 changes</b></summary>

- **About twenty new modes** (120 in all): Wireless M-Bus, tone decoders, COSPAS-SARSAT 406 MHz, MT63, Z-Wave, meteor scatter, analog FPV video, Wi-Fi HaLow, a P25 trunking follower, e-Czas, a working LBJ decoder, and KiwiSDR as a network radio.
- **New tools:** [band survey](band-survey.md), a [message database](messages.md) with full-text search, [watchlists](watchlists.md) with ntfy / Telegram / webhook alerts, a [time machine](time-machine.md) (a rolling memory of raw IQ), a pass scheduler, a [Doppler orbit fit](doppler-orbit.md), [self-calibration](self-calibration.md), [station health](STATION_HEALTH.md), an [interference hunter](rfi-hunter.md), HF propagation and GNSS navigation views, a [hydrogen line toolkit](hydrogen-line.md), [passive radar](passive-radar.md), an [RF camera](rf-camera.md) and a [protocol lab](protocol-lab.md).
- **TV and DAB:** a [TV recorder](tv-recorder.md) (programme guide grid, scheduled recording, timeshift, subtitle export) and a DAB programme guide with XMLTV export.
- **Plugin SDK** ([docs/plugins.md](plugins.md)): add a mode or action as a plugin file without rebuilding. **Headless server** ([docs/headless-server.md](headless-server.md)): `onair-server` runs OnAir without a window, controlled from a browser. **Raspberry Pi** ([docs/RASPBERRY_PI.md](RASPBERRY_PI.md)): arm64 install and which modes fit (untested on real hardware).
- **Audio and signals:** an [audio cleanup chain](audio-cleanup.md), [auto gain](auto-gain.md) with per-device tables, live translation to English ([docs/TRANSCRIBE.md](TRANSCRIBE.md)), [live rooms](live-rooms.md), auto-labelled spectrum, a calibrated signal identifier, a weather picture studio, [offline data packs](PACKS.md) and [sound card input](SOUND-CARD.md) for narrowband modes.
- **Interface:** a [touch interface](FEATURES.md), full-screen maps, an airband airport search with an On the air tab, a redesigned Transmit panel, a new logo.
- **Android:** direct USB for RTL-SDR radios. The APK is signed with a new permanent key, so **uninstall 0.2.6 first**; later updates install normally.
- **Fixes:** DVB-T2 16K extended pilot table (the 642 MHz capture decodes), TETRA no longer calls clear cells encrypted, HFDL strong bursts, DRA+ bit rate, mode switch while running, rtl_tcp greeting wait, Airspy HF+ late callbacks, accented paths on Windows.
- **Under the hood:** the C++ protocol layers were replaced by Rust; **Rust is now required to build** (see [Build from source](BUILD.md)).
- Most new modes and tools have been tested on synthetic signals only; the full list is in the release notes.
</details>

## What is new in 0.2.6

<details>
<summary><b>Show the 0.2.6 changes</b></summary>

- **[Android app](INSTALL.md#android-app):** the same decoder core with a phone interface (receive only).
- **Hamlib rig control and rotators:** drive CAT radios such as an Icom IC-R8600 or IC-7300 (frequency and mode follow in both directions), and aim an antenna rotator.
- **A real-time map of positions** (aircraft, ships, APRS, mesh nodes, radiosondes and more), with offline map files, a receiver-range plot on the ADS-B and AIS maps, and the public [map.on-air.dev](https://map.on-air.dev).
- **DRM data services:** slide show, broadcast website, programme guide (EPG), TPEG and Journaline. DAB and DRM share the same MOT object handling.
- **Whole-span decoding** for pagers (POCSAG, FLEX) and APRS / packet: every channel in the captured span at once.
- **TV:** teletext pages and DVB subtitles, recording with pause and live buffer, Whole span for DVB-T/T2 channels and DVB-S/S2 transponders from one wide capture (docs/WHOLESPAN-TV.md), user-set channel logos, DVB-S2X VL-SNR frames, DTMB LDPC on the GPU.
- **ADS-B:** feeds from other receivers, offline registrations and countries, range statistics, metric vertical speed.
- **FM:** a Mono switch for weak stations, a saved-station library, a scan with full RDS.
- **Scroll to tune:** the wheel (or a trackpad) steps the frequency field digit by digit.
- **Share my radio:** an rtl_tcp server for the radio OnAir is running.
- **More radios:** Fobos SDR, RX888 MkII (native USB 3 driver), RFNM, Red Pitaya, Pluto+ Ethernet, SpyServer, SDR++ server, Hermes Lite 2.
- **Transmit** of test signals into a dummy load, with the modes offered listed in [docs/TRANSMIT.md](TRANSMIT.md).
- **New modes** since 0.2.5: Tone decoders (CTCSS, DCS, DTMF, selcall), Wi-SUN FAN, e-Czas (Polish time service on 225 kHz), UAT 978 MHz, VDL Mode 4, TETRAPOL, Elektro-L, LBJ (Chinese railway warnings), COSPAS-SARSAT 406 MHz beacons, Wireless M-Bus, Z-Wave, MT63, FSQ, IFKP, meteor scatter, DECT, ANT+ sensors, NB-IoT and 5G NR cells, MAC, DVB-C, MeshCom, Q65 and the other WSJT modes, and more (120 modes in all).
- **Interface:** Light and High contrast themes, an accent colour, colour maps and scroll-back for the spectrum, a General receiver with up to 4 channels and MP3 / FLAC recording, a one-button **Record audio** in the Source panel (next to Record IQ) that saves the sound you hear in any mode to WAV, FLAC or MP3 (optionally only while there is sound, one file per transmission), **Monitor audio** (Source panel): hear the tuned channel of any mode, also the decoders that have no sound of their own, as USB / LSB / narrow FM / AM, on a chosen output device, a second device (a virtual audio cable for fldigi, WSJT-X, multimon-ng) or a local TCP stream (off by default, 127.0.0.1 only unless you allow other computers; see docs/AUDIO.md), a "Save bug report" zip, translations (German, French, Spanish, Chinese, Lithuanian, Hungarian, Japanese, Polish, Dutch, Portuguese, Italian, Russian).
- **Docker image** that shows the OnAir GUI in a browser, published with each release.
- **mbelib is no longer included** (patented AMBE/IMBE vocoders): see [Good to know](GOOD-TO-KNOW.md).
</details>
