# TETRA in 3.9

Two changes: a more sensitive pi/4-DQPSK demodulator, and a first part of TETRA Enhanced Data Service (TEDS) limited to identity and signalling. All results come from OnAir's own test signal. No real TEDS or weak-signal TETRA recording was available, so everything here is **simulated only**.

## Sensitivity

### What limited 3.8

Each symbol was detected against the one before it (differential detection, with both symbols noisy). The symbol instant came from the 33 training symbols of each slot. The carrier search needed a rise of 6 dB over the local floor. sensD measured the full receiver at 9 dB in the `sens_sweep` convention. That convention sets the SNR from the total power of both test cells, which is 2.15 dB more than the stronger cell's own SNR.

### What changed (core/src/tetra/tetra_rx.cpp, tetra_phy.cpp)

- **Coherent detection.** pi/4-DQPSK is modelled as a 4-state trellis. The carrier phase comes from the fourth power of `z_k exp(-j k pi/4)` over +-20 symbols, unwrapped modulo pi/2 (windows of 10 to 40 measured the same). Bit LLRs come from a log-MAP forward-backward pass over the slot and 11 symbols on each side.
- **Known bits as a-priori.** These cover training sequence 3 at both ends (and in the neighbouring slots), the normal or synchronisation training sequence, and the 80-bit frequency correction field of the synchronisation burst.
- **Timing.** Each slot's symbol instant comes from the matched filter energy over all 255 symbols. One least-squares line is then fitted over the slots of the capture, twice, dropping slots more than half a sample off the line.
- **Re-filtering.** After a tuning error above 150 Hz, the frequency is corrected before the matched filter, and the filter runs again.
- **Turbo detection.** A new log-MAP SISO decoder for the punctured 16-state RCPC code (`sisoDecode23`, `channelExtrinsic`) feeds extrinsic LLRs back to the slot detector. This repeats up to 3 times, until every signalling block of the slot passes its CRC. It runs for BSCH, SCH/HD, BNCH and SCH/F. Traffic bursts never go through it. When the AACH fails first time, it is decoded again from the refined bits.
- **Carrier search.** A rise of 4 dB over the local floor is now enough (was 6 dB). Noise alone still gives no carrier (test).

### Measured

The new harness is `test_tetra_sens`. It takes one carrier of the test cell at 72 kHz through `decodeCarrier`, with a random start slot and sample, AWGN in 25 kHz, +-1.2 kHz and +-50 ppm. A capture passes when it gives at least half the clean capture's signalling blocks (the sens_sweep criterion). The threshold is the lowest SNR in 1 dB steps where at least 7 of 8 trials pass. Run it with `ONAIR_TEST_LONG=1`.

| step | AWGN | +-1.2 kHz | +-50 ppm | blocks at 4 dB (of 131) |
|---|---|---|---|---|
| 3.8 (differential) | 6 dB | 6 dB | 6 dB | 14 / 7 / 14 |
| coherent + known bits | 5 dB | 5 dB | 5 dB | 64 / 46 / 51 |
| + timing line | 5 dB | 5 dB | 5 dB | 76 / 66 / 70 |
| + re-filter | 5 dB | 5 dB | 5 dB | 76 / 77 / 70 |
| + turbo (3.9) | **4 dB** | **4 dB** | **4 dB** | 122 / 121 / 119 |

At 5 dB, 3.8 decoded 5, 1 and 4 captures of 8. 3.9 decodes 8 of 8 for all three, which is the default check of `test_tetra_sens`. Each step was kept only because it gave a measurable gain. The timing line helps at 4 dB, and the re-filter helps the tuning-error case.

Full receiver: the two-cell test signal at 2 MHz, 8 s per trial, through `TetraReceiver`. The tuning errors are 150 Hz, -1.2 kHz and 400 Hz, and the clock errors 0, 20 and -50 ppm. A trial passes when it gives half the blocks of a 30 dB run. SNR is that of the stronger cell. The weaker cell is 1.9 dB lower.

| | passes down to | sens_sweep convention |
|---|---|---|
| 3.8 | 7 dB (6 dB: 1 of 3) | 9.2 dB |
| 3.9 | 5 dB (4 dB: only the stronger cell) | 7.2 dB |

That is about 2 dB in the full receiver and 2 dB in the demodulator. The ideal coherent QPSK bound for the same SCH/HD block is about 1.5 dB SNR in 25 kHz at half the blocks (a genie test, not kept). The rest is the cost of differential encoding and of estimating the phase from the signal.

CPU: `decodeCarrier` takes about 0.04 s per 2.3 s capture on a strong signal (was 0.03 s). Near the threshold it takes about 0.15 s, because the turbo passes run only on slots whose CRC fails.

### Not done

- **Soft Reed-Muller decoding of the AACH.** The (30,14) code has minimum distance 8. The hard decoder corrects 3 errors and already accepts about 7 % of random words. A Chase or ML decoder would accept a larger share of noise, so the hard syndrome decoder was kept. With this change the AACH is decoded again from the refined bits.
- **Losing a capture when no BSCH decodes.** Below about 3 dB, a capture without any decoded synchronisation burst has no scrambling code, and nothing in it is decoded. The live receiver passes the identity of a known cell to later captures, which softens this. The harness does not, so its thresholds are a little pessimistic.

