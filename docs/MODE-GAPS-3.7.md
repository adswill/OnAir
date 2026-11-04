# Mode and feature gaps for 3.7

Read-only audit of integ-37 (mode table app/ui/mode_table.cpp, docs/MODE-NUMBERS.md, core/, app/, docs/). Date 2026-11-04.
Verdicts: EXISTS, PARTIAL, NEW. Many of these were already built in 3.5/3.6; check before starting a worker.

## Modes A

| Item | Verdict | Evidence | Gap / next step |
|---|---|---|---|
| Wi-Fi/BLE/Zigbee scanner | EXISTS | "Wi-Fi networks", "Bluetooth LE", "BLE tracker detector", "Zigbee / 802.15.4" rows; core/src/wifi, ble, bletrk, zigbee; docs/bletrk.md | The ESP32 worker (queue) adds a cheap RX path. One combined "2.4 GHz scanner" view is missing (S). |
| LoRa/Meshtastic map | PARTIAL | core/src/mesh (Meshtastic, MeshCore, position/node decode), "LoRa (raw)", LoRaWAN, lorasat | Decoding exists; a dedicated mesh node map with links/hops/trails is missing (M). |
| Sub-GHz weather/sensors 433/868/915 | PARTIAL | "ISM sensors" row, core/src/ism (dec_weather, dec_energy, tpms, more); docs/ISM-COVERAGE.md: 45 of 380 rtl_433 protocols | 335 protocols remain; queued as the rtl_433 port with GPL-2+ notices. Unknown-signal analyser = Protocol lab / Signal Lab (exist). Effort L (batchable). |
| TPMS + key fobs RX | PARTIAL | core/src/ism/ism_dec_tpms.cpp (TPMS exists); docs/ISM-COVERAGE.md: key fobs / rolling code 0 of 31, "deliberately not added" | TPMS done. Key-fob decoding is a policy decision (earlier "not added: security"); needs owner OK. S-M. |
| DRM EPG/Journaline | PARTIAL | core/src/drm/drm_journaline.cpp, drm_data.cpp (slide show, MOT), app/drm/drm_ui.cpp; DAB journaline + SPI logos; DVB EPG in core/src/tv/epg.cpp | Journaline exists. DRM service-list/programme guide (SDC/EPG via MOT) not found. S-M. |
| NOAA weather radio SAME | EXISTS | core/src/same (rx, proto, gen), "NOAA Weather Radio SAME" row, docs/same.md | Alerts to watchlists could be checked. None needed. |
| GSM-R | EXISTS | "GSM-R" row, core/src/gsmr, app/gsmr | None (BCCH info only, by design). |
| UAT 978 MHz | EXISTS | "UAT" row, core/src/uat (decode, FIS-B, TIS-B, track) | None. |
| Pager + trunk all-in-one | PARTIAL | pager (POCSAG, FLEX, combine, CCIR), core/src/trunkpool (multi-radio trunking, per-call files, docs/trunking.md), MPT1327, LTR, EDACS, P25, DMR | Pieces exist separately; one "pagers and trunked radio" combined live view is missing (M). |
| Satellite pass auto-recorder | EXISTS | docs/pass-scheduler.md, app/panels/passes_ui.cpp, core/src/sched, docs/doppler-orbit.md | Works for weather sats and listed passes. Could add one-click "record every pass of everything above N deg" (S). |

## Modes B

