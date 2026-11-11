# 4.0: core features wired into the screens

Cards and readouts for the pure engines of group A to D. They live in `app/d40/` (one file per area, fed every frame from `d40Tick()` in the Insights tick) and are drawn as a collapsible header in the mode's own screen. Settings use `plat::prefs()` (keys `d40...`); lists the user supplies are text files in the data folder. Offline; nothing leaves the computer. Syntax-checked only (the app was not built in this lane); the logic under them is what the unit tests cover.

| Feature | Where | What the user sees |
|---|---|---|
| FM station auto-naming (`fm_d40.h nameStations`) | FM, under the census ("Station names and DX alarm") | A named list of every station above 6 dB: PS by majority of clean reads, a PI on two frequencies within 300 kHz as one row ("Also on"), Listen button. Needs the census on. |
| FM DX opening alarm (`DxOpening`) | same card | "DX opening alarm" checkbox (saved), "Learn normal band" checkbox: while ticked, stations seen are saved to `fm_dx_baseline.tsv`. With 3 or more foreign PIs not in the baseline the card shows a warning and posts an Insight. |
| DVB-S blind scan (`sat_d40.h blindScan`) | DVB-S, "Blind scan and dish pointing" | Button averages 40 spectrum frames of the capture and lists centre, width, symbol rate (snapped to standard rates, "~" when not) and C/N, with a Tune button. "Narrowest carrier" setting is saved. |
| DVB-S2 modcods (`modcodFor`) | same card | When locked: the three best modcods that fit the measured Es/N0 with margin. |
| Dish pointing aid (`PointingAid`) | same card | Checkbox: C/N, peak, dB below the peak, rising/falling/steady, "at the peak". No beep yet. |
| DVB-T echo table | Analysis > echoes | Already wired in 3.x (`tv_diag.h findEchoes`, table with pre-echo / beyond guard / inside guard): verified, nothing to add. |
| Explorer / sweep scanner activity map (`scan_d40.h ActivityMap`) | Sweep scanner, "Activity map and signal names" | Per 25 kHz cell and hour counts, saved to `sweep_activity.txt` every minute, merged over days. Needs about 20 sweeps in the same hour before alerts. |
| New-signal alert (`NewSignalWatch`) | same card | Amber lines: a signal in a cell that was almost always quiet at this hour (confirmed over 2 sweeps). |
| Hit classification (`classifyHit`) | same card | The persistent signals get a "Looks like" guess with confidence, a tooltip with the reason and their busy hours. Duty is the occupancy; level swing is estimated from the survey. |
| AM carrier offset (`am_d40.h carrierOffset`) | Short wave / AM tab, "Carrier, sideband and station ID" | Carrier frequency and offset in Hz from the 9 / 10 kHz (5 kHz above 1.7 MHz) raster. |
| Best sideband, AM or DRM hint | same card | Cleaner side for synchronous detection with its advantage in dB; "Looks like AM / DRM" after 3 agreeing readings. |
| AM station ID (`ScheduleTable`) | same card | Reads `am_schedule.csv` (khz,HHMM,HHMM,days,name,country, for example from an EiBi export) from the data folder. Nothing is bundled. Heterodyne finder is not wired: it needs the demodulated audio spectrum, which the AM screen does not expose. |
| DAB service ranking (`dab_d40.h ServiceRanker`) | DAB, "Service ranking and transmitters" | One reading every 5 s of the locked ensemble: mean SNR, spread, share decoded, best first (3 readings needed). The SNR is the ensemble's, so the ranking compares services across the ensembles visited. |
| DAB transmitter table (`TiiTable`) | same card | Reads `dab_transmitters.csv` (main,sub,name,lat,lon[,kW]); the heard TII pairs become names with distance and bearing from the receiver position; unknown pairs are listed. |
| ADS-B replay (`adsb_replay.h TrackReplay`) | ADS-B, new "Replay" view | Slider 0 - 60 minutes back; table of every aircraft at that time with interpolated position (~) and distance. Fixes are kept from when the window started, 1 hour. Not drawn on the map. |
| ADS-B orbit detector (`adsb_loiter.h`) | same view | "Flying circles or a holding pattern" list: turns, direction, radius, centre. |
| Transmit link budget, ramp, SWR (`tx_plan.h`) | Transmit settings, under the level, "Link budget, ramp and SWR" | Inputs (distance, losses, antenna gains, sensitivity, fade margin) saved; EIRP, path loss, received power, margin and the free-space range at zero margin from the radio's power table; ramp steps to the target power; typed SWR gives reflected share and back-off advice. Notes only, nothing is blocked. The frequency plan check was already in place. |
| Talkgroup activity, P25 / NXDN / TETRA (`trunkpool/tg_activity.h`) | P25 and NXDN Calls view, TETRA Talkgroups view | Airtime ranking, calls, radios, 50-minute timeline and busiest talkgroup, as DMR has. TETRA has no call log: calls are built from the slot states (clear voice with a talkgroup) and terminal numbers are never used, so its Radios column is empty. |
| ACARS channel load (`acars/acars_load.h`) | ACARS, Channels view | Per frequency messages per minute, good share and cumulative share over 30 minutes, and the fewest channels that carry 90 %. |
| AIS coverage and ship types (`ais/ais_coverage.h`) | AIS, Range view | 12 sectors: reports, typical and farthest range in nm, blind sectors; the ship type mix of the vessels held. |
| Pager capcode groups (`pager/pager_watch.h PagerGroups`) | Pager, Capcodes view | Capcodes paged together at least 3 times within 20 s as groups with their incident count. Keyword alerts were already in 3.x (`app/pager/pager_keywords.h`). |

## Not wired yet (core and test only)
- Group D: slideshow collector (`BlobCollector`), SFN follower (`SfnFollower`), LNB drift (needs the expected beacon frequency), QAM order guess, HD Radio sideband levels, MER per carrier, analogue TV standard and video score, FPV channel picker, heterodyne finder.
- Group C: AIS name cache (the vessel list already keeps names), ATIS tracker, ham callsign / CQ list, mode guess, FT8 propagation summary, clear TX frequency, band hop schedule, tone identification, DTMF number assembler, ISM burst classifier, Sarsat incidents.
- Group A / B items were already wired in 3.x or in their own lane.

## Notes
- New strings use the English text until translated: no German entries were added (the catalog check only warns for missing keys). They are all in `app/d40/` and the transmit card.
- The OnAir Web schema is generated from telemetry structs; no telemetry struct changed, so `tools/gen_web_schema.py --check` stays current and there are no schema entries to add. The new cards are desktop-only.
- `ais_coverage.h`: `AisClass` was renamed `AisShipClass` (and `aisClassName` to `aisShipClassName`): it clashed with the enum of `ais_tel.h` once both headers were in one file.
