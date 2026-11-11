# Smart features in 3.9: GNSS

Three engines in `core/src/smart5`, fed from the GNSS receiver's PVT solution and tracking loops. Test: `tests/smart5/test_smart5_science.cpp` (shared with the science items).

**Validated on simulated signals only.** Clock drift series with warm-up, noise and bad fixes; Nakagami-m faded intensities with thermal noise; C/N0 with a ground-reflection fringe over a three-hour pass. No recording was on hand.

| # | Engine | Header | What it reports |
|---|---|---|---|
| 21 | Radio clock from GNSS | `gnss_clock.h` | The SDR's oscillator error in ppm from the receiver clock drift of each fix (linear fit over the last 120 fixes, bad fixes rejected), its standard error, the warm-up trend, "settled" when the trend is under 0.005 ppm per minute, and the correction in Hz at any frequency for the other modes. |
| 22 | Scintillation index S4 | `gnss_s4.h` | Per satellite, S4 over 60 s after dividing out a 0.1 Hz trend, with the thermal-noise part taken off (Van Dierendonck et al., 1993: S4n^2 = 100 / CN0 x (1 + 500 / (19 CN0))); weak from 0.15, moderate from 0.3, strong from 0.6. |
| 23 | Multipath indicator | `gnss_multipath.h` | Per pass, C/N0 against sin(elevation) with a cubic trend removed; the strongest fringe gives its depth and the reflector height (h = fringe cycles per unit sin(elevation) x lambda / 2). A fringe of 1 dB or more that carries most of the residual is flagged as multipath, with the pass's azimuth. |

The tests sweep five oscillator errors from -25 to +18 ppm, S4 from 0.1 to 1 at C/N0 38 and 45 dB-Hz, and reflector heights of 0.8, 1.5 and 3 m with and without a 3 dB fringe.

Already present and not repeated: the GNSS interference and spoofing monitor and RAIM-style residual exclusion.