| Item | Verdict | Evidence | Gap / next step |
|---|---|---|---|
| POCSAG/FLEX pager map | PARTIAL | "POCSAG", "FLEX" rows, core/src/pager, msgdb | Decoding done; map of pager messages (geocoded from text or capcode area) missing. S-M. |
| Radiosonde tracker | EXISTS | core/src/sonde (RS41, RS92, DFM, M10, M20, LMS6), "Radiosonde", "Horus balloons", sondehub feeds (core/src/net/feeds.cpp) | None; prediction/landing map could be checked. |
| LoRaWAN / Helium sniffer | EXISTS | core/src/lorawan (rx, plan, bank, FSK), "LoRaWAN" row; Helium named in mesh_gen | Helium-specific view not separate (join/uplink are generic). S. |
| Keyless/garage analyser RX | PARTIAL | Protocol lab, Signal Lab, ISM analyser; fobs not decoded (see above) | Generic OOK/FSK analyser exists; named garage/fob decoders absent by policy. |
| Smart meters wM-Bus + ERT | EXISTS | core/src/wmbus + docs/wmbus.md; ERT-SCM in core/src/ism/ism_dec_more.cpp | IDM / SCM+ / US meter families still open in rtl_433 list. S. |
| Amateur sat digital voice/telemetry/SSDV | EXISTS | core/src/ssdv, cubesat (telemetry, db), qo100, lorasat, Horus; "CubeSat telemetry" | Amateur sat digital voice (FM/C4FM relay, M17 sat) is voice only through existing modes; no dedicated pipeline. S. |
| NAVTEX + weather fax | EXISTS | core/src/marine (navtex, fax, wefax stations, wefax archive), "NAVTEX", "Weather fax" rows | None. |
| TETRA/DMR talkgroup directory | PARTIAL | trunkpool/chanplan, voicekeys, DMR/TETRA rows | No built-in or user-curated talkgroup/RadioID directory with names across systems (docs/RADIOID.md exists for DMR IDs). M. |
| DVB teletext/subtitles/EPG viewer | EXISTS | core/src/tv teletext.cpp, dvbsub.cpp, epg.cpp, subtitle_export.cpp, docs/MHEG-REDBUTTON.md | None. |
| CPDLC + ADS-C | PARTIAL | vdl2_proto.cpp: CPDLC found and named, "ASN.1 PER body is not decoded"; HFDL and ACARS print ADS-C position reports | Full FANS-1/A CPDLC ASN.1 PER decode and structured ADS-C (contract/periodic/event) decode missing. M. |

## Features A

| Item | Verdict | Evidence | Gap / next step |
|---|---|---|---|
| One-click best settings | PARTIAL | docs/AUTOGAIN.md, docs/SCAN-IDENTIFY.md, docs/PRESETS.md, docs/SIMPLE-ADVANCED.md, docs/optimising-a-mode.md | Gain + scan/identify exist; one button that also sets rate/bandwidth/ppm for the mode is the queued "ease" lane. S-M. |
| Scheduled recordings | EXISTS | core/src/sched/sched.cpp, recmgr.cpp, record_rules.cpp, app/ui/kit/record_rules_ui.cpp, passes | Time-of-day scheduler for non-satellite modes could be checked (recmgr_ui). |
| Alerts on signal/message | EXISTS | core/src/watchlist, docs/watchlists.md, core/src/automation (rules, expr), msgdb | None. |
| Signal finder sweep | EXISTS | core/src/sweep, docs/scanner.md, band-survey.md, fast-scan.md, Identifier (docs/IDENTIFIER.md) | None. |
| Session replay | PARTIAL | docs/RECORD-REPLAY.md, time-machine.md, core/src/session (session_store, track_book) | IQ replay and rewind exist; replay of the decoded timeline (messages + map) as a movie is missing. M. |
| Share receiver with QR | EXISTS | app/webshare (qr.cpp, webshare.cpp), docs/ONAIR-WEB.md, LAN-SWARM.md | None. |
| Universal search | EXISTS | app/ui/command_palette.cpp, frequency DB, mode_table search keywords | Search of decoded messages across modes (msgdb) not in the palette. S. |
| Antenna/gain advisor | PARTIAL | docs/ANTENNA.md (designer), core/src/antd, AUTOGAIN, doctor | No "which antenna for this mode and band" recommender. S-M. |
| Export everything | EXISTS | core/src/export/exporters.cpp, docs/EXPORT-AUDIT.md (CSV, JSON, ADIF, KML, GPX, GeoJSON), WAV via recmgr | A single "export this session" bundle is missing. S. |
| Setup doctor | EXISTS | core/src/doctor/reception_doctor.cpp, app/panels/doctor_ui.cpp, docs/RECEPTION-DOCTOR.md, station_health | Doctor diagnoses reception; first-run hardware/driver doctor is the ease lane. |

## Features B

