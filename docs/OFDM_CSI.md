# OFDM TV: channel-state weighting and narrowband-interferer erasure (DVB-T, DVB-T2, ISDB-T)

Two things decide how well the error correction can use the demodulated cells of an OFDM signal: how reliable each cell is said to be, and whether a cell that
is wrong for a reason the receiver has not modelled is flagged.

## 1. Channel-state-weighted soft decisions (already in all three receivers; measured here)

Every receiver gives each equalised cell a noise variance `n0 = noise / (2 |H|^2)` (DVB-T `dvbt_rx.cpp` `equalise`, DVB-T2 `t2rx_data.cpp` with a per-carrier
noise track, ISDB-T `isdbt_demod.cpp` `sigma2 / |H|^2`), and the demappers scale the bit LLRs by it. A carrier in a deep notch of an echo channel therefore
counts for little and a strong one for a lot. This existed before this change; what was missing was a measurement, which `test_ofdm_csi` and
`test_ofdm_csi_isdbt` now make (`DECT2_OFDM_FLAT_N0=1` replaces the weights with their mean, for A/B tests only).

## 2. Narrowband-interferer erasure (new, `core/include/dect2/ofdm/csi_erase.h`)

A CW tone, an analogue carrier or LTE leakage on a few carriers adds power that the common noise estimate does not contain, and the cells there come out
with confident but wrong soft values. `CarrierInterference` tracks the slow average of each carrier's equalised power (data cells have unit mean power in all
three standards). A carrier whose average is more than 1.6 x (1 + 2 n0) is carrying something else: its noise variance is raised by the excess, and the cell is erased
(n0 = 1e6) when the excess is more than 10 x. Warm-up is 24 symbols per carrier, the average is an EMA of 1/32; on clean carriers nothing changes
(the threshold is about 4 sigma of the average). Always on, no setting; `DECT2_OFDM_NO_ERASE=1` switches it off for A/B tests.
Hooks: DVB-T data carriers in `equalise`, DVB-T2 data carriers per symbol, ISDB-T coherent (synchronous) segments. Differential (DQPSK) ISDB-T segments are not tracked.

## Measured (synthetic IQ, generator -> channel -> receiver; threshold = lowest SNR in dB with <= 5 % damaged packets, DVB-T; 90 % of packets, ISDB-T)

DVB-T 2K, 16-QAM 2/3, GI 1/4 (`test_ofdm_csi`):

| channel | flat n0 | channel-state | + erasure | gain of weighting | gain of erasure |
|---|---|---|---|---|---|
| AWGN | 9.5 | 9.5 | 9.5 | 0.0 | 0.0 |
| static echo -2 dB at 40 samples | 15.0 | 10.0 | 10.0 | +5.0 | 0.0 |
| echo -3 dB at 30 samples, 120 Hz Doppler | 14.0 | 10.0 | 10.0 | +4.0 | 0.0 |
| 24 CW tones, each 3 dB above a carrier | 11.5 | 11.5 | 9.5 | 0.0 | +2.0 |
| 24 CW tones, each 6 dB above a carrier | 14.0 | 13.5 | 9.5 | +0.5 | +4.0 |
| echo + 24 CW tones (+3 dB) | no lock | no lock | no lock | - | - |

ISDB-T mode 3, GI 1/8, 16-QAM 2/3, 13 segments (`test_ofdm_csi_isdbt`):

| channel | flat n0 | channel-state | + erasure |
|---|---|---|---|
| AWGN | 10 | 10 | 10 |
| static echo -2 dB at 100 samples | 19 | 13 | 13 |
| 24 CW tones, each 6 dB above a carrier | 13 | 13 | 11 |
| echo -3 dB + 24 CW tones (+6 dB) | 20 | no pass (lock) | 14 |

The echo gains are larger than the 0.5 to 2 dB expected because the "flat" baseline here uses the mean noise variance on strongly notched channels; against a
receiver that ignored the channel altogether the figure is the table's. Erasure never costs anything on AWGN, echo or Doppler channels. DVB-T2 uses the same
helper and passes its existing tests; it has no sweep of its own yet.
