# Sensitivity of every engine mode (4.1, feat41-sensAll)

All figures are from simulated signals: each mode's own built-in test signal, white Gaussian noise, synthetic impairments. No recording
was used. They are measured on GitHub's Linux runners (`sens-quick.yml`, modes `qa:REGEX@k/N`).

## Why a second harness

`sens_sweep` (docs/SENSITIVITY.md) measures the modes that have a receiver block in `tools/sens/sens_modes.h`, about 100 of them. Most
modes added in 3.9 and 4.0 have none, but every mode with a built-in test signal already runs through the QA sweep
(`tests/qa/test_qa_sweep.cpp`): generator -> 16-bit file -> Engine -> the mode's decoder, with the receiver's own count of checked units
(CRC, FEC, parity; the text decoders by what they save). `tools/sens/qa_sens.py` drives that binary's two stages for a series of SNRs, so
every mode is measured the same way and through the same Engine path a user has.

## Method

- **SNR** = power of the active part of the clean signal / noise power in the signal's own 99 % band (`B99`, measured on the clean signal
  by the QA generator stage, the floor of the generator removed). This is the QA sweep's `snr` file. It is a C/N in the occupied band,
  so for a single carrier it is close to Es/N0 with the roll-off counted. Where the test signal holds several carriers, spurs or a wide
  scene, B99 is wider than one channel and the figure is not comparable (marked "SNR band too wide": a -24 in the table is the bottom of
  the search, not a sensitivity).
- **Scenarios**: `awgn`; `cfoclk` = +2 kHz carrier and a +50 ppm sample clock with the same noise (a cheap radio's crystal); `offcentre` =
  the channel moved by a quarter of the mode's channel width (at least 2 kHz) and a -50 ppm clock, with the same noise. On HF the 2 kHz is
  more than a crystal gives and is outside an SSB-audio decoder's window: a "-" there is expected (qa_known.inc says so too).
- **Pass**: a trial passes when the receiver counts at least half the units of the clean run (the sens_sweep rule). A lock-only mode
  (monitors, identification-only modes) passes on its lock flag; those rows say so and their false rate is a false-lock rate.
- **Search**: 2 trials (two noise seeds) a point, from 24 dB down in 4 dB steps while every trial passes, then one 2 dB step; the
  impairment scenarios are measured at the same points and refined by 2 dB too. The threshold is the lowest SNR reached from the top
  where every trial passed. Resolution 2 dB. 8 s of signal a trial (32 s when 8 s gives no unit on the clean signal).
- **False decodes**: the noise-only file of every point at or below 6 dB (the same noise, no signal) through the same receiver; any
  checked unit counted there is a false decode. The column gives units / noise-only trials.
- **Theory**: for the modes whose physical layer is simple and known, the SNR in B99 an ideal receiver needs for 90 % of its units,
  from the bit error rate of the modulation: (1 - BER)^L = 0.9 for an L-bit unit, SNR = Eb/N0 + 10 log10(Rb / B99). Uncoded bound unless the
  note says otherwise (a code makes the true limit lower, so the gap shown is then a lower bound of the real gap). Noncoherent binary
  FSK: BER = exp(-Eb/2N0)/2; GMSK BT 0.3: Q(sqrt(1.36 Eb/N0)); DBPSK: exp(-Eb/N0)/2. A negative gap of more than 2 dB means the
  assumption does not fit the test signal (shorter unit, code, several signals in B99) and is marked.

Limits: 2 trials a point and 2 dB steps, so a threshold is good to about +-2 dB; a few rows show a trial that failed above the threshold
(threaded decoders on a loaded 4-core runner running three modes at once: TETRA is one), marked. Units that arrive only at the end of the
8 s file (long interleavers, message decoders) make the threshold latency-limited: marked; their decoder-alone figures in
SENSITIVITY-3.9-D.md (MFSK16 -15 dB in 2500 Hz, THOR16 -11 dB) are the better numbers.

Modes with no built-in test signal (the generator lives in their unit test only, e.g. DCR, GW FSK, the FSK modems, SSR, the ILS markers,
the Rivet modes) cannot run here; their figures come from their unit tests' noise sweeps (docs/RIVET-MODES-4.0.md, DCR-4.0.md,
FSKMODEM-4.0.md, SSRINT-4.0.md, MARKER-4.0.md) and are listed after the table.

TABLE

### Modes measured by their unit tests only (no engine test signal)

Noise sweeps printed by the unit tests on the GitHub runner (quick.yml runs 43376327883, 43383912499; SNR in the band each test names).

