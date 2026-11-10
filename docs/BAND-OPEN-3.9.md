# When is the band open (3.9)

Round-4 item 38 (scratchpad r4-per-mode list; VOACAP-style charts from your own spots). Branch feat39-r4.

- **FT8 / WSPR tab > When is the band open**: a table of bands by UTC hour over the last **7** or **30** days, from the stations OnAir heard on FT8, FT4, FT2, WSPR and the other WSJT modes.
  - The number in a cell is the number of days with spots in that hour.
  - The cell colour grows with that share.
  - The tooltip has the spots and the farthest one (from the locator, needs the receiver position).
- Every decode counts once. The store keeps 31 days, one record per day, band and hour, in `<data folder>/stats/band-open.txt`, saved every 10 minutes.
- The 24 h propagation dashboard stays as it was. This is the longer view.

## Tests
`tests/ham/test_ham_bandopen.cpp`: 20 m open 06-17 UTC on 10 days and 40 m at night on 4 of them, the 7 and 30 day matrices (days, spots, farthest), the band order, old days dropping out, the saved text. Run locally: all passed. The table was syntax-checked only.
