# Propagation (Tools > Propagation)

**4.0:** the models were rebuilt on the ITU-R recommendations (P.1812-8, P.533-14 / P.1239-4, P.676-13, P.838-3, P.618-14) with
validation against the ITU test data; see [PROPAGATION-4.0.md](PROPAGATION-4.0.md). The sections below describe the 3.9 models, which
stay selectable ("the 3.9 model", "use the 3.9 estimate").

Where can a transmitter be heard? The window has two modes, chosen by the toggle at the top.

**Simple**: put the transmitter on the map (a button arms the next click, or use your location, or type a position or locator), pick what it is
(FM broadcast, DAB+, DVB-T/T2, airband, ham 80 / 40 / 20 / 10 m SSB, 20 m FT8, 2 m and 70 cm FM, CB, PMR446, LoRa / Meshtastic, ADS-B or
Custom), and set the frequency and power. The answer is in plain words ("You should hear it up to about 40 km", or the skip zone on HF), a
coloured map (green good, yellow fair, red poor) with the range ring, and a good / fair / poor rating with one-line reasons where you listen:
a map click, or your location when it is set (ADS-B window).

**Advanced**: every input, in sections with a tooltip on each:

- transmitter: preset, power (W, kW, dBW, dBm), antenna gain, feeder loss, height, polarisation, an **antenna pattern file** (the hook for the
  antenna designer: CSV lines `azimuth_deg,gain_dbi`, optional `el,elevation_deg,relative_db`, `#` comments; it replaces the scalar gain by
  bearing from a boresight azimuth, and by take-off angle on HF);
- signal: frequency, noise bandwidth, required SNR;
- receiver: antenna gain, height, feeder loss, polarisation, the **radio** (RTL-SDR V3 / V4, Airspy R2 / HF+, SDRplay, HackRF One / Pro,
  ADALM-Pluto, LimeSDR, USRP, bladeRF, KiwiSDR, a ham transceiver, or a typed noise figure) and its gain setting, an optional preamplifier
  at the antenna; the chain noise figure and the sensitivity are shown as you type;
- path: the model (auto, free space only, smooth earth only, exact two-ray), HF modes (ground + sky, either alone), ground constants, the
  clutter class around the receiver, the man-made noise environment, atmospheric noise on or off;
- ionosphere and time: SFI, K index, date and time (now, or typed, UTC), with the sun and season at the transmitter shown;
- rain rate (with presets) and gases;
- terrain: elevation files you already have (below);
- map resolution (40 / 80 / 140 cells per side), the link-budget distance and the HF chart distance when no point is chosen.

The results are in tabs: **Map** (the heat map of the SNR margin, range rings, the numbers at the chosen point), **Link budget** (every term,
each explained in its tooltip; Export CSV writes the budget, the sweeps and the HF day chart), **Sweeps** (SNR against distance, range against
power, range against frequency, and on HF the MUF / FOT / LUF over the UTC day with your frequency marked), **Models** (what each model does
and does not cover). **Compare two scenarios** adds a scenario B (a copy of A to start): the inputs edit A or B, the budget gets B and B - A
columns, the sweeps a blue line, the map B's rings dashed.

Input checks say what is wrong and how to fix it (errors stop the computation; warnings and notes do not). The map, ranges and sweeps run on a
worker thread, one job at a time, cancelled when the inputs change; a smooth-earth 80 x 80 grid takes a few ms, with terrain about 0.1 s on a
desktop (the coarse grid is for a phone or a Raspberry Pi). Everything is saved (keys `prop_*`, scenario B `propB_*`; the 0.2.9 keys are read
as a Custom preset). Nothing is downloaded.

Code: `core/include/dect2/prop/prop.h` (models), `prop_terrain.h` (elevation files, profiles, terrain diffraction), `prop_scenario.h` (presets,
radios, checks, verdict, budget, sweeps, CSV); `core/src/prop/`; screens `app/prop/prop_ui.cpp` (window, worker, map, Simple) and
`app/prop/prop_adv.cpp` (Advanced). Tests: `tests/prop/test_prop_model.cpp` (the regression suite of known cases),
`test_prop_scenario.cpp`, `test_prop_terrain.cpp`. All formulas are written from the public ITU-R texts and textbooks; no code is copied.

## What was wrong in 0.2.9

- VHF/UHF: pure free space up to the radio horizon, then the P.526 far-field formula switched on: the path loss jumped about 47 dB at the
  horizon (2 m, 10 m / 2 m antennas: 100.8 dB at 18 km, 148.4 dB at 20 km) and the ground reflection was missing, so close-in ranges were far
  too optimistic and the edge was a cliff.
