# Sensitivity, 3.7 (feat37-sens): partial, no receiver changed

Harness: `sens_sweep` (tools/sens, see SENSITIVITY.md and SENSITIVITY-3.6.md), already has AWGN, cfo2ppm/cfo20ppm, offcentre, sro50ppm. No new harness was written.

## Gap candidates from the committed baseline (tests/sens/baseline/sens-baseline.json, Linux runner, full tier)

| mode | awgn dB | worst impaired scenario | loss | note |
|---|---|---|---|---|
| ATSC 1.0 | 15 | sro50ppm 24 | +9 | largest; clock tracking at 50 ppm |
| DVB-T 8K | 17 | sro50ppm 21 | +4 | clock resampler exists (dvbt_rx.cpp ClockCorrector); drift learnt from the CP loop (gain 0.000025/symbol), noisy at low SNR |
| DTMB | 16 | sro50ppm 20 | +4 | |
| DVB-T2 | 14 | sro50ppm 17 | +3 | |
| ATV (PAL) | -4 | sro50ppm 15 | +19 | threshold criterion probably sync-line count; check |
| FM | 10 | offcentre 14 | +4 | harness off-centre artefact for narrowband modes |
| dPMR | 13 | cfo20ppm 17 | +4 | |

## Measured in this pass (Mac, quick tier, 4 trials, 2 dB steps; coarse)

| mode | awgn | cfo20ppm |
|---|---|---|
| ATV | -4 | -6 |
| D-STAR | 7 | 7 |
| NXDN | 9 | 9 |
| dPMR | 7 | 7 |

Baseline (Linux, full) shows 13 dB for these three; the quick run reads 4-6 dB lower, so either the criterion/step differs between tiers or code changed since the baseline was made. Not resolved: re-run full on the Linux workflow before trusting either. Against theory (4FSK/GFSK about 7-9 dB) these are already at theory.

## Not done
No before/after numbers: the full sweep over the wideband modes took over an hour per scenario on the Mac and was stopped. No receiver was changed. Next step: run `sens-quick.yml` on GitHub for dvbt|dvbt2|atsc|dtmb with scenario sro50ppm, then raise the CP-loop drift gain / add a pilot-based drift estimate in dvbt_rx.cpp and compare.
