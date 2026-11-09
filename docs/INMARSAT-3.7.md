# Inmarsat 3.7: Aero acquisition

Audit result: the Aero (P/R/C) and Inmarsat-C chains were already extensive (soft Viterbi, UW flywheel, ADS-C/CPDLC/ACARS, EGC, +-20 kHz search for C).
The main gap found was Aero carrier acquisition at low SNR: the squared-signal line search averaged 4 FFT blocks, compared with a fixed threshold of 12,
and on a miss threw the accumulated power away, so weak carriers never accumulated enough to lock.

## Changed
- `aero_demod.cpp`: the carrier search keeps integrating (evaluations at 4, 8, 16, 32 blocks) with a threshold that falls with the number of blocks
  (12 at 4 blocks, 5.5 at 32); only after 32 blocks without a line does it start over.
- `tests/aero/test_aero_acq.cpp`: time-to-first-good-SU against Eb/N0 (8..1 dB), carrier offset (0, +350, -650 Hz) and 3 Hz/s drift, all three rates.

## Measured (locked runs of 4, per rate/offset; Eb/N0 per channel bit)
| rate | Eb/N0 | before | after |
|---|---|---|---|
| 600 | 1 dB, off-centre | 0/4 | 4/4 (20-36 s) |
| 600 | 2 dB, +350 Hz | 3/4 | 4/4 |
| 1200 | 1 dB, off-centre | 1/4 | 4/4 (8-10 s) |
| 10500 | 1 dB | 4/4 | 4/4 |
Acquisition sensitivity gain: about 1 dB on 600 and 1200 bit/s. Existing test_aero_demod/sweep/rx/impair still pass.

## Not done (queue)
Wider Aero search range than +-700/1500 Hz in the demod (the band search places carriers), R/T burst channels, STD-C changes, UI.