| Item | Verdict | Evidence | Gap / next step |
|---|---|---|---|
| Frequency DB auto-labels | PARTIAL | docs/frequency-db.md, app/freqdb, band-plans, bandplan | DB and Tuner drop-down exist; labels drawn on the waterfall automatically when a peak matches an entry - check; likely missing. S. |
| Auto-follow / skip scanner | EXISTS | app/scan/fast_scan_ui.cpp (lockout, priority, dwell, auto) | None. |
| Spectrum ghost overlay | PARTIAL | app/specan peak-hold; docs/band-survey.md (compare to earlier survey) | Compare-with-past exists in the analyser only; not on the main waterfall. S-M. |
| Interference hunter | EXISTS | core/src/rfi, app/rfi, docs/rfi-hunter.md | None. |
| Unified map of all tracked objects | PARTIAL | core/src/geo/maplayers.cpp, app/map, track_book, ui/main_tabs_unified.inc, gnss-map.md | Map layers per mode exist; one view of aircraft + ships + sondes + sats + mesh + sensors together to confirm (likely PARTIAL). M. |
| Unknown waveform to text helper | EXISTS | docs/protocol-lab.md, docs/SIGNAL-LAB.md, core/src/protolab, signallab | None. |
| Phone remote control | PARTIAL | docs/REMOTE-API.md (HTTP API with token, curl/Python), OnAir Web control page, QR | API and web page exist; a touch-friendly phone control page is unconfirmed. S-M. |
| Recording library with tags | PARTIAL | app/panels/recording_library_ui.cpp, recmgr_ui.cpp, docs/RECORDINGS-FOLDER.md | Library exists; no tags / notes / search by tag found. S. |
| Clock/frequency calibration | EXISTS | docs/self-calibration.md, core/src/selfcal, app/time/time_clocksync.cpp, GSM/cell and TCXO helpers | None. |
| "What is this?" button | EXISTS | docs/IDENTIFIER.md (key I), app/specx/specx_ident.cpp, docs/SCAN-IDENTIFY.md, RADIOID.md | None. |

## Ranked real gaps (NEW or PARTIAL)

No item came out fully NEW; the whole list is largely built. Order: value for effort.

1. rtl_433 protocol port (335 left; unknown-signal analyser exists) - L, batchable, already queued.
2. CPDLC ASN.1 PER and structured ADS-C decode (VDL2/HFDL/ACARS) - M.
3. Mesh node map (links, hops, trails) for Meshtastic/MeshCore/LoRaWAN - M.
4. Unified map of all tracked objects across modes - M.
5. Talkgroup/system directory for DMR/TETRA/P25 with names - M.
6. Recording tags and notes in the library - S.
7. Auto frequency labels on the waterfall from the frequency DB - S.
8. Ghost overlay (compare with past) on the main waterfall - S-M.
9. Pager and trunk combined live view and pager message map - M.
10. Session timeline replay (decoded events + map) - M.
11. One-click best settings that also sets rate/bandwidth/ppm per mode - S-M (ease lane).
12. Antenna advisor (mode and band to antenna suggestion) - S-M.
13. DRM service list / programme guide - S-M.
14. Key-fob decoders - S-M, needs an owner decision on the earlier "not added: security" policy.
15. Session export bundle, message search in palette, phone-friendly control page, 2.4 GHz combined scanner - S each.

## Other valuable modes not in the app (spotted in the mode table)

No match for these names in the mode table or core/src (verify before starting):
1. Starlink Ku downlink beacon/activity detector - popular, no decode, power/Doppler only. 
2. Thuraya / Iridium-style GEO L-band (Thuraya, Inmarsat BGAN) - only Inmarsat-C/Aero present.
3. NOAA APT (137 MHz analog weather satellite images) - Meteor LRPT exists but APT not seen in mode names (confirm; may sit under a satellite row).
4. FST4/Q65/MSK144 (WSJT-X weak-signal family) - only FT8/FT4/WSPR/JS8 rows found.
5. Yaesu System Fusion (C4FM) as its own row - not seen separately (confirm against voice plug-ins).
6. MICS/MedRadio 402-405 MHz implanted-device telemetry - low effort FSK, privacy-aware metadata only.
7. ERMES / Eurosignal pager legacy - only POCSAG/FLEX, no ERMES.
8. ADS-B MLAT/TIS-B correlation and time-difference feed - MLAT only appears as a feed target.
9. Bluetooth classic/ANT in 2.4 GHz via wide capture - only LE and ANT+ present.
10. OneWeb/Starlink-class LEO beacon doppler ranging for position (PNT) - extends GNSS row.
