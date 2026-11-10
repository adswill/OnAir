# Smart features in 3.9: TV

Five engines in `core/src/smart5`, fed with what the TV decoders already produce (PID counts, per-carrier SNR, Es/N0 and MODCOD, the TPS / L1 cell ID and the NIT, decoded analogue lines). Test: `tests/smart5/test_smart5_tv.cpp`.

**Validated on simulated signals only.** Synthetic PID counts (Poisson), per-carrier SNR for DVB-T 8k, DVB-T2 32k and DAB with three centre offsets, Es/N0 series with noise, a hand-built cell_list_descriptor and generated test lines at 13.5, 20 and 27 MHz with noise, offset and low-pass filtering. No recording was on hand.

| # | Engine | Header | What it reports |
|---|---|---|---|
| 11 | Service off-air detector | `pid_offair.h` | Learns each elementary PID's usual rate (20 s); a video or audio PID that stops or falls below 10 % of it for 3 s, while the mux keeps its rate and has under 1 % continuity errors, is reported "off air at the source" with the time. Nothing is judged while reception is bad. |
| 12 | OFDM interferer locator | `ofdm_interf.h` | Runs of carriers 6 dB or more below the channel median, with RF centre, width and depth, named as CW spur (up to 5 carriers), LTE / 5G block (width within 12 % of 1.08, 2.7, 4.5, 9, 13.5, 18 MHz or the NR widths), adjacent channel (touching an edge) or wideband. DVB-T/T2, ISDB-T, DTMB multicarrier and DAB. |
| 13 | Rain-fade and margin log | `sat_fade.h` | Margin above the quasi-error-free Es/N0 of the present MODCOD, the clear-sky level (90th percentile of 30 min, frozen during a fade), every fade with start, length, depth and whether lock was lost, every ACM / VCM MODCOD change. |
| 14 | Transmitter from cell ID | `cell_tx.h` | Parses the NIT cell_list_descriptor (0x6C, cells and subcells), joins the received cell_id with it and with the user's book, and gives the site name or the cell centre with distance and bearing. |
| 15 | VITS analyser | `vits.h` | Finds what is on a line without being told the layout: white bar (level, tilt), 2T pulse (half-amplitude duration, K pulse-to-bar), multiburst (each packet's frequency and level in dB against the first), staircase with subcarrier (differential gain and phase; the subcarrier is snapped to PAL, NTSC, PAL-M or PAL-N). |

## Sources

- DVB-S2 thresholds: EN 302 307-1 table 13 (ideal Es/N0, normal FECFRAME, AWGN). The 28 values were written from memory of that table and match the published figures (QPSK 1/4 -2.35 dB to 32APSK 9/10 16.05 dB); a check against the PDF is still to do.
- DVB-S thresholds: EN 300 421 (Eb/N0 4.5 / 5.0 / 5.5 / 6.0 / 6.4 dB after Viterbi for QEF), turned into Es/N0 with the inner rate and 188/204.
- cell_list_descriptor: EN 300 468 clause 6.2.7. Latitude is 16-bit two's complement in units of 90 / 2^15 deg, longitude 180 / 2^15; the extents are 12-bit; the point given is the south-west corner of the cell, so the centre adds half the extent.
- K factor: K_PB = 1/4 x |B - P| / P x 100 % (the usual pulse-to-bar rating). Differential gain is (max - min) / first step amplitude.

## Not done

- ATSC 1.0 has no per-carrier SNR (single carrier 8-VSB), so item 12 does not apply to it.
- The VITS analyser needs the decoder to hand over the test lines (17, 18, 330, 331); the wiring into the analogue TV screen is for the integrator.
