# Sensitivity 3.9, pass D (feat39-sensD): keyboard, picture, trunking, utility and satellite decoders

Branch feat39-sensD on integ-39. Every figure here comes from simulated signals: each mode's own generator, white Gaussian noise and the
impairments named. No off-air recording was used. Threshold = the lowest SNR at which at least 90 % of the seeded trials pass. Measured on
the Mac (Apple M-series, -O2, other workers loading the machine), so the CPU figures are rough (10 to 20 % between identical runs).

The method and definitions follow docs/SENSITIVITY-3.9-A.md and -B.md:

* Registry modes: tools/sens/sens_sweep.cpp trials (its SNR definition: active signal power against the noise in the stated bandwidth;
  Es/N0 for the satellite links), a trial passes when it decodes at least half the units of a clean run. 6 trials a point, 4 dB steps
  then 1 dB. Scenarios: AWGN, the carrier error of a 20 ppm radio (capped at a quarter of the channel), a 50 ppm sample clock.
  Test: `tests/sens/test_sens39d.cpp` (ctest `sens39d`, includes sens_sweep.cpp like sens39a); new registry entries in
  `tools/sens/sens_modes_d.h` (POCSAG and FLEX apart, Fusion with a real test signal, DVB-S 1 Msym/s, ALE, APT, EDACS, SmartNet, MPT 1327).
* Audio-level benches, 10 trials (8 for pictures) a point, a new noise and start each trial:
  `test_hfdig_kbdsens39` (Olivia, Contestia, MFSK, THOR, DominoEX: the key text "TEST 73" within the trial; SNR in 2500 Hz),
  `test_hfutil_sens39` (ARQ-E, ARQ-N, G-TOR: the whole message, +-30 Hz; SNR in 3 kHz),
  `test_hfdig_sens39d` (SSTV: a complete picture with a mean error of at most 20 levels, +-50 Hz and 50 ppm; HamDRM mode B: the JPEG
  byte exact, +-20 Hz; SNR in 3 kHz), `test_marine_fax_sens39` (weather fax: line lock and a mean row correlation over 0.5, +-30 Hz and
  20 ppm; SNR over 0 .. 6 kHz as in test_marine_fax_weak).
* Each test checks a few trials 2 dB above the recorded threshold by default (a few seconds); `ONAIR_TEST_LONG=1` runs the sweep.

CPU = CPU seconds per second of signal on the clean signal, receiver alone (all threads for the registry modes).

## Changed

| mode | threshold before -> after (AWGN / offset / clock) | CPU before -> after | change |
|---|---|---|---|
| Meteor LRPT OQPSK 72k, Es/N0 (1 Msps) | 5 / 8 / 5 -> **2 / 3 / 2 dB** (theory about 2.5) | 0.031 -> 0.030 | joint acquisition (1) |
| MFSK16, 2500 Hz | -9 / -9 / -8 -> **-15 / -15 / -14 dB** | 0.0008 both | likelihood-ratio soft bits (2) |
| THOR16, 2500 Hz | -5 / -5 / -4 -> **-11 / -10 / -10 dB** | 0.0004 both | soft bits instead of a gated hard decision (2) |
| DominoEX 11, 2500 Hz | -7 / -7 / -6 -> **-12 / -12 / -10 dB** | 0.0004 both | squelch at a peak ratio of 4 (2) |
| ALE 2G, 3 kHz | -6 / nothing decoded / -6 -> **-6 / -6 / -6 dB** (offset 282 Hz) | 0.015 both | tuning error from the spectrum (3) |
| ARQ-E 96 Bd cycle 8, 3 kHz | 3 -> **-2 dB** | 0.001 both | baud from the eye (4) |
| ARQ-N 96 Bd, 3 kHz | -1 -> **-3 dB** | 0.001 both | same |
| G-TOR 100 Bd, 3 kHz | 6 -> **-2 dB** | 0.004 both | same |
| DVB-S QPSK 2/3 1 Msym/s, Es/N0 | 4 / 4 / 4 dB, unchanged | **0.29 -> 0.13** | DVB-S tried before the S2 header hunt (5) |

