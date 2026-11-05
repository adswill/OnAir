# Inmarsat revamp (3.3): all carriers at once, one mode

Status: design, written before the code. Branch `feat33-inmarsat` (from `integ-32`, not part of 3.2).
Owner request (2026-10-26): "fully revamp inmarsat, a mode that can decode all at once for higher bandwidth supporting SDRs, and then decode voice as well with all the channels at once for AERO, like InmarScope but 10x better".

Everything marked **needs a user test with a real capture** has only been checked on synthetic signals.

## 1. Where we are today

| Part | Code | What it does | Limit |
|---|---|---|---|
| Aero | `core/src/aero` (`AeroReceiver`) | Band search finds P carriers (up to 6) in one capture, demodulates 600 / 1200 / 10500 bit/s, SU layer, ACARS, ADS-C, positions | P channel data only, its own search, no voice, no channel table in the UI |
| Inmarsat-C | `core/src/inmc` (`InmcReceiver`) | NCS common channel 1200 sym/s BPSK, EGC (SafetyNET, FleetNET), searches up to 3 more channels | one UI, separate mode from Aero |
| Voice | none (3.3) | `docs/CODECS.md`: OnAir "does not tune voice channels" | C channels not detected or decoded. 3.6: done, see section 4.4 |

Two separate modes, each with a private band search, each decoding what it finds without a shared view of the band.

## 2. What other tools do (public descriptions only)

I have only read public pages (READMEs, screenshots, forum posts). No code was copied, and none will be unless its licence allows it, in which case it is credited in `THIRD_PARTY.md`. Points below that I could not confirm are marked "(unverified)".

- **JAERO** (Jonti Olds, GPL-3): decodes Aero P channel (600, 1200, 10500 bit/s) and ACARS from one narrow audio/IQ channel at a time; one instance per carrier; the existing OnAir Aero code already cites its parser layout. Voice support: C channel through an AMBE decoder the user supplies (unverified detail of the framing). Strength: accuracy. Weakness: one channel per process, a manual setup per carrier.
- **InmarScope** (Windows, closed, unverified details): shows a wide spectrum, lists the carriers it finds, and decodes several Aero channels and voice at once. This is the model the owner wants us to beat.
- **Beat plan:** automatic carrier classification, a carrier table that never needs a manual setting, parallel decode of everything the radio covers, per-channel voice mixer and recording, a shared aircraft map, positions from ADS-C and text reports, cross-platform, and a CPU budget that degrades gracefully.

## 3. RF facts the design depends on

- Satellite to ground (L-band downlink) is 1525 to 1559 MHz. A ground receiver sees there: **P channels** (GES to aircraft packet data, 600 / 1200 / 10500 bit/s), **C channels** (circuit-mode voice, SCPC, GES to aircraft), and the Inmarsat-C TDM channels (NCS common channel, LES TDM channels, around 1537 to 1545 MHz).
- **R and T channels are aircraft to satellite**, on the L-band uplink, 1626.5 to 1660.5 MHz. Ground receivers see them only from nearby aircraft and they are weak. So "R / T" are an **optional second window** for radios that reach 1626 to 1660 MHz (HackRF and SDRplay do, RTL-SDR does up to 1766 MHz); the mode lists them, flagged "uplink, nearby aircraft only". The design does not promise them from the ground.
- Typical Inmarsat-4 / Alphasat / Inmarsat-6 satellites each put several P-channel groups and many C channels in a few MHz; the 600/1200 and 10500 P groups sit about 1 MHz apart (existing default 1545.57 MHz).
- The satellite is identified automatically (inmsat_satid.h) from three independent clues scored together: the decoded content (Aero system table satellite id, longitude and beam flag; Inmarsat-C ocean region), the frequency fingerprint (the set of P carriers against each satellite's published list) and the geometry (azimuth and elevation of each geostationary slot from the user's location; below the horizon counts against it). Geometry alone never names a satellite. Manual override under Advanced. Tables unverified: needs a user test with a real capture.

## 4. Architecture

