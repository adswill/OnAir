# Radiosonde landing prediction 4.0

Core: `core/include/dect2/sonde/sonde_predict.h`, `core/src/sonde/sonde_predict.cpp`, `core/src/sonde/sonde_predict_net.cpp`.
App: `app/sonde/sonde_predict_ui.cpp` (wired into `app/sonde/sonde_ui.cpp`). Tests: `tests/sonde/test_sonde_predict*.cpp`.

The 3.9 estimate (`sonde_land.h`, still there for its callers and tests) held the last horizontal drift, an exponential atmosphere
and a ground at sea level. 4.0 replaces it in the app. It is on by default and can be turned off under Advanced.

## What goes into a prediction

1. **The ascent's own winds.** Each sonde has a tracker fed with every frame at the full rate. The receiver's own track is thinned
   to 600 points, so it seeds the tracker only when the sonde was heard before the tracker existed. GPS glitches are thrown out
   first: any jump faster than 150 m/s across the ground or 100 m/s up or down, plus one pass of outlier removal in every layer. The
   ascent is then cut into 200 m layers. The wind of a layer is the least squares slope of position against time inside it,
   widened to at least 30 s, with a light 1-2-1 smoothing across layers. The error of each layer's wind grows by 1 m/s per hour
   since it was measured (Advanced: "wind change since ascent") and by 2 m/s per 100 km from where it was measured.
