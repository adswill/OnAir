# Sensitivity 3.9 E: wM-Bus, GNSS acquisition, CW timing, HD Radio AM (feat39-sensE)

Branch feat39-sensE on integ-39. The four leftovers of docs/SENSITIVITY-3.9-C.md and -D.md. Every figure here is from simulated signals
(each mode's own generator, white Gaussian noise, the sens_sweep impairments). No recording was used: simulated only.

| item | before | after | kept |
|---|---|---|---|
| Wireless M-Bus (sens39c mix, 100 kHz) | 17 dB | **11 dB** | yes |
| GNSS, 32 dB-Hz sky, 34 s (test_gnss_sweep) | 0 of 8 satellites tracked | **3 of 8** (2 with the frame) | yes |
| GNSS one-satellite acquisition, 16 ms (test_gnss_loops A) | 90 % point 35.8 dB-Hz | unchanged (see below) | - |
| CW skimmer timing | legacy clustering | unchanged (see below) | no |
| HD Radio AM (sens_sweep, 6 trials, 1 dB steps) | awgn 22, cfo20ppm 22 dB, lock 11 s | **awgn 18, cfo20ppm 18 dB**, lock 7.5 s | yes |

## 1. Wireless M-Bus (core/src/wmbus/wmbus_live.cpp, wmbus_proto.cpp)

Measured with `tests/sens/test_sens39c.cpp` (`ONAIR_SENS_CALIBRATE=1 ONAIR_SENS_MODES=^wmbus$`, 5 trials, 80 %) and a per-PHY bench (the
generator's SNR, burst power over noise in 250 kHz, 3 seeds x 4 s, frames with a good CRC).

| | threshold (100 kHz) | lock s | CPU x rt | false frames on noise |
|---|---|---|---|---|
| integ-39 | 17 dB | 0.43 | 0.120 | 0 |
| this branch | **11 dB** | 0.26 | 0.123 | 0 |

Frames per PHY (3 seeds, 4 s; the clean counts are S1 53, T1 70, C1 A 74, C1 B 58, R2 24):

| SNR in 250 kHz | S1 | T1 | C1 A | C1 B |
|---|---|---|---|---|
| 12 dB, integ-39 | 53 | 2.3 | 23.7 | 0 |
| 12 dB, this branch | 53 | 70 | 73.7 | 54 |
| 10 dB, integ-39 | 49.3 | 0 | 4.3 | 0 |
| 10 dB, this branch | 53 | 70 | 68 | 38.7 |
| 8 dB, this branch | 53 | 66 | 42.3 | 7 |

What changed:

1. **Channel filter of S1 and T1 / C1: 140 -> 84 kHz.** With +-50 kHz deviation and 100 kchip/s the 140 kHz filter let the discriminator
   run at its FM threshold. Measured: 0.75 x (105 kHz) about 3 dB, 0.6 x (84 kHz) 4 to 5 dB for T1 and C1, 0.5 x the same for T1 and worse
   for C1. A 15 kHz carrier error (17 kHz is 20 ppm at 868 MHz) still decodes: T1 66 and C1 A 51 frames at 10 dB (integ-39: 0 and 4).
2. **Chip value over 80 % of the chip** instead of the middle half: about 1 dB for T1 and C1.
3. **Soft symbol decisions.** A Manchester pair 00 / 11 or a 6-chip word that is not a 3-out-of-6 code word ended the frame. Now the pair
   is decided by the difference of its soft chips and the word by the code word with the largest correlation; a frame where more than a
   quarter of the symbols needed this (after 8) is dropped as a false sync. At 8 dB, T1: 66 frames against 14 hard; S1 at 6 dB: 52 against 22.
   It also fixed the noise-free case: integ-39 decoded only 53 of the frames of the clean test signal in test_wmbus_live (ISI from the
   Gaussian shaping makes 00 / 11 pairs), this branch 105.

Tried and not kept: averaging the complex lag products instead of the phase differences (no change), a lag-2 differential detector
(4 to 6 dB worse), 98 kHz filter (1 to 2 dB worse than 84 kHz).

Cost: at 25 dB with a 9 to 12 kHz carrier error, test_wmbus_live shows a few CRC failures (5 and 9 of about 115 frames; integ-39 none) and
the same count of good frames within 6 %. R2 (60 kHz channels, 14 kHz filter) is unchanged; it does not decode at 15 kHz off in either.

Test: test_wmbus_live has a new section: T1, C1 and S1 alone at 10 dB, with and without a 15 kHz carrier error, must give 40 to 90 % of
the clean frames (integ-39 fails all five). wmbus_engine, wmbus_proto, wmbus_wize, wmbus_leak and sens39c pass.

## 2. GNSS acquisition (core/src/gnss/gnss_rx_plan.cpp)

GnssAcq already is an FFT parallel code search (2 ms coherent pairs, 16 ms short and 64 ms long rounds). Measured on the
test_gnss_loops part A set-up (one satellite, 20 noise seeds, 5 PRNs searched, FFT units per search as the CPU measure):

| coherent / total | FFT units | 35.8 | 33.8 | 31.8 | 29.8 dB-Hz |
|---|---|---|---|---|---|
| 2 ms / 16 ms (shipped) | 5189 | 20 | 9 | 2 | 0 |
| 4 ms (8 offsets) / 16 ms | 10373 | 20 | 11 | 3 | 1 |
| 2 ms / 32 ms | 10373 | 20 | 20 | 15 | 3 |
| 4 ms (8 offsets) / 32 ms | 20741 | 20 | 20 | 16 | 6 |

At the same CPU, a longer non-coherent sum gains far more than 4 ms coherent (bit edges and the doubled Doppler grid eat most of it), so
the 4 ms code was not kept. The receiver's limit was the schedule: with nothing locked, the cold search widened to +-45 and +-170 kHz
(the +-170 kHz round alone takes about 35 s) before the first 64 ms round, so a weak sky with a good oscillator was never searched long.

Change: the cold search order is now first window 16 ms, **first window 64 ms**, +-45 kHz, +-170 kHz, then the wide ones with 64 ms
(the stage numbers and their meaning in the interface stay).

test_gnss_sweep (34 s, 4 Msps): 32 dB-Hz zenith 0 -> 3 of 8 satellites tracked (2 with the frame; no fix in 34 s), 36 and 40 dB-Hz
unchanged (5 of 8, no fix; 8 of 8, fix at 26 s). test_gnss_cfo: +-3 kHz first fix 26 s and +-79 kHz 56 s, the same as integ-39.
CPU: the acquisition works on a fixed budget of FFTs per input block, so the CPU per second is unchanged (gnss_rates 2.5 to 2.9 x real
time). The sweep now checks that the 32 dB-Hz sky gives at least 2 satellites. gnss_loops, gnss_rx, gnss_engine, gnss_rates pass.

Not done: the 16 ms one-satellite figure itself (35.8 dB-Hz) is unchanged; lowering it means a longer first round, which delays a strong
sky. The 36 dB-Hz sky's missing fix is the ephemeris time after late locks, not acquisition.

## 3. CW timing (not changed)

The adaptive classifier asked for exists: cw::FistDecoder (cw_decoder.h, 0.2.8), a Bayesian timing model in the log domain with learned
dot / dash / gap classes, a beam search and a character model, behind cw::Chain (cw_chain.h). The skimmer (CwReceiver) never used it: it
runs the 0.2.7 detector, Keyer and Timing. This pass wired Chain into the skimmer behind a switch and measured both on the receiver
(CwGenerator, "CQ DE LY2ABC LY2ABC K" repeated with 1 s pauses, 80 s, 2 seeds, CER of the middle copies):

| CER % at SNR in 500 Hz | 0 | 2 | 4 | 6 | 10 | 20 |
|---|---|---|---|---|---|---|
| machine 22 WPM, legacy | 26.9 | 13.8 | 4.6 | 1.5 | 0 | 0 |
| machine, adaptive | 60.0 | 16.9 | 8.5 | 5.4 | 6.2 | 0 |
| hand (12 % jitter, 3.4 ratio), legacy | 17.4 | 4.7 | 0 | 0 | 0 | 0 |
| hand, adaptive | 52.3 | 24.4 | 5.8 | 4.7 | 4.7 | 0 |
| sloppy (22 %, 2.7), legacy | 57.0 | 31.4 | 18.6 | 18.6 | 15.1 | 15.1 |
| sloppy, adaptive | 68.6 | 32.6 | 10.5 | 10.5 | 9.3 | 3.5 |
| Farnsworth 20 / 12, legacy | 64.3 | 54.8 | 7.1 | 0 | 0 | 0 |
| Farnsworth, adaptive | 71.4 | 64.3 | 40.5 | 26.2 | 9.5 | 0 |

With the 0.2.7 detector in front of the adaptive timing (T_LEGDET) machine is 7.7 / 4.6 / 0 at 4 / 6 / 10 dB and sloppy 2.3 at 10 dB.
The fist benchmark (tests/cw/cw_fist_bench.cpp, the chain alone) gives mean CER 15.0 -> 2.4 % at 15 dB over its 11 fists but
29.1 -> 27.4 % at 2 dB, machine 6.7 -> 14.4 %. The adaptive decoder misreads the first character after a pause ("CQ" as "NQ" or "I Q")
and loses at low SNR on steady keying and on Farnsworth spacing, so it is better only for irregular fists. Not a clean gain: the wiring was
reverted and the skimmer is unchanged. Next step: the first mark after a long gap (the speed and the mark level after the pause) and the
Farnsworth gaps in FistDecoder, then the switch.

## 4. HD Radio AM (core/src/hdr/hdr_phy.cpp)

The 38 dB and the missing 90 % point came from acquisition, not from the decoder's SNR. sens_sweep trials (hdr-am, 2 Msps, 20 s, a
trial needs half of the clean P1 frames) at 34 dB: 8 of 9 passed, one never locked and one locked at 11.3 s; the quick tier (2 trials at
the coarse 4 dB points) then reads 38 dB or no point at all depending on which trials fail. Traced block by block:

1. The prefix search switches between the hybrid and the all-digital filter every 10 blocks. When the switch came while the block numbers
   were being read (sync needs 4 in a row), the sync was cut off, and the all-digital filter's frequency estimate of a hybrid station ran
   off by a whole subcarrier in 10 blocks. Back on the hybrid filter the station synced with that error and gave no P1 frame.
   Now the filter stays while a valid block number was read in the last three blocks (up to 30 blocks), and each filter keeps its own
   frequency estimate.
2. In sync the frequency was frozen at the prefix estimate; the per-block carrier fit had to take the whole residual every block. Now the
   carrier's measured frequency corrects the next blocks (gain 0.3).
3. Oscillator phase noise: the carrier's phase wander inside a block (a zero-phase 40 Hz low-pass of the carrier, less its straight line)
   is taken out of every sample. With 1 Hz of linewidth (34 dB, 250 ksps) integ-39 gave no P1 frame and this branch 40 of 48.

sens_sweep, full mode (6 trials a point, 1 dB steps), Mac:

| scenario | integ-39 | this branch | lock median before -> after |
|---|---|---|---|
| awgn | 22 dB | **18 dB** | 11.0 -> 7.5 s |
| carrier error of a 20 ppm radio (20 Hz) | 22 dB | **18 dB** | 11.8 -> 7.5 s |
| sample clock 50 ppm fast | 18 dB | 18 dB | 7.8 -> 7.8 s |
| combined (2 ppm, 20 ppm clock, echo, 3 Hz phase noise) | no 90 % point at 34 dB | no 90 % point at 34 dB | - |

No false lock on noise in either. The clean reference rose from 69 to 75 P1 frames a trial (earlier sync). CPU: the phase-wander pass is
three passes over 8640 samples a block (0.19 s), small against the FFTs.

Phase noise of 3 Hz linewidth (the sens_sweep phasenoise scenario, also in combined) still gives nothing, before and after: at 1 MHz the
carrier is 30 dB over the digital subcarriers, and 3 Hz of Wiener phase noise puts about -20 dBc of it into each neighbouring 181 Hz bin,
over the reference subcarrier. Tracking the phase faster than about 90 Hz would follow the reference subcarrier itself. A broadcast
receiver's oscillator at 1 MHz is far cleaner than 3 Hz; this is a limit of the scenario, noted and not pursued.

Test: test_hdr_am starts an MA1 signal (30 dB, no tuning error) at four points and needs the first P1 frame within 10 s every time
(integ-39: never from one start, 11 s from another). hdr_am, hdr_rx, hdr_engine and hdr_wide pass.

## Tests and validation

Local (prebuilt integ-39 libraries plus the changed files, Mac): wmbus_live, wmbus_engine, wmbus_proto, wmbus_wize, wmbus_leak, sens39c,
gnss_loops, gnss_sweep, gnss_cfo, gnss_rx, gnss_engine, gnss_rates, hdr_am, hdr_rx, hdr_engine, hdr_wide, hdr_realworld. All simulated
signals; nothing here was checked on a recording.
