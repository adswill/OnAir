# Sensitivity 4.0, lane A, part 1 (feat40-sensA)

Measured with the committed generators only (no off-air recording), on the Mac, against the integ-40 core (3.9 receivers) for "before".
Numbers are the mean fraction of the clean-signal valid units (RS41: frames whose Reed-Solomon and block CRCs pass) decoded over 20 seeded
trials per SNR (random start inside the signal, random carrier phase, white noise), SNR in 10 kHz (the generator's definition for sondes).
`sens_curve` (new, `tests/sens/sens_curve.cpp`, built as `sens_curve` on non-Windows) prints this curve for any mode of the sens_sweep
registry: `sens_curve <mode> <snrFrom> <snrTo> <step> <trials> [cfoHz] [sroPpm]`. It is a finer view than the 90 % threshold of sens_sweep.

## 1. Radiosonde RS41: second, tighter channel filter

Cause. The receiver's channel filter is 10 kHz half width (20 kHz noise bandwidth) for every 400 MHz sonde type, wide enough for the 4.8 kHz
deviation of the M10 and for the carrier search error. An RS41 (4800 Bd, +-2.4 kHz) occupies about +-5 kHz, so the FM discriminator saw
about 3 dB more noise than it needed to and sat below its threshold from 8 dB down: the header was found (the carrier search and the sync
word are not the limit: 22 headers found in 24 trials at 6 dB) but the frames carried more than the 12 bytes per codeword that Reed-Solomon
corrects. A hard 1.5 % bit error rate is the Reed-Solomon limit; ideal non-coherent FSK reaches it at about 5.5 dB.

Change (`core/src/sonde/sonde_rx.cpp`, `SondeBitDecoder::narrowChannel()` in `sonde_bits.h`, true only for `Rs41Decoder`): next to the
wide filter every 400 MHz channel now runs a second filter of 4 kHz half width (8 kHz noise bandwidth) and a second discriminator with its own
mean; the symbol chain of the RS41 (4800 Bd) is fed from that one. The wide path still serves the other types, the carrier follow-up (AFC),
the 1 ms presence check (which is now also true when the narrow path sees the carrier). L-band (LMS6) is not changed. CPU: one more
decimating FIR of about 50 taps at 96 kHz per channel.

| RS41 alone | 90 % of the frames before | after | gain | 50 % point before / after |
|---|---|---|---|---|
| AWGN | 9 dB | 7 dB | 2 dB | 8.2 / 5.8 dB |
| 20 ppm carrier error (+8 kHz) | 9 dB | 6 dB | 3 dB | 8.3 / 5.5 dB |
| off-centre (+150 kHz) | 9 dB | 6 dB | 3 dB | 8.3 / 5.5 dB |
| 50 ppm clock | 9 dB | 7 dB | 2 dB | as AWGN |

(before: 7 dB 0 %, 8 dB 42 %, 9 dB 94 %; after: 5 dB 3 %, 6 dB 79 %, 7 dB 95 %; with +8 kHz after: 6 dB 91 %.)
Default test signal (RS41 + DFM-17 + M10 at the same level, the sens_sweep `sonde` mode): half of the units at 10.9 dB before, 8.8 dB after
(the DFM and M10 curves are unchanged by construction, so the mix cannot reach 90 %).
False decodes: noise-only runs give 0 units before and after (4 runs of 4 s). A false header needs the 64 bit sync word with at most one
error and then three block CRC-16 and the Reed-Solomon codewords to pass.
Not simulated: a real RS41's deviation, its TCXO drift and the real Gaussian shaping are assumed values in the generator (sonde_gen.cpp).

Test: `tests/sonde/test_sonde_sens40.cpp` (ctest `sonde_sens40`): RS41 at 7 dB with 0, +8 kHz and -3 kHz carrier error, 3 of 3 seeded trials
(the 3.9 receiver fails all three at 7 dB), DFM and M10 at 20 dB still decode, noise alone gives
nothing. The existing sonde tests were run against the changed objects: basic, all_rf, interf, extra_rf, rs41_roundtrip, rs41_kat,
rf_dfm, rf_m10, rf_m20, flight, profile all pass (test_sonde_engine and the GUI not run locally).

## Tried, measured, not kept

* **Soft-decision Reed-Solomon for the RS41** (the soft slicer value per bit, the 6 to 20 least reliable bytes of a failed codeword erased,
  errors-and-erasures Berlekamp-Massey): no gain (6 dB 84 % with and without). The failures sit at the FM-threshold cliff, where whole
  codewords carry more than 20 bad bytes, so erasing the doubtful ones does not rescue them. The block CRCs would have caught wrong
  corrections. Removed again.
* **Click suppression** (weighting or holding the discriminator output where the envelope fades) in the shared `FmFront` of the packet
  receivers: the APRS/AFSK 1200 curve did not move at 3 to 8 dB (24 trials each), so its threshold is not click noise. Reverted.
* **Clipping the sonde discriminator** at 3.5 to 8 kHz: worse (8 dB 42 % to 2 to 12 %). Reverted.
* **Wireless M-Bus, non-coherent tone correlators** (two complex correlators per chip against the carrier error from the discriminator mean,
  window 80 % and 100 % of the chip): 1.5 to 3 dB worse for T1/C1/S1 than the discriminator already in use. Per-PHY thresholds of the current
  receiver (one PHY at a time, 8 trials): S1 10 dB, T1 11 dB, C1 12 dB, which is the ideal non-coherent figure for the chip error rate a
  frame without FEC needs (about 12 dB for T1). The 17 dB of docs/SENSITIVITY-3.9-C.md is the five-PHY mix of the harness (units from the
  weakest PHY), not a receiver gap. Reverted.
* **Looked at, nothing to gain:** TETRA (coherent detector with training-sequence reference and soft bits since 3.9), Meteor LRPT (2 / 3 / 2 dB
  since the 3.9 joint acquisition: at theory), HD Radio AM and FM (16 to 17 dB in the harness is about 0 to 1 dB of digital sideband Es/N0,
  at theory for the rate-2/5 code), CW (the detector already works in a 100 Hz bandwidth, 4 dB in 500 Hz).

## Leads for the next pass

* The channel-filter lesson applies elsewhere: a receiver whose channel filter is wider than the signal plus the carrier uncertainty sits
  below its FM threshold for no reason. A fixed-width narrow path next to the wide one (as here) costs one FIR. Candidates to measure:
  M10 / M20 (+-4.8 kHz; filter 10 kHz half width now), DFM, POCSAG / FLEX, the 4FSK voice modes, EDACS, SmartNet, Fusion.
* The 20 ppm and off-centre cases are now better than AWGN-before for the RS41; a joint timing and carrier search for the other sonde types
  and a multi-symbol (MSDD) detector for h = 1 GFSK are open.
