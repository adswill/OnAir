# DVB-SH (3.9)

DVB-SH is mobile TV to handhelds from satellite and terrestrial repeaters, ETSI EN 302 583 (V1.2.1, 2011). It is used in the S band (2170 - 2200 MHz, 5 MHz channels), and it is also defined for UHF (8, 7, 6 MHz) and the L band (1.7 MHz). OnAir 3.9 adds the OFDM form, SH-A, as a new mode-list entry: **DVB-SH** (family 746, engine standard 748, active receiver 747). The TDM satellite form, SH-B, was added on branch feat39-bcast2 to the same entry (see "SH-B (TDM)" below).

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

## SH-B (TDM), branch feat39-bcast2

`core/include/dect2/dvbsh/dvbsh_tdm.h`, `core/src/dvbsh/dvbsh_tdm.cpp`, from EN 302 583 V1.2.1 clauses 5.5.2 and 5.6. The channel coding
(EFRAMEs, turbo code, bit-wise interleaver, rate adaptation, time interleaver) is the SH-A code above, unchanged.

- **TDM SH frame** (5.5.2.4). It starts with a HEADER of 3 CU: the 288-bit SOF (F3484536 ... ACCF9E4F) and the 5 760-bit signalling
  field. The DATA part follows: whole code words, NCW = (frame width - 3) div the code word's CU, and only the data part goes through
  the time interleaver. The PADDING part is the PRBS of figure 5.13, restarted every frame. The frame width comes from table 5.12
  (TDM and OFDM in the same bandwidth) or from the signalling.
- **Signalling field** (table 5.9). Signalling_Version, Frame_Width_CUs, Punct_Pat_ID, the five time interleaver parameters and a
  CRC-16 over the first 96 bits, then the LL part with its own CRC-16, zeros to 1 146 bits. It is turbo coded at rate 1/5 (pattern 0)
  and bit-interleaved with H(w) = 73 w mod 5 760 (table 5.6), with no 126-of-128 puncturing. A field whose RFU byte is not 0 is
  ignored, as the table says.
- **Constellations.** QPSK and 8PSK are the DVB-S2 mappings (figures 5.17 and 5.18 are DVB-S2's figures 9 and 10). 16APSK uses the
  DVB-S2 labels on rings R1 = 1/sqrt(7) and R2 = 3/sqrt(7) (gamma = 3, figure 5.19).
- **PL slots** (5.6.4). A slot is 2 176 symbols: a pilot field of 80 symbols (1 + j)/sqrt(2) before each of the two 1 008-symbol
  sub-slots, which carry 2, 3 or 4 CU. Each slot is PL-scrambled with the DVB-S2 Gold sequence of code n (default 0), restarted every
  slot. A frame is a whole number of slots for every entry of table 5.12.
- **Symbol rate and shaping.** The rate comes from table 5.13 for 8, 7, 6, 5 and 1.7 MHz, the OFDM guard and the roll-off (0.15, 0.25,
  0.35), with root raised cosine shaping. For every combination of width, guard, roll-off, OFDM constellation and TDM modulation, the
  test checks that a TDM frame lasts exactly one OFDM SH frame.
- **Generator** (`TdmGenerator`, `TdmSignal`). Symbols, RRC shaping at 4 samples per symbol, resampling to any rate with a clock error,
  a carrier offset and noise at a given Es/N0. The engine's test signal has it as option 5 of the DVB-SH entry (TDM QPSK, 8PSK,
  16APSK, roll-off 0.25).
- **Receiver** (`TdmReceiver`).
  - Front end: the DVB-S/S2 front end (resampler to 2 samples per symbol, RRC matched filter, Gardner timing loop).
  - Slot timing: the scrambled pilot fields of four slots, correlated in blocks of 20 symbols (carrier offsets up to 1/40 of the
    symbol rate). The coarse frequency comes from the block-to-block phase steps, the fine one from the two fields of a slot.
  - Tracking: per field the phase and level, linear phase interpolation across each sub-slot, a frequency loop, and a slot grid check
    of +-2 symbols.
  - Frames: SOF correlation for each of the three modulations, which finds the frame start and the modulation, then the signalling
    field.
  - Data: max-log demapping into the same int8 time de-interleaver and turbo decoder as SH-A.
  - Symbol rate: automatic. The 12 rows of table 5.13 for the width are tried in turn (0.1 s without slots, 0.7 s without a frame),
    or the rate can be set. The clock offset is measured from the symbols produced against the samples fed, relative to the nearest
    table rate.
  - Blockages: when the input power drops, the timing loop is held at the last good rate. Slots without pilots are erased (LLR 0)
    rather than demapped, since the front end's gain would otherwise make the noise look like confident symbols. The slot grid and
    the frame grid run on as a flywheel for up to 2 s, so that the de-interleaver keeps its alignment.