One new mode, **Inmarsat** (`core/src/inmsat`, `app/inmsat`, `tests/inmsat`), with the existing Aero and Inmarsat-C modes staying as they are (the new mode reuses their decoders; nothing is removed in 3.3).

```
radio IQ  ->  WidebandRx (ring)
              |
              +-> CarrierDetector   (one big FFT, averaged; finds peaks, measures width, classifies)
              |       |
              |       v
              +-> ChannelPlan  ----> FdChannelizer (shared forward FFT, one small inverse FFT per carrier)
                                       |-- P/Aero    -> AeroReceiver (one per carrier, 240 ksps in)
                                       |-- Inmarsat-C-> InmcReceiver
                                       |-- C voice   -> VoiceChannel (demod -> frames -> codec plug-in -> PCM)
                                       '-- R/T       -> (uplink window only) AeroReceiver burst mode, later
              ChannelManager: table, budget, focus, mute/solo, recording
              Results: messages, aircraft (AeroAircraft), EGC
```

### 4.1 Wideband capture and the shared channeliser

- `lora::FdChannelizer` (core/include/dect2/mesh/mesh_lora.h) already does one shared forward FFT with an inverse FFT per channel and a power-of-two decimation. It is the pattern reused here: the cost grows with the carrier count only by one small inverse FFT each.
- Carrier detector: an averaged power spectrum with a median floor, peak picking, width measurement (a 10500 bit/s carrier is about 10 kHz wide, a 600 bit/s one below 1 kHz, C channel about 6 to 8 kHz wide (unverified), Inmarsat-C 1200 sym/s about 2 kHz), and the existing rule that a carrier must show in two searches in a row. The radio's DC spike is ignored.
- Classification by width and spectral shape: *narrow* (below 1.5 kHz, P 600 bit/s or a bare carrier), *medium* (1.5 to 4.5 kHz: P 1200 bit/s and Inmarsat-C look the same here), *wide* (above 4.5 kHz: P 10500 or C channel). Measured on the Aero test signal: 600 bit/s 1.2 kHz, 1200 bit/s 2.0 kHz, 10500 bit/s 9.5 kHz. The class only picks which decoder is tried first; each decoder confirms by frame sync, and a carrier is relabelled by what actually decodes. Honest limit: C versus 10500 P cannot be told from width alone; the P decoder is tried first, and a carrier where only the unique-word search of the C-channel decoder succeeds becomes C.
- Doppler: Inmarsat satellites are geostationary (inclined orbits move a few kHz a day at most); the carrier tracker follows slow drift, and the plan re-centres a channel when its measured offset moves more than a quarter of the channel width.

### 4.2 Which radios

Coverage = sample rate (usable about 80 percent). Carrier capacity is limited by CPU, not by bandwidth. Estimates, to be measured in stage 1.

| Radio | Sample rate | Usable span | What it covers | Parallel carriers (target) |
|---|---|---|---|---|
| RTL-SDR (v3 / v4) | 2.4 Msps (up to 3.2) | about 2 MHz | one P group (600/1200) or the 10500 group and a few C channels | 4 to 6 |
| Airspy Mini / R2 | 3, 6, 10 Msps | 2.4 to 8 MHz | both P groups and many C channels | 8 to 12 |
| SDRplay RSP | up to 10 Msps | about 8 MHz | same | 8 to 12 |
| HackRF One | 10 to 20 Msps | 8 to 16 MHz | most of one satellite's downlink | 12 to 16 |
| HackRF Pro | 20 Msps (10 on the Pro tuning quirk) | up to 16 MHz | nearly all of the 34 MHz band needs two windows; one satellite fully | 16 to 24 |

