# My location on every map (4.1)

Request: AIS, APRS and some other maps did not show the user's own position, so the distance to a received object could not be judged.

## What changed

- Every map screen draws the station position: a ring with a cross at the position set in Settings > Location (the one receiver position the ADS-B screen, passes and the Map already use).
- Optional range rings around it. Settings > Location: `Show my location on maps` (on by default), `Range rings around my location` (off), `Range ring distances` (Advanced, default `50,100,200`, up to 8 values, in the distance unit of the app: nm or km as chosen for ADS-B).
- Distance and bearing from my position:
  - while the pointer is over any map, a small label in the top right corner shows the distance and bearing to the point under the pointer;
  - the AIS vessel panel has a `from me` row; the mesh and APRS tooltips, the DMR radio list and the selected Remote ID drone carry the same text;
  - screens that already printed a distance or bearing (ADS-B, HFDL, VDL2, sondes, Horus, FLARM, Map, packet) keep their own lines.
- No position set: nothing is drawn except a small hint in the top left corner of the map, "My location not set - click to set it". A click opens Settings on the Location page.

## How it is built

- `core/include/dect2/geo/mylocation.h`: distance (haversine, mean Earth radius), initial bearing, destination point, ring polyline, ring list parsing, number and bearing formatting. Header only, no dependencies.
- `app/map/my_location.{h,cpp}`: the drawing and the settings. One call, `mylocmap::overlay()`, is made at the end of `adsbmap::draw()`, which every map screen already calls, so a new map gets the marker without extra code. `mylocmap::infoLine()` and `mylocmap::distanceBearingText()` give the text for cards and tooltips.
- Rings are drawn as polylines of points at the ring distance, so they stay correct at any zoom and latitude, across the antimeridian (segments that would cross the whole map are skipped) and near the poles.
- Where a screen draws its own home marker as well, the two sit at the same spot.

## Tests (not yet run)

`tests/geo/test_mylocation.cpp`, ctest name `mylocation`:

- distance for known pairs (equator degree, London-Paris, Dubai-Vilnius, New York-Los Angeles), symmetry, antipodes, pole to pole;
- the antimeridian (short way round, longitudes beyond 180, bearing east and west across it);
- near and over the poles (bearing to a pole, identical points, all longitudes meeting at a pole);
- bearing and destination round trip over 200 pairs, compass names, formatting in km and nm;
- ring geometry: every point at the radius, closed, starts north, across the antimeridian, round a pole, radius capped at the antipode, bad input, ring list parsing, unit conversion;
- a source check: every map screen draws through `adsbmap::draw()`, no other file projects the map itself, `adsbmap::draw()` calls the overlay and has no early return, the helper uses the one receiver location, the app unit and the three settings, the settings are in the table, the AIS, mesh, DMR and Remote ID screens use the helper, and the file is in CMakeLists.

Validated by calculation and source reading only. Nothing was compiled or run on a screen.

## Map screens covered

All callers of `adsbmap::draw()`: ADS-B, ADS-L, Mode A/C, FLARM, ACARS, VDL2, VDL4, HFDL, Aero, Iridium, Orbcomm, Inmarsat, AIS, AIS satellite, marine, sondes, Horus, packet/APRS, mesh, DMR, D-STAR, M17, QSO maps, ham map, HF beacons, propagation, lightning, passes, GNSS, drive navigation, cell, NR, Remote ID, RC link, SAR beacons, SAME, ASTERIX, VOR, DAB TII, JS8, the general Map.