- **Engine and app.** The DVB-SH entry (one entry per protocol) is automatic by default. It tries every channel width as OFDM first,
  0.3 s each as before, then every width as TDM, 1.3 s each. Under Advanced: OFDM only, TDM only, and the TDM scrambling code number.
  The Receiver tab shows the modulation, symbol rate, clock offset, frame width, code rate and time interleaver. The summary line
  reads "DVB-SH TDM ...".

Measured (simulated; `tests/dvbsh/test_dvbsh_tdm.cpp`, ctest `dvbsh_tdm`, about 15 s). Packets carry their sequence number and a
checksum, so "good" means byte-exact and in order:

| case | impairments | result |
|---|---|---|
| QPSK 1/3, 5 MHz, GI 1/4, roll-off 0.15 (952 CU), rate automatic | 3 dB Es/N0, +12.3 kHz, +20 ppm, random start | 1584 good, 0 bad codewords, clock measured +20.1 ppm, CFO +12 308 Hz |
| 8PSK 1/2, 8 MHz, GI 1/8, roll-off 0.25, rate automatic | 10 dB, -7 kHz, -15 ppm | 2480 good, 0 bad, clock -15.0 ppm |
| 16APSK 2/3, 5 MHz, OFDM 16-QAM frame (672 CU), roll-off 0.35 | 14 dB, +2.5 kHz, +8 ppm | 6368 good, 0 bad |
| QPSK 1/3, early/late interleaver 2/24/2/1/1 | 6 dB, signal off for 50 ms | 2184 good, 0 bad codewords |
| the same, short (uniform) interleaver | the same blockage | 10 bad codewords, none passed as good |
| 8PSK 2/3c | 2 dB (below threshold) | every codeword flagged bad, no wrong packet |
| signalling field alone (BPSK) | Es/N0 -2 dB / -8 dB | decoded / refused |
| sweep (ONAIR_TEST_LONG): QPSK/8PSK/16APSK x rates 1/5, 2/7, 2/5c, 2/3c x 5, 7, 1.7 MHz, rate automatic | 8 / 13 / 17 dB, +1.5 kHz, +6 ppm | 36 of 36 with 0 bad codewords |

Engine (`test_dvbsh_engine`): the TDM 8PSK test signal (5 MHz, GI 1/8, roll-off 0.25) at 8 Msps through the automatic search, with
18 dB, +6 kHz and +8 ppm: found after the OFDM widths, 3896 good packets, 0 bad, clock +8.0 ppm, CFO +6 003 Hz. The OFDM cases of this
test are unchanged.

Interpretations (the standard leaves these open, or the PDF text was unclear):
- The SH frame capacity that the time interleaver's slice_distance refers to is taken as the DATA part of the TDM frame, the only part
  that passes the interleaver. For OFDM it is the whole 816 CU.
- NCW is the largest number of whole code words that fit after the header. Table 5.11 gives NCW only for OFDM.
- The formulas for TDM and OFDM in different bandwidths were not legible in the PDF text, so only equal bandwidths (tables 5.12 and
  5.13) are built in. Another frame width is still received, since it is signalled.
- The scrambling code number n is not in the signalling field. The receiver uses 0 unless it is set under Advanced.

## Limits and what is left

- **SH-B low latency.** The low latency extension (annex B) is not separated on TDM either. LL_par_present and the burst descriptions are
  read and shown, nothing more. TDM / OFDM code combining in an SH-B receiver (both paths at once) is not done; one waveform is received
  at a time.
- **Low latency extension (annex B)** and the **SHIP** packet (annex A) are not interpreted. SHIP arrives as an ordinary TS packet.
- **Channel estimate.** Frequency interpolation is linear, and time interpolation is a zero-order hold with phase and timing correction. That is fine for static and slowly changing channels. Fast fading at vehicle speeds would need proper time interpolation, which is not tested.
- **CPU.** The turbo decoder runs on the receive thread. At good SNR it averages 1 to 2 iterations, so an 8 MHz QPSK 1/3 signal at 10 Msps decodes in about real time on an M-series Mac. Near threshold (8 iterations) it is slower than real time, and a live radio then drops samples. Files play at any speed.
- **Interleaver memory.** The receiver allows 768 MB. The longest profiles (slice_distance 255 over several slices) need more and are reported as too long.
- The Android mode list (android/tools/gen_modes.py) has not been regenerated on this branch.
- docs/BCAST-3.9.md (feat39-bcast) lists DVB-SH as not done. This branch adds it.
