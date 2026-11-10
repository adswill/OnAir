# Sensitivity 3.9, pass A (feat39-sensA): broadcast, TV and satellite decoders

Threshold = lowest SNR (sens_sweep's definition, docs/SENSITIVITY.md; Es/N0 for the satellite links) at which at least 90 % of seeded trials
decode at least half the units of a clean run. CPU = CPU seconds (all threads) the receiver used per second of the clean test signal, fed
straight into the receiver (not through the Engine). Measured on the Mac with `ONAIR_TEST_LONG=1 test_sens39a` (6 trials a point, 4 dB
steps then 1 dB) while the Mac was loaded (load 20 to 30): CPU figures move by 10 to 15 % between identical runs. Simulated signals only:
every figure comes from the mode's own generator; no off-air recording was used.

The measurement is kept as a test: `tests/sens/test_sens39a.cpp` (ctest `sens39a`). By default it checks 3 trials at each recorded threshold
+ 2 dB (AWGN and a 20 ppm carrier error) and 2 noise-only trials per mode (about 45 s); `ONAIR_TEST_LONG=1` runs the full search for AWGN,
20 ppm carrier and 50 ppm clock and fails when a threshold is more than 1 dB worse than recorded. `ONAIR_SENS39A_MODE=<id>` picks one mode,
`ONAIR_SENS39A_SNR=<dB> [ONAIR_SENS39A_SCEN=<name>]` prints the six trials of one point.

## Results

| mode | threshold before (AWGN / 20 ppm / 50 ppm clock) | after | theory | CPU before -> after | note |
|---|---|---|---|---|---|
| MetOp AHRPT (`hrpt`, 6 Msps) | 4 / 4 / 4 dB | 2 / 2 / 2 dB | about 2 dB | 0.27 -> 0.21 | changed, see below |
| Meteor LRPT OQPSK 72k (`meteor`) | 5 / 8 / 5 dB | unchanged | about 2.5 dB | 0.034 | real gap, attempt reverted (below) |
| FY-3 AHRPT (`fy3ahrpt`) | 2 / 6 (2 dB: 5 of 6) / 2 dB | unchanged | about 2 dB | 0.67 to 0.78 | at theory; one 20 ppm trial in six at 2 dB fails |
| GOES HRIT (`goes`) | -1 / -1 / -1 dB | unchanged | about -1 dB | 0.10 | at theory |
| GK-2A LRIT (`gk2alrit`) | -1 / -1 / -1 dB | unchanged | -1.1 dB | 0.05 | at theory |
| GK-2A HRIT (`gk2ahrit`) | 2 / 2 / 2 dB | unchanged | 1.9 dB | 0.50 to 0.57 | at theory |
| Iridium (`iridium`) | 11 / 12 / 12 dB (35 kHz) | unchanged | - | 1.7 to 5 (load) | see the CPU lead below |
| AM envelope / synchronous (`am`, `am-sync`) | 27 dB | unchanged | - | 0.02 / 0.05 | the harness criterion (a tone 20 dB over the audio) sets this, not the detector |

Already at theory from earlier passes (docs/SENSITIVITY-3.6.md, -3.7.md) and not touched: DVB-T, DVB-T2, DTMB, ISDB-T, DAB, DRM, FM audio,
DVB-S2, DVB-C, ATV, CDR, FM RDS (about 1 dB over its estimate). ATSC is feat39-atscperf's. The top-10 CPU modes are feat39-qa's.

## What changed (core/src/hrpt/hrpt_rx.cpp)

1. Coarse carrier. The raised-cosine fit to the averaged spectrum is flat-topped at alpha 0.6 and was off by up to about 20 kHz even on a
   clean signal; from about 15 kHz off the narrow Costas loop sometimes did not pull in at 2 to 3 dB. The receiver now also takes the fourth
   power of the matched-filtered signal, whose line at four times the offset gives the carrier to a few tens of Hz; it is used when the
   line stands 4x over the rest of the search range, otherwise the search integrates up to 4x longer (32 ms) and then falls back to the fit.
2. While the loop settles (first 50 ms) after a fourth-power estimate its frequency is held within +-3 kHz: the wide start took it to a false
   lock at 48 kHz (0.13 rad/symbol) in about one start in ten at 3 dB.
3. Re-search after a slip: when frame sync was lost, no sync word came for three frames or three frames in a row failed Reed-Solomon, the
   receiver kept the wrong phase ambiguity until 3 s without frames. It now searches the ambiguity again at once (as the Meteor receiver
   already did).
4. CPU: the matched filter runs a block at a time through `convCorr` (dsp_compat) instead of one sample at a time, the front filter is skipped
   when it is the single unit tap (decimation 1), and the sync-word search is incremental (one step per pair instead of rescanning the whole
   buffer every 4096 pairs; it also locks at the second hit instead of up to 4096 pairs later). The mixer steps stay per sample, and the
   samples before the coarse carrier is known go one at a time (the estimate changes the mixer step; the first version mixed a whole block
   with the old step and lost lock in one trial in six, found by the A/B run).

Regression test: `tests/hrpt/test_hrpt_rx.cpp` case "MetOp, 6 Msps, Es/N0 2 dB, -35 kHz, 50 ppm" needs 25 lines; the old receiver gives 21
lines and 78 bad frames there and fails. The other hrpt_rx cases (offsets to +-45 kHz, Doppler ramp, 5 / 8 / 10 Msps, FY-3, NOAA) pass,
with 25 to 35 % less receiver CPU.

## Tried and not kept

* Meteor LRPT (OQPSK 72k): the 2.5 to 3 dB gap is real. Causes found: the symbol clock integrator walks to its +-0.5 % limit while the carrier
  loop is unlocked (this OQPSK timing detector is not phase independent), so the sync words are 16324 pairs apart instead of 16384 and the
  lock takes 1 to 6 s at 4 dB; the shape-fit coarse carrier is off by up to 900 Hz. A square-law line estimate (lines at 2f +- the symbol rate,
  accurate to tens of Hz), a 500 ppm clock range, a clock integrator held until carrier lock and a tolerant sync-word spacing were tried:
  4 dB went from 4 to 5 trials of 6 and lock time did not change, within trial noise, so it was reverted. At 3 dB the Costas loop still does
  not lock with the carrier exactly known; the next step is a joint timing/carrier acquisition (for example a data-aided search on the
  encoded sync word over timing phases) rather than more loop tuning. The attempt is not on the branch.
* Iridium CPU (lead, not changed): on the clean test signal 75 % of the burst detections fail to demodulate even at 60 dB, and each failure
  runs the +-12.5 kHz uplink fallback (5 demodulations). Switching the fallback off cut CPU 2.5x with the same valid frames, but the test
  signal has no uplink bursts, so that would be unproven; it needs an uplink test signal first (and the duplicate detections need a look).

## Not done

GNSS (an earlier weak-signal change was reverted on main), DVB-S (S1), Inmarsat, Orbcomm, CubeSat, APT and HD Radio have no figures in this
pass: the Mac was loaded the whole time and each of them needs its own harness entry or a longer sweep. The GitHub sweep run 42877952408
(sens-quick, Linux) of the unmeasured modes was still running at hand-over.
