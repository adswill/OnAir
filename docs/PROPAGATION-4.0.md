# Propagation calculator 4.0 (Tools > Propagation)

Owner's request (2026-11-12): "completely improve the propagation calculator, make it way better, at the moment it kinda sucks and isn't
realistic". This file records what was unrealistic in 3.9, what 4.0 computes instead, which public recommendation (and which version) each
part follows, and what was checked against published reference values. All code is written from the text of the recommendations; no ITU
software or third-party code is copied. The data tables that the recommendations publish (spectral lines, rain coefficients, numerical
ionospheric maps, decile tables) are factual values and are included with their source named next to them.

## 1. What was unrealistic in 3.9

Read from `core/src/prop/prop.cpp`, `prop_terrain.cpp`, `prop_scenario.cpp`, `app/prop/*` and `tests/prop/*` on integ-40 (d1e3df934).

### VHF / UHF / SHF (30 MHz and up)

- **One number per path, no statistics.** The loss was a single median value. Real planning asks "for 90 % of locations and 99 % of the
  time" (or 50 / 50 for a coverage estimate): there was no time percentage, no location variability and no way to see the fades that
  decide whether a link works at the edge of coverage.
- **Fixed 4/3 earth.** The effective earth radius was always 8495 km. The median refractivity gradient differs by climate (about 40
  N-units/km in Northern Europe, 50-70 over the Gulf), and anomalous propagation (super-refraction and ducting, the reason the Gulf hears
  Iran and India on VHF/UHF in summer) was not modelled at all.
- **Troposcatter with N0 = 325 only, at 50 % only.**
- **Terrain only from files the user already had**; no way to get them from inside OnAir. Without them the map silently used a smooth
  earth, with only the model name in the Models tab to tell the difference.
- **No profile view.** The terrain profile, the line of sight and the first Fresnel zone, the first thing a radio amateur looks at for a
  point-to-point link, were never drawn.
- **Gases**: a hand-drawn sea-level curve with the 22 and 60 GHz lines ("rough P.676 curve"), no temperature, pressure, humidity or height.
- **Rain**: P.838 coefficients at "a handful of frequencies", interpolated, for horizontal polarisation only, with the old P.530 path
  reduction; the rain rate was a single number with no time percentage, and nothing for slant (satellite) paths.

### HF (below 30 MHz)

- **The ionosphere was invented.** foF2 came from a home-made formula: `(5.5 + 0.045 R12)(1 - 0.35 sin^2 lat)` by day, 0.45 of that by
  night, blended with `sqrt(cos chi)`. Real foF2 depends on the magnetic field (the equatorial anomaly crests 15 degrees either side of the
  magnetic equator), on longitude, on the season in a non-obvious way (the winter anomaly: mid-latitude noon foF2 is higher in winter than in
  summer at high solar activity) and has a post-sunset enhancement; none of that was there.
- **One F2 layer at a fixed 300 km, no M(3000)F2.** The MUF was `foF2 sec(i)` at a flat 300 km mirror; the published distance factor
  (P.1240 Cd, dmax, B from M(3000)F2) was not used, so the MUF over 3000-4000 km was badly off.
- **No E layer**: no E modes (which carry 80 / 40 m by day up to 2000 km), no E-layer screening of F2 modes.
- **One mode only**: the fewest hops, nothing else, so a 2-hop path that is open while the 1-hop is not was never found, and the
  field strength was never the power sum of the modes.
- **Absorption at the path midpoint only**, with a fixed 1.2 MHz gyrofrequency.
- **Above-MUF loss used the E-layer form** `130 (f/MUF - 1)^2` for the F2 layer (P.533 gives `36 sqrt(f/fb - 1)` for F2, limited to 62 dB).
- **No long-path method** (P.533 uses a composite empirical method beyond 7000 km).
- **No reliability.** The output was the median SNR. Whether a band is "open" on a given day is a probability: the MUF and the signal vary
  from day to day (decile factors in P.1239 and P.842) and the noise varies too (P.372). Without that there is no realistic "reliability
  per hour and band", which is what VOACAP and P.533 users read.