- HF: the D-layer absorption used the angle of a ray reflected *at* the D layer (grazing, sec ~4.3 at 1000 km) instead of the F2 ray crossing
  it (sec ~2), giving 43 dB at 7.1 MHz over 1000 km at noon; 40 m daytime ranges collapsed. No above-MUF loss (a hard cut), no curvature on
  the ground wave, hops allowed beyond the geometric limit, no atmospheric noise, man-made and galactic noise as a max rather than a sum.
- Screen: a map click moved the transmitter unless a combo box was changed first; the readout and the heat map used different settings while
  computing; the result was copied every frame (twice) on the UI thread; a skip zone with a ground-wave part was shown as one range.

## Link budget

`Pr = Ptx - Lfeeder,tx + Gtx(bearing) - L + Grx` (dBm, at the receive antenna terminals); `L` is the sum of free space, the model's excess
loss, clutter, rain, gases and polarisation mismatch. Field strength `E[dBuV/m] = Pr - Grx + 20 log f[MHz] + 77.2`. SNR = `Pr - N`,
margin = SNR - required SNR; sensitivity = `N + required SNR`. Ratings: good at 10 dB margin or more, fair 0..10, poor below 0.

## Noise and the receiver

`N = kTB + 10 log(fa + f - 1)`, kTB = -174 dBm/Hz + 10 log B. `f` is the receive chain referred to the antenna (Friis): without a preamplifier
`f = Lfeeder * Fradio`, with one at the antenna `f = Fpre + (Lfeeder Fradio - 1) / Gpre`. The radio's noise figure is a typical figure for
its model (HF and VHF/UHF separately) plus `16 (1 - g)^2` dB for a gain setting g of 0..1 (a rough rule: low gain costs noise figure).

`fa` is the power sum (P.372) of:
- man-made noise `Fa = c - d log f[MHz]`: city 76.8 / 27.7, residential 72.5 / 27.7, rural 67.2 / 27.7, quiet rural 53.6 / 28.6;
- galactic noise `52 - 23 log f`, faded in between 5 and 12 MHz (the ionosphere screens it below its critical frequency);
- atmospheric noise (optional, below 30 MHz): a rough fit of the P.372 maps for mid-latitude summer, night `90 - 38 log f` (to 10 MHz) and
  day `45 - 10 log f`, both falling steeply above 10 MHz, blended across sunrise and sunset at the receiver, +10 dB in the tropics, -10 dB at
  high latitude, -10 dB in local winter.
The total is floored at 0 dB (290 K: the antenna also sees the warm ground).

## 30 MHz and up

- Free space (P.525): `32.45 + 20 log f + 20 log d`.
- Smooth-earth diffraction (P.526 section 3.2): beyond the marginal line-of-sight distance the first-term residue formula of section 3.1.1
  (`F(X) + G(Y1) + G(Y2)` with K and beta from the ground constants and polarisation, G limited to `2 + 20 log K`); before it, the smallest
  clearance of the ray over the curved earth against `0.552` of the first Fresnel zone, interpolated with the residue loss for the modified
  earth radius. 4/3 earth (8495 km). This is continuous at the horizon.
- Ground reflection: the plane-earth asymptote `40 log d[m] - 20 log(h1 h2)` (above free space beyond the breakpoint `4 pi h1 h2 / lambda`).
  Auto uses the larger of the two, then adds troposcatter. "Two-ray exact" uses `Lfs - 20 log |2 sin(k delta / 2)|` with its peaks and nulls.
- Troposcatter (P.452 section 4.3, 50 % of the time): `190.1 + Lf + 20 log d + 0.573 theta - 0.15 N0 + Lc` with `Lf = 25 log fG - 2.5 (log fG/2)^2`,
  N0 = 325, theta the angular distance (smooth earth, or the terrain horizon angles), power-summed with the rest.
- Receiver clutter (P.2108 section 3.1): for a receive antenna below the representative clutter height (suburban 10 m, urban and forest 15 m,
  dense urban 20 m), `J(nu) - 6.03` with `nu = 0.342 sqrt(fGHz) sqrt(hdif theta)`, street width 27 m. Open ground adds nothing (the ground
  reflection already has the height dependence).
- Rain: `k R^alpha` (P.838-3 horizontal coefficients at 1..100 GHz, interpolated in log f) over `d / (1 + d / d0)`, `d0 = 35 e^(-0.015 R)` km.
- Gases: a rough sea-level curve (7.5 g/m3) with the 22 GHz and 60 GHz lines.
- Polarisation mismatch: crossed linear 20 dB, linear to circular 3 dB, opposite circular 20 dB.

