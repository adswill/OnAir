# Sensitivity 3.9 C: aviation, marine, IoT and drone receivers (feat39-sensC)

All figures are from simulated signals (each mode's own test signal, `makeModeSynth`), not recordings.

## How it is measured

`tests/sens/test_sens39c.cpp` (ctest `sens39c`) plays each mode's clean test signal through white noise (and optionally a carrier offset and a
sample-clock error) into a fresh receiver, in seeded trials. A trial passes when the receiver counted at least half the valid units (CRC / FEC
passed) of a clean run, the same rule as `sens_sweep` (docs/SENSITIVITY.md). The SNR is the active signal power over the noise in the mode's
noise bandwidth, listed in `tools/sens/sens_modes.h`.

- Default (ctest): a short guard. Two trials per mode at a fixed SNR above the threshold must pass, and noise alone must give nothing.
- `ONAIR_TEST_LONG=1`: the full sweep. It finds the 90 % threshold in 1 dB steps with 10 trials, the carrier offset and clock error still
  decoded at threshold + 3 dB, the lock time, CPU per second of signal and false decodes on noise.
- `ONAIR_SENS_CALIBRATE=1` (the table below): 1 dB steps with 5 trials and an 80 % criterion. "CPU" is the CPU seconds per second of signal,
  one receiver, at threshold + 10 dB. "Before" is integ-39 (branch feat39-sensC-base: the same test, receivers unchanged), "after" is this branch.

New harness entries: UAT, FLARM, ADS-L, Remote ID, DJI DroneID, VDES, LoRa (raw), Wireless M-Bus.

## Results

Threshold = lowest SNR (1 dB steps) with at least 4 of 5 trials passing, in the noise bandwidth given. CPU = CPU seconds per second of signal, one
receiver. Before/after on the same machine. "CI" means a GitHub Linux runner, "Mac" means the owner's Mac under load. The Mac figures are the
median of 3 runs. Modes whose receiver this branch did not change were measured once.

| mode (test signal) | noise bw | threshold before -> after (dB) | CPU before -> after | lock s | note |
|---|---|---|---|---|---|
| **UAT 978** | 1.2 MHz | none (> 25) -> **8** | CI 0.085 -> 0.117, Mac 0.047 -> 0.060 | 0.36 | 3.8 could not decode a DO-282B signal at all (see below); the new cost is the resampler and the low-pass |
| **FLARM** | 200 kHz | 8 -> **4** | CI 0.136 -> 0.101 (fast phase); Mac 0.053 -> 0.052 (tone chips) | 1.31 | tone-energy chips and Chase decoding |
| **ADS-L** (synth mix) | 200 kHz | 6 -> **3** | CI 0.303 -> 0.232 (fast phase); Mac 0.142 -> 0.132 (tone chips) | 0.26 | M-band tone-energy chips |
| Z-Wave | 100 kHz | 10 -> 10 | CI 0.080 -> 0.079 | 0.26 | fast phase in the burst decode only: no measurable change |
| ADS-B, 1 aircraft | 2 MHz | 2 -> 2 | CI 0.219 / 0.209 | 0.26 | not changed |
| AIS, many ships | 25 kHz | 16 -> 16 | CI 0.102 / 0.100 | 0.26 | not changed (level mix of the ships) |
| AIS, 1 ship | 25 kHz | 4 | Mac 0.044 | 7.86 | not changed |
| ACARS, 1 aircraft | 25 kHz | 4 -> 4 | CI 0.075 / 0.076 | 0.75 | not changed |
| VDL2 | 25 kHz | 12 | Mac 0.235 | 0.56 | not changed |
| DSC (VHF ch 70) | 25 kHz | 3 | Mac 0.057 | 1.47 | not changed |
| Radiosonde (RS41 + DFM + M10) | 10 kHz | 11 -> 11 | CI 0.283 / 0.348 | 1.28 | not changed (CPU spread is run to run noise) |
| Zigbee | 2 MHz | 2 -> 2 | CI 0.593 / 0.597 | 0.26 | not changed |
| Bluetooth LE | 1 MHz | 8 -> 8 | CI 0.429 / 0.430 | 0.26 | not changed |
| Drone Remote ID | 1 MHz | 12 -> 12 | CI 2.444 / 2.457 | 0.25 | not changed; slower than real time on one CI core (see Not done) |
| DJI DroneID | 10 MHz | 3 | Mac 0.745 | 0.50 | not changed |
| VDES | 25 kHz | 5 | Mac 0.049 | 0.52 | not changed |
| LoRa (raw) | 125 kHz | -13 | Mac 0.099 | 0.75 | not changed |
| Meshtastic LongFast | 250 kHz | -20 | Mac 0.062 | 1.51 | not changed |
| Wireless M-Bus (mix) | 100 kHz | 17 | Mac 0.120 | 0.43 | not changed: two attempts did not help (see Not done) |

No mode gave a false decode on noise alone (2 trials each, both branches).

Success curves from the unit tests (90 % point, same signal, 3.8 path against 3.9 path in one binary):

| test | 3.8 | 3.9 |
|---|---|---|
| tests/uat/test_uat_rf.cpp, basic frame, 2.4 Msps, +20 kHz, SNR in 1.3 MHz | about 11 dB (with the 3.9 resampler; 3.8 itself: no decode) | about 7.5 dB |
| tests/flarm/test_flarm_chase.cpp, 1 and 2.4 Msps, SNR in 200 kHz | about 8.8 dB | about 4.8 dB |
| tests/adsl/test_adsl_tone.cpp, M-band Traffic, SNR in 200 kHz | about 7 dB | about 4 dB |


## What changed

