# Round 5 (3.9): smart features, broadcast, TV, satellites, GNSS, science

Worker feat39-smart5a, base pub/integ-39. Owner pre-approved the whole list (2026-11-10). Offline only, no new deploys.

**Checked first.** MODES.md, docs/*-3.7.md, docs/*-3.8.md, broadcast.md, r2-broadcast.md, r3-broadcast.md, r4-per-mode.md,
satdump-parity.md, ease-of-decoding.md, then a case-insensitive search of core/src and app for every item. Already present, so not
proposed: TR 101 290 monitor (tv/tr101290), GNSS interference and spoofing monitor (gnss/gnss_interf), RAIM-style residual exclusion
(gnss_solve), Doppler satellite identification (satmisc/sat_id), pass planner and scheduler (geo/sat_plan, pass_sched), hydrogen LSR,
galactic coordinates and rotation curve (astro/hline_*), riometer quiet-day curve (hamsci), meteor shower table (mscatter), DRM AFS,
station book, Whole span and SDC combining, DAB census, TV echo advice, satellite from NIT, ATSC 3.0 AEAT, FM census, AF follow, FM DX log.

Each engine lives in core/src/smart5 (one header per item in core/include/dect2/smart5) and is fed from data the decoders already
produce (telemetry structs, per-carrier figures, lock logs), so it runs on its own without a button.

## Broadcast

| # | Mode | Feature | What it does, automatically |
|---|---|---|---|
| 1 | FM | Station watchdog | Flags a broken station before you notice: dead air (carrier fine, audio silent N s), pilot lost on a station that is normally stereo, RDS stuck (PS and RT frozen for hours on a station whose RT normally changes), RDS clock wrong (CT more than 90 s off the computer's clock, with the drift trend per PI), over-deviation (peak above 75 kHz more than 1 % of the time). |
| 2 | FM | MPX power per ITU-R BS.412 | The 60 s integrated multiplex power in dBr (0 dBr = a tone at 19 kHz deviation), the peak deviation percentiles (50/99/99.9 %) and the share of time above 75 kHz. Regulators' measurement, normally a dedicated analyser. |
| 3 | FM | Transmitter fingerprint | Learns each PI's usual pilot injection, pilot frequency error and RDS level; when the same PI arrives with a different signature it says "a different transmitter of this network" (useful for DX and for same-PI networks without SFN). |
| 4 | AM | Split-carrier (het) analyser | Finds every carrier in the channel to about 0.1 Hz with a long FFT and interpolation, gives each one's offset from the nominal channel, its level and drift, and the het tone you hear; carriers are named from a user carrier book (precise offsets are how MW DXers identify stations). |
| 5 | AM | Modulation health | Positive and negative modulation peaks, negative-peak clipping (over-modulation), carrier shift under modulation and dead air. |
| 6 | DAB | Ensemble change log | Every reconfiguration with time: service added or removed, label change, bitrate or protection change, sub-channel moved. |
| 7 | DAB, DVB, ATSC, ISDB, DTMB, DRM, CDR | Fault or reception? | When one service fails while the ensemble or mux and the other services are clean, it is the station's feed, not your reception; when all fail together it is reception. A per-service error tracker with the verdict and when it started. |
| 8 | HD Radio | Blend alignment meter | Cross-correlates the analogue FM audio with the HD main programme and reports the time offset in ms and the level difference in dB; a station whose blend is misaligned (echo or jump when the radio blends) is flagged. |
| 9 | HD Radio | Digital sideband level | Power of each digital sideband against the analogue carrier in dBc (-20, -14, -10 dBc are the NRSC-5 steps) and the lower/upper imbalance, from the spectrum. |
| 10 | DRM | Best-hour book | For every DRM station heard: decode success per UTC hour and per frequency, kept between sessions, and the next window where it decoded best. |

## TV

| # | Mode | Feature | What it does, automatically |
|---|---|---|---|
| 11 | DVB, ATSC, ISDB, DTMB (TS) | Service off-air detector | Per-PID bitrate tracking: a service whose video or audio PID stops, or drops to a small fraction of its usual rate, while the mux is fine is reported "off air at the source" with the time. |
| 12 | DVB-T/T2, ISDB-T, DTMB, DAB | OFDM interferer locator | From the per-carrier SNR, finds groups of bad carriers, gives their RF frequency and width and names the likely cause: a CW spur, an LTE/5G block (by width), an adjacent channel edge. |
| 13 | DVB-S/S2 | Rain-fade and margin log | Es/N0 against the QEF threshold of the MODCOD (EN 302 307-1 table 13) over time; fade events with depth and length; ACM MODCOD changes; margin left. |
| 14 | DVB-T/T2 | Transmitter from cell ID | Joins the TPS or L1 cell_id with the NIT cell list (descriptor 0x6C) and the user's own record, and names the transmitter. |
| 15 | Analog TV | VITS analyser | Reads the test lines (2T pulse, bar, multiburst, staircase) when present and reports K-rating, frequency response and differential gain. |

## Satellites

| # | Mode | Feature | What it does, automatically |
|---|---|---|---|
| 16 | Weather sats (LRPT, HRPT, APT, CubeSats) | Learned horizon and next good pass | Learns from every decoded pass the elevation, per 30-degree azimuth sector, where your station really locks; predicts the decoded minutes of the next passes from that and picks the best one to record. |
| 17 | GOES, GK-2A, MSG, Elektro, Inmarsat, any geostationary | Sun outage predictor | From your position, the satellite's longitude and the dish's beamwidth (dish size and frequency), the days and minutes when the Sun passes through the beam, so a dropout is not taken for a fault. |
| 18 | GOES, GK-2A, Elektro, FY-4 | Product cadence watchdog | Learns how often each product arrives (full disk every 10 or 15 min, mesoscale, text) and flags a missing one, telling "the satellite did not send it" (lock held, no frames) from "you lost lock". |
| 19 | Inmarsat, Iridium, Thuraya, L-band | Carrier quality log | Per carrier: SNR, decoded frames per minute and outages over time, with sun outage and elevation marks. |
| 20 | CubeSats | Beacon interval learner | Learns each satellite's beacon period and announces the next expected beacon; a missing beacon while the satellite is up is flagged. |

## GNSS and navigation

| # | Mode | Feature | What it does, automatically |
|---|---|---|---|
| 21 | GNSS | Radio clock from GNSS | The SDR's crystal error in ppm from the receiver clock drift, offered as the correction for every mode. |
| 22 | GNSS | Scintillation index S4 | Per satellite, the amplitude scintillation index from the signal level, with an ionospheric scintillation flag. |
| 23 | GNSS | Multipath indicator | Per satellite, the C/N0 against the elevation trend: oscillation means a reflection near the antenna. |

## Radio astronomy and science

| # | Mode | Feature | What it does, automatically |
|---|---|---|---|
| 24 | Radio astronomy | Sun drift-scan pointing calibration | Leave the dish fixed ahead of the Sun; the drift of the power is fitted with a Gaussian: beamwidth, pointing error in azimuth and elevation, and sun-to-cold-sky ratio. |
| 25 | SuperSID | Sunrise and sunset marks per path | The times the day-night terminator crosses the VLF path's transmitter, midpoint and receiver, so the diurnal steps are not taken for flares. |
| 26 | Meteor scatter | Hourly rate against the shower calendar | Hourly counts against the expected rate of the active shower, with the radiant's elevation, so a real outburst stands out. |

## Status (2026-11-10)

All 26 items are built in core/src/smart5. Tests: smart5_broadcast (1-10), smart5_tv (11-15), smart5_sat (16-20), smart5_science (21-26). All four passed on GitHub quick.yml run 42881695526 (Linux). Every test uses simulated signals only. Wiring into the mode screens is left to the integrator. Area docs: docs/SMART5-{broadcast,tv,sat,gnss,science}-3.9.md.