Checked against: Okumura-Hata open area within 6 dB at 450 MHz (2..20 km) and 8 dB at 900 MHz; the 1 kW-ERP free-space field 106.9 dBuV/m at
1 km; a broad band around the P.1546 100 MHz land curves (150 m mast); an aircraft at 10 km heard to about its 425 km radio horizon.
Limits: no buildings beyond the clutter class, no foliage depth, no ducting or anomalous propagation, no sporadic E (a note warns at 30..60 MHz),
no time / location statistics; real coverage in towns and hills is usually shorter unless you load terrain.

## Terrain (optional, only from your files)

Load SRTM `.hgt` tiles (the name gives the south-west corner, e.g. `N54E025.hgt`; 1201 or 3601 samples per side, big-endian int16, voids
filled from their neighbours) or an uncompressed GeoTIFF in WGS 84 latitude / longitude (one band of int16 / uint16 / int32 / float32, strips or
tiles, either byte order, PixelIsArea or PixelIsPoint, GDAL_NODATA honoured). Compressed, BigTIFF and projected (UTM) files are refused with a
message saying how to convert them. With terrain, each path's profile (up to 200 samples) gives the delta-Bullington diffraction (P.526 4.5 /
P.1812 4.3): `Lbull(actual) + max(Lsph - Lbull(smooth), 0)`, with the heights above the mean terrain for the spherical-earth part and the
ground reflection, and the horizon angles for troposcatter. Paths that start or end outside the files use smooth earth; the range is then the
median of 8 directions (the best one is named), and the sweeps use smooth earth.

## Below 30 MHz: ground wave and skywave (an estimate)

- Ground wave: Norton's attenuation in Terman's form `A = (2 + 0.3p) / (2 + p + 0.6p^2)`, `p = pi d / (lambda x) cos b` for vertical
  (`b = atan((eps + 1) / x)`) and `/ cos b'` for horizontal polarisation (`b' = atan((eps - 1) / x)`), `x = 18000 sigma / f[MHz]`; sea 80 / 5 S/m,
  wet 30 / 10 mS/m, average 15 / 5 mS/m, dry 4 / 1 mS/m. Beyond the flat-earth range the P.526 smooth-earth loss (with the ground's K and beta)
  takes over when larger, so the earth's curvature is felt. The loss is quoted for an isotropic EIRP; a short monopole on ground is 4.77 dBi.
- Skywave: one F2 layer at 300 km. `foF2 = night + (day - night) sqrt(c)`, day `(5.5 + 0.045 R12)(1 - 0.35 sin^2 lat)`, night 0.45 day, c from the
  sun's zenith angle at the path midpoint (season and time), R12 from the SFI (`SFI = 63.7 + 0.728 R + 0.00089 R^2`), lowered 4 % per K above 3.
  Hops: the fewest with a take-off angle of at least 3 degrees (at most about 3200 km each). `MUF = foF2 sec(i)` at the layer. D-layer
  absorption per hop (CCIR 252 / P.533 form): `677.2 Ir sec(i100) / ((f + 1.2)^1.98 + 10.2)`, `Ir = (1 + 0.0037 R) max(cos(0.881 chi)^1.3, 0.1)`,
  with `i100` the angle where the F2 ray crosses 100 km; storms add 3 dB per K above 4 at latitudes over 55 degrees. Above the MUF
  `130 (f / MUF - 1)^2` dB (scatter), no skywave once that passes 40 dB. Loss = free space over the slant path + absorption + above-MUF loss +
  2 dB per extra ground reflection + 8.7 dB P.533 excess loss. Ground and sky are power-summed.
- Checked against textbook ranges: one-hop limit about 3840 km at 0 degrees; M(3000)F2 about 3; foF2 6..11 MHz at a mid-latitude summer noon
  (SFI 120), 2.5..6 at night; MUF(1000 km) 9..16 MHz by day, 4..9 by night; 7.1 MHz absorption over 1000 km 10..30 dB at noon (19 dB here),
  a few dB at night, much less at 14 MHz; a 20 m skip zone by day; 10 m closed at night near solar minimum; 80 m at night beyond 500 km.
- Limits: a deliberately simple model, good for "is the band open, roughly how far", not for a circuit plan. No E layer or sporadic E, no
  monthly maps, winter anomaly, equatorial crests or geomagnetic latitude, no fading statistics; SFI and K are typed by you.

## Licence note

Formulas only (ITU-R P.525, P.526, P.452, P.2108, P.838, P.530, P.676, P.372, P.368 background, P.533 / CCIR 252 absorption, Okumura-Hata in
the tests). The few numbers taken from the recommendations (P.838 coefficients at a handful of frequencies, a rough gas curve) are factual
values; no ITU software or data files are included.
