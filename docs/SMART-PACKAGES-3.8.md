# Smart packages (3.8): what was built

From `broadcast.md`, `hamvoice.md`, `aviosensors.md` (packages F, H, I, J, K, L) and the `r2-*.md` lists. Branch `feat38-smart`, on `integ-38`. Checked against `integ-38`, the other 3.8 branches and `docs/SMART-*-3.7.md` first.

**Everything below was only tested on simulated signals.** No real recording of these situations was on hand.

## Built

| Item | Where | Result |
|---|---|---|
| ADS-B trust score (avio 1) | `adsb/adsb_trust`, table, map ring | Learns level against range from confirmed aircraft. Flags: too loud or too weak for the range, beyond the radio horizon, position jumps, claimed speed against the positions, registration against address, one callsign on two addresses. 80 honest aircraft, level scatter up to 3.5 dB more than learned: 0 flagged. A hint, the UI says so. |
| ACAS conflict view (avio 3) | `adsb_track`, Rust `commb::acas_ra`, map | The advisory is read from DF16 MV and TC 28 subtype 2 as well as Comm-B. Line from the aircraft to its threat, a timeline on the aircraft. |
| VDL2 station map (avio 4) | `vdl2_tel.h`, `vdl2_rx`, map | Stations with no GSIF position get an estimate from the aircraft talking to them, a coverage ring, and each aircraft its last 5 stations. |
| ADS-C contracts (avio 5, half) | `aero/aero_adsc` | Uplink contract requests (periodic, event, emergency, cancel) with interval and thresholds, acknowledge and refusal, contract state shown on the aircraft. Layouts from libacars `adsc.c`, no code taken. |
| ADS-B deeper repair (r2 avio 10) | `adsb_track`, "up to 5 bits" | 4-5 flips among the 18 least certain bits, known address only, altitude must fit. 400/400 repaired for 4 and 5 flips in the simulated case. False repairs: 15 of 200000 random frames that already carry a known address. Off by default. |
| DRM Whole span (r2 1) | `drm/drm_find`, wide bank | Guard interval correlation proves DRM before a receiver starts, so AM carriers cost nothing. Modes A-D, 4.5-20 kHz, DRM+. Four signals plus an AM station, 14 to 30 dB: all four found, no false hits. Two decoded at once in the engine's bank. |
| DRM alternative frequencies (13) | `drm/drm_afs`, Radio tab | SDC types 3, 4 and 11 per ES 201 980 V4.3.1, with schedules. One click tunes a DRM frequency, others are listed. Through the real transmitter and receiver. |
| DRM station book (14) | `drm/drm_afs`, Radio tab | Every station heard, kept between sessions. |
| DRM SDC combining (r2 15) | `drm_dec` | A failed SDC is added to earlier copies, used only if the CRC passes. Static SDC: mode B at 7 dB from 0 of 32 blocks to 31 of 32. **It only helps if the transmitter repeats the SDC unchanged**; one that rotates entities gets nothing. |
| DAB census (10) | `dab/dab_census`, Scan tab | Every Band III ensemble in a capture, FIC only. Three ensembles in 8.192 Msps at 12 and 20 dB, with and without 1.8 kHz offset. One merged service list. |
| TV echo advice (15) | `channels/channel` | The strongest echo in km and what to do. |
| Satellite from the NIT (17) | `tv/si_tables`, NIT view | Orbital position most of its transponders give. |
| ISDB-T alert banner (18, half) | `isdbt_ui` | The TMCC flag was already read; now a banner. |
| ATSC 3.0 alerts (19) | `atsc3_ip`, ATSC screen | AEAT parsed and listed. **Element names are from memory of A/331, not checked: the standard could not be fetched.** |
| HD sideband note (8, part) | `hdr_ui` | Says which sideband is hit. Per-block sideband weighting already existed in `hdr_phy`. |
| Sensor health (avio 9, part) | `sensors/sensors_health`, table | Interval learned, silence, odd reading, two sensors on one id, falling battery. |
| Storm trend (avio 12) | `sferics/sferics_storm`, automation `sferics.storm` | Approaching 28/30, receding 30/30, steady never called moving, in the sweep. |
| ALE Whole span (r2 hamvoice 1) | wide bank | All known ALE channels of one capture, merged. |
| Signals on a schedule (avio 15) | `sweep/survey`, Survey tab | Regular comings and goings, and the next one due. |

## Not built, and why

- **POCSAG frame check (r2 hamvoice 21): not possible as proposed.** The low three bits of a capcode are not sent; the decoder takes them from the frame position, so there is nothing to compare.
- HD sideband rescue (8): the weighting already exists.
- ATN B1 CPDLC (avio 5, other half): needs ASN.1 tables and the X.25/CLNP stack.
- MLAT over the swarm (avio 2), sferics position from KiwiSDRs (11), Kiwi clock service (16): need timing across real network nodes, which has not been run.
- KAAL link adaptation and HARQ (22, 23): change the owner's over-the-air specs, need a decision first.

Not reached in this run, no blocker known: HD blend view (9), SFN solver (11), DAB/FM link table (12), antenna assistant (16, feat38-ease2 has a dish view), syndrome erasure (20), VBI data (21), analogue ghost (22), pass score (23), georeference (24), SSDV gap fill and Horus join (26, 27), sensor reverse engineering and repeat voting (8, 9), hydrogen drift scan (13), fast scan identify (14), KA link test (24), KA auto profile (r2), TV Whole span for ATSC/ISDB-T/DTMB/DVB-C (r2 2), HD and analogue TV Whole span, the rest of the r2 lists.

## Tests

`test_adsb_trust`, `test_adsb_deeprepair`, `test_aero_adsc_contract`, `test_vdl2_gsmap`, `test_drm_find`, `test_drm_afs`, `test_drm_sdccomb`, `test_dab_census`, `test_dvbs_orbit`, `test_atsc3_aeat`, `test_sferics_storm`, `test_sensors_health`, `test_ale_wide`, and a block in `test_survey`. Compiled and run standalone against the prebuilt 3.7 libraries, with the changed sources rebuilt; UI files syntax-checked only. Not run in the app.
