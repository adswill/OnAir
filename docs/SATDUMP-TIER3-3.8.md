# SatDump parity, tier 3 (3.8)

The third batch of the SatDump parity list: the GOES family beyond HRIT, GK-2A UHRIT and CDAS, one generic S-band CCSDS mode with a satellite table, and the UHF cubesats. All links run in one receiver class (`Sd3Receiver`, core/src/sd3), each with its own mode-list row, numbers (docs/MODE-NUMBERS.md, families 290 - 299 and 510, poll slots the same; may need to move at merge) and screen.

## Modes

| Mode | Family | Link | Products | Validated on |
|---|---|---|---|---|
| GOES GRB | 290 | 1686.6 MHz, DVB-S2 8.67 Msym/s (the existing receiver, a new BBFRAME callback), CADUs on VC 5 / 6 | ABI and SUVI pictures (JPEG 2000 blocks; pictures kept at 4 km and saved as PNG), metadata and GLM payloads saved | simulated only: DVB-S2 test signal at 14 dB Es/N0, +10 kHz, one second: 648 CADUs, 57760 packets, picture lines drawn. No public recording found |
| GOES GVAR | 291 | 1685.7 MHz, BPSK 2.11 Msym/s, NRZ-S, 32786-byte PN-locked frames | imager visible (reduced 4:1) and 4 IR channels | simulated only; satellites retired |
| GOES CDA | 292 | GOES-R 40 / 4 kbit/s (BPSK, NRZ-M), GOES-N PM 4 kbit/s | frame log | simulated only. SatDump's GOES-R CDA pipeline declares RS(255,223) on 252-byte frames (it reads past the frame); here the code is taken as shortened, unverified, frames are shown even when Reed-Solomon fails |
| GOES MDL | 293 | QPSK 200 ksym/s, 464-byte frames | frame log (SatDump does not parse the contents either) | simulated only |
| GK-2A UHRIT | 294 | 8070 MHz, DVB-S2 15.6 Msym/s, 2048-byte CADUs | xRIT files reassembled (CRC16 per frame, HDLC CRC32 per packet) and saved with their annotation name; not yet fed to the GK-2A picture pipeline | simulated only; the DVB-S2 receiver needs about 28 Msps (not HackRF). Long test, run with `SD3_FULL=1` |
| GK-2A CDAS | 295 | QPSK 3.096 Msym/s, RS 5x, fill 3 | frame log | simulated only |
| S-band CCSDS | 296 | 46-entry table generated from SatDump's pipelines (core/src/sd3/sd3_links.inc): BPSK, QPSK, OQPSK, PM, FSK; simple or K=7 convolutional, NRZ-M, randomiser, RS 223 / 239, sync words | frames, virtual channels, spacecraft IDs, frame log | every table entry decoded in simulation; five kinds also with offsets, 50 ppm clock, 500 Hz/s Doppler, off-centre, inverted spectrum and near threshold (table below). No real recording found |
| Geoscan | 297 | six satellites, 9k6 FSK, 66-byte PN9 frames with CRC | frame log | **real**: SatNOGS observation 13174376 (StratoSat-TK1, 2026-01-09, discriminator audio re-modulated): 6 frames with good CRC, AX.25 addresses readable; SatNOGS itself got 7 on that pass |
| Lucky-7 | 298 | 4k8 FSK, 16-bit sync, 39-byte frames, Si4463 scrambler, CRC | telemetry kind, frame log (picture chunks not assembled) | simulated only; on a real 2024 SatNOGS pass (archive.org, observation 10775131, 161 frames there) the sync word correlates but no frame passes the CRC: not resolved |
| Spino | 299 | 2k4 FSK, 0x2EFC9827, variable length | frame log | simulated only (the transmitter is listed inactive; no observations at that rate) |
| Erminaz | 510 | 9k6 FSK, CCSDS frames with RS, 435.775 MHz | frame log | simulated only; a real SatNOGS pass of ERMINAZ-2U on 435.9 MHz gave sync hits but no good frames (a different transmitter from SatDump's entry, not investigated) |

Excluded as asked: Tianwen, Psyche, JUICE, Hera, Orion, Chandrayaan, KPLO, TGO, Queqiao and the rest of the deep-space group. Table entries left out and why: Cluster low rate and Tianhe-1 (PM with a subcarrier), DMSP RTD, CFOSAT and Aeolus (their own deframers), SMOS X-band (8PSK), PRISMA and Sentinel-6 X-band (75 Msym/s), Iris X-band (LDPC), the Chinese spaceplane and Cosmos 2518 (no decoder listed in SatDump). SatDump's instrument decoders for the S-band satellites are not ported: this mode shows frames and channels only.

Simulated impairments, one link of each kind (`test_sd3_rx impair`): FORMOSAT-5 (BPSK), Jason-3 (QPSK, code, NRZ-M), SWOT (OQPSK, code), Hinode (PM), Falcon 9 (FSK) all kept frame lock with +0.5 % and -1 % of the symbol rate offset, 50 ppm clock, 500 Hz/s Doppler, a carrier off centre by 0.15 Rs and, for the PSK links, an inverted spectrum, at 8 dB above their floor. Floors tried: BPSK 9 dB, convolutional links 7 dB, PM 9 dB (all frames good); Falcon 9 at 12 dB starts failing (4 of 31 frames).

Automatic on top: picking a satellite tunes and sets the format; with "Follow the frequency" the table follows the tuned frequency. Geoscan and Spino have "Follow the next pass". Manual tuning stays in the normal controls.

GK-2A keys are not touched by this batch. Spacecraft telemetry is shown as received; nothing is decrypted.

## Left out / next
- GK-2A UHRIT files go to disk, not yet to the GK-2A picture pipeline; GRB pictures at 4 km only; no GLM JSON; SUVI colour not composed.
- GVAR sounder; MDL / CDA content (undocumented in SatDump too).
- Doppler for the S-band table would need a NORAD id per entry.
- GRB (DVB-S2) never run on a real recording; real TS/GS signalling may differ from the test signal.

Sources: formats and parameters from SatDump (GPL-3.0, Copyright Aang23 and contributors): plugins goes_support, gk2a_support, cubesat_support, spacex_support, resources/pipelines. Code written for OnAir; notice in each file; THIRD_PARTY.md and in-app credits. DecompWT, fcidecomp and libtomcrypt were not used.

Checks: core with `clang++ -fsyntax-only` and tests/sd3 standalone against the libraries of /private/tmp/b37t. The app was not built locally (overnight limits); the mode-numbers test runs on CI.
