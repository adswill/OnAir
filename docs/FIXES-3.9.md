# Fixes for 3.9 (feat39-fixes)

Simulated signals only. No real recording was used for any of these.

## DRM test signal froze at 16-QAM, protection 2 or 3
Cause: modes A to D only have protection levels 0 and 1 for 16-QAM. The generator built an invalid transmitter, and its fallback kept the invalid level. `DrmSynth::generate()` then waited forever for samples.
Fix: the level is clamped to the levels the QAM has. The fallback is the usual signal. A transmitter that cannot be built gives silence. The panel greys out the impossible choices.
Test: `drm_gencombo` runs every mode, occupancy, QAM and protection combination.

## DVB-S2 at 10 to 60 ksym/s
The manual rate now goes down to 10 ksym/s (the old floor was 0.05 Msym/s). Auto searches from 10 ksym/s, using a finer search spectrum (up to 131072 bins, every other block) at input rates up to about 13 Msps.
Test: `dvbs_narrow` uses QPSK 1/4 short frames with pilots at 25 to 33 ksym/s (32 ksym/s among them), roll-off 0.20, a carrier offset, a clock offset and noise. It runs with Auto and with the rate set by hand. 10 ksym/s is tested by hand only.

## Pagers and APRS on HF (-200 dBFS with Whole span off)
`pager_hf` runs pagers (POCSAG 512) and packet (1200 baud) through the engine at 466 / 144.8 MHz and at 6.1625, 3.5, 10.1476 and 29.25 MHz, with Whole span off and on. See the CI result below. The synthetic source is not a HackRF, so the test covers the engine's frequency, offset and rate path but not the radio driver.

## TETRAPOL channel filter control (#408)
The TETRAPOL channel filter was fixed at about +-6.5 kHz. It is now in the toolbar's Baseband filter row (key `tetrapol`), with width, shape and transition. It applies at each start and retune.
Test: `tetrapol_filter` covers the following:
- custom widths and shapes, with offsets and noise
- a 1.5 kHz filter, which stops decoding (this proves the setting is used)
- a carrier 12.5 kHz away and 10 dB stronger, with a 10 kHz filter

## Touch start on small screens after an upgrade (#139)
Every save writes `touchUi=false`, so an install from before 3.7 counted as having chosen and never got the automatic touch start. Now only `touchAutoSeen`, or touch already being on, counts as a choice. Test: `touch`.

## Slow-runner failures from 3.8 run 42841590406
- hdr_engine: the load check used the wall clock (91 % on a busy Linux runner). On Windows the test ended before the first 2 s window, so it read 0.0 %. The engine now also reports `SampleLoss::cpuPct` (thread CPU time against signal time). The test checks that value and waits for a measured window.
- gain_opt_engine: the synthetic source waits for the receiver, so on a busy runner up to about 1.8 s of samples made before a gain change sat in the ring. The measured level lagged one step behind the gain. The optimiser's clock now stops until those samples are processed (`Engine::backlogSec()`). This affects the test only: a live radio's ring is cut back already (`catchUp`).
- sonde_engine, iridium_app: these ran 20 s of wall clock, but Windows processed only 13.5 s of signal. They now count signal time. Iridium plays at half speed on CI, because its burst queue dropped bursts under load.
- scan_handover: the lock timeout gets the CI slack.
- gnss_glonass: six cases took more than 2400 s on a busy runner. It is now split into two tests (`gnss_glonass`, `gnss_glonass_b`) with the same cases, each with its own time limit.
- inmc_slow_rates (Windows only): the test now uses the thread CPU time of `feed()` instead of the process time. Not verified on Windows: if the receiver really is about 15x slower there than on Linux, this check will still fail, and that needs a Windows profile.
- specx_idmatrix: identical numbers on Linux and Windows (40.6 % / 72.2 % against limits of 42 / 75), and a pass on macOS. That is deterministic, not flaky. See the CI result below.
