# Orbit refinement from measured Doppler

The Passes window has an **Orbit from Doppler** section. During a tracked pass it follows the satellite's carrier in the spectrum, then fits small corrections to the satellite's two-line elements so the predicted Doppler curve matches what was measured.

## How to use it

1. Arm a pass in the Passes window (or Follow it under Advanced) with a satellite whose nominal downlink frequency is known (a beacon, NOAA APT 137.x, Meteor, ISS 145.800 / 437.800, an amateur satellite). The receiver must run, with the carrier inside the spectrum.
2. Tick **Measure the carrier during tracked passes**. About once a second, between 3 degrees elevation and LOS, the strongest narrow peak within the gate (default +-4 kHz around the predicted carrier) is recorded as one measurement. A peak must stand at least 8 dB above the median of the gate.
3. After the pass press **Fit orbit**. The plot shows the measurements (green), the curve the downloaded elements predicted (grey), the refined curve (blue) and the residuals of the refined curve (amber).
4. **Save refined TLE** appends the elements to `derived_doppler.tle` in the TLE folder. The file starts each entry with a comment saying it is derived from a measured Doppler curve and the name ends in "(derived)". The downloaded TLE files are never changed. Tick **Use the derived elements for predictions** to let the pass planner use them (the last saved entry of a satellite wins).

## What is fitted

Core code: `core/src/geo/doppler_fit.cpp` (`fitDopplerOrbit`), using the repository's own SGP4 (`core/src/geo/passes.cpp`).

* Corrections to mean motion, mean anomaly, inclination and RAAN, plus one constant frequency offset (the receiver's reference error).
* Damped Gauss-Newton (Levenberg-Marquardt) with numerical derivatives of the SGP4 Doppler. Each correction has a prior of the size a fresh TLE is typically off (mean anomaly 0.6 deg, mean motion 0.002 rev/day, inclination 0.03 deg, RAAN 0.05 deg), so one pass cannot throw an element far from its published value.
* Outliers (carrier lost, interference) are dropped against the robust scatter of the residuals and the fit is repeated.
* The estimated timing error is the 1-sigma uncertainty of the along-track position at the pass, in seconds of orbital motion, from the covariance of the fit, scaled up when the residuals are larger than assumed. It is shown next to the figure the unrefined elements were assumed to have.

## Limits

* **A good frequency reference is needed.** A constant error of the receiver's clock is absorbed by the offset parameter, but a drifting reference looks like a change of orbit. Use a GPS-disciplined or well calibrated receiver, and check the fitted offset: a value of several kHz means the nominal frequency was wrong or another signal was tracked.
* One pass constrains mostly the along-track position (timing) and, weakly, the cross-track geometry. Inclination and RAAN corrections stay close to zero unless the pass is long and clean. The refined set is meant for predicting the next passes of the same satellite from the same place in the following days, not as a replacement catalogue entry. Drag (B*) and eccentricity are not fitted.
* The result is **derived data**, not an official element set, and is marked as such wherever it is saved. Do not publish it as a TLE of the satellite.
* The receiver is assumed to be tuned to `Frequency` in the main window; if the receiver is in the mode where only the decode offset moves (a sticky LO), the measured frequency is wrong by that offset.
* Only low earth orbits are handled (the same limit as the pass planner).

## Tests

`tests/geo/test_doppler_fit.cpp` (ctest name `doppler_fit`) makes a Doppler curve from a perturbed orbit (0.35 deg mean anomaly, 0.0012 rev/day, 0.03 deg RAAN, 0.01 deg inclination) with a 600 Hz receiver offset, fits it from the unperturbed elements and checks that the timing error of the prediction falls from 29 s to under a second and the offset is recovered; also with 10 and 40 Hz noise and 3 kHz outliers, a null case (no perturbation), too few samples, the TLE formatter round trip and the peak tracker on a synthetic carrier.
