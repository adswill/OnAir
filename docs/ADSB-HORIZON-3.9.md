# ADS-B theoretical range outline (3.9)

Round-4 item 16 (scratchpad r4-per-mode list; tar1090 + heywhatsthat). Branch feat39-r4.

- **ADS-B > Display > Theoretical range**:
  - **Import upintheair.json...** reads the file heywhatsthat.com makes for a panorama (the one tar1090 uses). OnAir only reads the file; download it in your browser.
  - **Work out from elevation files** uses the SRTM / GeoTIFF files already added in the propagation tool (`prop_terrain`) and the antenna position. It gives rings for 10 000, 20 000 and 40 000 ft. The antenna height above ground can be set (default 10 m).
- The rings are drawn dashed on the map, labelled with their altitude, next to the actual range outline. The choice and the file are saved.
- Method for the worked-out rings: for each of 360 bearings, step out 0.5 km at a time up to 500 km. The highest terrain angle seen from the antenna is kept, with a 4/3 earth radius for refraction. An aircraft at a given altitude is visible until its angle falls below that terrain angle. Missing heights count as sea level.
- The AIS map does not draw the rings (the item named AIS as optional).

## Tests
`tests/adsb/test_adsb_horizon.cpp`:
- flat sea: every bearing and altitude gives the radio horizon of antenna plus aircraft height, within 2 km
- a 600 m ridge 30 km north cuts the 300 m ring there but not to the south, and shortens the 10 000 m ring
- no elevation data
- the upintheair.json reader with text and number altitudes, a ring with too few points, and bad files

Run locally: all passed. The upintheair.json layout is the one tar1090 reads (id, lat, lon, rings with alt and points). It was checked against a hand-written sample, not a file downloaded from heywhatsthat. The UI was syntax-checked only.