The 34 MHz downlink never fits at once, so the mode has a **Whole band** option that steps the window (like the fast scan) when no carrier is being followed, and a **Hold** state that stays on a window while carriers decode. One Auto button picks the window from the satellite table (the satellite with the most carriers seen, else the one nearest the user's longitude).

### 4.3 Channel manager

A table, one row per detected carrier: frequency, type (P / C / Inmarsat-C TDM / R / T), bit rate, SNR (carrier over floor), state (seen, locked, in sync, data), last activity, likely satellite, frames and SU counts, voice flag. Click a row to focus it (detail, spectrum zoom, log, and for voice solo). Everything decodes in parallel; the **CPU budget** is a cost model (cost per decoder kind times carriers): when the sum exceeds the budget the manager parks the lowest-priority carriers (never focused or active ones) and says so in the table ("parked: CPU"). Rows disappear when a carrier has been silent for two minutes (a P channel is continuous; a C channel is only on during a call).

### 4.4 Voice

- Aero C channel is circuit voice (AMBE-family vocoder, user-installed; framing in Inmarsat's Aero SDM, not public).
- **Plumbing built in 3.3 stage 3:** `VoiceChannel` objects, one per active C carrier, each a frame source feeding a `codec::SpeechCodec` from the existing plug-in system (`core/include/dect2/codec`, `onair-codec-<id>` in the user's codecs folder, notice accepted once). A per-channel mute / solo mixer, a recorder (WAV per call, with carrier, start and end), and the "install codec" message when the plug-in is missing (silent, never an error).
- **Not shipped:** any patented codec; nothing is downloaded or linked. Unencrypted traffic only; no cracking; user keys only where a user-imported key applies, as in docs/KEYS.md.
- **3.6, solved:** the open problem above was solved by reading JAERO (MIT, github.com/jontio/JAERO): the C channel is 8400 bit/s OQPSK (root raised cosine, roll-off 0.6), a 500 ms frame of two 52-symbol unique words (one per arm) and 4096 coded bits, interleaved in blocks of 256, punctured to rate 2/3, convolutional code K = 7 (the P channel code), the P channel scrambler, giving 25 AMBE frames of 12 bytes and 3 signal units. Code: `core/src/aero/aero_cchan.*` (framing and coding), `aero_crx.*` (one carrier: mixer, decimator, demodulator, framer), `aero_cgen.*` (test signal), the manager's per-window C receivers (`inmsat_manager.cpp`, `updateC`) and the mixer's `pushCFrame`. A real recording from JAERO's sample set decodes (CRC-16 of every signal unit, call-progress unit with aircraft and ground station). The AMBE decoder is the user's plug-in `aeroambe`, built from `tools/addons/aeroambe` and libaeroambe's mbelib: the plug-in was run on the same recording and gives speech bursts (checked from level and zero-crossing statistics; nobody listened).

### 4.5 UI

One mode "Inmarsat" with tabs: **Carriers** (table + spectrum with the carriers marked), **Messages** (ACARS, ADS-C, Inmarsat-C EGC, with filters by registration / label / channel), **Aircraft** (map from ADS-C and position reports, tracks), **Voice** (the active calls, mute / solo / record, codec status). One **Auto** button does band-window choice, carrier detection and parallel decode; manual controls (fixed carrier list, per-carrier decoder, CPU budget) live under Advanced. English strings, an in-app "?" help entry, and `docs/INMARSAT.md` for users.

## 5. Stages

1. **Wideband channeliser and carrier detector** (`core/src/inmsat/inmsat_detect.*`): synthetic multi-carrier IQ with offsets, Doppler drift, noise, fading and adjacent carriers; tests for detection, classification, no false carriers on noise, stability across chunk sizes.
2. **Parallel data decode** (`inmsat_manager.*`): the channeliser feeds one `AeroReceiver` per P carrier and one `InmcReceiver` for the Inmarsat-C carriers; the manager owns the table, budget and results. Tests with synthetic Aero (`aero_gen`) and Inmarsat-C (`inmc_gen`) signals mixed in one wideband capture.
3. **Voice plumbing** (`inmsat_voice.*`): C-carrier detection, `VoiceChannel`, mixer, recorder, codec plug-in interface. Tests without a codec: silent output and the "install codec" message; with a fake test codec for the mixer and recorder only.
4. **UI and engine hookup** (`app/inmsat`, mode registration, strings, help, docs).

## 6. Testing rules

Synthetic signals with real-world impairments (frequency offsets, noise, fading, adjacent carriers, Doppler). A loopback pass is never reported as "works"; every stage lists what a real capture still has to show. Third-party facts are cited in code comments; GPL-3 compatibility is checked before any reuse.
