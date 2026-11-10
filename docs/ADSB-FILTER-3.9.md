# ADS-B filter and search (3.9)

Round-4 item 4 (scratchpad r4-per-mode list). Branch feat39-r4.

- A search box and a **Filter** button sit above the Live table, the Map and the Radar (and in the Simple view). The table, the side list, the map icons, trails, paths ahead, winds and ACAS lines, and the CSV/JSON export show only the aircraft that match. "n of m shown" and **Show all** appear while a filter is on. The filter is saved (`adsbFilter`).
- **Search**: words separated by spaces. Each word must be found, in any case, in the address (hex), callsign, registration, type or operator. The last three come from the registration database.
- **Filter** pop-up:
  - an altitude band (from / to, ft). An aircraft without an altitude fails a band. One on the ground counts as 0 ft.
  - squawks: a list (`7500, 7600, 7700`) or a prefix (`75*`)
  - emitter categories (`A3, B`). One letter takes every category of that letter.
  - ground and airborne, ground only, or airborne only
  - military address blocks only
- **Military blocks**: US AE0000-AFFFFF and UK 43C000-43CFFF. These come from spotters' lists, not from an ICAO document. Military aircraft of other countries have no known block, so this filter does not find them.

## Tests
`tests/adsb/test_adsb_filter.cpp` checks each part on a mixed set of six aircraft, then all 64 combinations of the parts (the result must be the intersection of the parts alone). It also checks the block edges, the non-ICAO flag bit, and the settings round trip with bad values. Run locally: all passed. The UI was syntax-checked only.