- **Atmospheric noise**: a two-line fit, no proper time blocks.
- **SFI typed only.** (The app does not fetch solar data anywhere, so this stays a typed value; the sunspot number can now be typed directly.)

### Satellite and space links

- **Not covered at all**: no slant range, elevation, Doppler, slant-path rain (P.618) or gaseous attenuation for an earth-space path.

### Receiver side

- **Required SNR typed by hand** (presets used 10-12 dB for everything). OnAir measures the decode threshold of each of its modes
  (docs/SENSITIVITY.md), but the calculator could not answer "will mode X decode here?".

### Screen

- No plot of path loss against distance by model, no terrain profile with the Fresnel zone, no HF hour-by-band chart, no satellite panel.

## 2. What 4.0 computes

| Part | Recommendation (version) | Code | Notes |
|---|---|---|---|
| Free space | ITU-R P.525 | core/src/prop/prop.cpp | unchanged |
| Two-ray ground reflection | textbook exact two-ray, plane-earth asymptote | prop.cpp | selectable ("two-ray exact"), drawn in the loss-by-model sweep |
| VHF/UHF/SHF terrain path (Auto) | **ITU-R P.1812-8 (09/2025)** sections 3.5-4.9, Attachments 1 and 2 | core/src/prop/p1812.cpp | delta-Bullington diffraction for the median and the beta0 earth radius, troposcatter, ducting / layer reflection, the 4.6 blend, location variability; p = 1..50 % of time, pL = 1..99 % of locations; used above 6 GHz too (P.452 shares the mechanisms) |
| Earth curvature / refraction | ITU-R P.453-14, P.1812 eq. (6) | itu_atmos.cpp | k50 = 157 / (157 - dN) from a typed lapse rate (climate presets); super-refractive k = 3 for the beta0 part |
| Terrain | SRTM 1 arc-second tiles ("Terrain Tiles" on AWS, opt-in download into the data folder, kept at 3 arc-seconds), or the user's .hgt / GeoTIFF | prop_terrain.cpp, app/prop/prop_adv.cpp | no terrain: a flat sea-level profile, with a warning in the verdict, the Models tab and the Profile tab |
| Fresnel clearance | first Fresnel radius 17.32 sqrt(d1 d2 / (f d)) | prop_scenario.cpp `profileView` | Profile tab: ground bent for the k-factor, line of sight, first zone, 60 % line, worst clearance |
| Gases | **ITU-R P.676-13 (08/2022)** Annex 1 (44 O2 + 35 H2O lines), terrestrial (2.1) and layered slant path with ray bending (2.2.1) through **P.835-7** | itu_atmos.cpp | replaces the hand-drawn curve |
| Rain | **ITU-R P.838-3 (03/2005)** full regression, any tilt and elevation; **P.530-19** 2.4.1 terrestrial; **P.618-14** 2.2.1.1 slant | itu_atmos.cpp | rain height: latitude fit of the P.839-4 map (the map itself is not shipped) |
| HF sky wave | **ITU-R P.533-14 (08/2019)** with **P.1239-4 (08/2023)** CCIR maps, **P.1240-2** MUF and Rop, **P.842-5** deciles | core/src/prop/hf_p533.cpp | see below |
| Noise | **ITU-R P.372-18 (09/2026)** man-made Table 1 / 2, galactic | hf_p533.cpp, prop.cpp | atmospheric noise stays a fit (P.372 gives it as maps and a program only) |
| Satellite links | P.618-14 rain, scintillation (2.4.1), total (2.5), sky noise (3); P.676-13 slant gases | core/src/prop/sat_link.cpp | look angles from a WGS 84 station, LEO range rate and Doppler, G/T, C/N0, margin |
| Receiver / decode | OnAir's measured thresholds (docs/SENSITIVITY.md) | core/src/prop/mode_snr.cpp | "Will it decode?": 47 modes, white noise / real-radio / fading thresholds and their bandwidths |

### HF in detail

