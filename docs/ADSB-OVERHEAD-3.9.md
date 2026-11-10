# ADS-B overhead log (3.9)

Round-4 item 8 (scratchpad r4-per-mode list, planefence). Branch feat39-r4.

- **ADS-B > Overhead** lists every aircraft that came within a radius of the receiver today (local time). The default radius is 3 km, with an optional ceiling in ft (0 means any altitude). Each pass is one row: the closest moment, the distance, the altitude at that moment (or "ground"), callsign, type, registration and the ICAO address.
- **Overhead now:** a green line above the Live table, the Map and the Radar shows the aircraft inside the radius at the moment, closest first (at most three).
- A pass starts when an aircraft is first heard inside the radius. It ends when the aircraft is heard outside it again (5 % margin), or when it has not been heard inside it for 2 minutes. A short gap in reception therefore does not split a pass.
- Each finished pass is appended to `<data folder>/adsb-overhead/<YYYY-MM-DD>.csv` (one file per local day). At start the day's file is read back, so the list survives a restart. **Save CSV...** exports today's list.
- CSV columns: `utc_closest, icao, callsign, registration, type, closest_km, alt_ft, first_utc, last_utc, ground`.
- Distances are measured from the receiver position. A hint appears when the position is not set.

## Tests
`tests/adsb/test_adsb_overhead.cpp`:
- straight passes at 0.2, 1 and 2.5 km cross-track, with and without 50 m range jitter. Each must give one pass with the right closest distance and time, the database type and registration.
- a 60 s reception gap inside one pass (one pass), and a 10 minute gap (two passes)
- two aircraft at once, one above the ceiling and one passing outside (neither logged)
- the local day of a pass, and the CSV round trip with commas and quotes in fields

Run locally: all passed. Simulated tracks only. The UI was syntax-checked only.
