# Real-signal test library

What the tests decode from a recording made on the air (not from OnAir's own generator). Excerpts are small (at most 2 MB), live under `tests/data`, and run in the normal test tier in about a second each. Everything else in `tests/*/test_*_realworld.cpp` / `*_impair.cpp` is the generator put through `tests/common/impair.h`: those are impairment tests, not recordings.

Rules: owner recordings are used with the owner's consent; public files only with a licence that allows redistribution (credit in `THIRD_PARTY.md`); a file with no licence stated is not copied into the repository, only listed here as a lead. Downloaded files were handled as untrusted data (own scratch directory, read by our own decoders only).

## Library hook for CI

`tools/signal_library.py` decodes every recording listed in `tests/real/signal_library.txt` with `dect2cli` and fails when the output lacks the expected text (test `signal_library`). Set `ONAIR_SIGNAL_LIB` to a folder with its own `manifest.txt` (same format) to add recordings too large or not licensed for the repository; missing files are reported and skipped. A clip saved from the Reception Doctor (`.sigmf-data`, 8-bit I/Q) can be listed there once somebody knows what it should decode to.

## Modes with a real-recording test

| Mode | Source | Licence | Excerpt | Test | Status |
|---|---|---|---|---|---|
| ADS-B (1090 MHz) | dump1090 `testfiles/modes1.bin` (gvanem/Dump1090, from antirez/dump1090) | MIT / BSD-3 | `tests/data/adsb/modes1_2msps_cu8.bin`, 714 KB, 2 Msps cu8, 0.18 s, one aircraft (4D2023) | `real_adsb` (tests/real) | NEW. 57 short + 126 long messages, all CRC-good, one address, 54 positions, 54 velocities |
| DTMB | owner's own HackRF capture, 554 MHz, 10 Msps | owner | `tests/data/dtmb/owner_554mhz_10msps_2MB_iq.s8`, first 0.1 s | `real_dtmb` | NEW. PN945, 16QAM 0.8, mode 2, multi-carrier, CFO -560 Hz. 2 MB stops at the system information; the full capture decodes 0 bad codewords and a clean TS (needs about 8 MB for the first packet: too big for the repository) |
| Marine DSC (MF/HF) | `Dsc_examples.mp3` from the owner's downloads (origin unknown) | not stated | `tests/data/marine/dsc_mf_12500_s8.raw`, 22 s, 12.5 kHz, 8 bit | `real_dsc` | NEW. Two individual calls 8291 kHz (005030001 <-> 002320001), ECC ok, EOS 117 and 122. Audio played in as IQ at 250 ksps, offset 1700 Hz |
| Inmarsat Aero (C-channel voice, 8400 bit/s) | JAERO `samples/8400bps_ambe_sample.ogg` | MIT | `tests/data/aero/jaero_c8400_48k.s8` | `aero_cchan_real` | existing |
| HFDL | owner, Hermes Lite 2, 13.351 MHz, 2026-10-17 | owner | `tests/data/hfdl/real_burst.h` | `hfdl_real` | existing, one burst |
| DRM | Dream project recordings (DLR mode A, DW mode B, RNW mode C) | not stated (test data only) | `tests/data/drm/real_cells.h`, `real_aac.h` | `drm_known` (cells) and the AAC test that includes `real_aac.h` | existing |
| COSPAS-SARSAT 406 MHz | PLB burst from issue #223 | sent by a user | `tests/data/sarsat/epirbsignal_223.wav` | `sarsat_rx` | existing |
| Multitone (FFSK + tones) | Multitone_2015-12-31 27599.8 kHz (owner's download) | not stated | `tests/data/multitone/owner_multitone_8k_s8.raw` | `multitone_real` | existing, structure only (format not public) |
| PACTOR-II | owner, 8532.8 kHz, 2026-11-01 | owner | `tests/data/pactorhf/owner_pactor2_8k_s8.raw` | `pactorhf_real` | existing, structure only (no FEC / scrambler spec) |
| ATSC 3.0 | ROUTE video + audio samples | origin not recorded here | `tests/data/route_video.mp4`, `route_audio.mp4` | `atsc3_remux`, `atsc3_receiver`, `atsc3_stream`, `atsc3_engine` | existing (post-demod media, not IQ) |

`test_lbj_real` reads the reporter's LBJ recording from `$LBJ_SAMPLE_DIR` or `~/Downloads` and skips when absent: it never runs on CI. `test_dvbt_real` and the other `*_realworld` tests are generator-based.

## Looked at and not usable (yet)

| Lead | Result |
|---|---|
| mattgodbolt/PALindrome (PAL, SigMF) | no licence file (all rights reserved by default); masters are git-LFS, 32 MS/s RX888 / 20 MS/s Airspy: not copied |
| live.ariss.org HamTV IQ | the site offers MPEG-TS, not IQ; `/iq.html` is 404; HamTV was reported offline in 2025 |
| daniestevez/grcon23-ctf (DVB-S2) | repository is Unlicense, but holds baseband frames (`bbframes.u8`) and the challenge text, not the IQ (543 MiB, on ctf-2023.gnuradio.org, no licence stated) |
| adron-s/rx1090 `IQ-ADS-B/hackrb-dump-00.sb` | 75 MB, no licence file seen; one 0.18 s file from dump1090 already covers ADS-B |
| ningzichun/dtmb-sdr releases | not fetched; the owner's own 554 MHz capture covers DTMB |
| sigidwiki `File:*.zip` | not fetched (licence per file, mostly unstated); to try per mode when a gap matters |
| sdrplay.com/iq-demo-files, osdn.net | dead ends (known) |
| owner `DVB-T 5MHz 5333333sps_IQ.wav` (2017, 0.5 s) | origin and licence unknown: not copied; fine for a local check |
| owner `audio_408...Hz` / 409.184 / 410.1646 MHz, 137.1038 MHz (48 kHz IQ as stereo WAV) | the signal identifier calls them unknown (410.1646 MHz: a 6000 Bd digital signal with no known sync); not identified, so no test |
| owner `CDRrecording1.mp3` | audio of a CDR station; the DRA clip in `tests/cdr/cdr_dra_clip.h` already covers the audio path |
| KiwiSDR `SND.*_k_cwn.wav`, `_k_amn.wav` | 1.4 s only |

## Modes with no real recording yet

Everything not in the first table. Grouped; each needs a real IQ or audio file, ideally with a known answer:

- TV and broadcast: DVB-T, DVB-T2, DVB-S, DVB-S2/S2X, DVB-C, ISDB-T, ISDB-S, ATSC 1.0 (IQ), analogue TV (PAL, NTSC, SECAM), DAB, HD Radio, CDR, FM / RDS, C-QUAM, DRM+.
- Aviation: ACARS, VDL2, VDL4, VDL Mode 2 log, UAT, Mode A/C, ASTERIX, airband, DME, VOR, ILS, ATIS, FLARM, ADS-B beyond the single file (weak, multi-aircraft, DF18/TIS-B).
- Marine: AIS, VDES, NAVTEX, DSC VHF ch 70, weather fax, SITOR.
- Satellite: Iridium, Globalstar, Orbcomm, Inmarsat (other channels), Argos, Meteor LRPT, NOAA APT / HRPT, GOES, GK-2A, Fengyun, ELEKTRO, QO-100, satellite beacons, SARSAT second generation, LoRa satellites, CCSDS.
- Voice and trunking: DMR, P25 (P1, P2), NXDN, D-STAR, dPMR, TETRA, TETRAPOL, MPT 1327, EDACS, LTR, Fusion, M17, GSM-R.
- HF data: PSK, FT8 / FT4 / WSPR / JS8, ALE, ARDOP, MT63, FSQ, IFKP, SITOR, PACTOR-III/IV, ham modes, time signals (BPC, DGPS, eLORAN).
- Paging, ISM and IoT: POCSAG, FLEX (real frames of issues #95 / #261 are modelled, not stored), ZigBee, Z-Wave, wM-Bus, LoRa / LoRaWAN, Meshtastic, MeshCore, Sigfox, Wi-SUN, BLE, NFC, DECT, DJI DroneID, Remote ID, nr / NB-IoT / cell, radiosondes (RS41, DFM, M10, M20, Horus), railway (LBJ: only a local file).
- Other: GNSS, SBAS, multitone and tones, CW, e-Czas, KAAL / KAVB / KAPP / KAFT / KAIT / KAWS (own modes: no air signal exists).

## Decoder findings from the real files

- ADS-B: the recording decodes fully (every one of the 183 messages is CRC-good). `adsbtool rx` prints `0 good` in its closing summary for the same file because it reads telemetry once after the last feed; the messages and the table above it are right. Cosmetic, not changed.
- DTMB: on a 0.1 s excerpt the first telemetry report does not exist yet (state stays "searching"), while the log already reports the header and the system information; the test reads the log lines. On the whole capture lock and TS are clean.
- DSC: decodes the real calls, no failure found. The MF/HF path needs the audio offset to be set (1700 Hz), as for any SSB feed.
- No decoder failure was exposed by these recordings.

## Adding a recording

1. Cut the excerpt (<= 2 MB), put it in `tests/data/<mode>/` with a line in this file and in `THIRD_PARTY.md` (source, licence).
2. Write `tests/real/test_real_<mode>.cpp` (read through `DECT2_SOURCE_DIR`, check what the file is known to hold) and add the mode to the `foreach(m adsb dtmb dsc)` list in `CMakeLists.txt`.
