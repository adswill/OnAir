# Waterfall and spectrum resolution (4.0)

The owner's request: zoomed in, the waterfall looked blurred, like a WebSDR waterfall. SDR++ has a clean one. The spectrum should support
higher resolutions too.

## Why it blurred (3.9)

1. **Coarse bins stretched across many pixels.** The automatic FFT stops at 65536 bins (about 125 Hz bins, 36.6 Hz at 2.4 Msps, 305 Hz at
   20 Msps), and the history keeps at most 65536 columns. Zoomed in to a few kHz, one bin covered tens or hundreds of pixels.
2. **The picture was interpolated twice.** The OnAir look had `wfSmooth` on: zoomed in, the view blended levels between columns
   (`slook::lerpLevel`). The view texture then had fewer texels than the plot had pixels, and the GPU stretched it with a linear filter.
   The trace used a cubic curve between bins. Together this gave the soft, smeared look.

## What 4.0 does

### Zoom FFT (core `dsp/zoom_spectrum.h`)

When the full-band bins are coarser than the plot's pixels (`fs / fftSize > span / pixels`), the engine also runs a zoom transform of the
visible span:

- mix the view's centre to DC (NCO renewed from a double phase every 1024 samples),
- decimate by D, a power of two: a 4-stage CIC on 64-bit integers (factor up to 1024), then 47-tap Kaiser half-bands (two, or more past
  a CIC of 1024). The half-band passes 0.78 of the decimated band flat (0.001 dB ripple) and rejects aliases into it by at least 78 dB;
  the display uses the inner 0.75 of the band,
- transform N points (a power of two, 4 x the plot's pixels, 512 .. the cap, 65536 by default, 1M at most), overlapped so frames keep
  coming about 30 a second, and average up to the frame's blocks like the full-band analyser.

`zoomfft::update` picks the plan: D is the largest power of two that keeps the view under half the decimated band (room to pan), N stays
the same across an octave of zoom, so the zoom restarts once per octave; a pan that stays inside the usable band keeps the plan, and a
pan out of it re-centres on the view at a whole number of bins, so the rows can shift exactly. The zoom transform runs on the existing
spectrum thread after the full-band one; nothing else changes in the threading. It stops when the window is minimised or covered, when
the plot is not drawn for half a second, and whenever the full-band bins are already fine enough.

Levels are dBFS per bin as in the full-band analyser (a tone reads the same in both; the noise floor per bin is lower in the finer bins,
by 10 log10 of the bin ratio). With "per Hz" on, both read the same floor.

### The picture (app `gfx/wf_view.h`, `gfx/spec_pipe.h`, `gfx/zoom_detail.h`)

- The waterfall view is built **a texel per framebuffer pixel** over exactly the visible range (`WfView::updateExact`), so the GPU never
  stretches it across. Columns finer than pixels give each pixel the highest column whose centre falls in it (a one-bin signal lands in
  exactly one pixel, never lost, never doubled); coarser columns become sharp steps (the column under the pixel's centre; no level is
  made up between two columns): `spipe::pixelLevels`.
- The zoom spectrum has its own waterfall history, parallel to the main ring (row i of one is the same moment as row i of the other).
  Each row comes from the zoom history where it has one, else from the main history: rows from before a zoom show at the main
  resolution (sharp, not stretched), later rows at the zoom resolution. A pan of the same shape shifts the zoom rows by whole bins; a new
  shape or a retune starts them again (the main rows show meanwhile). The zoom history's colour range follows its own noise floor,
  placed in the colour map as the main one, so the noise has the same colour in both.
- The trace, max hold and live line come from the zoom spectrum's real bins when it covers the view (the average in power and the hold
  on those bins), with straight lines between bins where bins are sparser than pixels (the cubic curve is gone).
- A **resolution readout** at the bottom right: `RBW <Hz>/bin <Hz>/px`, with "zoom FFT" when the zoom spectrum draws.

### Settings

Spectrum button > FFT size: Auto (from the sample rate, as before) or 4096 .. **1M** (was 256k). Spectrum button > Resolution (advanced):

| Setting | Default | What |
|---|---|---|
| Zoom FFT | on | the zoom spectrum described above |
| Zoom FFT bins at most | 65536 | cap of N (4096 .. 1M) |
| Window | Hann | Hann, Blackman-Harris, Nuttall, flat top, rectangular (all at unit rms: the noise reads the same) |
| Overlap | Auto | how much long transforms share; Auto = as before up to 256k, at most 3/4 above (a 1M FFT at 2.4 Msps: about 9 frames a second) |
| Resolution readout | on | the Hz per bin / Hz per pixel line |
| Soft zoom (the look before 4.0) | off | the 3.9 interpolated waterfall and cubic trace |

Prefs keys: `specZoomFft`, `specZoomMax`, `specWindow`, `specOverlap`, `specResReadout`, `wfSoft`; `specFft` now goes to 1048576.

## Numbers

### Resolution: Hz per bin drawn at several zoom levels (plot 1920 px wide)

Before = 3.9 (the automatic FFT, one history column per bin, 64k at most); after = 4.0 with the zoom FFT. From
`tests/app/test_zoom_detail.cpp` (printed on the CI run).

