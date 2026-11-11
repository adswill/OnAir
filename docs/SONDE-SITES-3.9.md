# Radiosonde launch sites (3.9)

Round-4 item 39 (scratchpad r4-per-mode list; SondeHub site pages). Branch feat39-r4.

- Every sonde's first fix is checked: when it is low (below 2500 m) and the next points climb, it is a launch. Launches within 5 km of each other are one site, placed at the mean of its launches. A sonde first heard high (out of range at launch) adds no site.
- When a sonde of a known site has come down after a burst (the last position more than 1000 m below its highest point and below 1500 m) and has not been heard for 2 minutes, its last position is kept as that site's landing.
- **Radiosondes > Sites** lists each site with its position, launches, landings, where its sondes usually land (the distance and bearing of the mean landing from the site) and the last launch. Click a name to rename it ("Kaunas" instead of "Site 1").
- The map shows the sites (blue triangles) and a heat map of their landings (orange dots). The checkbox in Sites turns them off.
- Kept in `<data folder>/stats/sonde-sites.txt`.

## Tests
`tests/sonde/test_sonde_sites.cpp`:
- two launches 2 km apart make one site, one 40 km away another, a sonde first heard at 9 km is no launch
- landings after a burst only, counted once, and later for a sonde still in the air before
- renaming, the saved text, a new launch at a known site after loading

Run locally: all passed. Simulated flights only. The view and the map layer were syntax-checked only.
