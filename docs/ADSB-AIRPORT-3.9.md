# ADS-B arrivals and departures board (3.9)

Round-4 item 28 (scratchpad r4-per-mode list). Branch feat39-r4.

- **ADS-B > Airport**: the nearest large or medium airport to the receiver (from the built-in airport list), or any ICAO / IATA code typed in. Every second the aircraft near it are checked: within 8 km of the airport's reference point and below 2000 ft above the field elevation.
  - **Landed**: seen descending low near the field, then reporting the ground. An approach that is lost low near the field (most receivers lose aircraft below their radio horizon before touchdown) also counts after 60 s without a message.
  - **Took off**: on the ground near the field (or first heard there low), then airborne, low and climbing.
  - A go-around, an overflight and an approach to another field are not movements. A turnaround (land, then depart again later) gives both.
- Each row shows the local time, landed or took off, callsign, registration (database) and the runway from the track (`27` for 270°). The runway number is not corrected for magnetic variation, so it can be one off from the painted number.

## Tests
`tests/adsb/test_adsb_airportboard.cpp`:
- a 3° approach to the ground
- a turnaround departing the other way
- an approach lost at 300 ft
- an overflight at 30 000 ft
- a go-around, an approach to a field 30 km away
- a departure first heard already climbing
- runway names

Run locally: all passed. Simulated tracks only, not a recording at a real airport. The view was syntax-checked only.
