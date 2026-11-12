# Sensitivity 4.0, pass X2 (feat40-sensX2): picture, sound and measurement modes

Branch feat40-sensX2 on integ-40. Every figure is from simulated signals (the mode's own generator, white Gaussian noise and the impairments
named); no off-air recording was used.

## Summary

| mode | figure | 3.9 | 4.0 | conditions | kept |
|---|---|---|---|---|---|
| SSTV Martin M1 | mean picture error at 6 dB, test / textured picture | 20.2 / 30.6 levels | 6.6 / 12.6 levels | 3 kHz, no offset, 2 trials | yes |
| SSTV Martin M1 | PSNR at 6 dB, test / textured | 18.0 / 16.4 dB | 24.3 / 19.8 dB | as above | yes |
| SSTV Martin M1 | lowest SNR with mean error 20 or less (test picture) | 8 dB (+-50 Hz, +-50 ppm, 6 trials) | about 2 dB (no offset, 2 trials) | 3 kHz | yes |
| SSTV Robot 36 | mean error at 10 dB, test / textured | 27.0 / 37.4 levels | 12.3 / 19.2 levels | 3 kHz, no offset, 2 trials | yes |
| SSTV Robot 36 | lowest SNR with mean error 20 or less (test picture) | 14 dB | about 7 dB | 3 kHz | yes |
| SSTV PD120 | PSNR at 10 dB, test / textured | 15.4 / 12.6 dB (+-50 Hz, +-50 ppm) | 21.9 / 15.5 dB (no offset) | 3 kHz | yes |
| SSTV Robot 36 | crash on a weak picture | segfault at 4 dB | fixed | | yes |
| Weather fax 120 lpm | sens39 pass (line lock, mean row correlation over 0.5), 3 trials a point | 3 of 3 at 4 dB, 0 of 3 at 2 dB | 3 of 3 at 2 and 1 dB | 0 .. 6 kHz, +-30 Hz, +-20 ppm | yes |
| Weather fax 120 lpm | mean row correlation at 4 / 2 / 0 dB | 0.601 / 0.460 / 0.338 | 0.796 / 0.634 / 0.448 | as above | yes |
| NOAA APT | picture PSNR at 15 / 5 / 2 / 0 dB (generator SNR in 40 kHz) | 30.5 / 13.8 / 10.2 / 8.4 dB | 32.8 / 15.2 / 12.8 / 11.0 dB | 2.5 kHz offset, 30 Hz/s, 80 ppm | yes |
| NFM voice (general receiver) | 12 dB SINAD point | 8.4 dB in 12.5 kHz | not changed | 1 kHz tone, 2.5 kHz deviation | no (see below) |

"About" points are between two measured SNRs. The 4.0 filter was finished after the Mac went on battery, so its impairment sweep (offsets,
6 trials) is left to the guard test on GitHub; the impairment sweep below is of its second step alone.

## Method (SSTV)

`tests/hfdig/sstv_sens40_bench.cpp` (a bench, not a ctest; uses `sstv_sens40_util.h`): `sstv_sens40_bench <mode> <from> <to> <step> <trials>
[picture] [offHz] [ppm]`. One picture per trial through `makeSstvTx`, white noise in 3 kHz (the definition of test_hfdig_sens39d.cpp), the tuning
error applied by a Hilbert frequency shifter and the clock error by resampling, so both work with any picture. Two pictures: the test picture
(colour bars, a grey gradient, block letters: large flat areas) and a textured one (smooth colour fields, sharp-edged shapes, stripes of 4 and 8
pixel periods, small dots), so a filter that only blurs cannot win. Figures: mean absolute error (levels) and PSNR against the sent picture.
The PSNR floor at high SNR is the mode itself (chroma subsampling, the 3 kHz audio band): Robot 36 is 22.7 dB on the test picture at 40 dB.
`SSTV_NF=0` switches the new filter off (the 3.9 picture), same noise.

## SSTV: noise-adaptive picture filter (core/src/hfdig/hfdig_sstv.cpp)

Cause. A pixel is the mean tone over its time slot, i.e. the phase difference over 1.5 (PD120) to 4.6 samples (Martin M1) of 8 kHz audio. FM
noise rises with frequency, so the error of one pixel is mostly high-frequency noise that neighbouring pixels share with opposite sign; nothing
in 3.9 removed it.

Changes (both on by default, `HfdigSstv::setNoiseFilter(false)` restores the 3.9 picture):
1. Along each line. The tone noise of one pixel is measured on every sync pulse (the spread of pixel-long means inside the steady middle of the
   pulse, averaged over lines). Each pixel's tone goes through a local Wiener filter over 7 pixels with that noise, scaled to the pixel length
   of the scan. Flat stretches are averaged, edges are kept; on a clean signal the measured noise is near zero and the line is unchanged.
2. On the picture. The noise of each colour is measured on the picture itself (median absolute 3x3 Laplacian of the raw rows); each row, once
   its neighbours are final, goes through a 3x3 Lee filter with twice that noise. At 30 levels of noise and more, single pixels far from all
   eight neighbours (FM clicks) are first replaced by their median. A row is shown raw until it is filtered (one or two rows of lag).