Columns: registry modes AWGN / 20 ppm carrier / 50 ppm clock; keyboard modes AWGN / a tuning error inside the decoder's margin (MFSK16
6 Hz, THOR16 and DominoEX 30 Hz) / a 100 ppm clock.

Per point, Meteor AWGN before (the integ-39 receiver): 5 dB 6/6, 4 dB 1/2; after: 3 dB 6/6, 2 dB 6/6, 1 dB 0/6 (sync found, frames fail
Reed-Solomon: the code's limit). 20 ppm (2758 Hz) after: 3 dB 6/6, 2 dB 1/6. Lock at the threshold + 3 dB: 0.5 s.

1. **Meteor LRPT** (`core/src/meteor/meteor_rx.cpp`). SensA found the cause: the OQPSK timing detector is not phase independent, so the
   clock walked to its +-0.5 % limit while the carrier loop was unlocked, and the shape-fit carrier was off by up to 900 Hz. Now:
   * For OQPSK the mean of the squared matched-filter output has two lines, at twice the carrier offset plus and minus the symbol rate.
     A 32768-point FFT (about 0.1 s) finds them; their mean gives the carrier (to about 1 Hz), their spacing the symbol clock (a few
     ppm), and the phase of one against the other the timing (modulo half a symbol). The shape fit stays for QPSK and as the fallback.
   * Both loops then start narrow (carrier Bn T 0.002, the timing gains of the settled state) and the timing loop is held for 50 ms; for
     1 s the carrier stays within 300 Hz of the line estimate. With the wide start, the timing and the carrier slipped together by half a
     symbol and a quarter turn again and again below 3 dB (each slip cost 1 to 3 s of frames); the loops also narrow once frames decode
     on the other acquisition paths.
   * The ambiguity search scores every pairing and transform and takes the best pair of sync words a frame apart, each with up to 10 of
     52 hard errors (was the first with up to 7: at 3 dB the true words have 5 to 12 errors in the difference domain, so a frame in two
     was missed).
   * 80k interleaved: the marker search ranks candidates by error weight; the mirrored half-symbol alias matched the marker too and,
     with the line acquisition landing on either pairing, deinterleaved garbage for good.
   * `core/src/meteor/meteor_proto.cpp`, StripRows: before a row time is known, a later time stamp starts a new row (a pass that began
     mid-row put the next row's first channel into the first row; found because the lock now comes earlier).
   Not reached: the channel 64 kHz off centre (outside the +-15 kHz search, as before) and the 50 Hz Wiener phase noise scenario (fails
   before and after; a 50 Hz linewidth is far worse than a 137 MHz front end).
2. **MFSK and THOR** (`core/src/hfdig/hfdig_kbd_dsp.h` fskSoftBits, `hfdig_kbd_mfsk.cpp`, `hfdig_kbd_ifk.cpp`). Each bit's soft value is
   the log likelihood ratio of the tones that carry a 1 against those that carry a 0 (Rician against Rayleigh, from running estimates of
   the noise and of the tone, the per-tone SNR capped at 10 dB so a tone between two bins does not give sure but wrong bits), 32 per unit
   into the 8-bit Viterbi inputs. MFSK used fldigi's metric (the magnitudes of all 16 tones summed onto each bit); THOR took a hard
   decision on the strongest bin and erased the symbol below a peak-to-average of 6 or 1.2. The Viterbi lock metric and its levels are
   unchanged. A gain of 16 lost the MFSK32 picture behind the signal finder (test_hfdig_mfsk), 32 passes everything.
   DominoEX has no FEC: its characters were held back until the peak bin stood 6 times over the average of the others; noise alone gives
   2.5 to 2.8, and at 4 six minutes of noise (DominoEX 5, 11, 22, with and without the signal finder) still give no character.
   Not changed: Olivia and Contestia (already soft, at -15 / -14 / -12 dB).
3. **ALE** (`core/src/ale/ale_rx.cpp`). While no word is in sync, the averaged audio spectrum (1024 points) is searched up to 400 Hz off
   for the eight tone bands against the bands just outside them; a clear error, seen twice, with no word for 2.5 s, moves the tone
   filters, and the AFC after each word may follow to 400 Hz (was 150). The 125 Hz tone filters found nothing beyond about 100 Hz before.
   Acting on every estimate cost 1.5 dB at the AWGN threshold; the gating keeps AWGN at -6 dB. Lock with the 282 Hz error: about 5 s.
4. **ARQ-E, ARQ-N, G-TOR** (`core/src/hfutil/hfu_fsk.cpp`). The automatic baud: for each candidate the two tone energies are integrated
   over one symbol and sampled at 12 clock phases; the lowest candidate whose best phase gives decisions at least 1.6 times sharper than
   its worst, and within 15 % of the sharpest, is the baud. The transition line it used before (now the fallback) drowned in the extra
   transitions noise makes: with the baud and the shift given (as under Advanced), the old receiver already reached -1 / -2 / -2 dB. The
   old finder also picked a multiple of the G-TOR 100 baud at 7 dB in one trial in three. Unchanged: G-TOR 300 (10 dB: 170 Hz shift at
   300 Bd is a modulation index of 0.57, the tones are far from orthogonal) and ARQ-E 184.6/4 (0 to 1 dB).
5. **DVB-S** (`core/src/dvbs/dvbs_rx.cpp`). The DVB-S test (a fourth-power estimate and one sync search) now runs before the DVB-S2 header
   hunt (fitHeader over every gain and code, the VL-SNR hunt), which the profile showed as the dearest part of acquiring. This is
   acquisition CPU: a locked receiver does not hunt. DVB-S2 at 10 Msps: CPU 0.377 -> 0.389 (noise), lock time and the 3 dB point unchanged.

## Measured, not changed

| mode | threshold (AWGN / 20 ppm / 50 ppm clock) | noise bandwidth | CPU | note |
|---|---|---|---|---|
| Olivia 8/250 | -15 / -15 (+10 Hz) / -14 (100 ppm) dB | 2500 Hz | 0.0005 | soft already |
| Olivia 32/1000 | -14 / -13 (+20 Hz) / -12 dB | 2500 Hz | 0.0007 | |
| Contestia 16/500 | -12 / -13 (+25 Hz) / -12 dB | 2500 Hz | 0.0004 | |
| SSTV Robot 36 | 14 dB | 3 kHz | 0.0004 | the 20-level picture criterion; 13 dB: 0 of 8 (the error grows smoothly with the noise) |
| SSTV Martin M1 | 7 dB | 3 kHz | 0.0004 | |
| HamDRM mode B (16-QAM) | 11 dB | 3 kHz | 0.0023 | |
| Weather fax 120 lpm | 3 dB | 0 .. 6 kHz | 0.0026 | line lock holds lower; the row correlation sets it |
| FreeDV | not measured | | | the modem is libcodec2, a library the user installs; nothing of OnAir's to tune |
| Yaesu Fusion (C4FM) | 8 / 8 / 8 dB | 12.5 kHz | 0.011 | the sens_modes.h entry had no test signal (std 130 has no mode synth); sens_modes_d.h builds one |
| TETRA (cell test signal) | 9 / 9 / 9 dB | 25 kHz | - | sens_sweep --full, 6 trials (quick tier, 2 dB steps: 10 / 10) |
| TETRAPOL | 9 / 10 / 9 dB | 12.5 kHz | 0.0033 | soft decoder and AFC already |
| EDACS control channel | 12 / 12 / 12 dB | 12.5 kHz | 0.0044 | GFSK through the discriminator; the generator's deviation gives h = 0.29 |
| Motorola Type II (SmartNet) | 4 / 7 / 4 dB | 12.5 kHz | 0.0045 | see below |
| MPT 1327 | 9 / 11 / 9 dB | 12.5 kHz | 0.0029 | FFSK on FM: at the FM threshold |
| POCSAG | -1 / 1 / -1 dB | 25 kHz | 0.019 | |
| FLEX | 5 / 4 / 5 dB | 25 kHz | 0.017 | |
| HF ALE | see above | | | |
| Inmarsat-C | 0 / 0 / 0 dB | 1.2 kHz | 0.025 | lock 10 to 17 s (frames are 8.64 s) |
| Inmarsat Aero | 11 / 11 / 11 dB | 12.5 kHz | - | sens_modes.h entry |
| Orbcomm | 7 / 7 / 7 dB | 10 kHz | 0.024 | |
| NOAA APT | -8 dB or lower (the lowest point tried) | 34 kHz | 0.019 | lines with sync |
| GNSS (GPS L1 C/A) | acquisition 20 of 20 at 35.8 dB-Hz, 6 of 20 at 33.8; tracking holds at 32 dB-Hz; a 40 dB-Hz sky gives a fix, 36 dB-Hz 5 of 8 tracked and no fix | | | test_gnss_loops / test_gnss_sweep; an earlier weak-signal change was reverted on main, not touched |
| DVB-S (S1) | 4 dB Es/N0 (QPSK 2/3; QEF theory about 5.9 dB for BER 2e-4, the half-the-packets criterion is looser) | 1 Msym/s | see above | |
| CW skimmer | 4 / 4 / 2 dB | 500 Hz | 0.014 | |

## Tried and not kept

* **SmartNet**: a soft two-state Viterbi for the OSW code (instead of the syndrome repair) gave the same 4 / 7 dB: the limit is the FM
  discriminator, a cliff from 6 of 6 to 0 of 6 within 1 dB. A channel filter of +-4.5 kHz instead of +-7 kHz gave 3 dB AWGN (+-3.5 kHz:
  2 dB, but 10 dB with the 3.1 kHz offset); an AFC from the discriminator mean converged on the clean signal but did not recover the
  offset case near the threshold (noise pulls the mean towards the channel centre). The narrower filter was not kept: the generator's
  1.2 kHz deviation is not checked against real control channels.
* **THOR lock**: the lock metric relative to the input confidence (instead of the fixed 127) bought nothing at 0.5 and gave characters
  from noise at 0.25.
* **TETRA**: decision-feedback differential detection (each symbol against a reference averaging the previous ones, turned by their
  decided steps) gave 10 dB against 9 dB for plain differential detection at reference weights 0.5 and 0.7; reverted.
* **CW**: the chain tunables (envelope smoothing, weak-signal smoothing, open ratio) changed nothing in the skimmer path the registry
  uses; sensB's estimate stands (about 8 dB over a perfect-clock decoder), and a real gain needs new timing work.

## Leads

* TETRA 9 dB in 25 kHz is about 10.4 dB Es/N0 for pi/4-DQPSK at 18 ksym/s with its coding: probably 2 to 3 dB to find, not in the
  differential detector (above).
* EDACS 12 dB and Fusion 8 dB: discriminator receivers; a two-tone (or four-tone) matched-filter detector would be the next step.
* GNSS acquisition at 35.8 dB-Hz: longer coherent and non-coherent integration would gain a few dB at more CPU.
* The sens39d harness gets nothing from the threaded TETRA receiver in its clean-signal check (sens_sweep itself does); not looked into.
* `test_dvbs_narrow` fails six cases against the prebuilt integ-39 library on the Mac with and without the DVB-S change; left to CI.

## Tests

`sens39d` (Meteor and ALE by default, about 10 s; the rest with ONAIR_TEST_LONG=1), `hfdig_kbdsens39`, `hfutil_sens39`, `hfdig_sens39d`,
`marine_fax_sens39` (each 1 to 2 s by default). Run against the prebuilt integ-39 core with the changed files: meteor_rx, meteor_proto,
hfdig_kbd, hfdig_mfsk, autoparam_kbd, ham_kbd, ale_rx, ale_wide, hfutil_fsk, hfutil_engine, dvbs_rx, dvbs_s1, dvbs_rx2, dvbs_impair pass;
noise-only checks in every bench give no output. meteor_engine does not run in that link set-up (it fails the same way without the
changes). GitHub quick.yml (Linux, run 42909490424): sens39d, meteor_rx, meteor_proto, meteor_engine, hfdig_kbdsens39, hfdig_kbd,
hfdig_mfsk, hfutil_sens39, hfutil_fsk, hfutil_engine, ale_rx, ale_wide, ale_engine, hfdig_sens39d, marine_fax_sens39, autoparam_kbd,
ham_kbd: 17 of 17 passed.
