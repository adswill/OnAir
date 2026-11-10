# Smart features in 3.9: radio astronomy and science

Three engines in `core/src/smart5`, using the shared Sun ephemeris (`sun.h`). Test: `tests/smart5/test_smart5_science.cpp`.

**Validated on simulated signals only.** Sun transits through Gaussian beams of 0.95, 3 and 8 deg with receiver gain drift and noise, Sun geometry for a real VLF path, and Poisson meteor counts with a daily curve, the Geminids and an injected outburst. No recording was on hand.

| # | Engine | Header | What it reports |
|---|---|---|---|
| 24 | Sun drift-scan calibration | `drift_scan.h` | From a fixed-dish total-power trace: the half-power beamwidth (drift rate 15.04 deg/h x cos(declination); the Sun's disk taken out as a Gaussian of 0.31 deg), the pointing error along the Sun's path as azimuth and elevation corrections, and the Sun-to-cold-sky ratio. With an expected Sun excess the error across the path is estimated from the peak's shortfall; otherwise it asks for a second scan at another hour. |
| 25 | VLF terminator marks | `vlf_terminator.h` | For a transmitter-receiver path, the sunrise and sunset times at the transmitter, the great-circle midpoint and the receiver, at the ground (-0.833 deg) and at the D region (85 km, Sun at -9.3 deg), and the share of the path's D region in sunlight at any time. |
| 26 | Meteor rate against the shower calendar | `meteor_rate.h` | Learns the station's sporadic curve per local hour and its sensitivity to shower meteors (counts per ZHR x sin(radiant elevation)); the expected count per hour; an hour above expected + 3 sigma + 2 and 1.5 x expected is flagged as a possible outburst. |

## Sources and limits

- Shower radiants, dates and ZHR: the IMO working list of visual meteor showers, rounded, written from memory; activity falls off as 10^(-B |days|) with B from 0.1 to 0.8. Good enough for an expected rate, not for shower association of single meteors.
- Day of year ignores leap days (one day off in leap years after February).
- The drift scan assumes a Gaussian main beam and the optical Sun's size; at low frequencies the radio Sun is larger, so the beamwidth reads slightly high below about 1 GHz.

Already present and not repeated: the hydrogen-line LSR and rotation curve, the riometer quiet-day curve and the meteor shower table in the meteor scatter mode (which has dates and rates but no radiants).