- foF2 and M(3000)F2: the CCIR (Oslo 1966) numerical maps recommended by P.1239-4 (13 x 76 and 9 x 49 coefficients per month and solar
  level, Table 1 geographic functions), with the modified dip X = atan(I / sqrt(cos lat)) from the epoch-1960 sixth-order field model at
  300 km (DGRF 1960 coefficients), linear in R12 up to 160. foE from P.1239 section 4 (A B C D, the night minimum).
- Control points as P.533 Table 1; E modes (hop up to 2000 km, three orders, paths up to 4000 km) with foE sec i110; F2 modes (the lowest
  order with a hop up to dmax and five more) with the P.1240 / P.533 eqs. (3)-(8) MUF, the 3.5.1.2 rule for paths longer than dmax; E-layer
  screening (section 4); mirror heights from section 5.1 (the three cases on x = foF2/foE); elevation (13) and slant range (19).
- Loss per mode (18): free space over the virtual slant range, absorption, above-MUF loss (25)/(26), 2 dB per ground reflection, the auroral
  loss of Table 2 (geomagnetic latitude, local time, season, range), Lz = 8.72 dB; field (17), power sum (28).
- Beyond 7000 km the composite method (5.3): fM with the K factor and the 24-hour fBM, fL over 24 hours with the night LUF and the sunset
  decay, Etl with Gap and Ly, interpolated over 7000-9000 km (42).
- Operational MUF = basic x Rop (P.1240 Table 1); MUF on 90 % / 10 % of days from the P.1239 Tables 2 and 3 decile factors; the probability
  of support (P.533 3.6).
- SNR (45) with the P.372 noise plus the receive chain; deciles: within the hour 8 / 5 dB (HFBC-87), day to day from P.842 Table 2 by
  f / MUF and geomagnetic latitude, noise deciles from P.372 Table 2 (man-made), 2 dB galactic; BCR as P.842 / P.533 section 10.1.
- The hour x band chart (HF hours x bands tab): reliability and median SNR for the ten ham bands plus the chosen frequency, 24 UTC hours.

Deviations, on purpose and stated: the absorption Li uses the closed-form CCIR Report 252 expression `677.2 sec(i) (1 + 0.0037 R)
F(chi) / ((f + fL)^1.98 + 10.2)` at the two 90 km penetration points of each hop, with fL = |fH sin I| from the field model, because P.533's
AT_noon, phi_n and p come as graphs only (Figs. 1-3); the atmospheric noise is the 3.9 fit; the transmit gain is one value for all
take-off angles unless a pattern file is loaded; the K index is no longer used (the maps are monthly medians).

## 3. Validation (what matches)

All on GitHub (quick.yml), tests/prop/:

