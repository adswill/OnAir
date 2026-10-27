# RTL-SDR Blog V4 above 1.766 GHz by harmonic mixing (experiment)

Question: can the V4 (R828D tuner, rated 0.5 MHz to 1.766 GHz) receive Wi-Fi at 2.4 GHz and S-band satellites at 2.2-2.3 GHz with software only?

Status: tool and analysis ready, **not yet run on hardware** (the V4 was not attached when this was written; see "Not verified"). Prediction below.

## 1. Prior art (only what was found; nothing is invented)

| What | Source | Finding |
|---|---|---|
| Harmonic-mixing driver hack, R820T2 up to 6 GHz | [rtl-sdr.com, "Tuning an R820T2 RTL-SDR up to 6 GHz via a Harmonic Mixing Driver Hack"](https://www.rtl-sdr.com/tuning-an-r820t2-rtl-sdr-up-to-6-ghz-via-a-harmonic-mixing-driver-hack/) | Driver change by Hayati Augen in the librtlsdr development branch. Above 1.766 GHz it uses the **5th harmonic** automatically; the **3rd** can be chosen with the harmonic flags of the tools. Reception worked up to 4.2 GHz; sensitivity "decent" to about 2.7 GHz, then falls a lot. A 1760 MHz high-pass filter (Mini-Circuits VHF-1760+) was needed, otherwise signals below 1.7 GHz alias on top of the wanted one. Imaging and aliasing are called a major problem; some frequencies came out a few MHz off. Tested on an RTL-SDR Blog **V3**, which gave the strongest signals of the dongles tried, credited to its front-end matching not attenuating L-band and above. A HackRF 2.45 GHz test signal and a baby monitor were shown. Reader comments: Wi-Fi seen on a V3; 4.2 GHz reached without LNA or filter, big drop above 2.7 GHz; E4000 dongles do not work. No loss figures in dB are given. |
| Harmonic code | [librtlsdr development branch, tuner_r82xx.c](https://github.com/librtlsdr/librtlsdr/blob/development/src/tuner_r82xx.c) | Default harmonic 5. Starts with the harmonic above 1.77 GHz, retries with it if the PLL does not lock above 1.76 GHz. PLL is programmed with `lo / harmonic`. VCO limits in code 1.77-3.9 GHz (the register comments say 1.75-3.6). Odd harmonics 3, 7, 11, 15 flip the sideband (`harm_sideband_xor`); 5 does not. IF 3.57 MHz (6 MHz filter), 4.57 MHz (7-8 MHz), else a table value. |
| SDR++ source with harmonic controls | [rtl-sdr.com post](https://www.rtl-sdr.com/modified-rtl-sdr-source-for-sdr-with-manual-controls-for-r820t-2-r828d-tuners-and-harmonic-reception/), code [Sultan-papagani/sdrpp_new_rtlsdr_source](https://github.com/Sultan-papagani/sdrpp_new_rtlsdr_source) | Exposes the harmonic mode. Warns of a lot of imaging and weak signals; expect to need strong band-pass filtering and an external LNA. No measurements. |
| V4 front end | [rtl-sdr.com V4 release post](https://www.rtl-sdr.com/rtl-sdr-blog-v4-dongle-initial-release/), [V4 datasheet PDF](https://www.rtl-sdr.com/wp-content/uploads/2024/12/RTLSDR_V4_Datasheet_V_1_0.pdf), [V4 guide](https://www.rtl-sdr.com/v4/) | The SMA input is split by a **triplexer** into HF / VHF / UHF paths (the R828D has three inputs); notch filters (broadcast AM, FM, DAB) on the R828D open-drain pin, "a few dB" deep. Rated 500 kHz to 1.766 GHz. The pages do not give the response of the UHF path above 1.7 GHz. |

**Not found:** a schematic-level or measured response of the V4 triplexer above 1.7 GHz; any harmonic-reception test on a V4 specifically; any measured 3rd-harmonic loss in dB; the R828D mixer's harmonic response in a datasheet (only the driver behaviour above is known). If you know of one, add it here.

What the sources establish:
- The PLL cannot lock above about 1.76 GHz, but nothing stops the **LO** from being set low (27 MHz to 1.7 GHz). The mixer then also converts RF at N x LO (odd N). The hack is exactly "set LO = target / N". The sources do not say why the mixer responds; the mechanism (square-wave LO has odd harmonics) is the textbook explanation, not a quoted source.
- Odd harmonics (3, 5, 7) work; even ones are suppressed by the balanced mixer (not shown by a source here).
- The IF filter does not matter for the product: it filters after the mixer. The IF (3.57 MHz) shifts which RF lands in the window.
- Loss: no figures. Reports say it works at 2.4 GHz with a V3 and stays usable to about 2.7 GHz.
- V4 risk: the V3 worked because its input does not attenuate L-band. The V4 triplexer is built to separate bands below 1.7 GHz; if its UHF path rolls off, 2-3.5 GHz loses more on the V4 than on the V3. Unknown until measured.

## 2. How the tuner does it (librtlsdr and OnAir)

- librtlsdr `set_center_freq(F)`: the R82xx driver sets `LO = F + IF` and programs the PLL (VCO 1.77-3.9 GHz divided by 2..64). F above about 1.766 GHz makes the PLL fail to lock; the stock Blog / osmocom library has no harmonic code (only the development branch has).
- OnAir's own RTL-SDR source (`core/src/source/source_rtlsdr.cpp`) just calls `set_center_freq`; for a V4 it only treats f < 24 MHz specially (library upconverter) and warns above 1.766 GHz. It has no harmonic mode. If the experiment works, a "harmonic" option would be a thin layer on that call (see 5).
- The tool does the same trick without the development branch: it tunes the library to **F_set = LO - IF**, which lands the LO where wanted and keeps the V4's frequency-based input switch on the UHF path (LO 360-1200 MHz is all UHF).
- Mapping: `T = N x LO - IF` (hypothesis A, like the fundamental) or `T = N x LO + IF` (B). The odd-harmonic sideband flip in the development driver suggests the 3rd may be mirrored. The tool does not assume: it prints both candidate targets and labels the mirror from the measured motion.
- Tracking filter: the R82xx also switches an RF band-pass by F_set. It follows the LO (360-1200 MHz), not the target, so it attenuates the harmonic target. This is a reason for the expected loss. No register access is used here.

### Harmonic table (IF 3.57 MHz, hypothesis A; `v4range --table` prints it)

| Target MHz | 3rd: LO / F_set | 5th: LO / F_set | other products while tuned for 5th (LO x n - IF) |
|---|---|---|---|
| 1800 | 601.19 / 597.62 | 360.71 / 357.14 | 1x 357, 3x 1079, 7x 2521, 9x 3243 |
| 2000 | 667.86 / 664.29 | 400.71 / 397.14 | 1x 397, 3x 1199, 7x 2801 |
| 2200 | 734.52 / 730.95 | 440.71 / 437.14 | 1x 437, 3x 1319, 7x 3081 |
| 2400 | 801.19 / 797.62 | 480.71 / 477.14 | 1x 477, 3x 1439, 7x 3361 |
| 2600 | 867.86 / 864.29 | 520.71 / 517.14 | 1x 517, 3x 1559, 7x 3641 |
| 2800 | 934.52 / 930.95 | 560.71 / 557.14 | 1x 557, 3x 1679, 7x 3921 |
| 3000 | 1001.19 / 997.62 | 600.71 / 597.14 | 1x 597, 3x 1799, 7x 4201 |
| 3200 | 1067.86 / 1064.29 | 640.71 / 637.14 | 1x 637, 3x 1919, 7x 4481 |
| 3400 | 1134.52 / 1130.95 | 680.71 / 677.14 | 1x 677, 3x 2039, 7x 4761 |
| 3600 | 1201.19 / 1197.62 | 720.71 / 717.14 | 1x 717, 3x 2159, 7x 5041 |

Reading: while tuned for the 3rd harmonic at 2.4 GHz (LO 801 MHz) the dongle also hears 797 MHz directly (LTE band 20 downlink, 791-821 MHz, in many countries) and the 5th and 7th products (4.0 and 5.6 GHz, i.e. 5 GHz Wi-Fi). Tuned for the 5th at 2.4 GHz (LO 481 MHz) it hears 477 MHz directly (UHF TV and PMR, near 470-694 MHz) and 1.44 GHz and 3.36 GHz. These are the aliases; the strong sub-GHz signals are the real problem, since the V4 UHF path lets them straight in. The identification step labels them "fundamental" (k=1), not harmonics.

Image: the tuner's image rejection puts the mirror at +-2 IF = 7.14 MHz away: outside the 2.4 MHz window, so it is not an issue for narrow targets; for a 20 MHz Wi-Fi channel the window sees only a slice.

## 3. The tool

`tools/v4range.cpp`, CMake target `v4range` (also `v4range_selftest` in ctest). Loads librtlsdr at run time (Blog or osmocom, `--lib` to point at one); no other dependency.

- Opens the dongle, direct sampling off, bias tee off, manual gain (highest by default), 2.4 Msps, asks for a 6 MHz IF filter so the library uses the fixed 3.57 MHz IF.
- Sets the LO as above, dwells 0.25 s per step (Wi-Fi beacons come every 102 ms), computes mean and max-hold spectra (2048-point FFT), band power, noise floor (median bin) and peak.
- **Harmonic identification:** for each detection (band power 3 dB above the quiet baseline of the sweep, or a narrow peak 15 dB over the step floor) it moves the LO by df (100 kHz) and tests how the spectrum shifted: k = +1 fundamental, -1 image, +-3, +-5, +-7 harmonics (sign = mirrored or not), 0 = fixed in the baseband (DC spike, spurs). The fixed baseband response is first removed (median over the sweep), so it works on wideband signals such as Wi-Fi, not only on single carriers. It reports "unclear" when the best hypothesis does not beat the runner-up clearly.
- CSV columns: harmonic, LO, dongle frequency, both target candidates, band dB, floor dB, peak dB, peak baseband kHz, excess dB, detected, identified k, label, score, known signal (Wi-Fi ch1/6/11, Bluetooth/ISM band, GPS L1, S-band range). Levels are uncalibrated (dB relative to the full scale); compare steps with each other.
- `--selftest` (no hardware): synthetic narrow and Wi-Fi-like wideband signals for fundamental, image, 3rd, 5th, 7th, upright and mirrored, plus a fixed spur; all 15 checks pass.

## 4. What the owner does

1. Plug in the V4, the stock telescopic antenna 0.5-1 m from the Wi-Fi router (a router talking to a client gives the most traffic). Keep the other USB devices off; close OnAir and anything else using the dongle. Install librtlsdr (Mac: `brew install librtlsdr`; the Blog fork is best for the V4: https://github.com/rtlsdrblog/rtl-sdr-blog).
2. Build: `cmake --build build --target v4range` (or `clang++ -std=c++17 -O2 tools/v4range.cpp -o v4range`).
3. Run: `./build/v4range` — about 1 minute. It first checks FM and GPS L1 below the limit (is the rig alive), then sweeps 2380-2500 MHz on the 3rd and 5th harmonic and identifies the strongest detections. Output: console list plus `v4range.csv`.
4. Whole range later: `./build/v4range --full` (1.8-3.6 GHz, about 12 minutes for both harmonics). S-band satellites need a dish and LNA (add `--range 2200-2300`).

Troubleshooting: "no RTL-SDR found" or read errors: another program holds the dongle, or the USB link is flaky; on Linux blacklist `dvb_usb_rtl28xxu`. If every harmonic reads about the same, try `--gain 40` (overload) and a lower-gain antenna.

## 5. Reading the result and prediction

A success is a line like `target 2437.0x MHz ... 5th harmonic (upright)  known: Wi-Fi ch6` (or 3rd; "mirrored" is fine, it only tells the spectrum is reversed) at the right place for hypothesis A or B, at roughly the three Wi-Fi channel positions or across 2402-2480 for Bluetooth, with excess of several dB. Channels 1, 6, 11 should be a plateau about 20 MHz wide in target; in LO terms 4 MHz wide for the 5th and 6.7 MHz for the 3rd: that width is a second check on the harmonic number. If A or B is consistently the one that matches, the next step is a calibrated mapping.

Other labels: "fundamental" or "image" = a strong sub-GHz signal (LTE, TV, GSM) leaking in; this is the expected failure mode. "fixed in baseband" or "unclear" = nothing real. A silent sweep that still shows the FM and GPS reference means the harmonics are too weak, not that the rig is broken.

Honest prediction (not measured): works for strong nearby signals only. The development-branch reports (on a V3) show a usable response to about 2.7 GHz, so Wi-Fi next to the antenna should show up, probably with 20-40 dB less sensitivity than at 1.5 GHz (my estimate from the generic fact that the tracking filter and the harmonic conversion both attenuate; no source gives a number). The V4 triplexer may add loss above 1.7 GHz that the V3 did not have, so the V4 may do worse than the V3 reports. 2.2-2.3 GHz S-band satellites will not be received without a dish and a preamp, and not with the 3rd or 5th harmonic alone; at best a very strong beacon with a large dish. Do not expect good selectivity: with no pre-filter, sub-GHz signals mix in; a 1.7 GHz high-pass filter (as the original article used) ahead of the dongle is the first thing to add, and is outside this "no hardware change" experiment.

If the dongle shows nothing above the baseline for Wi-Fi at one metre with both harmonics, the answer is that the V4 front end blocks it, and a V3 or a filter-free path is needed: report that and stop.

## Not verified

The V4 enumerated once during development (the library found it), then dropped off the USB bus, so no read from hardware was completed. Everything about the hardware path (read sizes, tuning, gain list) is written from the API and is untested on a radio. The identification logic is tested with synthetic data only. The first run may need small fixes.
