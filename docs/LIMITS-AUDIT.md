# Limits audit (feat37-caps)

Rule: no artificial limit that a busy receiver can hit. Tracker tables are large and old entries leave by age; the caps in
`core/include/dect2/limits.h` only stop a runaway feed from using all memory. Message lists that are copied into the telemetry every
frame are 2000 long (a message list is a log, the totals are counted separately).

| Where | Old cap | New cap | Reason |
|---|---|---|---|
| VDL2 aircraft (`vdl2_rx.cpp`) | 200, silent drop | 10000 + 6 h age | users saw it stop at 200 |
| VDL2 ground stations | 64, silent drop | 4096 + 6 h age | same |
| VDL2 / HFDL message lists | 400 | 2000 | log, copied per frame |
| HFDL aircraft (`hfdl_rx.cpp`) | 200 | 10000 + 6 h age; full re-sort replaced by move-to-front | O(n log n) per message at 10000 |
| ACARS aircraft (`acars_rx.cpp`) | 100 | 10000 + 6 h age (swept every 256 messages) | |
| ACARS message list | 200 | 2000 | |
| Inmarsat aircraft / messages / logons / EGC | 200 / 300 / 200 / 150 | 10000 / 2000 / 2000 / 2000 | |
| AIS vessels (`ais_rx.cpp`) | 300, oldest evicted | 10000, oldest evicted at the cap | |
| ADS-B table / listed per report | 2000 / 128 | 10000 / 10000 (age expiry already existed) | the list was cut at 128 |
| UAT aircraft listed | 128 (table unlimited, expiry) | 10000 | |
| Aero aircraft / messages / logons | 100 / 200 / 100 | 10000 / 2000 / 2000 | |
| Iridium ACARS aircraft / messages | 100 / 200 | 10000 / 2000 | |
| ASTERIX tracks | 3000 | 10000 | |
| APRS duplicate filter `seen_` | 500 | 5000 | dedupe window, not a list |

Left as they are (not a table a busy receiver fills, or a fixed protocol size): UAT FIS-B/log 64 (rolling log), per-aircraft tracks
(24 to 100 points), Orbcomm tracks 600 points, ADS-B frame log 64, AIS alert log 50 / NMEA 50 / recent 500, railway messages 400,
Iridium messages 100, trunk pool messages 200, QO-100 tracks 64, FLARM/other mode-specific lists, ACARS log per aircraft 50 and 400
aircraft (`acars_log.h`; the log keeps the newest text per aircraft). Not done: a "maximum tracked" row (the caps are only a memory guard).

UI: the VDL2 aircraft and ground station tables draw only the visible rows (ImGuiListClipper) and show "N aircraft, M ground stations".

## Remove after (Settings > Decoding)
One row per family (VDL2, ACARS, HFDL, ADS-B, AIS, UAT, Inmarsat): Default / 1 min / 5 min / 30 min / 1 h / 6 h / 24 h / Never / Custom,
plus one "Custom remove-after time (minutes)" row. Default is the mode's own rule: 6 h for VDL2/ACARS/HFDL, the existing drop time for
ADS-B and UAT, keep forever for AIS and Inmarsat (as before). Stored as `stale<Family>` / `staleCustomMin` in the prefs and pushed to the
trackers by `applyStaleFromPrefs()` into `dect2/limits.h`. Expiry runs on the receiver's own clock (VDL2/HFDL at the end of every feed,
AIS at every report, ACARS at every report, UAT/ADS-B in their existing sweep, Inmarsat when the report is built), frees the memory
(shrink_to_fit / empty map) and the counts shown follow the lists. Tests: `vdl2_caps` and `ais_caps` run the sample clock past the limit.
Not covered by a test: ACARS, HFDL, UAT, ADS-B and Inmarsat expiry (same helper, no generator wired).
