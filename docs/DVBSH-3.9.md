# DVB-SH (3.9)

DVB-SH is mobile TV to handhelds from satellite and terrestrial repeaters, ETSI EN 302 583 (V1.2.1, 2011). It is used in the S band (2170 - 2200 MHz, 5 MHz channels), and it is also defined for UHF (8, 7, 6 MHz) and the L band (1.7 MHz). OnAir 3.9 adds the OFDM form, SH-A, as a new mode-list entry: **DVB-SH** (family 746, engine standard 748, active receiver 747). The TDM satellite form, SH-B, is not implemented.

**Validation status: simulated signals only.** Every test plays OnAir's own generator into OnAir's receiver. No real DVB-SH recording was available, and no DVB-SH network is known to be on air in Europe today. The block sizes, tables and bit orders follow the standard's text. They have not been compared with another implementation, because none with a usable licence was found.

The spec text was taken from the free ETSI PDF (EN 302 583 V1.2.1), with TS 102 584 (the implementation guidelines) used for the time interleaver's tap derivation, figure 7.12. Both were extracted with a PDFKit script. No third-party code was used, so nothing is added to THIRD_PARTY.md.

## What is implemented

Common part (clauses 5.1 - 5.4), in `core/src/dvbsh/dvbsh_fec.cpp`:

- **Mode adaptation.** A CRC-16 (0x1021, register 0xFFFF) on each MPEG packet, which replaces the sync byte. The 114-bit EHEADER (TIS, UPL, DFL, SYNC, RFU, CBCOUNTER, CRC-16) and 8 packets make a DATAFIELD.
- **Stream adaptation.** Padding to the 12 282-bit EFRAME, then the EScrambler (1 + X^14 + X^15, loaded with 100101010000000).
- **3GPP2 turbo code.** Two 8-state RSC encoders (d = 1 + D^2 + D^3, n0 = 1 + D + D^3, n1 = 1 + D + D^2 + D^3) and the 3GPP2 interleaver (table 5.5, n = 9 for 12 282 bits and n = 6 for the 1146-bit signalling field). All twelve puncturing patterns are supported: Punct_Pat_ID 0 - 11, rates 1/5, 2/9, 1/4, 2/7, 1/3, 1/3c, 2/5, 2/5c, 1/2, 1/2c, 2/3 and 2/3c. Each has its tail pattern with symbol repetition (tables 5.2 and 5.3). The encoder output length matches NTCB in table 5.7 for every pattern.
- **Turbo decoder.** Log-MAP with a linear correction term, at most 8 iterations. It stops as soon as the EHEADER CRC and all 8 packet CRCs check.
- **Bit-wise interleaver** H(w) = (m * w) mod NTCB (table 5.7), followed by rate adaptation: the first 126 bits of every 128 are kept, giving NBIL (table 5.8).
- **Time interleaver.** 48 branches on 126-bit interleaving units. The branch delays come from the five TPS parameters (common_multiplier, nof_late_taps, nof_slices, slice_distance, non_late_increment), computed as in TS 102 584 figure 7.12. This covers uniform, early/late and multi-slice profiles, which can spread a codeword over many SH frames. The receiver keeps quantised LLRs (int8) in its FIFOs. It refuses a profile that needs more than 768 MB and says so.

OFDM part (clause 5.7), in `core/src/dvbsh/dvbsh_ofdm.cpp`, `dvbsh_gen.cpp` and `dvbsh_rx.cpp`:

- **SH frame.** 816 capacity units: NCW codewords plus PRBS padding CUs (table 5.11). The frame is aligned to OFDM frames and super-frames as in table 5.10, where 1 to 16 OFDM frames make one SH frame.
- **Modes.** 1K (new in DVB-SH: 853 carriers, 25 continual pilots, 7 TPS carriers, permutation table 5.16), plus 2K, 4K and 8K reusing the DVB-T tables in `dect2/dvbt` (the same in both standards). Guard intervals are 1/32, 1/16, 1/8 and 1/4.
- **Mapping.** Bit demultiplexing (5.7.1.2), the symbol interleaver (no in-depth interleaver), then QPSK, 16-QAM, and hierarchical 16-QAM with alpha 1, 2 or 4 (HP and LP streams, each a full SH chain). The DVB-T constellations and demapper are reused.
- **TPS with the DVB-SH fields.** s53 = 1 and length indicator 37. The HP rate is in frame 1 and the LP rate in frame 3. The 32 interleaver bits are spread over the four frames (figure 5.36). The super-frame number is in s35, the cell id in s40 - s47, and the BCH (67,53) code corrects up to two errors.
- **Receiver.** It finds the FFT size and guard blindly by folding the cyclic-prefix correlation over 80 000 samples. It then finds the fractional offset from the prefix and the integer carrier offset (up to 1/8 of the FFT size) from the continual pilots. TPS frame sync uses the sync word and the BCH. Parameters are read from one whole super-frame. The channel is estimated from the scattered and continual pilots. Every symbol, the estimates are corrected for common phase and timing drift with a least-squares phase slope across the continual pilots. The FFT window moves by one sample whenever the drift reaches one sample. Data starts at the next SH frame start, then goes through the time de-interleaver and turbo decoding. The output is 188-byte packets, with transport_error_indicator set when a packet's CRC-16 fails.

Engine and app (`core/src/dvbsh/dvbsh_engine.cpp`, `app/dvbsh/dvbsh_ui.cpp`):

