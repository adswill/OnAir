# Smart features, round 5 (3.9): aviation, drones, radiosondes

Branch `feat39-smart5c`, on `integ-39`. The list and the checks against earlier rounds are in
`scratchpad/smart37-backup/r5-avia-marine-iot.md`. ADS-B manoeuvre alerts, the glider thermal map and HFDL propagation
are on the feat39-smart5b list and are not repeated here.

**All of this was tested on simulated data only.** No real recording of these situations was available. The VOR / ILS
check also runs through the real generator and receiver.

| # | Item | Where | What was measured |
|---|---|---|---|
| 1 | ADS-B atmosphere sounding | `core/include/dect2/adsb/adsb_atmos.h`, ADS-B > Atmosphere | Model atmosphere, 60 aircraft, temperature noise 0.5 / 1.5 / 3 C. Tropopause found within 2000 ft of the model's 36089 ft. Freezing level within 900 ft. Jet core within 2600 ft, 8 degrees and 100+ kt. FL200 wind within 8 kt. The 355/5 degree wrap averages to 0. |
| 2 | ADS-B altimetry cross-check | same header, Atmosphere view | 12 aircraft in warm, standard and cold air (slope fitted): the one aircraft 420 ft off is flagged once, the others never. A 600 ft jump of the whole band gives one "GNSS problem" and no aircraft is blamed. Two other aircraft are not enough to compare against. |
| 3 | Airport weather from ACARS / VDL2 / HFDL | `core/include/dect2/acars/acars_metar.h`, Weather view in all three modes | METAR, SPECI and TAF in European and US forms: VRB, MPS, CAVOK, 1 1/2SM, M02/M04, A2992, VV001, a TAF's first group only. Flight category edges. Junk around a report. A corrupted wind group drops only that group. The newest report per airport is kept. |
| 4 | Out-landing alert | `core/include/dect2/flarm/flarm_outland.h`, FLARM > Out-landings | Glider flights with 0 / 30 / 60 % packet loss and 3 / 15 m GNSS noise. One alert for a field landing 25 km from an airfield and one 8 km from it. No alert for a landing 0.3 km from an airfield, a 60 s hop, 15 s standing, or a ground-reporting tug. Two landings in a day give two alerts. Nearest airfield: the built-in airport table (all kinds, within 30 km). |
| 5 | Navaid signal check | `core/include/dect2/vor/navaid_check.h`, VOR and ILS > Signal check | Real generator to real receiver at 15 dB SNR with a 3 kHz offset; the full sweep (40/25/15 dB, 0 / 3 kHz) runs with `ONAIR_TEST_LONG=1`. VOR depths and FM deviation come out within limits. The localiser tones measure 20 +- 1.5 % on the course line, and the SDM is 40 % off it. A subcarrier 1.9 % high is marginal, 3.5 % high is out. Depth cases on telemetry cover out, within, marginal, glide path, marker and ident. |
| 6 | Drone airspace alerts | `core/include/dect2/rid/rid_alerts.h`, Remote ID > Alerts | Climb to 160 m gives one "high" alert, repeated after 10 minutes. A drone 3 km from an airport gives one alert. An aircraft passing 600 m away at 150 m above gives one alert, naming the aircraft. Aircraft 1.9 km away or 1300 m above give none, and neither do stale aircraft or an unknown drone altitude. |
| 7 | Sonde ascent sounding | `core/include/dect2/sonde/sonde_profile.h`, Radiosondes > Sounding | Flight to 30 km through a model wind and temperature field, 0 / 30 % frame loss, 3 / 10 m noise, thinned to one point per 6 s. 55+ layer winds, at most 2 off by more than 1 m/s. Freezing level within 250 m, tropopause within 700 m, burst within 60 m, climb rate within 0.2 m/s. |
| 8 | Landing prediction before burst | same header, Sounding view | Against the simulated flight's real landing. From 12 km: within 70 km (measured about 62 km off). From 20 km: within 26 km (about 23 km). From 28 km: within 3 km. The error comes from the winds above the highest layer measured so far, which are extrapolated; in this model they are weaker than the jet below. The burst height (default 30 km, a slider) moves the point. No prediction is given after burst (the existing descent prediction takes over), without sonde time stamps, or for a burst set below the sonde. |

## Limits and honest notes

- **Annex 10 limits are from memory** (Annex 10 Vol I is not freely available and was not checked). The limits are:
  - VOR: 30 Hz AM and 9960 Hz depth 28-32 %; subcarrier within 1 %; deviation 480 +- 30 Hz; ident up to 10 %.
  - Localiser: tones 18-22 %, SDM 36-44 %, ident 5-15 %.
  - Glide path: tones 37.5-42.5 %, SDM 75-85 %.
  - Marker: 91-99 %.
- The 30 Hz frequency is not checked, because the receiver does not measure it (it reports a fixed 30).
- The generator always sends nominal depths, so depths outside the limits are tested on telemetry only.
- Drone near aircraft needs live ADS-B telemetry at the same time as Remote ID. That means ADS-B as the second mode,
  which one radio cannot provide (1090 MHz and 2.4 GHz). With one radio, only the height and airport alerts work.
  Airports for that alert are large, medium or scheduled ones within 10 km.
- DJI DroneID and ADS-L / OGN are not wired to the drone alerts and out-landings yet. The core logic takes any fix.
- The atmosphere sounding needs Comm-B weather replies (4,4 / 5,0 / 6,0), which come only where a radar interrogates.
- The altimetry check needs TC 19 GNSS-baro differences from at least 4 aircraft in one 4000 ft band.
- Nothing here is kept between sessions.

## Tests

`adsb_atmos`, `acars_metar`, `flarm_outland`, `vor_navcheck`, `rid_alerts`, `sonde_profile`.