| Sample rate | View | Hz per pixel | 3.9: Hz per bin (pixels per bin) | 4.0: Hz per bin |
|---|---|---|---|---|
| 2.4 Msps | whole band | 1250 | 73.2 (0.06) | 73.2 (full band) |
| 2.4 Msps | 240 kHz | 125 | 73.2 (0.6) | 73.2 (full band) |
| 2.4 Msps | 100 kHz | 52.1 | 73.2 (1.4) | 36.6 (zoom FFT) |
| 2.4 Msps | 20 kHz | 10.4 | 73.2 (7.0) | 9.16 |
| 2.4 Msps | 5 kHz | 2.60 | 73.2 (28) | 2.29 |
| 2.4 Msps | 1 kHz | 0.52 | 73.2 (141) | 0.29 |
| 10 Msps | whole band | 5208 | 153 (0.03) | 153 (full band) |
| 10 Msps | 100 kHz | 52.1 | 153 (2.9) | 38.1 |
| 10 Msps | 20 kHz | 10.4 | 153 (15) | 9.54 |
| 10 Msps | 5 kHz | 2.60 | 153 (59) | 2.38 |
| 10 Msps | 1 kHz | 0.52 | 153 (293) | 0.30 |
| 20 Msps | whole band | 10417 | 305 (0.03) | 305 (full band) |
| 20 Msps | 100 kHz | 52.1 | 305 (5.9) | 38.1 |
| 20 Msps | 20 kHz | 10.4 | 305 (29) | 9.54 |
| 20 Msps | 5 kHz | 2.60 | 305 (117) | 2.38 |
| 20 Msps | 1 kHz | 0.52 | 305 (586) | 0.30 |

In 3.9 each of those bins was also blended across its pixels (level interpolation plus the GPU's linear stretch). In 4.0 every row
is drawn a texel per pixel, and wherever the zoom FFT runs there is at least one bin per pixel.

### CPU (ms of CPU per second of signal, GitHub runner)

From `tests/dsp/test_zoom_spectrum.cpp`: the full-band analyser at its automatic size, the full-band analyser at the size that would be
as fine as the zoom spectrum for a 10 kHz view on 2000 px (the brute-force alternative, capped at 1M), and the zoom spectrum.

| Sample rate | Full-band, automatic size | Full-band as fine as the zoom (brute force) | Zoom FFT, 10 kHz view on 2000 px |
|---|---|---|---|
| 2.4 Msps | 32.0 (32768 bins, 73 Hz) | 208.4 (512k bins, 4.58 Hz) | 11.9 (D 64, 8192 bins, 4.58 Hz) |
| 10 Msps | 73.4 (65536 bins, 153 Hz) | 756.8 (1M bins, 9.54 Hz: the cap) | 32.4 (D 256, 8192 bins, 4.77 Hz) |
| 20 Msps | 91.2 (65536 bins, 305 Hz) | 729.6 (1M bins, 19.1 Hz: the cap) | 60.0 (D 512, 8192 bins, 4.77 Hz) |

Run 43166344847 (ubuntu-24.04, Linux quick job). The first version of the mixer (one phasor rotation chain, `std::complex<float>`
products) cost 15.8 / 45.6 / 84.8 ms; the mixer now runs eight phasor lanes with the products written out and the CIC registers in
locals. The full-band analyser itself is unchanged.

The zoom spectrum adds its cost only while it runs (zoomed in past the full-band resolution, window visible).

## Tests

- `zoom_spectrum` (tests/dsp/test_zoom_spectrum.cpp): the split of D; the plan over a sweep of sample rates (48 kHz .. 61.44 Msps), plot
  widths (320 .. 7680 px), spans and off-centre views: a bin per pixel or finer, the view inside the usable band, room to pan, whole-bin
  offsets, the caps; the automatic choice (off when the full-band bins suffice, kept on small pans and inside an octave, a new plan on a
  big pan, a deeper zoom, a new sample rate); unit gain of the decimator for D 2 .. 4096 (within 0.35 dB); alias rejection of tones 0.55
  to 17 decimated rates away (more than 65 dB under an in-band tone); two tones 12 Hz apart near +100 kHz at 2.4 Msps, off any bin, in an
  off-centre view, resolved with a deep dip (the 64k full-band FFT cannot); the tone level and the noise floor against the full-band
  scaling; a view at the band's edge at 10 Msps; windows (unit rms, ENBW); the overlap rule; 1M cap; settings changed while another thread
  feeds; CPU figures.
- `zoom_detail` (tests/app/test_zoom_detail.cpp): 3000 random spikes, zooms and pans: a one-column signal in exactly one pixel when columns
  are finer, never lost when coarser; four columns on 1000 pixels give four sharp steps; the dB version for the trace; the zoom history
  (rows beside the main ring, missing rows, a pan shifts the rows by whole bins, a new shape and a retune clear them); the exact view (a
  spike kept from 65536 columns on 1500 pixels, a sharp ~55 px block at 2400x zoom with no level in between, zoom rows used only inside
  the zoom band, row by row == rebuilt, low-pass on pixel rows); the resolution table above.
- Existing `plot_util`, `spec_look`, `spec_axis`, `wf_fast`, `spectrum_*` keep passing (the old view path is still there for the soft look
  and the render harness).

All of this is on generated signals (tones, white noise). No screenshots were taken: the Mac was on battery during the work (no local
builds); the GUI change is checked by building the app on CI only.
