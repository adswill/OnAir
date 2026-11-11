# Sensitivity 3.9 E: wM-Bus, GNSS acquisition, CW timing, HD Radio AM (feat39-sensE, CW: feat39-cwfist)

Branch feat39-sensE on integ-39. The four leftovers of docs/SENSITIVITY-3.9-C.md and -D.md. Every figure here is from simulated signals
(each mode's own generator, white Gaussian noise, the sens_sweep impairments). No recording was used: simulated only.

| item | before | after | kept |
|---|---|---|---|
| Wireless M-Bus (sens39c mix, 100 kHz) | 17 dB | **11 dB** | yes |
| GNSS, 32 dB-Hz sky, 34 s (test_gnss_sweep) | 0 of 8 satellites tracked | **3 of 8** (2 with the frame) | yes |
| GNSS one-satellite acquisition, 16 ms (test_gnss_loops A) | 90 % point 35.8 dB-Hz | unchanged (see below) | - |
| CW skimmer, CER % at 2 / 4 dB (4 seeds, section 3) | machine 13.2 / 6.6, sloppy 35.6 / 28.2, Farnsworth 53.5 / 18.0 | **machine 6.6 / 2.6, sloppy 21.0 / 10.9, Farnsworth 36.0 / 8.7** (adaptive timing, feat39-cwfist) | yes |
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

## 3. CW timing (feat39-cwfist: the skimmer now uses the adaptive decoder)

The adaptive classifier is cw::FistDecoder (cw_decoder.h, 0.2.8), a Bayesian timing model in the log domain with learned dot / dash / gap
classes, a beam search and a character model. The skimmer (CwReceiver) ran the 0.2.7 Timing. The first pass (sensE) wired FistDecoder in
and found it better on irregular fists but worse at low SNR, with Farnsworth spacing and on the first character after a pause, so it was
reverted. feat39-cwfist fixed those weaknesses in FistDecoder and switched the skimmer to it.

Changes in core/src/cw/cw_decoder.cpp:
1. Wide gaps. The character gap may now be learned up to 15 dots (was 6.7) and the word gap up to 40 (was 20). Farnsworth 20 / 12 sends
   about 8 and 19. The gap classes are fitted from the data: the intra start is the shortest quarter, the character start is the median
   of the longer gaps, and the word start is their top. A fit with more word gaps than character gaps is rejected. The classes are also
   fitted from the backlog at the lock, which now waits for three gaps between characters. Before this, Farnsworth's first transmission
   read "C Q D E L Y 2".
2. Noise in the gaps (low SNR). The detector opens on noise for a few ms in the gaps of a weak signal, and those blips had pulled the
   dot length down to the noise. At 0 dB a 22 WPM signal was read at 60 WPM, all "E" and "T". Three changes:
   - The lock ignores marks under half a dot.
   - The marks are fitted anchored on the dashes: a 2-means in linear time, then a 3-class mixture (blip, dot, dash).
   - A blip on a noisy signal no longer counts against the fit, so it no longer drops the lock.
3. Soft mark / space decisions. A clear drop-out (under 0.3 dot) or spike (under 0.33 dot) is merged as before. Other events are decided
   by the model when the detector is noisy, measured as the learned rate of clear spikes (over 1.5 %). The model compares the three events
   as read against one merged event plus the log chance of a detector error. The single-element word ("E", "T") between longer gaps has a
   1 % prior. In the beam, a mark at the start of a word may be noise, and the character model decides, so "CQ" wins over ";Q" or "<AR>Q".
   This is the fix for the first character after a pause. The soft merges are off on a clean signal: there, those patterns are a change of
   speed, and the stress case 12 -> 24 WPM went to 34 % CER until this gate was added.
4. Timing after a pause. After a pause (over twice the word gap), the next word (up to 10 marks) is held. If another dot length explains
   its marks and gaps at least twice as well, the model moves to that speed before the word is read. Decoder alone, "TEST DL1XYZ ..." after
   a 3 s pause at another speed: 25 -> 15, 20 -> 10 and 30 -> 18 WPM read "T E T" before and "TEST" now; 15 -> 30 and 12 -> 24 WPM lost the
   word before and read it now.
5. A lost lock keeps what it learned. When the lock drops (a long noisy stretch) and comes back within 25 % of the old speed, it starts
   from the old classes. Before this, Farnsworth copies after a drop read one letter a word.

Skimmer (core/src/cw/cw_rx.cpp): the adaptive timing is the default and runs behind the same debounce as cw::Keyer. The 0.2.7 timing is
kept behind CwReceiver::setTimingDecoder(1), used for comparison and by the tests. The whole-span slice skimmers follow the setting.
setKeyTap gives tests the detector output. CPU: test_cw_wide (12 signals, whole span) measures 5.89 s for 40 s, against 5.85 s with the old
receiver.

### Results: the whole receiver

These figures come from tests/cw/cw_skim_bench.cpp `rx 4 80`: CwReceiver at 250 kHz, CwGenerator sending "CQ DE LY2ABC LY2ABC K" with
1 s pauses at +500 Hz, 80 s, seeds 1 - 4. CER is the edit distance of every copy after the first, against the decoded text. This is
stricter than sensE's middle copies: the last copy counts too, and with Farnsworth's few copies the best one could no longer stand in for
the rest. The scenarios are machine 22 WPM, hand 20 WPM (12 % jitter, 3.4 ratio), sloppy 20 WPM (22 %, 2.7) and Farnsworth 20 / 12.

| CER % at SNR in 500 Hz | 0 | 2 | 4 | 6 | 10 | 20 |
|---|---|---|---|---|---|---|
| machine, 0.2.7 timing | 30.5 | 13.2 | 6.6 | 1.1 | 0 | 0 |
| machine, adaptive (now) | **23.6** | **6.6** | **2.6** | **0.3** | 0 | 0 |
| hand, 0.2.7 timing | 30.4 | 13.8 | 1.9 | 0.4 | 0 | 0 |
| hand, adaptive (now) | **25.4** | **6.5** | **1.2** | **0** | 0 | 0 |
| sloppy, 0.2.7 timing | 58.3 | 35.6 | 28.2 | 20.4 | 20.4 | 21.6 |
| sloppy, adaptive (now) | **47.7** | **21.0** | **10.9** | **7.2** | **6.6** | **6.3** |
| Farnsworth, 0.2.7 timing | 65.7 | 53.5 | 18.0 | 7.0 | 0 | 0 |
| Farnsworth, adaptive (now) | **59.9** | **36.0** | **8.7** | **1.7** | 0 | 0 |

The adaptive timing is no worse than the 0.2.7 timing at every point, and better at every point where either has errors. The second capture
(seeds 1 - 2, captured separately) gives the same result. The decoder was tuned on saved detector output (`cap` / `rep`). Replaying the
0.2.7 timing on it reproduces the receiver's own figures exactly. Because the adaptive timing is never worse, the skimmer uses it for every
signal, and no per-signal choice is needed.

For comparison with sensE's table, measured the same way (middle copies only, 2 seeds), FistDecoder before this branch gave Farnsworth
4 dB 26.2 % and machine 0 dB 45.4 %.

### Results: the chain alone

These figures come from tests/cw/cw_fist_bench.cpp: cw::Chain, 11 fists, QSO text, 3 seeds x 200 characters, adaptive decoder before and
after.

| mean CER % at | 2 dB | 6 dB | 10 dB | 15 dB | 25 dB |
|---|---|---|---|---|---|
| QSO text, before | 27.4 | 4.0 | 2.3 | 2.4 | 2.4 |
| QSO text, after | **23.1** | **3.5** | 2.2 | 2.2 | 2.1 |
| free text (2 seeds), before | 27.8 | 6.5 | 5.8 | 5.4 | 5.6 |
| free text, after | **22.2** | **6.0** | 5.6 | 5.3 | 5.5 |
| the skimmer's detector in front (T_LEGDET, 2 seeds), before | 31.6 | 4.4 | 2.3 | 2.3 | 2.3 |
| same, after | **19.7** | **3.1** | 2.0 | 2.0 | 2.1 |

A few points of the 11 fists got slightly worse: straight key 1.5 -> 1.8 % at 10 - 25 dB, heavy 22.9 -> 23.8 % at 2 dB, and slow-hand
9.3 -> 9.6 % at 6 dB. The stress set (straight key, 3 seeds) moved from before to after as follows:
- clean 1.6 -> 1.4 %
- key clicks 5.6 -> 5.6 %
- noise crashes 2.3 -> 2.5 %
- 12 -> 24 WPM 6.0 -> 5.8 %
- 25 -> 10 WPM 6.0 -> 5.6 %

Noise alone gave 0 characters in 4 x 3 minutes, before and after.

Not fixed: the first character of a signal's first transmission is still sometimes wrong ("GQ", "FQ" for "CQ"). The skimmer opens a signal
on a carrier it has just seen, inside that first mark, so the first mark is cut. The 0.2.7 timing prints "*" there. Rules that dropped the
first character also dropped good ones (6 WPM), so none was kept.

Tests: tests/cw/test_cw_fistrx.cpp (ctest cw_fistrx, about 11 s) covers:
- the decoder alone: Farnsworth 20 / 10 from the first transmission, blips in a pause, speed changes after a pause, a lock lost in noise;
- the receiver: the bench's points (machine and hand at 2 dB, Farnsworth at 4 dB) under the 0.2.7 timing's figures;
- sloppy at 10 dB against a live run of the 0.2.7 timing;
- "K CQ DE" after every pause, and the Farnsworth flag with a 90 Hz tuning error;
- noise alone.

ONAIR_TEST_LONG=1 runs the whole table. cw_rx, cw_wide, cw_morse and cw_engine pass with the adaptive default. All of this is on simulated
signals only: no CW recording was used.

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
gnss_loops, gnss_sweep, gnss_cfo, gnss_rx, gnss_engine, gnss_rates, hdr_am, hdr_rx, hdr_engine, hdr_wide, hdr_realworld; for CW
(feat39-cwfist) cw_fistrx, cw_rx, cw_wide, cw_morse, cw_engine. All simulated
signals; nothing here was checked on a recording.