Measured, 3.9 -> 4.0, mean error (levels) / PSNR (dB), 2 trials a point, no offset:

| mode, picture | 22 dB | 14 dB | 10 dB | 6 dB | 2 dB |
|---|---|---|---|---|---|
| Robot 36, test | 9.7/21.7 -> 6.2/22.4 | 18.3/18.4 -> 9.0/21.4 | 27.0/15.5 -> 12.3/19.8 | 41.4/12.0 -> 18.4/17.2 | 82.9/6.8 -> 66.2/8.0 |
| Robot 36, textured | 16.3/19.6 -> 11.5/20.2 | 27.2/16.8 -> 14.8/19.0 | 37.4/14.4 -> 19.2/17.5 | 52.7/11.4 -> 26.8/15.3 | 108.7/5.9 -> 99.0/6.7 |
| Martin M1, test | 4.1/29.8 -> 2.0/31.2 | 8.5/25.2 -> 3.1/29.5 | 12.9/21.8 -> 4.2/27.6 | 20.2/18.0 -> 6.6/24.3 | 37.5/12.2 -> 19.2/15.0 |
| Martin M1, textured | 8.4/25.6 -> 5.3/26.3 | 14.7/22.4 -> 6.8/24.6 | 20.8/19.7 -> 8.6/22.8 | 30.6/16.4 -> 12.6/19.8 | picture not complete in both |

PD120 (4.0, no offset): test picture 16 dB 24.7, 10 dB 21.9, 4 dB 14.1 dB PSNR; textured 16.4 / 15.5 / 12.8 dB (3.9 with +-50 Hz, +-50 ppm:
test 20.3 / 15.4 / 8.0, textured 15.1 / 12.6 / -). A 5-tap line filter is 0.4 to 1 dB worse at 6 to 10 dB, a 9- or 11-tap one 0.1 to 0.8 dB better
on PD120 only: 7 taps was kept. Twice the measured noise in the line filter loses 0.3 to 0.6 dB on the textured picture.

At 30 to 40 dB every mode and picture is the same or better (textured Martin M1 30 dB 26.5 -> 26.7 dB, Robot 36 20.4 -> 20.5 dB). The second step alone
(Lee filter, measured over 6 trials with +-50 Hz and +-50 ppm, 2 dB steps): pass point (mean error 20 or less in 6 of 6) Martin M1 8 -> 6 dB,
Scottie S1 8 -> 6 dB, textured Martin M1 12 -> 6 dB, textured Scottie S1 14 -> 8 dB, Robot 36 and PD120 unchanged (the line filter is what
helps those). The along-line filter was added after the Mac went on battery, so its numbers above are without offsets and from 2 trials a point;
the guard test runs it with the offsets. Noise only: 0 pictures in 480 s (the filter does not touch detection).

Bug fixed on the way: Robot 36 on a weak signal crashed the 3.9 decoder (segfault at 4 dB in 3 kHz): when the first line's separator read as
odd, the decoder used the pending chroma of line -1 (`pendLine == k - 1` with `pendLine = -1`), an empty vector.

Not kept: summing the phase steps as power-weighted vectors (a pulse-pair estimator, to make clicks weightless) instead of unwrapping. The
oversampled noise has a real lag-1 correlation that pulls every pixel toward 1900 Hz: worse at every SNR (Martin M1 at 8 dB: 1 of 4 pictures
complete). Widening every pixel's window by a fixed factor helps a noisy picture but blurs a clean one; the measured-noise filter above does
it only where the noise is.

Guard test: `hfdig_sstv_sens40` (tests/hfdig/test_hfdig_sstv_sens40.cpp): Martin M1 at 6 dB and Robot 36 at 10 dB with +-50 Hz and +-50 ppm,
filtered mean error under 14 / 18 levels and 4 levels better than unfiltered; textured picture at 30 dB costs at most 0.3 dB PSNR; Robot 36 at
4 dB does not crash; 60 s of noise give no picture.

## SSTV guard on GitHub (Linux runner, quick.yml run 43144140304)

hfdig_sstv_sens40 with +-50 Hz and +-50 ppm: Martin M1 at 6 dB mean error 21.6 -> 7.5 levels, Robot 36 at 10 dB 28.7 -> 13.2; textured picture at
30 dB PSNR 26.50 -> 26.59 (Martin M1), 20.37 -> 20.47 dB (Robot 36); Robot 36 at 4 dB no crash; 60 s of noise: 0 pictures. hfdig_sstv (every
mode at +-100 Hz and +-100 ppm, the engine path) and hfdig_sens39d pass.

## Weather fax: row filter for weak signals (core/src/marine/marine_fax.cpp)

