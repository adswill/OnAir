# Satellite tracker rework (3.8)

Owner order: "we really need to improve the tracker, it could use a rework". Branch `feat38-sattrack`, from v3.7.0. Window: View > Passes.
Code: `app/panels/passes_ui.cpp` (screen), `core/src/geo/sat_plan.cpp` (`core/include/dect2/geo/sat_plan.h`: filters, orbit age, the pass runner), `tests/geo/test_sat_plan.cpp`.

## Problems found in 3.7

Walked through the question "what can I hear from my location next, and how do I get it?" in an isolated copy of the app with current Celestrak elements for Vilnius, then read the code. Line numbers are v3.7.0 `app/panels/passes_ui.cpp`.

| # | Problem | Evidence |
|---|---|---|
| 1 | The list answers the wrong question: Globalstar and Orbcomm filled the screen, and it held nothing you could plainly use. | Screenshot of the walkthrough: of the first ~40 rows nearly all Globalstar and Orbcomm, by default on. |
| 2 | Three satellites decommissioned in 2025 were listed as receivable. | NOAA 15, 18, 19 in `kKnown` (lines 56-65); [NOAA notice](https://www.nesdis.noaa.gov/news/suspension-of-poes-data-users-after-june-16-2025-and-decommissioning), [OSPO status](https://www.ospo.noaa.gov/Operations/POES/status.html). |
| 3 | One pass appeared as several rows, one per downlink, and those rows conflicted with each other in the scheduler ("skipped: higher priority" against itself). | `kKnown` lines 57-77 are per link; `replan` line 398. |
| 4 | The tracker had its own short list: 3 amateur satellites, while the modes' CubeSat table knows 430. | `loadCubes` line 165 vs `cubesat_db`. No shared list. |
| 5 | The minimum elevation cut the pass itself: AOS and LOS were when the satellite crossed that elevation, so recording lost the start and end. The test `test_sat_plan` shows the cut pass is shorter than the real one. | `compute` line 331 passes `S.minEl` to `findPasses` (its `minEl` is the crossing level). SatDump and Gpredict use this as a filter on the peak. |
| 6 | A receiver the user was using was retuned anyway. With "Schedule passes by itself" and FM running, FM was moved to the satellite frequency once a second and the IQ recorded, despite the tooltip saying it is left alone. | Line 490 logs "not changed", then lines 493 and 501-505 record and retune regardless of `weStarted`. |
| 7 | Only one pass could be armed, and a new TLE download silently cancelled it. | `S.once` single flag; line 462: match by `fabs(aos - selAos) < 1`, else line 463 `disarmOnce`. Moving the min el slider did the same. |
| 8 | The radio was retuned every second during a pass whether or not the frequency had moved, each retune a hardware glitch. | Line 501: `now - S.lastTune >= 1.0` with no threshold. |
| 9 | After a scheduled pass the app stayed on the satellite's mode and frequency, instead of putting back what the user had. | `endPass` lines 373-390 restores only the nominal frequency. |
| 10 | No offline fallback in the planner, although the app ships a TLE snapshot. No TLEs meant "No TLEs yet". The age was one line inside a collapsed section and warned only after 14 days. | `loadTles` lines 240-250; `tleSection` line 658. |
| 11 | No filter for what your radio or antenna can receive: 1.7 GHz HRPT rows were shown to a 137 MHz antenna user. No map or footprint in the window. | `drawWindow` filters lines 912-922. Map only under View > Map. |
| 12 | "What would I get" was not shown: only a mode name such as `Globalstar` or `L-band`, and Meteor-M N2-3 had the wrong frequency. | Table lines 959-960. `kKnown` line 71 had N2-3 at 137.100; [SatDump's list](https://satdump.org/Satellite-List) has 137.9. |
| 13 | The prediction ran on the UI thread (every 10 minutes, for the whole list). | `compute` line 331 in the frame loop. |
| 14 | The screen had TLE data, Orbit from Doppler and the scheduler above the table, pushing the list down. | Walkthrough screenshot; lines 910-923. |

Compared with Gpredict (lists, map, polar view, Hamlib rigs and rotors), SatDump's autotrack (scheduling, record and live decode, minimum elevation at TCA), Look4Sat (list of active and upcoming passes, polar view, map with footprint and ground track, up to a week ahead), N2YO (category lists and live map with footprint) and Orbitron (rotor and radio drivers). CubicSDR has no tracker: it is a tuner, so it sets no expectation here. All of it is OnAir's own code; nothing is copied from those programs, so THIRD_PARTY.md gained no entry.

## What changed

**One screen.** Overhead now on top, then every upcoming pass for the receiver position (the shared location setting) for two days. Each row shows satellite, when, peak, length, path (rise > set), the signal, what you get, and Arm. Right: the chosen pass with its downlink, orbit age, one big button, and a Sky / Map switch (map with the ground track and footprint, centred on the receiver). Bottom: 24-hour timeline with the plan, skipped passes and the rest of the list, click to select. Manual settings (Follow, schedule, priorities, horizon mask, retired satellites, TLE servers and paste, Orbit from Doppler, finished passes) are behind **Advanced**.

**Shared list.** One satellite per NORAD number with all its downlinks: this file's weather and ISS rows, the CubeSat table (`cubesat_db`) and the Smart decoder's user additions, the LoRa presets, Orbcomm, Globalstar. No new database. The default downlink is chosen by `bestLink` (hearable first, then decoded, then the easiest band), and a dedicated mode beats the SatNOGS row of the same downlink. The live map still shows only the short list.

**Filters.** Weather, ISS, Amateur, CubeSat, Orbcomm, Globalstar; a peak elevation filter (now a filter on the peak: a listed pass is received horizon to horizon, with a horizon mask under Advanced); "Only what my setup can hear" (the selected radio's range and sample rate, and an antenna band menu); find by name. Retired satellites hidden. Nothing is blocked: untick to arm anything.

**Arm and queue.** Arm a pass with one click; as many as you like, kept across restarts. The plan is `planPasses` as before, with armed passes winning any group priority. A trimmed or skipped pass says so in the row (hover for who it conflicts with). `PassRunner` runs a slot: 20 s before AOS it sets the mode and tunes with Doppler, starts recording at AOS, follows Doppler through the same `engine.retune` path with a 150 Hz dead band (LoRa is left to its decoder as before), stops at LOS and restores the receiver's mode and frequency. An armed pass takes over a running receiver and puts it back; a pass taken by "Take every listed pass" leaves a running receiver alone unless Advanced says otherwise. If the user changes the mode mid-pass, the runner lets go. A transmission is never interrupted: the pass is reported as skipped. New elements no longer lose an armed pass: it is matched by culmination within 5 minutes. Results go to `passes_history.tsv` in the data folder and Advanced > Finished passes.

**Orbits.** Built-in snapshot first, then downloads and pasted sets (newest epoch wins; pasted and derived still override), plus the Celestrak `cubesat` group. Age is on the header line and per pass; from 3 days it shows an estimated time error, and the pass can still be armed. Update once a day while the window is open or a pass is armed.

**Doppler Orbit, rotator, automations, gallery.** The Orbit from Doppler section, antenna mount (now follows the run), `satellite.aos` / `satellite.los` events and the gallery's pass labels are reused as they were.

## Tests

`tests/geo/test_sat_plan.cpp` (ctest `sat_plan`, registered with the others):
- SGP4 against the published 00005 vector at 360 minutes; the wider Vallado check stays in `test_passes`.
- Pass search against a one-second brute force at Vilnius, Tromso, Svalbard, 89.5 N, McMurdo and the equator for a polar orbit: every pass of 40 s or more found within 2 s.
- Peak filter keeps horizon-to-horizon passes.
- Filters, antenna and radio limits, retired.
- Stale, expired and future-epoch TLEs; merge keeps the newest.
- Conflict resolution with an armed pass; AOS/LOS and reservations over midnight.
- Runner: full simulated pass (warm-up, tune, Doppler, record, restore), takeover, busy skip, transmit skip, same-mode, user change mid-pass, LoRa, cancel, no re-run, failed start.

Each key behaviour was also checked by mutation: nine deliberate breaks of the logic (peak filter, dead band, restore, transmit, busy, priority, coarse step, newest merge, stale class) each made the test fail. `passes`, `pass_sched` and `doppler_fit` still pass.

GUI: run hidden and muted from a renamed copy with `DECT2_DATA_DIR` and `HOME` isolated. The screen, filters, Arm (queue written and "planned"), Sky / Map switch and timeline were checked this way, and a hidden real-time run past an actual AOS.

## Not tested

No live pass on real hardware: no antenna was pointed at a satellite in this work, so reception, decoding and the actual Doppler tracking accuracy at AOS are not verified. The runner's receiver control is proven against a fake host (the unit test) and the app's wiring only with whatever the isolated run's default source is. The Meteor frequencies are from SatDump's list and may move (see the list's own note). `Arktika-M` stays unusable in the tracker: `Sgp4` is near-earth only and reports "orbit not usable".

## Decisions for the owner

- Cubesats default off (over 400 satellites otherwise swamp the list); the filter is one tick.
- Retired NOAA satellites are hidden by default.
- A pass with under 2 minutes left is reported as too short, as the scheduler's existing `minUsefulSec`.