- The engine resamples from the radio rate to the OFDM rate of the channel. The channel width is automatic: 5, 8, 7, 6 and 1.7 MHz are tried in turn, each for 0.3 s with no OFDM found, and only widths the radio rate can hold are tried. A fixed width is under Advanced in the tuner. A checkbox picks the LP stream of a hierarchical signal.
- The transport stream goes to the TV player (`onTsPackets`), like DVB-T's.
- Telemetry, a summary line, a Receiver tab (TPS parameters, interleaver profile and its length in seconds, codeword and packet counts, turbo iterations), status lamps and an SNR history.
- Test signal (engine standard 748): 5 / 8 / 7 / 6 / 1.7 MHz, FFT size, QPSK or 16-QAM, any of the 12 rates, SNR and CFO. The 1.7 MHz channel always uses 1K.

## Measured results (simulated)

Turbo code with the bit interleaver and rate adaptation, on BPSK + AWGN (`tests/dvbsh/test_dvbsh_fec.cpp`, ONAIR_TEST_LONG=1, 20 codewords of 12 282 bits per point, Eb/N0 per information bit):

| rate | BPSK capacity limit | FER 100 % at | FER 0/20 from | gap to the limit |
|---|---|---|---|---|
| 1/5 | -0.97 dB | -0.47 dB | +0.03 dB | 1.0 dB |
| 1/3 | -0.50 dB | 0.00 dB | +0.50 dB | 1.0 dB |
| 1/2 | +0.19 dB | +0.69 dB | +1.19 dB | 1.0 dB |
| 2/3 | +1.06 dB | +1.56 dB | +2.06 dB | 1.0 dB |

For example, rate 1/3 at +0.25 dB gives BER 2.2e-3 (6 of 20 frames wrong). This is in line with published log-MAP results for 3GPP2 turbo codes of this block size. All 12 patterns decode with no errors 1.5 dB above their rate's limit. The short test asserts BER below 1e-5 at limit + 1.25 dB for 1/5, 1/3, 1/2 and 2/3, and failure at limit + 0.5 dB for 1/3.

OFDM round trips (`tests/dvbsh/test_dvbsh_chain.cpp`). Every packet carries its own sequence number and checksum, so "good" means byte-exact.

| case | impairments | result |
|---|---|---|
| 2K QPSK 1/3, GI 1/8 | 4 dB SNR, +7.3 kHz (1.6 carriers), +15 ppm clock, random start | 760 good, 0 bad, 0 bad codewords |
| 8K 16-QAM 1/2, GI 1/4 | 12 dB, -23 kHz (20 carriers), echo -6 dB at 300 samples | 2696 good, 0 bad |
| 1K 16-QAM 2/5c, 1.7 MHz, GI 1/32 | 13 dB, +1.2 kHz, -30 ppm clock | 2520 good, 0 bad |
| 2K hierarchical 16-QAM alpha 2, HP 1/3 / LP 1/2c | 16 dB, +2.5 kHz | HP 1240 good, LP 1880 good, 0 bad |
| 2K 16-QAM 1/3, early/late interleaver (2/24/2/1/1, 1.17 SH frames) | 14 dB, signal off for 25 % of an SH frame | 2368 good, 0 bad; the same blockage with the short uniform interleaver: 10 bad codewords |
| 2K QPSK 2/3 at 2 dB | below threshold | every codeword flagged bad, no bad packet passed as good |
| sweep (ONAIR_TEST_LONG): 1K/2K/4K/8K x QPSK/16-QAM x rates 1/5, 2/7, 2/5c, 2/3c, all guards | 9 / 15 dB, +3.1 kHz, +5 ppm | 32 of 32 combinations decode with 0 bad packets |

The clock tracking holds up to about 100 ppm. At 200 ppm the 1K mode still decodes, but its pilot SNR falls to about 15 dB, because the channel estimate is a zero-order hold over the 4-symbol pilot pattern.

Engine (`tests/dvbsh/test_dvbsh_engine.cpp`): the test signal at radio rates through the engine receiver with automatic width. These all ran at 18 dB SNR and 8 ppm:

- 5 MHz at 8 Msps: found first, 1368 good packets.
- 8 MHz at 10 Msps: found after the 5 MHz try, 2880 good packets.
- 1.7 MHz (1K) at 2.4 Msps: 1240 good packets.

Local runs: dvbsh_fec 0.7 s, dvbsh_chain 12 s, dvbsh_engine 4 s (Apple M-series).

## Limits and what is left

- **SH-B (TDM)** is not implemented: no 8PSK / 16APSK, PL slots, pilots, PL scrambling or signalling field reception. The turbo code for the 1146-bit signalling field is in place and tested.
- **Low latency extension (annex B)** and the **SHIP** packet (annex A) are not interpreted. SHIP arrives as an ordinary TS packet.
- **Channel estimate.** Frequency interpolation is linear, and time interpolation is a zero-order hold with phase and timing correction. That is fine for static and slowly changing channels. Fast fading at vehicle speeds would need proper time interpolation, which is not tested.
- **CPU.** The turbo decoder runs on the receive thread. At good SNR it averages 1 to 2 iterations, so an 8 MHz QPSK 1/3 signal at 10 Msps decodes in about real time on an M-series Mac. Near threshold (8 iterations) it is slower than real time, and a live radio then drops samples. Files play at any speed.
- **Interleaver memory.** The receiver allows 768 MB. The longest profiles (slice_distance 255 over several slices) need more and are reported as too long.
- The Android mode list (android/tools/gen_modes.py) has not been regenerated on this branch.
- docs/BCAST-3.9.md (feat39-bcast) lists DVB-SH as not done. This branch adds it.
