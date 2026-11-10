# plane-alert-db categories (3.9)

Round-4 item 31 (scratchpad r4-per-mode list; plane-alert / planefence). Branch feat39-r4.

- **Watchlists > Import plane-alert-db...** reads `plane-alert-db.csv` from github.com/sdr-enthusiasts/plane-alert-db (download it in your browser). It makes one aircraft list per category ("plane-alert: Other Air Forces", "plane-alert: Police Forces", "plane-alert: USAF" ...), all off at first. Switch on the categories you want alerts for. The alerts are the watchlist's: notification, sound, ntfy, Telegram or webhook, with its quiet hours and limits.
- Importing again (a newer file) replaces the plane-alert lists and keeps the categories you had switched on. Your own lists are not touched.
- Columns are found by name, so a reordered file still works: $ICAO, $Registration, $Operator, $Type, $ICAO Type, #CMPG, $Tag 1-3, Category. The header was checked against the file on GitHub on 2026-11-10. Rows without a valid 6-digit address are skipped.
- The ADS-B table shows the category after the callsign, with the operator, registration, type and tags in the tooltip. It reads a copy of the imported file kept in the data folder.

## Tests
`tests/adsb/test_adsb_planealert.cpp`:
- the real header, a quoted comma in a tag, lower-case hex, bad and short addresses, a row without a category
- the lists per category, off at first
- matching an ADS-B event through the watchlist
- the index, columns in another order and a byte-order mark, a file that is not plane-alert-db

Run locally: all passed. The full 2.4 MB file was not imported in a test. The UI was syntax-checked only.
