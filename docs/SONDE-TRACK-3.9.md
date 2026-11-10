# Radiosonde flight track as KML / GPX (3.9)

Round-4 item 37 (scratchpad r4-per-mode list; auto_rx KML, SondeHub tracker). Branch feat39-r4.

- The sonde detail has **track KML...** and **GPX...**. They save the flight of the selected sonde as two lines with altitude, the ascent up to the highest point and the descent after it, plus these points:
  - the first position heard
  - the burst: the highest point, when the sonde has since come down more than 1000 m
  - the last position, named "landed (last heard)" when it came down below 1500 m after a burst
  - the predicted landing while it is still falling
- KML opens in Google Earth (lines with absolute altitude). GPX opens in OsmAnd and other map apps (tracks with `<ele>`).
- The track is the receiver's, at most 600 points, kept for the 5 most recently heard sondes.

## Tests
`tests/sonde/test_sonde_flight.cpp`:
- a simulated flight to a 31 km burst and down to 400 m: two lines meeting at the burst, the burst and landed points, KML and GPX with altitudes
- a sonde still rising with a prediction
- a single position

Run locally: all passed. Simulated flight only. The buttons were syntax-checked only.
