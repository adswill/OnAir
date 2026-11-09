# OnAir 3.7

3.7 follows 3.6.0. This list is written from the git log since the v3.6.0 tag (195 commits on the 3.7 branch), from docs/PATHWAY.md and from the per-feature documents named below. It names only what the log and those documents show. The mode count (191, was 178 in 3.6) is generated from the mode list in the app by tools/release/gen_changelog.py.

**Measured** numbers come from OnAir's own generated test signals, not from over-the-air recordings, unless a line says so. Read **Known limits** at the end before relying on a new decoder.

## New modes

- **NOAA APT** (family 260), **NOAA HRPT** (261), **NOAA GAC** (262) and **NOAA DSB** (263), **Meteor HRPT** (264), **Elektro-L RDAS** (265), **Arktika-M RDAS** (266), **Arktika-M GGAK** (267, experimental) and **AWS L-band** (268). Pictures grow line by line in the panel and are saved as PNG. The Passes planner knows the new satellites, "Follow the next pass" arms tracking, and the picture saves itself when no new lines come for 90 s. Formats and parameters follow SatDump (GPL-3.0); the code was written for OnAir, and the notices are in the files, THIRD_PARTY.md and the in-app credits. Details: docs/SATDUMP-TIER1-3.7.md. Elektro-L LRIT and HRIT gained the 294 ksym/s and 1.15 Msym/s link choices.
- **Inmarsat BGAN** (family 245): carrier search over 1525 - 1559 MHz, forward bearers and their signalling (bulletin board, beam, satellite, network, return schedule) from ETSI TS 102 744. Receive only. It never parses per-terminal messages (they carry subscriber identities), has no user data or voice path, and does not decode IsatPhone (GMR-2 class carriers are only labelled). Details: docs/BGAN-3.7.md.
- **Sub-GHz sensors** (family 241): the rtl_433 pulse detector, slicers and 377 decoder entries (GPL-2.0-or-later, vendored in third_party/rtl_433 with its notices), a live device list with history graphs, an unknown-signal analyser that writes a flex decoder, custom decoders, and CSV / JSON / MQTT-style export. Receive only. It decodes the protocols rtl_433 lists and only those. Details: docs/SENSORS-3.7.md.
- **Sferics (lightning)** (family 243): a strike, flash and per-minute counter from the IQ of any radio, with 50 / 60 Hz mains impulse trains rejected. It shows when and how strong, not where. Details: docs/SFERICS-3.7.md.
- **Thread** (family 244): 802.15.4 beacons, MAC headers, 6LoWPAN and MLE in the clear; encrypted frames stay marked encrypted (no key is asked for or recovered). **Zigbee sub-GHz**: 868.3 MHz and 906 - 924 MHz BPSK, with channel presets. Details: docs/THREAD-ZIGBEE-3.7.md.

## Share my radio with one button

- **Server > Share this radio**: one panel that starts the web server, gets a public link, and shows a link and a QR code. The link comes from the OnAir relay when it answers, otherwise from a free Cloudflare tunnel (the helper is downloaded once, with your OK). Everything else sits under Advanced: radio and range, decoders and limits, audio quality, allowed and blocked frequencies, access (open, password, invite links; passwords are stored hashed), who may tune (everyone, members, a time-limited control slot, nobody), a transmit opt-in that is off by default, and sharing automatically when OnAir starts.
- **List on web.on-air.dev** shows exactly what will be sent and lists the radio after one confirmation.
- Listener list with Kick and Block 24 h, blocked addresses, relay health in plain words.
- The browser page got a control-slot button and a listener count. An end-to-end test of relay, directory, server and browser was added.
- The relay service itself is not hosted yet, see Known limits. Docs: docs/ONAIR-WEB.md, docs/HOST-A-RECEIVER.md, docs/ONAIR-WEB-RELAY.md, docs/ONAIR-WEB-DEPLOY.md.

## Android

