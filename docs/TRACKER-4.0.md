# Satellite tracker redesign (4.0)

Owner order (2026-11-12): "make that way nicer UI-wise". Window: View > Passes. Branch `feat40-trackui`.
Code: `app/panels/passes_ui.cpp` (the screen), `core/include/dect2/geo/sat_plan.h` (one new tag), `tests/geo/test_sat_plan.cpp`.
The 3.8 rework (lists, planner, runner, filters) is in `docs/SATTRACK-3.8.md`; none of its logic changed here, only the screen.

## Before (3.9)

One header line (receiver position, orbit age with Update orbits, Advanced, full screen), then a row of group checkboxes with the peak
slider, the hearable tick, the antenna menu and a find box, then "Overhead now" buttons. Below, a pass table on the left (Satellite, Starts,
Peak with n/5, Length, Path, Signal, You get, Arm) and a narrow detail column on the right whose bottom held either a sky plot or a small
map (a Sky / Map radio switch), never both. The map showed only the chosen pass's track and footprint. The 24-hour timeline at the bottom.

## After (4.0)

- **Header**: as before, plus Show map / Hide map.
- **Left column (wide windows)**: a satellite search box, the groups as coloured chips (Weather, ISS, Orbcomm, Amateur, Globalstar, CubeSat
  and the new **Iridium**), and the list of every satellite of the shown groups that matches the search: a group-coloured dot, the name,
  and on the right its elevation now (green, above the horizon) or the time to its next pass. A tick puts a satellite on the map; a click
  shows its next pass. The chips are the same filter (and the same `passShow<g>` settings) as the old checkboxes, so they filter the pass
  list too.
- **World map (middle)**: the shared slippy map at world zoom, with the night side shaded, the terminator and the sub-solar point; for each
  satellite ticked in the list, the one picked and the one of the chosen pass: the last orbit (faint), the next orbit, the footprint circle
  and the current position with its name (hover: position, altitude, above your horizon); the chosen pass's stretch of ground track in the
  accent colour; the receiver. Orbits are worked out every 2 s. Default map satellites: ISS, Meteor-M N2-3 and N2-4.
- **Pass panel (right)**: the chosen pass, or the current / next listed one: name, a large monospaced countdown (AOS in, or LOS in while it
  is on), times, peak, length, direction, the quality badge, **Doppler** (live with its rate during the pass; at AOS and LOS before it), the
  downlink menu, what you get, the orbit's age at the pass with the timing warning, and one button **Track and record this pass** (the same
  arm as before: at AOS the runner tunes, sets the mode, follows the Doppler shift, records and decodes, then puts the receiver back;
  Cancel / Stop while armed or running, with the plan's status and conflicts). The polar **sky plot** sits next to it (or under it when
  the panel is narrow): elevation rings, compass points, the track with AOS, TCA (diamond, with its elevation) and LOS marked, and the
  satellite's place now during the pass.
- **Pass list**: Satellite (group dot; hover: what you get), Starts, Max el, Length, Direction, a **Quality** badge (poor / low / fair /
  good / excellent from the existing 1-5 rating, hover for the reason), Downlink, Arm / Cancel with the plan status. The next pass (the
  first listed one still to come or under way) is highlighted and its Starts cell counts down. Above it: peak slider, hearable tick, antenna
  menu and Overhead now.
- **Timeline**: unchanged.
- **Narrower windows**: under 1100 px the left column becomes a row (search box and chips) above the list; under 760 px the map is left
  out and the pass panel takes the full width.

## Every feature and setting, and where it is now

