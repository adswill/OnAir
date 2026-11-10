# Smart features, round 5 (3.9): marine

Branch `feat39-smart5c`. The list is in `scratchpad/smart37-backup/r5-avia-marine-iot.md`. AIS ducting and NAVTEX
warnings in force are on the feat39-smart5b list.

**Simulated data only.**

| # | Item | Where | What was measured |
|---|---|---|---|
| 9 | AIS behaviour flags | `core/include/dect2/ais/ais_behave.h`, run by the AIS receiver (`ais_rx.cpp`); listed with the anomalies in AIS > Alerts; automation event `ais.behaviour` | With 0 / 40 % lost reports and 3 / 12 m noise: <br>- Rendezvous: two ships stopped 0.1 nm apart for 50 min give one flag for the right pair. A ship passing at 12 kn gives none. <br>- Status against motion: "under way using engine" while stopped for over an hour gives one flag per ship. "Moored" while making 4.5 kn gives one flag. <br>- Loitering: a 0.4 nm circle at 4 kn for 2.5 h is flagged, a straight track is not. <br>- Draught and destination: a draught change is flagged; a destination that differs only in case is not. <br>- In port: two ships stopped next to a learned berth are not a rendezvous. Aids are ignored. |
| 10 | Anchor watch | same | A 40 m swing for an hour, then a drag at 0.6 kn to the south-east: one "dragging anchor" alert within 20 minutes of the start of the drag, with the direction. A swing that grows by 30 m in a gust never alerts. |

Severity: dragging anchor is an alarm (2); draught and destination are information (0); the rest are warnings (1).

Weather fax, DSC, VDES: nothing new. The fax schedule, the DSC/AIS/SART/NAVTEX incident board and the VDES decoder
already cover what was proposed.

## Notes

- Berths are learned from vessels reporting moored or at anchor. A rendezvous within 2 nm of one is ordinary.
- In a new area, the first rendezvous next to an unlearned quay can still be flagged.
- Nothing is kept between sessions.

## Tests

`ais_behave`; the AIS receiver change is covered by `ais_anomaly`, `ais_alerts` and `ais_engine`, and the new catalog
row by `automation`.