2. **A public wind forecast** for the layers the ascent did not measure: above the highest point heard, or every layer when the
   ascent was missed. It is weighted against the ascent wind by inverse variance (forecast 2.5 m/s). Sources (Advanced, "wind
   forecast"):
   - **Open-Meteo GFS** (default). Pressure-level winds and geopotential heights from 1000 to 10 hPa, 28 levels, at up to three
     points: the sonde, the midpoint and the first-guess landing, rounded to the 0.25 degree GFS grid. It asks for one hour back and
     six hours ahead. Each answer is cached in memory and as a file in `<data>/sonde-wind/`, keyed by the points and the 3-hour slot.
     There is at most one request every 2 minutes, in a background thread.
   - **The SondeHub prediction (Tawhiri)**, when the existing "Online flight prediction" is on. Each step of its descent path
     becomes a wind (displacement over time, at the step's mean height).
   - **None.**
   - NOAA GFS GRIB2 straight from NOMADS was not done. A GRIB2 decoder with complex packing is a large piece of code, and Open-Meteo
     serves the same GFS 0.25 degree data as JSON for only the points that are needed.
3. **The descent so far.** The sea-level fall rate K is fitted to the measured vertical speed, with v = K sqrt(rho0 / rho(h)):
   - The density profile is built from the sonde's own temperatures (hydrostatic, in 250 m layers), and ISA where it sent none.
   - The fit is weighted least squares with exponential forgetting (300 s), a ridge-limited linear change of K with height (at most
     5 % per km, 25 % in all) and one robust pass. The first 30 s after the burst are skipped. It is refitted on every update.
   - A burst with no parachute is the same model with a large K and is flagged when K is above 11 m/s.
   - The descent's own layer winds in the 3 km already passed are compared with the prior (ascent and forecast) there. That
     difference, the change of the wind since the ascent, is carried down and fades over 3 km.
4. **The final approach.** A Kalman filter (constant velocity) on east, north and height runs over the newest fixes. Its process
   noise is set so the velocity follows changes with a time constant of window / 5. The window is 60 s by default below 2 km above the ground
   (Advanced 30 to 120 s), 180 s higher up. It has a 5-sigma innovation gate against glitches. The difference between the filtered
   drift now and the prior at the sonde's height replaces the prior in the layers just below and fades with depth (600 m by
   default, Advanced "drift reach below").
5. **The ground under the predicted point.** The descent is integrated step by step (5 s, then 2 s below 1.5 km above the ground,
   then 1 s below 200 m) and stops where it crosses the terrain model. This is the iteration "the point moves as the ground height
   changes", done exactly. Terrain:
   - SRTM tiles (Terrain Tiles on AWS, the same tiles and folder as the propagation tool), loaded from disk.
   - Downloaded on demand when allowed, 1 x 1 degree, 10 to 25 MB each, kept at 3 arc-seconds.
   - Without a tile, the fixed ground value is used.
   - Copernicus GLO-30 was not used: its tiles are DEFLATE-compressed GeoTIFF, and the app's reader takes only uncompressed ones.
   - RS41, RS92, M10 and MRZ send heights above the WGS 84 ellipsoid, while the terrain is above the geoid. For those types the EGM96
     geoid height (a 2 degree table, `core/src/sonde/egm96_2deg.inc`) is added to the terrain. The geoid is -106 to +82 m around the
     world, so without this step the ground would be wrong by tens of metres.
6. **The uncertainty.** A Monte Carlo ensemble (200 members by default) varies:
   - the fall rate (log-normal, the fit's own error plus 2 %),
   - a wind error correlated over 1.5 km in height (the drift filter's error at the sonde, the prior's error below),
   - the start position,
   - the ground (8 m with terrain, 20 m without).

   The map shows the members as dots, the nominal ground track, the 50 % and 95 % circles and the 95 % ellipse, and the time to
   landing. Below the sonde's details it shows "landing spread" and "ground there". It updates about once a second for the selected
   sonde. Other sondes get the nominal point only, every 3 s.

## Network and privacy

Nothing is sent unless **"Download terrain and wind forecasts for the landing prediction"** (map, "me / prediction") is on. It is off
by default. Offline mode (Settings) overrides it. When the switch is on:

- **Open-Meteo** receives up to three points rounded to 0.25 degrees and an hour.
- **The terrain server** receives the names of the 1 degree tiles.

Terrain tiles and forecast files already on disk are used either way. The Tawhiri source uses only what the existing online
prediction (its own switch) has already fetched.

## Accuracy

Measured on GitHub. The simulated tables are from run 43198880298, `ctest sonde_predict_harness`, with `ONAIR_SONDE_TUNE=1` for the
tuning sweep.

**Height** below means the sonde's height above the ground when the prediction was made. **Last frame** is the prediction made at
the last frame heard. Every error is the distance to the true landing.

### Simulated flights

There are 137 simulated flights, run by `tests/sonde/sonde_predict_sim.h`. The truth deliberately does not use the predictor's own
model:

- non-ISA temperatures (plus or minus 15 K, lapse rate 5.5 to 7.5 K/km, tropopause at 9.5 to 16 km),
- parachute drag that changes by up to 8 % with height,
- a boundary layer with veer, shear, a 10 to 65 m/s jet stream, stratospheric wind and fine structure of 0.4 to 3 km,
- wind that changes by 0.2 to 1.2 m/s per hour in four height patterns and by 2 m/s per 100 km,
- gusts (0.5 to 2 m/s, 10 to 60 s),
- a swing of 1 to 8 m, GPS noise of 1.5 to 4 m horizontal and 3 to 6 m vertical, GPS glitches of kilometres, two dropouts of 20 to
  120 s and 3 % lost frames,
- hills from 0 to 960 m.

The flights fall into five groups:

| Group | Flights | Sonde last heard at |
|---|---|---|
| Normal | 60 | 150 to 1200 m above the ground |
| Heard nearly to the ground | 25 | 20 to 120 m |
| Fast low-level wind change (front) | 20 | as normal |
| Ascent missed | 20 | first heard 8 to 16 km up on the way down |
| No parachute | 12 | as normal |

The predictor is given a terrain model equal to the truth plus a 5 m error. The forecast is the truth without its fine structure,
plus a 2 m/s error correlated over 2 km.

Median error / 90th percentile, metres. The number in brackets is how many flights were evaluated at that height.

| Predictor | 10 km | 5 km | 2 km | 1 km | 500 m | 200 m | last frame |
|---|---|---|---|---|---|---|---|
| old, as the 3.9 app used it (ground at sea level) | 17630 / 32512 | 3914 / 8772 | 1748 / 3363 | 1387 / 3024 | 1231 / 2433 | 946 / 1827 | 1146 / 2529 |
| old, with the true ground | 14048 / 27905 | 2953 / 6154 | 1092 / 2224 | 668 / 1442 | 308 / 971 | 213 / 483 | 366 / 1040 |
| new, offline (ascent, fit, drift filter, terrain) | 2065 / 13437 | 1268 / 3299 | 477 / 1329 | 243 / 602 | 112 / 237 | 46 / 110 | 116 / 380 |
| new + forecast | 1215 / 2604 | 876 / 1609 | 402 / 802 | 250 / 566 | 151 / 253 | 51 / 117 | 121 / 406 |
| flights | (131) | (136) | (136) | (119) | (62) | (28) | (137) |
| new + forecast: truth inside r50 / r95 | 66 % / 98 % | 71 % / 99 % | 82 % / 99 % | 69 % / 100 % | 60 % / 100 % | 57 % / 96 % | 66 % / 99 % |
| new + forecast: median r50 / r95 (m) | 1706 / 3638 | 1220 / 2500 | 640 / 1321 | 345 / 750 | 173 / 355 | 58 / 135 | 187 / 377 |

What the table shows:

- The forecast matters most high up (10 km: 2.1 km to 1.2 km) and when the ascent was missed. Low down it adds nothing, because the
  measured drift wins there.
- The r50 and r95 radii are slightly wide, not narrow: they hold 57 to 82 % and 96 to 100 % of the landings.
- The last-frame error is mostly the unmeasured wind between where the sonde was lost (up to 1.2 km) and the ground.

Tuning: the sweep covered drift reach 300 to 3000 m, wind change 0.5 to 2 m/s per hour and final-approach windows of 30 to 120 s.
At the last frame the medians stayed between 144 and 186 m. The defaults chosen are 600 m, 1 m/s per hour and 60 s.

### Real flights

The real-data check uses three RS41 flights from the SondeHub telemetry API, 2026-11-09 to 11 (`ctest sonde_predict_real`, in the
same run). All three sondes kept transmitting after they landed, so the landing point is known to a few metres, and so is the
ground height in the sonde's own altitude reference. The check is offline only: the ascent winds, the fit and the drift filter, with
no forecast. Old means 3.9 with the true ground and a 4 s velocity.

Error in metres, old / new:

| Flight | 10 km | 5 km | 2 km | 1 km | 500 m | 200 m | last frame (height above ground) |
|---|---|---|---|---|---|---|---|
| Y3432290 | 23338 / 3478 | 9785 / 1518 | 2987 / 846 | 617 / 199 | 313 / 219 | 59 / 39 | 37 / 12 (90 m) |
| Y2212795 | 13240 / 926 | 1943 / 607 | 846 / 497 | 349 / 314 | 76 / 202 | 39 / 25 | - / 27 (67 m) |
| X2652450 | 41845 / 3225 | 10773 / 2038 | 4600 / 598 | 777 / 773 | 1139 / 580 | 44 / 98 | 7 / 26 (29 m) |

What the real flights show:

- From 10 km down to 2 km the new predictor is 4 to 15 times closer.
- From 200 m and at the last frame both are within tens of metres. The old one is sometimes better there: a few seconds of
  straight-line drift is hard to beat.
- The real 500 m errors (200 to 580 m) are larger than the simulated median. Three flights is not a statistic.

### About the 20 m goal

The 20 m goal is **not reached in general**. The new predictor got within about 12 to 30 m only when the sonde was heard below
100 m (real flights). On simulated flights at 200 m the median error is 46 m, and it is about 120 m from a sonde lost 150 to 1200 m
up. Below roughly 1 km the limit is the wind of the last few hundred metres, which nothing measures before the sonde gets there:
gusts, the boundary layer and the time since the ascent.

The remaining levers are reception closer to the ground (a chase receiver) and a high-resolution model of the boundary layer. The
GFS levels are too coarse for that. Everything here is validated on simulated flights, plus three real flights without a forecast.


## Credits and data licences

- **Open-Meteo:** the API and the saved test sample `tests/data/sonde/predict/openmeteo_gfs_sample.json` are CC BY 4.0, "Weather data
  by Open-Meteo.com".
- **NOAA GFS:** public domain.
- **Terrain Tiles** (Mapzen / Linux Foundation, AWS Open Data; mostly SRTM, NASA/USGS, public domain): see the joerd attribution list.
- **NGA EGM96:** public domain.
- **SondeHub telemetry:** the three test flights in `tests/data/sonde/predict/sh_*.csv` are CC BY-SA 2.0.

All of them are listed in `THIRD_PARTY.md` and in the in-app Credits.