| Feature or setting (settings key) | 3.9 place | 4.0 place |
|---|---|---|
| Receiver position (shared location) | header | header |
| Orbit age, built-in set warning, Update orbits | header | header |
| Advanced page (TLE data, Orbit from Doppler, Follow and schedule, Finished passes) | header button | header button, page unchanged |
| Full screen (Esc leaves, `ONAIR_MAP_FULL` dev switch) | header | header |
| Group filters (`passShow0`..`passShow5`) | checkboxes | chips in the satellite column (or the row above the list); `passShow6` for Iridium |
| Peak elevation filter (`passMinEl`) | filter row | filter row above the list |
| Only what my setup can hear (`passOnlyHearable`) | filter row | filter row |
| Antenna band (`passAntenna`) | filter row | filter row |
| Find a satellite | filter row | search box in the satellite column (filters the satellites and the passes) |
| Overhead now | own row | row above the list |
| Pass table columns: satellite, starts, peak, length, path, signal, you get | table | Satellite, Starts, Max el, Length, Direction, Downlink; "you get" in the name tooltip and the pass panel |
| Pass quality 1-5 with its reason | "n/5" in the Peak column | Quality badge (hover: n/5 and the reason) |
| Arm / Cancel per pass, plan status, conflict tooltip | last column | last column |
| Chosen pass: times, peak, downlink menu, yields, orbit age, timing warning | detail column | pass panel |
| Live az / el / range and Doppler during a pass | detail column | pass panel (Doppler also before the pass, at AOS and LOS, and its rate) |
| AOS countdown | detail column | pass panel, large; and in the list for the next pass |
| Arm this pass / Cancel this pass / Stop this pass | detail column | pass panel ("Track and record this pass") |
| Antenna mount note | detail column | pass panel |
| Sky plot | detail column (Sky) | pass panel, always shown, with AOS / TCA / LOS marks |
| Map with the pass track and footprint (`passShowMap`) | detail column (Map) | the world map in the middle, always shown on wide enough windows; `passShowMap` is no longer read (both views show at once), `passBigMap` hides the map |
| Timeline (plan, skipped, others; click selects) | bottom | bottom |
| AOS reminder banner and log line (`passNotifyMin`) | everywhere | unchanged |
| Horizon mask (`passHorizon`), retired satellites (`passShowRetired`), take every listed pass (`passSched`), switch a running receiver (`passSchedTakeOver`), priorities (`passPrio<g>`), keep the SDR free (`passResv`, `passResvFrom`, `passResvTo`), follow a satellite, auto start / record, point the antenna mount | Advanced | Advanced, unchanged (an Iridium priority added) |
| TLE sources, Keplerpass, custom servers, paste, automatic update (`passTleAuto`, `passTleOk`, `passTleTry`), extra CubeSats (`passCubes`), Smart decoder additions (`csmUserTx`) | Advanced | Advanced, unchanged |
| Orbit from Doppler (`passDopMeasure`, `passUseDerived`) | Advanced | Advanced, unchanged |
| Armed queue across restarts (`passQueue`), finished passes (`passes_history.tsv`) | - | unchanged |
| A mode's one-click "follow the next pass" and next-pass line, the Argos and Globalstar Doppler overlays, the live map's satellites, What satellite is this? | other screens | unchanged |

New settings: `passBigMap` (map shown, default on), `passMapSats` (the satellites on the map, default `25544,57166,59051`).

## Iridium group

Celestrak's `iridium-NEXT` list is downloaded with the others (`iridium.txt`). Each satellite named `IRIDIUM ...` gets one downlink for the
Iridium burst decoder (mode family 19) at the mode's default frequency (1622 MHz unless the mode table says otherwise, 1610-1630 MHz).
The radio does not follow the Doppler shift for it (`radioFollowsDoppler` false, as for LoRa): the decoder takes bursts from many
satellites at once. The group is off by default (about 80 satellites) and the satellites are not added to the live map's list.
`SatTag` gained `TagIridium = 64` (`TagAll` 127); `test_sat_plan` checks the new kind filter.

## Tests and what was not checked

- `sat_plan` (Iridium tag) and the app build are run on GitHub `quick.yml` for this branch.
- The screen itself was not looked at: the Mac was on battery, so no local build or screenshot. To do on mains with a renamed app copy and
  isolated settings: wide, medium and narrow windows; with and without map tiles; a pass under way; Light and High contrast.
- No live pass was received in this work.
