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


## feat37-sens2: measured before/after (threshold = lowest SNR dB with >= 90 % success; same harness, same scenario)

Harness: sens_sweep, full tier, 6-12 trials; GitHub Linux runner (CI runs 41854172438, 41983944559, 41986088050) and Mac for the narrowband modes.

| item | before | after | verdict |
|---|---|---|---|
| FM off-centre (140 kHz off), Linux | 14 dB (committed baseline) | 5 dB (awgn 5) | FIXED: the station finder demanded the window 10x over the noise floor; now 1.6x. Mac before/after 11 -> 5 dB. fm-rds 18 -> 16 (= its awgn). fm_* ctests (8) pass, no-offset FM unchanged at 5 dB |
| ATSC sro50ppm | baseline 24 (awgn 15) | 15 (awgn 15) | already gone on integ-37, no change made |
| DVB-T sro50ppm | baseline 21 (awgn 17) | 15 (awgn 15) | already gone |
| DVB-T2 sro50ppm | baseline 17 (awgn 14) | 14 (awgn 14) | already gone |
| DTMB sro50ppm | baseline 20 (awgn 16) | 18 (awgn 17, 12 trials) | 1 dB, within trial noise; no change |
| ATV sro50ppm | baseline 15 (awgn -4) | -5 (awgn -4) | already gone (the old 15 was stale) |
| dPMR cfo20ppm | baseline 17 (awgn 13) | 7 (awgn 7), Mac | already gone |
| D-STAR / NXDN / dPMR awgn | baseline 13 | 7 / 8 / 7 (full tier, Mac, 6 trials) | RESOLVED: the full and quick tiers agree; the 13 dB baseline is stale (code improved since). Needs a Linux re-baseline; not done |

So the SRO gaps listed above were stale in the committed baseline: with current integ-37 code none of the wideband modes loses more than 1 dB at 50 ppm, and no SRO tracker was written (it would have had no measurable gain). Narrowband off-centre for NXDN/dPMR gives no lock at any SNR (140 kHz+ shift is outside a 6.25 kHz channel: harness artefact, not a receiver gap).
The committed baseline was updated for fm and fm-rds only (merged from the Linux run). The other stale entries should be refreshed with a full Linux sweep (update_baseline).
