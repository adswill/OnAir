# Heard over days and years: ADS-B and AIS (3.9)

Round-4 items 11, 23 and 33 (scratchpad r4-per-mode list). Branch feat39-r4. They share one store, `core/include/dect2/common/seen_store.h`.

## The store
- It keeps every identity ever heard (ICAO address or MMSI) with its first and last local day, the number of days it was heard, its last name (callsign or ship name) and its type.
- It keeps one record per local day: unique identities, the new ones among them (never heard before), positions, messages, the farthest position and who it was, and the identities per hour.
- It also keeps an all-time range outline per 10° bearing sector in three bands, and the farthest position ever, overall and per band.
- It is saved to `<data folder>/stats/adsb-seen.txt` and `ais-seen.txt` every 5 minutes when something changed and when the receiver stops, and read back at start. It survives a restart. It keeps at most 400 days and 2 million identities.

## ADS-B (items 11 and 23)
- **ADS-B > History** shows:
  - the airframes ever heard, today's unique and new ones, positions and the farthest today and ever
  - bar charts over 30 or 365 days: unique aircraft, positions and the farthest per day
  - the all-time outline per altitude band: below 10 000 ft, 10 000-25 000 ft, above
  - CSV of the days
- Only real ICAO addresses are counted (not TIS-B track numbers). A position counts when it changes.
- The Live table marks an aircraft heard for the first time ever today with a green **new**.
- New event **adsb.first_ever** (hex, callsign, registration, type, operator, alt_ft, distance_km, unique_ever) for rules and alerts. It is not sent until the store holds at least two days, because on the first day everything would be new. `adsb.new` still means new in this session.

## AIS (item 33)
- **AIS > History** shows:
  - vessels ever heard, today's unique and new ones, and the farthest today
  - the farthest ever per class (A, B, others)
  - unique vessels per day (30 days) and per hour today
  - the all-time list of vessels (MMSI, last name, type, first and last day, days heard) with a search box
  - CSV of the days
- Distances need the receiver position.

## Tests
`tests/adsb/test_adsb_seenstore.cpp`:
- unique and new identities per day over days
- a save and load in the middle, and identities on later days
- hours, kinds and the farthest per band through the save
- a late report of an earlier day, the local time zone at midnight, the day cap
- the CSV, bad files and bad lines

Run locally: all passed. The UI parts (the feed from the receivers, the views) were syntax-checked only. Simulated data only.