| mode | band | 90 % point | 50 % point | false | source |
|---|---|---|---|---|---|
| FSK modems, Bell 103 300 Bd (24 bytes) | 3 kHz | **6 dB** (4.0: 14 dB) | 3 to 6 dB (4.0: about 10 dB) | 0 (20 s + 4 x 120 s noise, one tone, a non-standard tone pair) | test_x40_fskmodem |
| FSK modems, Bell 202 1200 Bd | 3 kHz | 10 dB, 65 of 72 bytes (4.0: 14 dB) | 6 to 10 dB (4.0: about 10 dB) | 0 | test_x40_fskmodem |
| DCR (6.25 kHz) | 6.25 kHz | 6 dB, 16 of 16 calls | about 5 dB (9 of 16) | 0 wrong calls | test_x40_dcr |
| GW FSK 100 Bd | 3 kHz | 6 dB | 2 to 4 dB | 0 | test_x40_gwfsk |
| SSR interrogations (Mode A) | pulse amplitude / noise rms | 12 dB (1098 of 1200) | 9 to 12 dB | 0 wrong modes | test_x40_ssrint |
| ILS markers | 12 kHz C/N | 8 dB (4 of 4 named) | 0 to 4 dB | 0 wrong names | test_x40_marker |
| Rivet modes (XPA, XPA2, CROWD36, FSK200/500, FSK200/1000) | 3 kHz | see RIVET-MODES-4.0.md (XPA2 -14.5 dB ... FSK200/500 +1.3 dB) | | 0 | test_x40_xpa etc. |

Theory for these: Bell 103 noncoherent FSK, 300 b/s, 240 bits at 90 %: Eb/N0 11.5 dB, 1.5 dB in 3 kHz (gap now 4.5 dB, was
12.5 dB); Bell 202 1200 b/s: 7.5 dB in 3 kHz (gap 2.5 dB); DCR (NXDN physical layer, K = 5 code): the NXDN receiver's 9 dB in
6.25 kHz (sens_sweep) is the reference, DCR is at or under it.

## Fixes (item 3) with before / after

| mode | change | before | after | false decodes |
|---|---|---|---|---|
| FSK modems (Bell 103 / V.21 / V.23 / Bell 202) | the burst gate at 3 x the floor (was 8 x); the band, sideband and tone-share gates on the power over the noise (they used the total power, so the noise in the empty sideband rejected every burst under about 8 dB in 3 kHz) | Bell 103: 10 dB 2 of 3, 6 dB 0 of 3; Bell 202: 10 dB 2 of 3, 6 dB 0 | Bell 103: 6 dB 3 of 3 (all 72 bytes); Bell 202: 10 dB 3 of 3 (65 / 72 bytes) | 0 in 20 s + 4 x 120 s of noise (two levels), one tone, a non-standard tone pair, a 600 baud burst; Bell 202 at 6 dB gave one message with the bytes shifted (0 of 72 in place) |
| DCR | tried: the sync search taking one wrong sync symbol of ten | 6 dB 16 / 16, 5 dB 9 / 16, 0 wrong | 6 dB 14 / 16, 5 dB 12 / 16, 1 wrong call, a scan case lost | **reverted** |

The other modes furthest from theory, and why they were not changed in this pass:

- **G-TOR 300 Bd** (+9 dB to the uncoded bound): 200 Hz shift at 300 Bd is a modulation index of 0.67 and the noncoherent tone
  detector loses most of that (SENSITIVITY-3.9-D.md found the same). The gain needs a coherent / sequence detector of the CPFSK; not
  done in this pass.
- **MDC-1200** (22 dB in its 3.9 kHz band, about 17 dB C/N in 12.5 kHz): the MDC decoder is only fed while the voice squelch is open
  (chv_rx.cpp `emit`: `if (mdc_ && b.gate)`), and the default squelch opens at 5 dB of quieting. Feeding it always (its own sync and
  CRC decide) is the likely fix; not verified in this pass, so not committed.
- **HF packet 300 Bd** (+4.7 dB): the same non-orthogonal shift (200 Hz at 300 Bd) as G-TOR, partly physics.
- **ATIS** (+2.4 dB), **NGHam**, **LBJ** (+1.3 dB): within the 2 dB resolution of this table plus the FM and code effects the bound does not count.
- **TETRA** (26 dB here, about theory in its own sens test): the trials that fail are not SNR-driven (a 22 dB point passed both trials, a
  24 dB point failed one): the threaded receiver on a loaded runner. Not a sensitivity loss.
- **MFSK, Olivia/MT63/keyboard modes, ALE, HFDL, CIS, KAIT, multitone, Polish time code (eczas)**: units arrive at the end of the 8 s file
  (long interleavers, minute frames): the threshold is limited by the trial length; the decoder-alone sweeps (SENSITIVITY-3.9-D.md) are the figures.

False decodes found by the noise-only runs (blocks counted on noise alone, not lock flags):
- **nfc** (798 units in 2 noise trials): known, qa_known.inc says its counter has no check (picture lines / measurements).
- **noaaapt** (221 units in 16): APT counts picture lines, which carry no check (the sensData worker has APT).
The lock-only monitors (callisto, erminaz, radiojove, shortwave, beaconevents) lock on noise by design: their lock flag is "something is
there", not a decode.
