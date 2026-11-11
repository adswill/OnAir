# Sensitivity 4.0, lane A, part 2 (feat40-sensA2)

Same method as SENSITIVITY-4.0-A.md: `sens_curve` (generator signals, random start and carrier phase, white noise, SNR in 10 kHz for sondes),
fraction of the clean-signal units decoded, 10 trials a point (8 for iMet-54), "before" = integ-40 at b0a1be3e5. Simulated signals only.

## 1. Narrow channel path for RS92, DFM and iMet-54

The 4.5 kHz half-width path next to the wide one (RS41, part 1) now also feeds the symbol chains of RS92 (4800 Bd), DFM (2500 Bd) and
iMet-54 (4800 Bd). Change: `narrowChannel()` returns true in those three decoders. No new DSP.

| type, condition | 90 % point or fraction at the cliff, before | after | gain |
|---|---|---|---|
| RS92 AWGN | 8 dB 8 %, 6 dB 0 % | 8 dB 95 %, 6 dB 68 % | about 2.5 dB |
| RS92 +8 kHz (20 ppm) | 8 dB 5 % | 8 dB 95 %, 6 dB 70 % | about 2.5 dB |
| RS92 +150 kHz off-centre | 8 dB 14 % | 8 dB 95 %, 6 dB 71 % | about 2.5 dB |
| RS92 50 ppm clock | 8 dB 8 % | 8 dB 95 %, 6 dB 68 % | about 2.5 dB |
| DFM AWGN | 8 dB 50 %, 6 dB 0 % | 8 dB 60 %, 6 dB 40 % | about 1.5 to 2 dB at the 50 % point |
| DFM +8 kHz | 8 dB 10 %, 6 dB 0 % | 8 dB 70 %, 6 dB 30 % | about 2 dB |
| DFM 50 ppm clock | 8 dB 50 %, 6 dB 0 % | 8 dB 60 %, 6 dB 40 % | about 1.5 dB |
| DFM +150 kHz | 8 dB 10 % | 8 dB 70 %, 6 dB 50 % | about 2 dB |
| iMet-54 AWGN | 19 dB 91 %, 16 dB 3 % | 19 dB 98 %, 16 dB 98 %, 13 dB 20 % | about 3 dB |
| iMet-54 +8 kHz | 19 dB 93 %, 16 dB 0 % | 19 dB 98 %, 16 dB 96 %, 13 dB 10 % | about 3 dB |

(RS92 and DFM curves are in the 4 to 14 dB range, 2 dB steps; iMet-54 in the 10 to 22 dB range, 3 dB steps.) Noise-only runs: 0 units
before and after for all three (4 runs of 4 s each).
Not kept: MRZ (the narrow path made `sonde_extra_rf` fail: nothing decoded at any carrier error, its deviation is wider than the filter in the
generator) and Meisei iMS-100 (the harness decodes 0 % before and after, so it cannot be measured; left as it was). M10 / M20 keep the wide
path: at 9600 Bd they need more than the 4.5 kHz half width.
Test: `sonde_sens40` extended (RS92 8 dB at 0 and +8 kHz 4 of 4 trials, iMet-54 16 dB +8 kHz 4 of 4, DFM 8 dB +8 kHz 2 of 4; the 3.9 receiver
fails these). `sonde_extra_rf`, `sonde_rf_dfm`, `sonde_rs92_rf`, `sonde_all_rf`, `sonde_interf`, `sonde_basic`, `sonde_rs92_kat` pass locally.
Real RS92, DFM and iMet-54 deviations and skirts are not measured: the generator's values are assumed.
