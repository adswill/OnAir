# Ease of decoding (3.8): first-use help (branch feat38-ease1)

From scratchpad/smart37-backup/ease-of-decoding.md, items E3-E12, E14, E15. E1 was excluded by the owner; E13 is a fallback for E1 only and was not needed. Automatic actions: once per mode (prefs `firstStart.*`, `firstBand.*`, `navaidAutoFetched`), only with a real radio, an empty list, the default frequency, not while recording or transmitting; each says what it did and has a way back. Nothing here transmits.

| Item | What it does | Where |
|---|---|---|
| E3 | Meteor LRPT, Orbcomm, Argos, LoRa satellites, SSTV on the ISS frequency: next pass and "Follow the next pass" in the hint strip, orbits fetched/updated once a day while it shows, via the feat38-sattrack `PassRunner`. ISS SSTV is a pass link with `eventOnly` (never the default link of the ISS) | `app/ui/mode_hints.cpp`, `passesFamilyLine` |
| E4 | Geostationary modes: azimuth, elevation, LNB skew for your location, or "below your horizon"; a note in the mode browser (never greys the mode). Satellite positions are the published slots | `ease::geoSatsOf`, `modeGeoNote` |
| E5 | First FM / DAB Start scans (existing scan), tunes the strongest, plays the first audio service, with Stop and a way back | `app/ui/first_start.cpp` |
| E6 | First DVB-T, ATSC, ISDB-T, DTMB Start scans the country's plan (first planned band), plays the lowest LCN of the strongest multiplex | same |
| E7 | First Airband Start with a position loads the nearest airport with frequencies | `airportLoadNearest` |
| E8 | FT8/FT4/FT2/WSPR/JS8: a steady median DT corrects the slot clock (shown on the DT line); four silent slots trigger one wider slot search (not JS8). TX slots are untouched | `core/include/dect2/hfdig/clock_comp.h` |
| E9 | FT8, FT4, WSPR, JS8, PSK31, RTTY, CW first opening with a position picks a band below 0.85 MUF (prop model, mid-cycle solar flux) and says why | `firstBandPick` |
| E10 | Per-mode "silence is normal" line for burst and scheduled modes, with the time listened | `ease::quietNote`, `nextStepLine` |
| E11 | Antenna need line per mode from a data table and the quarter-wave sum, with a warning for the antenna stated in Setup; also in the mode browser | `ease::antennaNeed` |
| E12 | A GNSS fix held 15 s fills an unset station position (Undo); more than 5 km from a set one offers to use it | `gnssPositionTick` |
| E14 | VOR, NDB, DME: nearest stations from OurAirports `navaids.csv` (public domain, fetched once with a notice), one-click tune, best crossing pair for the VOR fix. The VOR station list picks up the same file when none was imported | `core/src/vor/navaid_list.cpp` |
| E15 | ADS-B aircraft "Listen on airband": nearest airport to the aircraft, Tower / Approach / Centre by height and distance (best guess, labelled) | `airbandListenAt`, `ease::listenTypes` |

## Not done, and limits

- E14: `navaids.csv` has no ILS: localizers stay on the VOR list import.
- E15 switches this receiver to Airband; a second radio running ADS-B has to stay in a second receiver window.
- E8: a wrong clock also means wrong TX slots; TX stays on the computer clock (nothing here touches TX). The DT line says so when the correction is in use.
- E9 uses a mid-cycle solar flux guess, not live data, for the first pick only.
- Elektro-L slots disagree between sources (76E, 14.5W, 165.8E): all three are listed.

## Tests

Pure logic: `tests/app/test_ease.cpp` (ctest `ease`), `tests/vor/test_vor_navaids.cpp`, `tests/geo/test_sat_plan.cpp` (event-only link). `tests/hfdig/test_hfdig_clockcomp.cpp` runs the real FT8 decoder with a simulated wrong clock (late stations, fast clock, right clock, and out by 4 s for the wider search). All signals are simulated; nothing here was checked on air. No GUI run of this branch (no full app build here, per the 3.8 rules): the integrator's hidden-mute GUI pass is still to do for the hint strip.