- **UAT (bug fixes):**
  - The demodulator sliced two input samples as one bit at any sample rate. A real 1.041667 Mbit/s signal at the default 2 Msps slipped 4 % and
    never synced. Only the old test signal decoded, because it was made the same way: 2 samples a bit, +-2.2 degrees a sample.
  - The input is now resampled to exactly 2.083333 Msps (ExactResampler, which also low-passes wider captures). This also removes the samples
    the old linear interpolator dropped at every chunk boundary.
  - The phase is computed in floating point (fastAtan2). The 8-bit I/Q table used before lost weak signals from 12 and 16-bit radios.
  - The frame level is measured from the frame's own samples, and the noise from the quietest blocks. The SNR used to read about 0 dB.
  - The test signal (and the transmit path, which uses it) is now binary CPFSK, h = 0.6, at the DO-282B rate, with Gaussian BT 0.5 smoothing.
    The smoothing is an approximation: the standard limits the spectrum, not the pulse. Uplink frames now get the uplink sync word.
  - The synth sent dump978's fec_tests vector with its 6 byte errors still in, so every frame was at the RS limit before any noise. It now sends
    the corrected codeword.
- **UAT (sensitivity):**
  - A 3-tap binomial low-pass before the phase: about 3.5 dB.
  - Reed-Solomon erasures of the least reliable bytes when the plain decode fails: up to 4 for a long frame, 2 for a basic one, 4 to 10 per
    uplink block. The new functions are in third_party/uat_fec/fec.c, marked as OnAir additions. The gain is about 0.3 dB.
  - The erasure limits keep a random word passing below about 1e-6 per sync. Noise-only checks give 0 frames with erasures on and off.
- **FLARM:**
  - Chips are the energy at +50 kHz minus the energy at -50 kHz over one chip: noncoherent detection of the h = 1 GFSK. This replaces the summed
    FM discriminator, which suffered clicks below about 10 dB.
  - CRC-failed packets get a Chase search over the 6 least reliable bits (63 patterns, ordered by summed margin). The V6 parity and the
    two-packet agreement still stand behind it.
  - The discriminator uses fastAtan2 instead of std::arg.
  - Together, about 4 dB. The existing FLARM impairment tests still pass: +-10 kHz, +-50 ppm, collisions, key-time boundary.
- **ADS-L:** the M-band (the same 100 kchip/s +-50 kHz GFSK) uses the same tone-energy chips, about 3 dB. The O-bands (h 0.5 / 0.65) keep the
  discriminator, now with fastAtan2. The test_adsl_rx sweeps pass: every band and parameter, offsets, clock errors, overlaps, FLARM in the same
  capture.
- **Z-Wave:** fastAtan2 in the burst discriminator. No measurable CPU change; kept because it is not slower.
- **Harness:** UAT, FLARM, ADS-L, Remote ID, DJI, VDES, LoRa (raw) and wM-Bus are added to tools/sens/sens_modes.h, so `sens_sweep` covers them.

New tests:
- tests/uat/test_uat_rf.cpp: rates from 2 to 10 Msps, +-50 kHz, a 1e-3 input level, noise only, and the 3.8 slicer against the 3.9 one.
- tests/flarm/test_flarm_chase.cpp
- tests/adsl/test_adsl_tone.cpp
- tests/sens/test_sens39c.cpp (ctest sens39c): a short guard for 9 modes; `ONAIR_TEST_LONG=1` runs the full sweep with offset and clock-error
  tolerance; `ONAIR_SENS_CALIBRATE=1` prints this table.


## Not done

- **ADS-B multi-hypothesis for overlapping replies:** not attempted. A decoded frame makes the search skip its length, so a weaker reply that
  starts inside it is lost. Recovering it needs the stronger frame subtracted in the complex domain, including the radio's pulse shape. A stronger
  reply arriving inside a weaker one already works, because the weaker one then fails its CRC.
- **LoRa fractional offset and drift:** the demodulator already estimates the fractional carrier and timing offsets from the preamble and the
  down-chirps, and runs a timing loop for clock drift. LoRa (raw) reaches -13 dB in 125 kHz and Meshtastic LongFast (SF11) -20 dB in 250 kHz.
  No change.
- **AIS collisions and two channels:** the existing coast-mode collision cancellation and its test (tests/ais/test_ais_collision.cpp) already cover
  this. No change.
- **Wireless M-Bus** (17 dB, about 5 dB worse than noncoherent FSK theory for T1 / C1), two attempts:
  - Smoothed zero crossings for the chip clock: about +1 dB for S1 and C1, but fewer R2 frames even at high SNR.
  - A tone correlator for S1 / T1 / C1: worse once there was noise, because its carrier-offset term comes from the noisy discriminator mean.
  - Neither was kept. Next step: a preamble-based carrier and timing estimate before the chips are sliced.
- **Remote ID CPU:** 2.4 s per second of signal on one CI core at 20 Msps (0.75 on the Mac). The Wi-Fi demodulator is about 0.43 and the BLE front
  end about 0.32 on the Mac. The Wi-Fi receiver belongs to the Wi-Fi mode, so it was left alone. The BLE front end could take a polyphase
  decimator.
- **Radiosonde RS41 soft Reed-Solomon:** the frame decoder gets hard bits only (Rs41Decoder::push), so erasures would need the bit slicer's
  soft values plumbed through. Not done.
- **SIMD / allocation:** no hot-loop allocation was found in the changed receivers. FLARM, ADS-L and Z-Wave lost their libm phase calls; no
  explicit SIMD was added.
- **The full ONAIR_TEST_LONG sweep** (offset and clock-error tolerance at threshold + 3 dB for every mode) was not run to the end. The
  per-receiver tests above cover offsets and clock errors for the changed modes.
- All results are from simulated signals. No recording of UAT, FLARM or ADS-L was available.