| Test | Reference | Result |
|---|---|---|
| test_prop_p1812 | ITU-R SG3 validation set P_1812_8.zip: 7 profiles (sea 91 % / land, horizontal / vertical, 1 to 110 km, LoS, sub-path and trans-horizon, with and without clutter) x 3 time percentages = 21 cases, plus the published intermediate values (dlt, dlr, theta_t, theta_r, beta0, hstd, hsrd, hte, hre, hm, Lbulla, Ldsph, Lbs, Lba) | passes: every Lb within the 0.02 dB test tolerance (0.003 dB worst in the development check), intermediate values within 0.01 |
| test_prop_itu | CG-3M3J-13-ValEx-Rev8.3.0: P.676 specific attenuation at 12 / 20 / 60 / 90 / 130 GHz (oxygen and water vapour separately), P.676 Annex 1 slant path example (28 GHz, 30 deg: 0.4708 dB), P.838-3 kH kV aH aV and gamma at 14.25 GHz, P.618-14 rain at 0.001..1 % for three sites, P.618 scintillation | passes: P.676 within 1e-5 relative, the slant path within 0.003 dB, P.838 within 1e-6, P.618 rain and scintillation within 1e-4 relative |
| test_prop_hf | the C++ CCIR map evaluation against an independent evaluation written from the P.1239 text during development (0.01 MHz); the 3000 km MUF = foF2 x M(3000) (the definition of M(3000)); winter anomaly, solar-cycle and day/night behaviour, the dip equator near Huancayo, foE values, textbook band behaviour, skip zone, long-path method and its continuity across 7000-9000 km, the chart | passes (behaviour and port checks only, see below) |
| test_prop_sat | closed-form GEO look angles (London to 28.2 E: 25.4 deg, 145.6 deg, 39031 km), ISS horizon range rate 7.20 km/s and 3.5 kHz Doppler at 145.8 MHz, a Ku DVB-S2 budget against a hand calculation, the threshold table | passes |
| test_prop_model / scenario / terrain | the 3.9 regression suite (pinned to the 3.9 models, which stay selectable), plus the 4.0 defaults through evaluate(): the P.1546 100 MHz land band, 1 % time above 50 %, 90 % locations = 1.28 sigma, ADS-B to the radio horizon, HF via P.533; tile names / URL / .hgt.gz reader | passes; on a flat profile P.1812 at 100 MHz / 150 m sits below the empirical P.1546 curves beyond the horizon (pure spherical-earth diffraction), measured 83.0 / 50.3 / 26.7 / 12.6 dBuV/m at 10 / 50 / 100 / 200 km against about 85 / 57 / 40 / 20 for P.1546 (1 kW ERP, 50 % / 50 %): equal at 10 km, 7 to 13 dB lower beyond the horizon; the test band was widened to 20..50 dBuV/m at 100 km; a 350 m ridge on a 38 km 2 m path with 10 m antennas lowers the loss from 148 to 142 dB (obstacle gain: the path is already beyond the horizon), with 60 m / 30 m masts it blocks a line-of-sight path (more than 15 dB) |

One finding while validating: the first extraction of the P.676 Table 1 from the PDF lost the a6 value of the 118.75 GHz line; the ITU
workbook showed it at once (1 % off at 90 GHz, 5 % at 130 GHz). With the published row (a5 = -0.439, a6 = 0.079) all ten values agree to
better than 0.001 %.

Not validated against published reference values: the HF field strength and reliability (the ITU's P.533 reference program and its
D1 test data bank were not available), the satellite budget beyond the hand calculation, and anything on real off-air measurements.
The HF numbers are therefore "P.533 as implemented here", checked for internal consistency and textbook behaviour only.

Cost on a GitHub runner: an 80 x 80 map takes 0.04 s with P.1812 on a flat profile and 0.65 s with P.533 on HF (worker thread).

## 4. Screen (Tools > Propagation, Advanced)

- Inputs: Signal has **Will it decode?** (an OnAir mode and channel condition set the bandwidth and SNR); new **Statistics and climate**
  section (time %, locations %, location spread, climate presets for dN / N0 with the k-factor shown); Ionosphere has the sunspot number
  and the 3.9 HF model switch; Path model lists P.1812 (auto), free space, smooth earth, two-ray, the 3.9 model; Terrain has the opt-in
  **Download elevation tiles** (radius, the path to the chosen point, at most 9 tiles, cached in the data folder under terrain/).
- Results: Map, Link budget (P.1812 parts, HF reliability and decile MUFs), Sweeps (new: path loss against distance for five models),
  **Profile** (terrain, line of sight, first Fresnel zone, 60 % line, worst clearance), **HF hours x bands** (reliability heat map with the
  median SNR in each cell and the MUF by hour), **Satellite** (GEO or LEO, look angles, Doppler, P.618 / P.676 losses, G/T, C/N0, margin
  against a mode), Models.
- The map pin, the Simple screen and the verdict work as before; the verdict now names ducting, the flat-earth case and the HF reliability.

## 5. Decisions taken without the owner (asleep)

- Auto now means P.1812-8 (VHF and up) and P.533 (HF); the 3.9 models are kept as choices, and the old regression suite is pinned to them.
- Default time 50 %, locations 50 % (median); planning users set 90 % locations. Location spread 5.5 dB (P.1812 Table 6).
- No live solar data: the app fetches none today; SFI or the sunspot number are typed (as before).
- Tiles from the AWS Terrain Tiles bucket (open data, gzip SRTM 1"), kept at 3" to stay light on phones; only on a button press.