- The mode list has every row of the desktop table: **183 rows, was 108**. Generated results cards and tables cover 107 more standards, and the mode list generator reads the real mode table again (it had stopped reading the right file, so 75 modes never reached Android).
- Fixed: the receiver froze for good about once in 3 to 15 mode changes (a lost engine command, possibly the "frozen" reports in #201); wrong panels on many modes; a crash path when Android refused the foreground service type; volume now reaches every receiver that plays sound.
- Picking a mode starts it (Settings can turn that off), larger text, sample rate and radio settings under Advanced, a USB radio that becomes ready replaces the test signal, and a battery-exemption offer.
- Tested on an Android 15 emulator only, see Known limits. Details: docs/ANDROID-3.7.md.

## Auto gain

- Reworked against a new test harness: every radio gain model (RTL-SDR, HackRF, Airspy, SDRplay, Pluto, USRP, rtl_tcp, SpyServer) against five mode classes, 11 scenarios each. 40 of 40 cells failed some limit before, **5 of 40 still miss one** (listed in Known limits).
- Main changes: the decoder's quality and the signal level are judged separately; a settled loop backs off to 15 or 30 minutes instead of probing every 30 s; an overload from outside the channel is told apart from one inside it; the gain comes back after an interferer leaves; the gain holds on idle and bursty channels; a VFO step inside the span no longer restarts the search; gain steps no longer re-tune the LO on HackRF, RTL-SDR, Soapy radios, Pluto and USRP (that broke the stream); wide captures and ADS-B got their own rules.
- Auto gain never touches the transmit gain. The Auto control is on top of the gain section, the sliders under Advanced. Details and the per-cell table: docs/AUTOGAIN-3.7.md.

## Smarter decoding (all measured on generated signals)

- **FM**: a census of every station in one capture (PI, name, stereo), AF follow that proves the PI after each retune, DX alerts for foreign stations, click repair (+1.3 dB audio at 10 dB C/N), a first-adjacent canceller (+12 dB audio with a +20 dB neighbour), keyed RDS combining (+5.7 % groups under fast fading, no wrong names). Details: docs/SMART-fmam-3.7.md.
- **AM and shortwave**: an AM census with the medium-wave raster found by itself, a fade predictor, frequency diversity (7.0 dB against 4.3 / 3.4 dB for the channels alone), and a station estimate that always says "estimated" and uses only your own schedule file.
- **FT8, FT4, FT2**: cross-slot soft combining (about 2 dB on repeated messages), QSO-aware a-priori decoding, early decode about 4 - 5 s before slot end; **RTTY auto lock** (speed, polarity and tone place right in 40 of 40 test cases at 15 dB). docs/SMART-ft8-3.7.md.
- **Pagers and packet**: positions found in page text (UTM / MGRS checked to 5 m on 400 random points), text repair of lost code words (0 wrong guesses in the test), an AX.25 digipeater path graph, and a demodulator vote that recovers +14 % frames at 1 - 5 dB with no wrong frames. Medical-looking pages are not analysed unless you ask. docs/SMART-pager-3.7.md.
- **Digital voice**: soft-decision AMBE frame error correction with concealment for DMR, D-STAR and NXDN (garbage frames reaching the codec 17.2 % to 1.9 % at the test noise level), heard-ID repair, trunk call de-duplication across sites, control-channel health with auto park, a TETRA SDS / LIP parser (parser only, not wired to the receiver), a talkgroup name directory. A symbol equaliser exists but is **off by default** (no net gain through the real FM front end). docs/SMART-voice-3.7.md.
- **Mesh and LoRa**: one topology model for Meshtastic, MeshCore, Zigbee, Thread and Z-Wave (busiest relays, single points of failure), and a wideband LoRa survey that finds and classifies several signals in one capture. docs/SMART-mesh-3.7.md.
- **Maritime**: a distress incident board joining DSC, AIS and NAVTEX, an AIS anomaly engine, ALE link quality, HFDL frequency learning and Iridium Doppler matching. docs/SMART-maritime-3.7.md.
- **Inmarsat Aero**: carrier acquisition integrates longer at low SNR, about 1 dB more sensitive on 600 and 1200 bit/s (docs/INMARSAT-3.7.md). **FANS-1/A CPDLC** messages are decoded (vendored libacars tables, MIT) and shown in the VDL2 and ACARS / Aero details.
- **Iridium**: data slots (checked by Reed-Solomon and zero-sum) are no longer sent to the voice plug-in, lost frames leave silence of the right length, and a call with no sound says why. Lowering the detection threshold was measured and not adopted (no decodable-frame gain). docs/IRIDIUM-3.7.md.
- **DMR, NXDN, TETRA**: a clear call no longer flips to "encrypted" or swaps IDs from a single decode (found by a bug hunt; DMR now needs two agreeing embedded link controls).

## Ease of use

- **Auto setup, Reset this mode and a next-step line** in every mode panel; one-click **POCSAG + FLEX**; military UHF / VHF AM band presets; a colour intensity slider and a spectrum ghost overlay.
- **Recording library** with tags, notes and search; a shared list of recent voice clips with retention.
- No more 200 / 64 aircraft and station walls: entries expire by age with a user setting. AIS country and MMSI type from the MID, with a country filter. Sensor history graph with pinned sensors.
- Live RDS control and TS network output (UDP, RTP, HTTP); SSDV from a WAV recording; ESP32 / nRF24 module radio over USB serial (untested on hardware); Hytera Basic and Enhanced Privacy keys that you supply; an external FFmpeg folder setting.
- Digital video generator: easy mode, a Preview tab, hardware encoders that are listed also start (peterdewitte).
- GPU use for ATSC 3.0 LDPC, ISDB-T Viterbi and a Metal FFT kernel, a "Use GPU where available" tick with a per-mode override, a table-driven ATV noise blend. Audit: docs/GPU-AUDIT-3.7.md, docs/PERF-3.7.md.

## Fixes and pull requests

- **#439** analog TV from a file now plays against the signal clock, not at decoder speed. **#454** DVB-T2, DTMB and DVB-C transmit muxes are padded to the channel's real bit rate. VDES ASM moved to channels 2027 and 2028 (161.950 and 162.000 MHz). Mode Builder crash and wheel zoom limits fixed.
- **Radios and drivers** (#450, #420, #367, #340 / #337): RTL-SDR tunes before setting the rate (the Blog library had switched an R820T into direct sampling for a moment on open); Pluto falls back to 2.1 Msps with software resampling when the AD9361 refuses a lower rate; the Windows package could carry an old librtlsdr without V4 Lite support and now rebuilds and checks it. SDR++ server and SpyServer samples that are NaN or infinity no longer poison the filters. KiwiSDR handshake and error messages. Not tested on the real radios named in those issues.
- Pull requests merged: **#444**, **#445**, **#446**, **#467** (peterdewitte: video generator easy mode and Preview, Windows camera and microphone by name, hardware encoders, LoRa APRS transmit), **#447** (bellx2: RTL-SDR on macOS reads and converts on separate threads), **#461** (DME-3: Mode Builder FSK acquisition near zero lead-in), **#437** (BI8SYN: Chinese UI fonts, 6.8 MiB and consistent across desktops).
- Discord reports fixed: squelch control, radio switch, menu restore, auto gain, wide DMR grid, fax fit.
- Not merged: #448 (shared FFmpeg libraries; delay loading adds risk, and 3.7 has an external FFmpeg folder setting) and #414 (what we could use went into 3.6).

## CI and tests

- A whole-matrix auto gain harness (`ctest autogain_matrix`), per-feature tests named in each docs/SMART-*-3.7.md, a web end-to-end test, a rtl_tcp chunking test, an Android all-modes smoke script, a CI check that the generated Android mode list is current, long FM sweeps moved to the slow tier.

## Contributors

Code and fixes in this release from **peterdewitte** (Peter De Witte), **bellx2** (Ryu Tanabe), **DME-3** (Dimitri Garbi) and **BI8SYN**, with thanks to everyone who reported the issues named above. The DecSAT-derived work from earlier releases keeps its credit to **Arclamp**. The full list is in CONTRIBUTORS.md, which is generated from tools/contributors.tsv.

## Known limits

Please read these before relying on a new feature.

- **Most new decoders were tested on simulated signals only.** Validated on real recordings: **NOAA APT** (4 s of a public recording, 5 lines), **NOAA HRPT** (public baseband, 23 frames, plausible AVHRR channels) and **Meteor HRPT** (public baseband, 384 CADUs, plausible MSU-MR channels). NOAA GAC, NOAA DSB, Elektro-L RDAS, Arktika-M RDAS and AWS are synthetic only (no public recordings found). The same holds for Sub-GHz sensors (rtl_433 families rebuilt as IQ from pulse trains, no live capture), Sferics, Thread, Zigbee sub-GHz, the Z-Wave route reading and the smart FM / AM / FT8 / pager / voice / mesh / maritime features.
- **Inmarsat BGAN has not been validated on a real signal.** No public recording of a forward bearer was found. It is built from the published ETSI specification and generated signals. The F80T1Q-1B bearer's interleaver and mapping tables are not published, so that bearer is identified but not decoded.
- **Iridium silent-WAV fix is unproven** without a real recording: it is tested with generated bursts and a stub plug-in. The voice plug-in is not shipped.
- **Auto gain was tested on a simulated front end only; no radio was attached.** 5 of the 40 matrix cells still miss a target: HackRF FM (retune takes 13.5 s against a 12 s limit, and the bursty-channel quality is 2.0 dB under the best against a 1.5 dB limit), SpyServer FM (settles 17 dB under the best after a retune), HackRF wide capture after a retune to an empty band, Airspy wide capture, and Airspy ADS-B to wide mode switch.
- **Android was tested on an emulator only**, with the built-in test signal: no phone, no real radio, no USB OTG, no thermal or battery behaviour. The wide TV modes (DVB-T2 / T / C / S / S2, ATSC, ATSC 3.0, ISDB-T, ISDB-S, DTMB) will usually not keep up on a typical phone. Missing on Android: TV picture and sound (FFmpeg is stubbed out), DAB+, DRM, HD Radio and CDR audio, digital voice (no import screen for codec plug-ins), a picture view for weather satellites, fax, SSTV and analog TV, a channel scan (#435), and Mode Builder, Plugins and Internet TV (listed but not selectable). Full list: docs/ANDROID-3.7.md.
- **WebOnAir uses temporary Cloudflare links** until the relay is hosted: `relay.on-air.dev` does not exist yet, so the link changes every time you share. A real browser over the internet, a Raspberry Pi and a real certificate have not been tried.
- **Arktika-M GGAK is experimental**: the existing GGAK receiver finds the carrier but never frame-locks on its own test generator, so Elektro GGAK may share the fault.
- **Elektro-L LRIT and HRIT rate variants** (294 ksym/s, 1.15 Msym/s OQPSK) were not tested on any signal, synthetic or real.
- **DRM issues #331, #332 and #423 are not fixed.**
- Other things found open while writing this: #319 (hang after long DTMB reception) has no fix; the soft-decision BCH for Iridium, TETRA SDS wiring, FLEX text repair, first-adjacent cancellation for HD Radio and PSK / Hell auto lock are not done; a symbol equaliser for DMR is off by default.