## TEDS (identity and signalling only)

Source: ETSI EN 300 392-2 V3.8.1, clauses 5.4, 5.8-5.17, 8.3.1, 9.4.4.2.5, 9.4.4.3.2, 9.4.4.3.6 and 21.4.4.1. No user data is decoded, and nothing is decrypted.

**Decoded:**

- **The cell's TEDS capabilities** (core/src/tetra/tetra_teds.cpp, `parseExtServices`). They come from the extended services broadcast, which is SYSINFO optional field 11 (tables 21.67 to 21.69). Section 1 gives pi/8-D8PSK support, section 2 gives 25, 50, 100 and 150 kHz QAM support. The two sections arrive in different SYSINFO PDUs and are combined per cell (`TetraCell::tedsD8psk`, `tedsQamMask`). The cell details show "TEDS: pi/8-D8PSK ..., QAM ...".
- **pi/8-D8PSK slots** on a phase-modulation carrier. They are found by the D8PSK normal training sequences 1 and 2 (equations 9.7 and 9.8), which become two new templates in the slot search. Their signalling blocks are decoded: SCH-P8/HD (196 bits, (324,19) interleaving) and SCH-P8/F (412 bits, (648,23)), with CRC-16, RCPC 2/3 and the cell's scrambling. A SYSINFO sent with pi/8-D8PSK has its 28 reserved bits removed and then goes to the normal parser. D8PSK traffic is not decoded. The carrier and cell details count the D8PSK slots and blocks. Detection is differential (max-log over the 8 steps), not the coherent path.

**Identified only (not decoded):**

- **QAM carriers.** Rises of the spectrum that give no TETRA slot structure go to `TedsIdentifier`. It checks the width against M x 2.7 kHz for M = 8, 16, 32 or 48 sub-carriers (equation 5.5). It then takes each sub-carrier through a root raised cosine 0.2 matched filter at 2400 symbols/s and looks for the common symbol clock, which at least 3/4 of the sub-carriers must share. Finally it checks that the sub-carrier symbols are not Gaussian (normalised fourth moment between 0.8 and 1.6). A QAM carrier appears once in the carrier list, with its bandwidth, sub-carrier count and an estimate of the constellation. The noise is removed from the fourth moment first. 4-QAM gives about 1.0, 16-QAM 1.28 and 64-QAM 1.34. 16 and 64 are told apart only above 28 dB, otherwise the list says "16- or 64-QAM". Carriers that are not TETRA can also be marked pi/4-DQPSK or pi/8-D8PSK from the fourth and eighth powers of the 18 ksym/s differential.
- **Why not decoded:** the QAM burst structure (Annex P symbol maps of sync, pilot and header symbols per sub-carrier), the 8-state PCCC turbo code, the RM(16,5) of the QAM slot information channel and the QAM logical channels (BSCH-Q, AACH-Q, SICH-Q, BNCH-Q with SYSINFO-Q) together are a decoder several times the size of this one. They are left for a later release. The identifier uses only the physical-layer parameters above.

**Test signal** (scene 3, "TEDS cell and QAM carrier"): the scene 0 cell announces pi/8-D8PSK and 25 and 50 kHz QAM, alternating section 1 and section 2 by multiframe. Its timeslot 3 sends pi/8-D8PSK SCH-P8/HD bursts (SYSINFO in the D8PSK form, null PDUs), following table 9.8 and the phase balance of table 9.16. It also adds a 50 kHz 16-QAM carrier at 392.5625 MHz (`QamGen`). `QamGen` follows clause 5 for sub-carrier frequencies, 2400 symbols/s, RRC 0.2 and the constellations of tables 5.4 to 5.6. Its pilots, one in 8 symbols on the unit circle, are its own and not the Annex P pattern.

**Tests** (`test_tetra_teds`):

- Extended services: encode/parse round trips, every section 1 combination and every QAM mask.
- D8PSK: the step table and the burst's phase balance.
- The TEDS cell under four impairments, from clean to 10 dB with +-900 Hz and +-40 ppm: all 39 D8PSK slots are found, 72 to 78 SCH-P8 blocks decode, and both extended services sections are read. A cell without D8PSK shows none.
- The identifier on all 12 combinations of 25/50/100/150 kHz and 4/16/64-QAM with a tuning error. 4-QAM 25 kHz at 6 dB, a pi/4-DQPSK carrier, a carrier of only D8PSK slots, analogue FM voice and noise.
- The live receiver on scene 3: the cell's capabilities and D8PSK slots, and the QAM carrier listed once as 50 kHz with 16 sub-carriers.

## Files

core/src/tetra/tetra_teds.cpp and core/include/dect2/tetra/tetra_teds.h (new). Changed: tetra_rx.cpp (detector, turbo loop, timing, D8PSK slots, identification), tetra_phy.cpp (SISO, D8PSK burst, `Modulator::pushSteps`), tetra_gen.cpp (scene 3) and tetra_tel.h (new fields). app/tetra/tetra_ui.cpp shows the TEDS lines, the carrier kinds and scene 3. tests/tetra/test_tetra_sens.cpp and test_tetra_teds.cpp are new.
