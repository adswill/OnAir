# Smart features in 3.9: satellites

Five engines in `core/src/smart5`, plus a small Sun ephemeris (`sun.h`) shared with the science items. Test: `tests/smart5/test_smart5_sat.cpp`.

**Validated on simulated signals only.** Synthetic pass tracks with a blocked sector, Sun geometry checked against an independent computation, product and beacon timings with jitter and misses. No recording was on hand.

| # | Engine | Header | What it reports |
|---|---|---|---|
| 16 | Learned horizon and next good pass | `horizon_learn.h` | Per 30 deg azimuth sector, the elevation from which frames really arrive (learned from every pass; 10 deg until a sector has a minute of samples); the decoded minutes each coming pass will give and the best one to record. Saved between sessions. |
| 17 | Sun outage predictor | `sun_outage.h` | From the site, the geostationary satellite's longitude and the dish's beamwidth (70 lambda / D), every day and minute when the Sun is within half the beamwidth plus its radius of the satellite; `inSunOutage()` for the decoders' dropout reason. |
| 18 | Product cadence watchdog | `product_cadence.h` | Learns each product's period from its arrivals (median interval, missed products divided out); an overdue product is "not sent by the satellite" when lock held through its window, "you lost lock" otherwise. GOES, GK-2A, Elektro, FY-4 and any file-based downlink. |
| 19 | L-band carrier quality log | `carrier_log.h` | Per carrier, SNR and decoded frames per minute, availability, and each outage with its reason: sun outage, satellite low (under 10 deg), signal faded (6 dB down) or unexplained. Inmarsat, Iridium, Thuraya. |
| 20 | CubeSat beacon interval learner | `beacon_learn.h` | Period and jitter from the beacons heard over several passes with 40 % lost (intervals counted as whole periods, least squares); the next beacon's time; a beacon missed while the satellite is up is counted and flagged. |

## Sun position

`sun.h` uses the Astronomical Almanac low-precision formulae (mean anomaly, mean longitude, ecliptic longitude with the two-term equation of centre, obliquity, GMST), good to about 0.01 deg from 1950 to 2050. The test checks the 2026 March equinox (declination under 0.1 deg), the June solstice (23.44 deg) and the Greenwich solar noon elevation on that day.

The sun outage test checks two cases: an observer on the equator under the satellite gets outages at local noon within days of the equinox; from Vilnius towards 9 deg E with a 0.8 m dish at 11.7 GHz every outage has the Sun within the beam of the satellite's topocentric direction, in spring and in autumn.