Same cause as SSTV: a pixel is the mean tone over 3.3 samples at 12 kHz. Below the wide-filter SNR (22 dB, the narrow path) each row's tones now go
through a local Wiener filter over 7 pixels with the pixel noise measured on the row itself (1.4826 x the median absolute second difference /
sqrt 6, which the chart's edges hardly move), averaged over rows. Strong signals (the wide path) are not touched. `FaxDecoder::setNoiseFilter(false)`
gives the 3.9 rows. Test `marine_fax_sens40` (conditions of marine_fax_sens39, 3 trials a point, same noise both ways), GitHub Linux runner:

| SNR (0 .. 6 kHz) | 10 | 4 | 2 | 1 | 0 | -1 | -2 dB |
|---|---|---|---|---|---|---|---|
| mean row correlation, 3.9 | 0.843 | 0.601 | 0.460 | 0.401 | 0.338 | 0.284 | 0.077 |
| 4.0 | 0.933 | 0.796 | 0.634 | 0.537 | 0.448 | 0.359 | 0.094 |
| sens39 passes of 3, 3.9 / 4.0 | 3 / 3 | 3 / 3 | 0 / 3 | 0 / 3 | 0 / 0 | 0 / 0 | 0 / 0 |

About 2 to 3 dB on the sens39 criterion. The line lock (rows, line rate, drift) is the same both ways at every point; marine_fax_sens39,
marine_fax_weak and marine_fax_decode pass. (sens39 recorded its 90 % point at 2 dB over 8 other seeds; on these 3 seeds the 3.9 decoder
fails at 2 dB, so the seeds here are harder.)

## NOAA APT: line filter (core/src/wxsat/wxsat_apt.cpp)

Each line's space A (47 words of steady grey after sync A; its middle 31) gives the noise of one envelope sample: its variance and its median
absolute deviation, both averaged over lines. Image A and B of the line then go through (1) a click step: a sample more than 4 robust sigmas from
the median of its 5 neighbours (an FM click below the 137 MHz carrier's threshold) is replaced by that median, and (2) a local Wiener filter over
7 samples with the variance. The sync search and line lock are not touched (same lines found both ways at every SNR). `setAptNoiseFilter(false)`
(WxsatReceiver) gives the 3.9 picture. Test `wxsat_apt40` (receiver test signal, 2.5 kHz offset, 30 Hz/s Doppler rate, 80 ppm clock, PSNR of
image A and B after a least-squares gain and offset), GitHub Linux runner, one 16 s run a point:

| generator SNR (40 kHz) | 30 | 15 | 10 | 5 | 2 | 0 | -3 dB |
|---|---|---|---|---|---|---|---|
| PSNR 3.9 | 32.24 | 30.51 | 17.06 | 13.81 | 10.18 | 8.38 | 6.68 |
| 4.0 | 34.14 | 32.83 | 17.27 | 15.20 | 12.78 | 10.98 | (Wiener only: 7.45) |

The test picture is smooth (ramps and slow waves with one sharp step), so a real Earth picture with more detail gains less. The step from 15 to
10 dB is the FM threshold of the carrier; a PLL or feedback FM demodulator (threshold extension) is the next step there and was not tried.
Meteor LRPT, MetOp / FY-3 AHRPT and GOES / GK-2A HRIT are at theory already (SENSITIVITY-3.9-A.md, SENSITIVITY-4.0-A.md): not changed.

## NFM voice: click removal looked at, not kept

General receiver, NFM, 1 kHz tone at 2.5 kHz deviation, SNR in 12.5 kHz, SINAD over 2 s. 3.9 curve: 16 dB 19.8, 12 dB 15.8, 10 dB 13.8,
9 dB 12.8, 8 dB 11.6, 7 dB 9.9, 6 dB 8.0, 4 dB 4.7 dB SINAD. The 12 dB SINAD point (8.4 dB) is above the FM threshold: a model of the channel
gives 2 phase slips a second at 8 dB, 30 at 6 dB, 143 at 4 dB, so the noise there is Gaussian and click removal cannot move it.
Two click removers were built and measured: replacing spike samples in envelope dips by the last good frequency (worse: -1.7 dB SINAD at 7 to
8 dB, it cuts the non-slip excursions in half) and removing whole cycles from dips whose extra phase is near 2 pi (+0.2 to +0.3 dB from 7 dB down,
0 above). Neither was kept. What would move the 12 dB SINAD point is the audio bandwidth (4 kHz low-pass now; FM noise rises with frequency,
3 kHz would be about 3.7 dB less noise) or de-emphasis, which change the default sound of every NFM user: left for the owner.

## Looked at, not changed

* AM: the general receiver's AM processor already picks synchronous detection (both sidebands or the cleaner one) and has log-MMSE noise
  reduction; the harness criterion (27 dB) is set by the tone test, not the detector.
* Analog TV: noise reduction modes exist (`AtvReceiver::setNoiseReduction`); not re-measured here.
* Hydrogen line: integration up to 3600 s, median baselines, RFI flagging and Gaussian fits exist (core/src/astro); nothing missing found.
* Fast scanner / explorer: the per-bin threshold (8 dB over a 25th-percentile floor) could get a multi-bin test for wide weak carriers at the
  same false-alarm rate; not built: it changes every scan and needs a local false-alarm sweep, and the Mac went on battery during this pass.
