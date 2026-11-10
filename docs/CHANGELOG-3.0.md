# OnAir 3.0

3.0 comes after 0.2.9. The next versions are 3.1, 3.2 and so on. This text is built from the project notes (docs/PATHWAY.md), the git history and the status tables of the work branches. Generated 2026-10-23 from pub/integ-30-final by tools/release/gen_changelog.py.

**Experimental** means a mode or feature that has only been tried on signals OnAir generated itself. Nobody has confirmed it on a real transmission yet, so it may fail on the air. **Measured** numbers below come from OnAir's own test signals (noise, carrier offset, clock error, echoes, fading), never from over-the-air recordings, unless the line says so.

## In short

- **14 new modes**, 158 in all (NFC, SBAS, ASTERIX, Argos, Sigfox, Marine VHF voice, ISDB-S, P25 Phase 2, Horus, LoRaWAN and LoRa satellites, and more), plus a **Mode Builder** and a **Signal Lab** for protocols that have no mode yet.
- **Better weak-signal decoding** in FT8/FT4/FT2, PSK, RTTY, DAB, DRM, RDS, DVB-S2, DVB-T/T2, satellite weather pictures, GNSS, ADS-B, AIS, pagers and digital voice.
- **Lighter and faster**: less CPU in the common receivers, much lower idle power, a faster FFT, a quicker DVB-T.
- **A calmer interface**: one status strip, one signal indicator for every mode, an Audio pop-up in every mode, auto gain for every radio, a rebuilt first-run setup with lessons, presets, a merged Scanner, a Scheduler, Backup and restore.
- **Voice you can follow**: one voice monitor for all digital voice modes, keys you supply yourself, and user-installed codecs. OnAir still ships no patented codec.
- **Network**: a completed Remote API, OnAir Web (listen from a browser), a LAN swarm and a self-registering directory of OnAir Web servers.
- **Raspberry Pi fixes** for the CM5 and for OpenGL 3.1 systems, and many fixes from your reports (list at the end).

## New modes

New in the mode list since v0.2.9 (14, from app/ui/mode_table.cpp):

| Mode | Group | Family number |
|---|---|---|
| TV (auto) | Tv | 0 |
| ISDB-S (BS) | Tv | 154 |
| Marine VHF | Maritime | 155 |
| Argos | Satellite & Navigation | 152 |
| Digital voice (auto) | Two-Way Radio | 9 |
| P25 Phase 2 | Two-Way Radio | 180 |
| NFC / RFID 13.56 MHz | Iot & Sensors | 148 |
| Sigfox | Iot & Sensors | 153 |
| SBAS | Satellite & Navigation | 150 |
| ASTERIX | Aviation | 151 |
| LoRaWAN (all channels) | Iot & Sensors | 200 |
| LoRa satellites | Satellite & Navigation | 201 |
| PSK demodulator (experimental) | Satellite & Navigation | 202 |
| PACTOR-I (experimental) | Hf Utility | 203 |

The mode list also has Mode Builder and Plugins (not counted as modes).

What they do, in short:

