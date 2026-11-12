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

(Filled in step by step below as each part lands.)