- **NFC / RFID 13.56 MHz** (receive only): ISO/IEC 14443 Type A and B, ISO/IEC 15693, UIDs, APDUs and NDEF text and URLs. It needs an HF receiver running at 2.4 Msps or more. A transmit side exists behind the Arm gate and a 13.553 to 13.567 MHz check; it has only been run as a dry run in tests. *Experimental.*
- **SBAS** (WAAS, EGNOS, MSAS, GAGAN, SDCM, BDSBAS): the correction messages, decoded without a GPS lock. *Experimental.*
- **ASTERIX**: radar track data (CAT 021, 048 and 062) read from the network or a file and shown on the shared map. It needs no radio.
- **Argos** (401.650 MHz platform uplinks, issue #256) and **Sigfox** (uplinks, receive only). A3 and Argos-4 bursts are only listed. *Experimental.*
- **Marine VHF voice**: channels with regional plans (INT, EUR, BAL, USA, CAN), a priority watch, a scan over the whole span, a call log and a recording per call. A DSC call that names a channel opens this mode on that channel.
- **Long-range and satellite AIS**: four channels at once (AIS 1 + 2 and the long-range 75 + 76, message 27). A narrow radio hops between the pairs.
- **ISDB-S (BS)**: Japanese BS satellite TV through an LNB, rebuilt on ITU-R BO.1408-1. *Experimental.*
- **P25 Phase 2** (TDMA): calls, talkgroups and sites. EDACS can follow analogue voice channels. *Experimental.*
- **Horus balloons**: Horus Binary v1 and v2 and Horus RTTY telemetry, a map with tracks, a payload table and an altitude chart. v2 LDPC, v3 and Wenet are not included. *Experimental.*
- **LoRaWAN (all channels)** across a regional plan, and **LoRa satellites** with Doppler tracking. The satellite presets are unverified. *Experimental.*
- **ROS** is recognised by the signal identifier (identify only; the protocol is not public, so it cannot be decoded).
- **MHEG-5**: a structure parser and a carousel viewer (file list, save all, preview). No real MHEG-5 stream was available, so nothing is rendered from a real broadcast.
- **PACTOR-I monitor** (receive only; PACTOR II to IV are proprietary and are not decoded). *Experimental.*
- **Mode Builder**: describe a digital protocol (modulation, line code, sync, framing, FEC, CRC, fields) in a JSON definition and decode it, with no code.
- **Signal Lab**: blind analysis of an unknown digital signal (burst finder, modulation family, symbol rate, line code, framing). It can export a Mode Builder definition.
- **DRM+ and HD Radio AM** audit and fixes.
- **Iridium**: voice calls through a user-installed plug-in (`onair-codec-iridium`), plus ACARS, packet and pager views (issue #281).
- **TETRAPOL voice and polarity** (issues #241, #273): the frame coding now follows PAS 0001-2; polarity is found by itself; voice needs the user-installed `onair-codec-tetrapol` plug-in and is only played for calls whose own signalling says key index 0.
- **ISM sensors** get 315, 345 and 390 MHz buttons and 16 more rtl_433-derived decoders.
- **X-Midas BLUE** IQ files (read, record, write).

## Better decoding (measured on generated signals)

Every number in these tables was measured with OnAir's own test signals and test programs. None of it is confirmed on an over-the-air signal. "Before" is the 0.2.9 behaviour, "after" is the 3.0 work branch named in the line. A table appears here only once its branch is merged; until then it is listed under "Not in this build yet".

### Amateur and HF

| What | Before | After | Where measured |
|---|---|---|---|
| FT8, 50% decode point (8 stations a slot, SNR in 2500 Hz) | -19.8 dB | -21.0 dB | dec-hf-030, test hfdig_ftxsens |
| FT4, 50% point | -15.0 dB | -16.3 dB | same |
| FT2, 50% point | -11.8 dB | -13.0 dB | same |
| Crowded FT8 slot (100 overlapping stations, -20 to 0 dB), decodes | 83 | 97 (old decodes all kept, 0 false decodes on 60 noise slots) | same |
| JT65, 50% point | -24.0 dB | -24.4 dB | same |
| PSK31/63 threshold | 8 dB | 5 dB | same |
| PSK31/63, 10% character errors (31 Hz bandwidth) | about 10 dB | about 8 dB | same |
| RTTY 45/170, 5% character errors (3 kHz bandwidth) | about -3.5 dB | about -6 dB | same |

WSPR (about -30 dB) and JT9 (-26 dB) were already at the published bar and did not change. A comparison with ft8_lib on the same recordings gave a 50% point near -18.8 dB and 39 of 100 decodes in the crowded slot. WSJT-X, JTDX and fldigi themselves were not run. The deeper FT8 stage costs more CPU: a crowded slot takes about 6.6 s instead of about 4.6 s.

### Broadcast audio

| What | Before | After | Where measured |
|---|---|---|---|
| DAB+ threshold, plain noise | 3.8 dB | 2.9 dB | dec-dab-030, test dab_sens |
| DAB FIC threshold | 2.4 dB | 1.6 dB | same |
| DAB single-frequency network, 0 dB echo | 5.5 dB | 5.0 dB | same |
| DAB FIC in a vehicle channel (TU6) | 7.0 dB | 5.5 dB | same |
| DAB+ time to first audio at 10 dB (median) | 0.61 s | 0.47 s | same |
| DAB band scan: time spent on an empty channel | 2.2 s | 0.7 s | same |
| DAB, 4.5 dB and +-100 ppm sound clock error | no audio | no audio lost | dab-audit-030, docs/DAB-AUDIT.md |
| DAB, 0 dB echo inside the guard, silence per minute | 5.2 s | 0 | same |
| DAB, impulsive noise, FIC blocks received | 74% | 100% | same |
| DRM, 0.3 s holes every 15 s, dropout per minute | 8.8 s | 0 | drm-audit-030, docs/DRM-AUDIT.md |
| DRM, 5 s holes, dropout per minute | 34 s | 17 s, no restarts | same |
| DRM, 16-QAM at 9 dB, frames lost | 4.3% | 1.2% | same |
| RDS, 90% of blocks right, stereo FM | 19.5 dB | 15 dB | dec-audio-030, docs/FM-AUDIT.md |
| RDS, 90% of blocks right, mono FM | about 22 dB | 15 dB | same |

On the same IQ the RDS decoder was about 6 dB ahead of redsea. The DAB audio work (clock error followed inside a frame, FFT window under echoes, fades no longer end the lock, lost USB blocks recovered quickly, impulse blanker, a concealing playout with a 0.2% rate control) is in docs/DAB-AUDIT.md; the DRM work (hold through fades, Doppler from pilots, a playout that follows the sound card clock within +-300 ppm) is in docs/DRM-AUDIT.md. DAB with the 24 kHz MP2 frame size (issue: garbled audio on a user recording) now cuts frames by their headers, and scrambled stations are no longer played.

### Satellite TV, terrestrial TV and satellites

| What | Before | After | Where measured |
|---|---|---|---|
| DVB-S2 QPSK 1/4, distance from the standard's Table 13 (frame error rate 15%) | +0.1 dB | -0.3 dB | dec-satTv-030 |
| DVB-S2 QPSK 1/2 | +0.2 dB | -0.1 dB | same |
| DVB-S2 QPSK 3/5 | +0.5 dB | 0 dB or better | same |
| DVB-S2 8PSK 3/5 | +0.3 dB | 0 dB or better | same |
| DVB-S2 32APSK 4/5 | +0.2 dB | 0 dB or better | same |
| DVB-S2 short frames, QPSK 3/5 | +0.3 dB | 0 dB or better | same |
| DVB-T2 L1 signalling, QPSK 1/2 threshold | 5.0 dB C/N | 3.0 to 3.5 dB | dec-dvbt-030 |
| DVB-T2 8K 16-QAM 1/2 in a vehicle channel, Doppler | 5 Hz | 20 Hz | same |
| DVB-T, 2K 16-QAM 1/2 in a vehicle channel, Doppler | 40 Hz | 100 Hz | same |
| DVB-T on plain noise | - | 1.0 to 2.0 dB lower threshold | same |
| DVB-T, 0 dB echo at 0.9 of the guard interval | not found | decodes | same |
| Meteor LRPT QPSK 72k | 4 dB Es/N0 | 2 to 2.5 dB | dec-sat-030 |
| Meteor LRPT OQPSK 72k | 5 dB | 3.5 dB | same |
| Meteor LRPT OQPSK 80k | 6 dB | 4 dB | same |
| MetOp AHRPT | about 4 dB | 2 dB | same |
| GNSS, satellites with decoded frames (3 seeds, 34 to 40 dB-Hz skies, 45 s) | 48 | 57 | same |
| GNSS cold search with the radio offset remembered | about 45 s | about 10 s | same |

The DVB-S2 numbers are distances from the standard's table (EN 302 307-1) for the whole receiver on generated signals; the first packet locks after 0.35 to 0.6 s at 3 dB above quasi-error-free at 1 to 2 MBd. The DVB-S2 LDPC change is a corrected min-sum with a factor measured per MODCOD (all 55 S2X included); the Vulkan version stays plain and the CPU retries. GNSS also writes RINEX 3.04 observation and navigation files; an independent single-point solution from them lands within about 20 m. GNSS acquisition still needs about 30 to 31 dB-Hz and, without a remembered offset, the 64 ms search still waits about 45 s.

### Data modes, voice modes and pagers

| What | Before | After | Where measured |
|---|---|---|---|
| ADS-B | - | 2 to 2.5 dB more sensitive | dec-data-030 |
| AIS, 90% point in 48 kHz | 2.6 dB | 1.5 dB | dec-data-030, test_ais_sweep |
| dPMR, 50% point | 14 dB | 7 dB | dec-voice-030 |
| NXDN, 50% point | 10 dB | 5 dB | same |
| P25, 50% point | 8 dB | 6 dB | same |
| D-STAR, 50% point | 9 dB | 7 dB | same |
| NXDN48 at 3 dB, blocks decoded of 176 | 21 | 143 | same |
| POCSAG/FLEX, right pages at 0 dB (of 216) | 48 | 66 (76 with the later soft repair) | same |
| POCSAG/FLEX, false pages at -2 dB | 23 | 10 | same |

The pager repair compared with multimon-ng on the same signal gave 56 against 29 pages at 0 dB and 119 against 62 at 2 dB, with 0 to 2 false addresses where multimon-ng printed 6 to 20. LoRa (raw): at Semtech's published limits SF7 to SF12 decode 80 / 83 / 97 / 97 / 97 / 100% of frames, and 100% two dB above; no change was needed. An AIS-catcher comparison could not be made (it decoded nothing from OnAir's generated files).

### Other decoding work (already in the build)

- Signal identifier: new measurements (tone spacing, constellation guess, OFDM carrier spacing, chirp width) and a verify step. On 30 generated cases it names the right mode in its top three 87% of the time at 15 dB SNR and 93% at 25 dB; the older table-only test went from 39% to 52% top-1 (docs/IDENTIFIER.md).
- Whole-span detector: per-cell noise floor, drifting carriers keep one id, every wide-bank mode replays the half second before a carrier was found. New wide modes: MPT1327, Yaesu Fusion, LTR, EDACS. Test: 8 of 8 carriers found, 0 false.
- APRS and packet: six slicers per tone pair, a made-up TNC test bench (offsets, baud clock error, deviation, pre-emphasis, twist, collisions, 9600 G3RUH), and Emphasis and Fix-bits settings. No GitHub issue was a real decoding bug; the reports came from Discord and a sample is still wanted for the 12 kHz filter case.
- Analogue TV: NICAM-728 and A2 stereo sound, a "show snow" option, the carrier tuned at the centre of the capture (issues #239, #73).
- TETRA: choose the listening carrier and timeslot (first clear voice, slot 1 to 4, talkgroup or ISSI), talkgroup and ISSI of clear calls from D-SETUP, a hardened path against repeated audio (issue #267, not reproduced on air).
- Weak WEFAX signals (narrow filter before the discriminator), SELCAL on HF voice channels (USB, LSB, AM), the RWM time signal, GNSS dropped-satellite log (#189), COSPAS-SARSAT presets (#214) and Meteor LRPT rows placed by on-board time over lost lock (#79).
- DTMB TV sound in DRA through the user-installed DRA add-on (#266), and AVS+ video through an FFmpeg add-on you build yourself. Neither is bundled.
- TV: HD and SD pairing (HD checkbox), a guide language choice (#268), a live service list that follows the multiplex, and a "scrambled" badge only when the content really is scrambled.
- Faster, more sensitive Pager, ham and voice decoders were tested against guards that fail if a later change costs sensitivity or adds false decodes.

## Faster and lighter

| What | Before | After | Where measured |
|---|---|---|---|
| DVB-T 8 MHz receiver, CPU seconds per signal second | 0.675 | 0.404 (-40%) | perf2-dvbt-030, docs/PERFORMANCE-DVB.md |
| DVB-T2 32K 256-QAM | - | 21% less CPU | same |
| Whole engine, 8 MHz DVB-T at 10 Msps | 79% of a core | 55% of a core | same |
| Portable FFT on Linux and Windows (arm64) | - | 2 to 2.8 times faster, same bits | perf2-core-030, docs/PERFORMANCE-CORE.md |
| Wide-bank carrier mixer | - | 66% less CPU | perf-dsp-030, docs/PERFORMANCE-DSP.md |
| FM receiver | - | 31% less | same |
| General NFM / AM / SSB | - | 27% less | same |
| TETRA | - | 23% less | same |
| LTR / EDACS front end | - | 45% less | same |
| DVB-T search / ATSC search | - | 33 to 50% / 21% less | same |
| Idle wake-ups of the share, feeds and similar threads | 49.7 a second | 7.0 a second | perf-power-030, test power_idle |
| Analysis thread wake-ups | 400 a second | 30 to 210 a second | perf-ui-030, docs/PERFORMANCE.md |
| RF camera picture uploads | 70 MB/s | 9 MB/s | same |

- A covered window on macOS draws at most 10 frames a second with no GPU work, a window on battery draws at 30, a background receiver at 10 (video keeps 60). View > Power has Low power mode and Save power when hidden. A performance overlay (View > Performance overlay, or `DECT2_PERF=1`) shows frames, CPU per thread and wake-ups.
- All of it is measured on a Mac (and on Linux where the notes say so). On a Raspberry Pi or a Windows laptop on battery it has not been measured.
- DVB-T/T2 and DVB-S2 LDPC on the GPU also runs on Linux through Vulkan (issue #39): the Metal kernels were ported. It passes a bit-exact test against the CPU on a software Vulkan driver and on Metal; no real Linux GPU has run it.
- Faster scan of a wide span ("Fast scan" in the Scanner): FFT channel power per hop with a per-bin noise floor, hit tracker and recording of hits from the wide capture. HackRF's own sweep mode is not used, and hop times on real hardware are not measured.

## Spectrum, waterfall and tuning

- **Waterfall stays under its frequencies** after a click-retune, also when paused, and flush plots are as wide as the waterfall (issues #270, #262, and a Discord report).
- FFT up to 256k bins, waterfall detail up to 64k (issue #243); a **noise blanker** before the spectrum and the decoders (issue #264); more colour maps and markers; a drag on the waterfall past the band retunes like one on the spectrum.
- **Waterfall speed** down to one row per 30 s and per minute, averaging of the rows and clock-time labels (issue #237).
- **3D waterfall** from the waterfall pop-up (the history as a height field). The merged version was written without a finished screenshot check, so look at it before trusting it.
- **Spectrum looks**: "OnAir classic", "SDR++ style" (new default) and "Crisp", with 1-2-5 tick labels that never clip, one point per pixel and a reference-matched look. The research behind it is in docs/SPECTRUM-RESEARCH.md.
- **Tuning**: settings are saved a short time after the last tuning step instead of on every step. IQ file playback can tune by an offset while the spectrum stays on the file centre, and an rtl_tcp radio can run at any sample rate you type (0.1 to 40 Msps, "advanced") with a throughput meter that says when the stream is slower than asked.
- **Click to decode** no longer moves the radio when a gain change fires (issue #236).
- **Docking**: View > Docking > Use docking (Ctrl+Alt+D, Cmd+Option+D on macOS) turns the screen into a dock space; drag a title out to float it. It is off by default. Windows on another monitor need Dear ImGui's multi-viewport branch and are not part of 3.0 (issue #242).
- **Layouts per mode** are remembered (View > Reset layout).

## Using OnAir

- **One status strip** with labelled cells: FREQUENCY, MODE, RECEIVER, STATE with a lamp, CPU as a level bar, and a mute at the right edge.
- **One signal indicator** for every mode (a four-segment bar with No signal / Weak / Locked / Good, safe for colour-blind users) in the strip and at the top of the Receiver tab, a 30 s CPU sparkline in the CPU cell, and a one-click fix. **Clean** and **Compact** fonts are choices in Settings; the current look stays the default.
- **Audio** pop-up in every mode: per-mode volume, squelch (level / SNR / CTCSS / DCS with hysteresis and tail), high and low pass, 5-band EQ, notch and auto notch, AGC, de-emphasis, stereo, clarifier or CW pitch, a clip meter, an audio spectrum with click-to-notch, a per-mode output device (docs/AUDIO.md).
- **Auto gain** for every radio and mode (Auto, Optimise once, Safe start) with radio gain tables, a stage planner and ADC headroom meters; amplifier and bias tee are never switched without your permission (docs/AUTOGAIN.md).
- **First-run setup** in six steps (what do you want to do, which radio, which antenna, which region) and **lessons** that point at the real control and check that you did it. Help > Setup and lessons.
- **Automatic standard detection** for TV and for digital voice: pick "TV (auto)" or "Digital voice (auto)" and OnAir finds the standard from the spectrum or from frame syncs (docs/AUTODETECT.md).
- **Presets** per mode (tuning, gain, audio and the mode's own options) with import and export (docs/PRESETS.md). **Modes shown**: hide the modes you never use, mark favourites.
- **Scanner** is one view built on the memories (scan list, groups, priority check, live CTCSS/DCS reading), with the new **Fast scan**. **Scheduler** (times, repeats, cron-like, satellite passes) and a recordings manager. **Datasets** manager for frequency lists, airports, band plans and the downloadable packs. **Backup and restore** of settings, memories, presets and more (secrets only with a passphrase).
- **Frequency (ppm) calibration** wizard (Tools > Calibrate frequency).
- **Search** over decoded messages (Ctrl+K), shareable frequency lists (CHIRP, SDR++, SDR# XML, plain CSV) and GeoJSON export.
- **Time display**: UTC or local everywhere (View > Time display). Pager and tone message tabs can save to a text file with daily rotation.
- **Ham view**: one operating screen with tabs (decode list coloured by new DXCC / band / grid, QSO panel, log, awards, map, spots), more modes (PSK125/250, QPSK, every MFSK/THOR/DominoEX/Hell/Olivia/Contestia variant, JS8 transmit, MT63/FSQ/IFKP, SSTV send, CW keyer), rig control and a safety layer (country band plans, duty limits, an audit log, a hard stop on a lost rig). Share ham contacts on the public map is off by default and has its own consent text.
- **TX studio** (a signal bench with plots and measurements, no radio) and cleaner transmit signals in every mode: the old generators of several modes were far too strong or clipped (for example the Codar, Datawell, FSQ, IFKP, CW demo and ALE demo outputs, and PSK31, JS8 and MT63 demo), which are fixed. Details in docs/TX-OPTIMISATION.md.
- **Tools**: an antenna designer (thin-wire method of moments, docs/ANTENNA.md), a reworked propagation tool, ionosphere from your own receptions (kept on your computer), WEFAX auto-save gallery, image enhancement for every picture mode (slant, noise, colour; non-destructive), a DSC ship list import and map, receive beamforming (delay-and-sum, MVDR, LCMV) for coherent arrays.
- **Keys**: the Voice keys window (Tools) manages keys you add yourself, with an OS keychain provider and a locked/unlocked state.
- **Translations**: about 220 entries were added to each of the 12 catalogs. They are machine-made and not reviewed by a native speaker. Portuguese is split into Brazil (pt) and Portugal (pt_PT) (issue #280). A review pass is planned after the text freeze.
- **GUI audit**: every screen was checked against the interface rules (first-use window sizes fitted to small displays, bare pixel widths scaled, untranslated strings, an ImGui cursor error). Details in docs/GUI-AUDIT.md.

## Voice (you install the codec)

OnAir does not ship patented speech codecs. The voice modes play sound once you put the matching plug-in (`onair-codec-<id>`) in the codecs folder. A **Voice codecs** page in the Help menu shows what is installed; the full rule is in docs/CODECS.md.

- **One voice monitor for every digital voice mode** (DMR, P25 phase 1, NXDN, dPMR, D-STAR, Yaesu, M17, TETRA): one call at a time across slots and carriers, allow, mute and priority lists, hold, squelch on the bit error rate, per-mode volume and output device, per-call WAV and JSON recordings and a history search. The same strip is in every voice tab, and Help > Voice monitor opens it in a window.
- **Voice keys you supply yourself** (P25 ADP, DES, AES-256; NXDN scrambler, DES, AES): the key store is encrypted on disk, shows a legal notice before first use and applies a key only to a call whose own signalling names it. There is no key search, recovery or cracking, TETRA's TEA ciphers are not implemented, and DMR vendor privacy is left at detect-and-skip. Without a matching key the call is reported and skipped as before (docs/KEYS.md).
- **Codec folder search**: plug-ins are now found in the folder next to the program, in a folder you name, in `ONAIR_CODEC_DIR` and in the user folder (issue #279, a Windows install in C:/Radio).
- xHE-AAC for DRM through a libfdk-aac you install yourself is found in the codecs folder, and the DRM Audio line says whether it is loaded, not installed or failed, with the reason (issues #266, #247).
- New plug-in slots: `onair-codec-iridium` (Iridium voice), `onair-codec-tetrapol`.
- Changes in what the listen controls do: TETRA "Listen to" (first clear voice, slot, talkgroup, ISSI), DMR "Listen to" (first heard, slot 1, slot 2), FM audio options (de-emphasis off, forced stereo, 12/8/5 kHz audio bandwidth, SNR squelch; issue #213).

## Radios and platforms

- **Auto gain tables** for HackRF, RTL-SDR (R820T/R828D, Blog V3, V4, V4L and E4000), rtl_tcp, Airspy, Airspy HF+, SDRplay, Pluto, Lime, USRP, bladeRF, Fobos, RX888, SpyServer and KiwiSDR.
- **SoapySDR** hardening: named gain stages, a rate note when a rate is not supported, unique labels and hot-plug handling. The radio listing no longer searches the network by default (issue #215). KiwiSDR streams recover from a step back in the sequence and from stalls (issue #257, tested with a fake Kiwi only).
- **Raspberry Pi**: the CM5 waterfall works again (its GPU takes textures only 4096 wide; wide pictures are shrunk, keeping the brightest value of each pair of columns; issue #284), and OnAir starts on systems that offer only OpenGL 3.1 instead of quitting silently (issue #283: it tries 3.2, 3.1, 3.0 and, if all fail, shows the reason and the Mesa override to use). Neither fix has been tried on real Pi hardware; a software-GL CI run checks the 3.1 and 4096 limits.
- **Android**: the interface is redone in the desktop's look (adaptive bottom bar or rail, two panes on tablets, receiver, mode picker, radio picker, settings, first-run guide), USB radios through the system's USB permission flow, mode panels for FM RDS, ADS-B, AIS, message modes, and a walk-around direction finder that fits a path-loss model to GPS and compass readings. Not rebuilt or tried on a phone since the rework: treat the Android items as untested.
- **Linux**: a NixOS flake with package, module and dev shell; an x86 build no longer uses AVX2 code in the speech-to-text library on CPUs without it (issue #226).
- Fixes for crashes: waterfall assert in debug builds (issue #104, partly), an rtl_tcp reconnect that froze the UI for up to 10 s (issue #201 in part; the cause of the Android stall still needs a log).

## Network: API, OnAir Web, swarm, map

- **Remote API** completed: a token on the loopback address too (made automatically), a rate limit, versioned routes, and endpoints for auto gain, bandwidth, squelch, volume, mute, recording, memories, scheduler and audio, events by polling and WebSocket, a rigctl TCP server and UDP JSON lines. Routes that transmit or touch keys or credentials are refused by rule (docs/REMOTE-API.md).
- **OnAir Web**: onair-server serves many listeners from one capture. Each listener gets its own slice (AM, NFM, WFM, USB, LSB, CW, squelch, AGC) with audio and a waterfall over WebSocket in a browser client, open, password or invite access, budgets for CPU and bandwidth, and HTTPS through the existing TLS layer. The protocol refuses transmit, key and record requests, and voice and pager decoders stay off until the host accepts a legal notice. No listener IPs are logged by default. Measured: about 0.5% of an M2 Pro core per NFM listener. Not tried with a real radio, over the internet, on a Pi or with a real certificate (docs/ONAIR-WEB.md). A directory where servers register themselves (signed heartbeats) is in docs/ONAIR-DIRECTORY.md; host-chosen frequency limits (allow and block lists, presets) were written without a build.
- **OnAir Web decoders**: ADS-B, AIS, APRS, FT8/FT4/WSPR, SSTV, WEFAX, Meteor LRPT, RDS, CW and RTTY run on the server for browser listeners (pictures are sent once and nothing is stored on the host).
- **LAN swarm**: OnAir instances on one network can find each other (opt-in, private addresses only), pool their results on one map and share memories. The control channel is encrypted and authenticated and pairing uses a 160-bit secret. Opt-in, off by default. Nothing in the network part has been tested on more than one machine.
- **Live map**: clustering when zoomed out, a Ham contacts rail, and a consent popup with Accept, Always yes and Decline.

## KAAL

KAAL is the own digital voice mode designed for OnAir (not a standard). In 3.0 it gets frame version 4: four voice slots in the 20 kHz channel (16 ksym/s, 40 ms superframe, chirp sync, QPSK control channel) with slot budgets of 8.0, 6.0, 4.8 and 3.2 kbit/s (docs/KAAL-SLOTS.md), and a new own speech codec for those slots, "KV4" (docs/KAAL-CODEC.md). Everything is measured on generated or recorded narrow-band test sentences with objective scores only; there has been no listening test and no over-the-air test. At 8 kbit/s KV4 about equals the old 12 kbit/s codec on low-band SNR. A receiver-side speech enhancer is a separate step (docs/KAAL-ENHANCE.md).

## Issues from GitHub

| Issue | Reporter | Status | What |
|---|---|---|---|
| #284 | fazer666 | fixed | Raspberry Pi CM5: no waterfall (texture limit 4096) |
| #283 | fazer666 | fixed | Quits silently on OpenGL 3.1 systems (Pi): now falls back and explains |
| #226 | Cotin-Fan | fixed | No start on CPUs without AVX2 (speech library built with AVX2) |
| #201 | SuspectTheGoose | partly | Android/rtl_tcp: retune no longer freezes the UI up to 10 s; the stall cause needs a log |
| #104 | gvanem | partly | Recoverable ImGui errors are logged instead of asserting |
| #262 | DG9BFC | fixed | DAB: clicking inside a wide capture slid the spectrum |
| #249 | DG9BFC | partly | DAB transmitter list and map: list, import and export; no free German site list |
| #210 | vk1kcm | answered | DAB+ crash on RTL-SDR was fixed in 0.2.8 |
| #257 | grahampaull | fixed | KiwiSDR "stopped sending samples" (tested with a fake Kiwi only) |
| #215 | Real-DK0TE | fixed | Radio search no longer touches unselected network devices |
| #268 | grue74 | added | DVB-C guide language choice |
| #277 | swallswall | partly | Analogue TV transmit speed: small trim, not reproduced |
| #178 | xxj737 | fixed | DTMB labelled DVB-T in the spectrum and band strip |
| #266 | WasyaTheWolf | added | DRA sound for DTMB TV through the DRA add-on |
| #275 | xxj737 | needs sample | CDR decode failed (needs a new recording) |
| #279 | mw0cqu | fixed | Codec plug-ins are found in non-default install folders |
| #39 | YZYMC | added | Vulkan GPU LDPC on Linux (no real GPU tried) |
| #270 | dj1fti | fixed | Waterfall and spectrum alignment |
| #243 | CALIGV | added | FFT to 256k, waterfall detail 64k |
| #264 | CALIGV | added | Noise blanker |
| #246 | AKGhost | added | Android waterfall redraw (Android rework, untested on a phone) |
| #280 | ffishman | added | Portuguese split into pt-BR and pt-PT |
| #213 | WasyaTheWolf | partly | FM audio options, C-QUAM, VHF DSC and TETRA voice exist; GK-2A has no sample |
| #267 | djnutmac | partly | TETRA repeated audio: hardened, not reproduced |
| #269 | swallswall | added | Pager Capcodes view and watch list |
| #281 | microsoft-blows | added | Iridium ACARS, packet and pager views |
| #256 | Ugo300 | added | Argos mode |
| #218 | ffishman | partly | Mesh packet times use the computer clock; one clock for every mode needs a design decision |
| #258 | f5gkw | fixed | TETRAPOL 10 kHz channel spacing box |
| #148 | catheno | fixed | Airband channels not saved or deleted |
| #237 | microsoft-blows | added | Slower waterfall speeds, averaging, clock labels |
| #236 | microsoft-blows | partly | AGC retune moved the radio onto the click; ACARS searches the whole capture |
| #235 | Voyager-III | fixed | Aircraft info on the full-screen map |
| #233 | microsoft-blows | partly | RTL AGC switch and decimation |
| #85 | unilabs-exp | added | RBW list, map labels, ADS-B clock |
| #248 | Maxtino | added | Network radio guide |
| #242 | microsoft-blows | partly | Docking in the window; windows on another monitor need Dear ImGui multi-viewport |
| #252 | kevgfire | fixed | DMR audio help showed the wrong codec state |
| #216 | starvald | added | PACTOR-I monitor (II to IV are proprietary) |
| #176 | xxj737 | partly | MHEG-5 structure parser and carousel viewer; no real sample |
| #79 | PY2PLL | partly | Meteor LRPT rows placed by on-board time |
| #66 | f0cyf | answered | HFDL test setup from the report added; weak-signal WEFAX filter |
| #230 | vaizki | fixed | Spectrum frequency labels |

## Thanks

- **@drfruct** (Dmitry Obedkov): 9 commits since v0.2.9.
- **@xeonliu** (liu): 2 commits since v0.2.9.
- Pull requests merged since v0.2.9: #259 from drfruct/tetra-voice-one-stream; #260 from drfruct/tetra-voice-follow; #263 from drfruct/tetra-voice-calls; #272 from drfruct/tetra-carrier-floor; #276 from xeonliu/fix/dtmb-gbk-service-names.
- Earlier credits stay in the README: **@ffishman** (mesh clock fix, PR #217, and an offered PR for one engine-wide clock, issue #218) and **@frank-1225** (Chinese railway decoders, PR #231).
- Everyone who sent an issue, a recording or a Discord message: the reporter of each fixed issue is named in the table above. Third-party code and credits are in THIRD_PARTY.md and in Help > About.
- Work in this release was written with Claude (Anthropic) as co-author; the commits say so.

## Known limits

- Every mode marked *Experimental* above, and every measured number in this text, comes from OnAir's own generated signals. Nobody has checked P25 Phase 2, NXDN, dPMR, D-STAR, Iridium voice, TETRAPOL voice, the new LoRa satellite presets, ISDB-S, Horus, NFC or ASTERIX on a real transmission.
- Voice decoding of DMR, P25, NXDN, D-STAR, dPMR, Yaesu, TETRA, TETRAPOL and Iridium needs a codec you install yourself (docs/CODECS.md). DMR's bit order is confirmed on one live system only.
- Ham: the SDR transmit path exists in the core but is not wired into the Ham view, which keys a Hamlib rig and a sound card. Transmitting always needs the Arm button and your licence settings.
- TETRA: talkgroup and ISSI come from D-SETUP only, written from memory of EN 300 392-2 and not checked against the standard. The EDACS message layout is this program's assumption.
- DAB modes II to IV are not implemented. DVB-S2 at low symbol rates and GNSS acquisition (about 30 to 31 dB-Hz) are not improved.
- Untested on real hardware: the Vulkan GPU decoder, KiwiSDR recovery, the Raspberry Pi fixes, the Android interface, auto gain on every radio, SoapySDR gain stages, rtl_tcp at high rates, OnAir Web with a radio and a browser over the internet, the swarm on two machines, DTMB DRA and AVS+ audio and video from a real mux.
- Some merged items were written without a full build at the time (see docs/PATHWAY.md): host-chosen frequency limits for OnAir Web, the 3D waterfall check, the synthetic-wideband radio group dialog. They need a look before release.
- The Winradio WR-G31DDC is not supported (its API is 32-bit only). PACTOR II to IV, VARA and ROS are proprietary and are not decoded.
- Translations: the new texts are machine-translated and unreviewed.
- Issue #218 (one clock for every mode) and #242 (windows on a second monitor) are only partly done; see the issue table.

## Not in this build yet

These are finished or half-finished on work branches that are not merged into pub/integ-30-final. They are not part of 3.0 until they are.

- A faster build for developers (laptop worker, quick CI) is in docs/QUICK-CI.md and docs/REMOTE-BUILD.md. *(Faster and lighter; waiting for quick-ci-030, remote-build-030)*

- **RTL-SDR Blog V4 Lite** is listed by name. **Synthetic wideband** stitches 2 to 4 narrow radios into one wide stream (core and user guide; the radio-picker "Group radios" dialog is not finished). *Experimental.* *(Radios and platforms; waiting for synth-wideband2-030)*

- **Security review**: a read-only review of the network and privacy code found one high and four medium problems. The fixes for the backup restore paths, the rigctl port, the web allow-list, the key store lock-out and the directory salt are in; the rest follows the table in docs/SECURITY-REVIEW-3.0.md. *(Network: API, OnAir Web, swarm, map; waiting for security-fix-030)*

## For maintainers

190 work branches (`*-030`) exist on pub; 156 are merged into pub/integ-30-final. Not merged: build-speed-030, build-speed2-030, channels-fix-fin-030, ci-k0-030, compscore-030, docking-fin-030, dvbs-narrow-030, dvbs-narrow-fin-030, fix-issues2-030, iq-offset-tune-fin-030, issue-modes-030, lan-swarm-fin-030, laptop-installer-030, laptop-monitor-030, mode-mercury2-030, mode-mercury2-fin-030, mode-psk-030, mode-psk-fin-030, perf2-audio-bcast-030, perf2-aviation-data-030, perf2-sat-misc-030, perf2-sat-tv-030, perf2-voice-030, quick-ci-030, remote-build-030, secfix-review-030, security-fix-030, security-review-030, span-design-030, synth-wideband2-030, tcp-rate-fin-030, webonair-decoders-fin-030, wholespan-tv-030, wholespan-tv-fin-030.

Issue numbers in commit messages since v0.2.9 that this text does not mention (check by hand, some are pull requests): #84, #164, #180, #228, #229, #259, #260, #263, #272, #276, #282.

## Planned for 3.1 (not in 3.0)

- TV tuner stick support (#180), translation of the roughly 1600 new texts (new texts show in English; existing translations keep working), the Mercury 2, whole-span TV and DVB-S narrow-band follow-ups, the GNSS weak-signal changes (held back after two GNSS tests failed), a faster developer build, and the Android APK if it is not attached to this release (it follows).
- About 20 texts changed in the UI polish and lost their translation until retranslated; they show in English.
