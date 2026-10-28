# Mode Experience Standard (design, 3.4)

Status: design only. No code in this branch. Written from the group audits of 2026-10-28 (digital voice, analog voice, aviation, maritime, satellite and science, TV and video, broadcast radio, pagers and messaging, IoT/ISM, navigation and time, mesh/LoRa, rail/drones/wireless/cell, tools/custom) and the existing shared infrastructure. Every file named below was named in those audits; items marked NEW do not exist yet. Where an audit said "verify", the first task of the owning wave is to verify before building.

Owner goal: every mode should be more comfortable. Conversations (Inmarsat voices, DMR calls, message threads, tracks, passes) are tracked, we know what each one is, auto-record, more data per mode, more organised, better presentation.

Owner rules this document keeps: easy by default, advanced under Advanced; no limits on bandwidth or sample rate; never block transmit; no patented codecs shipped; unencrypted voice only, decryption only with user keys; decoder quality first; one mode entry per protocol; English strings (tr()) and "?" help on everything new; test with real-world impairments.

## 0. What the audits found (the five common gaps)

1. Nothing remembers. Lists are newest-first rings of 32 to 1000 rows, in memory, lost on Clear or mode change. Only some modes are in `DECT2_RESULT_SLOT` (core/src/results/engine_results.cpp); inmc, argos, sarsat, goesdcs, qo100, ssdv, mscatter, astro, dect, wmbus, zwave, sigfox, nfc, esubghz, ebicab, marine VHF, tones lose lists.
2. Almost nothing leaves the process. Only a handful of event types exist (gnss.fix, time.minute, acars.message, vdl2.message, hfdl.message, adsb.new, adsb.aircraft, ais.vessel, dsc.call, lora.packet, sonde.update, sonde.landed, rid.drone, ble.device, ble.tracker, aprs.packet, pager.message). Digital voice, trunking, TV, broadcast, satellite, cell, IoT and KA modes publish none, so msgdb (View > Messages), watchlist, rules, rooms and rx_json never see them. rx_json.cpp has lists only for LBJ, KLW, TDCS, Horus.
3. Several unrelated call/conversation models. `voice::Monitor` (core/include/dect2/voice/voice_monitor.h) is the best, but DMR (`DmrCall` 100), P25 (`P25Call` + `trunk::CallLog`), NXDN, dPMR, D-STAR, M17, `IridiumCall`, `Qo100Conversation`, `MarineVhfCall`, `AirbandActivity`, `TetrapolCall` each keep their own log with different columns.
4. Export and auto-save are patchy. `resexp::saveButton` (app/state/results_export.cpp), `msglog_ui::bar` (app/ui/msglog_ui.h) and `xport::GeoTrack` (core/include/dect2/export/exporters.h) exist and are each used by a few modes.
5. Every mode names and orders its tabs differently (Activity, Calls, Call log, Frames, Packets, Log, Messages, Table, Ships, Beacon...). Many strings are raw literals, not tr(); many modes have no "?" help.

Recording is the same story: General has a transmission recorder (core/src/general/general_rx.cpp `setTransmissionRecording`, `transmissions.csv`), Marine has WAV per transmission (`MarineVhfReceiver::setRecording`), `voice::Monitor` has per-call WAV plus JSON, Iridium records calls, QO-100 per conversation, `audio_control_ui.cpp` has a fourth recorder, Airband has none.

## 1. Mode types and the standard views

Every mode is assigned one primary type (some have a secondary). The type decides which views the mode must show. The standard is a contract: same tab names, same order, same toolbar, so the user learns it once.

### 1.1 The common frame (all types)

Implemented once as `ModeFrame` in app/ui/kit (NEW `app/ui/kit/mode_frame.h/.cpp`), called from each mode's `tab()` hook in app/panels/modeui.cpp.

Tab names and order (a mode shows only those that apply; order never changes):

| # | Tab | Contents |
|---|-----|----------|
| 1 | Live | What is happening now: the mode's existing live view (now talking, picture, constellation, now playing). |
| 2 | Log | Event/message table with search, filter, sort, wall time (UTC/local switch via `timefmt`), Save and Auto-save (`msglog_ui::bar`). |
| 3 | Conversations | The session list plus detail (type-specific name: Calls, Threads, Tracks, Passes, Programmes, Flights). Section 2. |
| 4 | Map | Only where positions exist. Shared layer app/map (`adsbmap::draw/project`). |
| 5 | Stats | Statistics strip plus timeline (activity per hour, per talkgroup/channel, quality over time). |
| 6 | Signal | Receiver diagnostics (constellation, eye, counters, tuning). Existing receiver() content. |
| 7 | Notes and Rules | Per-mode auto-record rules (section 3), watch list, notes on the selected item. |
| 8 | Advanced | Everything expert: decoder tuning, raw frames, FIC/FIG style trees, Settings tabs (LoRa Settings, Drift, PSK Settings). Collapsed by default. |

Rules:
- Default tab is Live. A beginner never needs tab 6 to 8.
- A mode with only one thing to show keeps one tab, but still gets the status header and the export menu.
- Tabs are `subNav` (not RadioButtons or `typo::beginTabItem`); ANT+, DECT, VDL4, UAT, NFC, Astro move to `subNav`.
- Names go through `tr()`. Old names map: Activity, Call log, Calls, Messages, Packets, Frames become Log or Conversations as the data demands (Airband Activity becomes Log; DMR Calls becomes Conversations; M17 Activity becomes Conversations).

### 1.2 Status header (above every tab)

One line, NEW `modeHeader()` in app/ui/kit, fed by the existing `*Summary()` helpers (`dmrSummary`, `airbandSummary`, `marineVhfSummary`, `tonesSummary`, `inmcSummary`, `aeroSummary`, `ridSummary`, `bletrkSummary`, `cellSummary`, ...) and the common telemetry block (state, snrDb, blocksOk/blocksBad, dataValid):

`[lock lamp] Mode name  |  freq / bandwidth  |  SNR 14 dB  |  rate  |  counts (calls today, active now, errors)  |  recording: ON (3 files, 120 MB)  |  last heard 12:03:11`

The recording indicator is clickable and opens Notes and Rules. Units always in the header labels.

### 1.3 Export menu (all types)

One "Export..." button per Log/Conversations/Map/Stats view (NEW `exportMenu()`, wraps `resexp::saveButton`, `xport::*`, `msglog_ui::bar`). Entries offered only if the data supports them:
CSV, JSON, text log (daily auto-save), KML/GPX/GeoJSON (when lat/lon columns exist, via `xport::GeoTrack`), audio (WAV/FLAC/MP3 via `audio_rec.h`), images (PNG via gallery), transcript (TXT/SRT via `TranscriptStore`), PCAP (802.15.4, LoRaTap, 802.11ah where frames are kept), session bundle (section 2.6). "Copy" (clipboard) stays as an extra.

### 1.4 Type definitions and required views

**Type V, Voice (calls with audio).** Digital voice (DMR, P25 1/2, NXDN, dPMR, D-STAR, Fusion, M17, TETRA, TETRAPOL), trunking (MPT1327, LTR, EDACS), analog voice (Airband, Marine VHF, General, QO-100 SSB), satellite voice (Iridium, Inmarsat Aero voice carriers, KAAL).
Views: Live (now talking per slot/channel, voice strip `voiceui::strip`), Log (control messages and data), Conversations = Calls (columns Time, Source, Target/Talkgroup, Channel/Slot, Duration, Level/BER, Flags, Audio play button, Position), call detail (timeline, play, notes, tags, linked messages/positions), Stats (airtime per talkgroup/radio/channel per hour, busiest, encrypted share), Map (LRRP/NMEA/D-PRS positions), Rules.
Unit: a call (continuous talk, hang time ends it); secondary unit: a roster entry (radio/talkgroup/callsign) with first/last seen, call count, airtime.

**Type M, Message and data-link (threads).** Pagers (capcode), ACARS, VDL2, HFDL, Aero and Inmarsat-C messages, NAVTEX, DSC, Meshtastic/MeshCore/MeshCom/LoRa APRS chat, APRS (AFSK), KAPP/KAWS chat, Flipper chat, Multitone.
Views: Live, Log (searchable message table, detail pane), Conversations = Threads (per aircraft/registration, per capcode, per station pair, per channel, per distress event), Stats, Map (where positions), Rules.

**Type T, Track (entity moving in time).** ADS-B, UAT, FLARM, ASTERIX, Mode A/C, VDL4, AIS, VDES, radiosonde, Horus, SSDV with balloon, Remote ID, DJI, LBJ/KLW/TDCS trains.
Views: Live (table with detail), Map (tracks), Conversations = Tracks/Flights/Voyages (session per entity with first/last, max range/altitude, track points kept after the entity ages out), Log (squawk changes, emergencies, messages attached), Stats (range plot, counts per hour), Rules (record track on emergency, geofence).

**Type D, Device or network inventory.** ISM sensors, Zigbee, HaLow, Wi-SUN, Z-Wave, Sigfox, ANT+, Wireless M-Bus, NFC, DECT (metadata only), Wi-Fi, BLE, BLE trackers, cell (LTE/NR/NB-IoT/GSM-R), LoRaWAN, LoRa raw, EBICab.
Views: Live, Devices/Networks (sortable, filter, detail), Log (frames), Conversations = Device sessions (first/last, count, reading history; for networks the join/association sessions), Stats (counts per hour, quiet-device alarm), Map only if positions, Rules (new device, silent device, level).

**Type B, Broadcast programme.** DAB, FM/RDS, DRM, HD Radio, CDR, AM, C-QUAM, SAME alerts.
Views: Live (now playing, station card, slides), Services (station/service table with search, sort, PTY filter, favourites), Log = Programme log (Conversations = Programmes: each song/show/alert as an item with time, artist/title/text, file), Stats, Map (TII transmitters, SIS positions, SAME areas), Rules (record per item, per keyword, TA flag, SAME event).

**Type P, Picture or pass (image/data product per satellite pass).** Meteor LRPT, HRPT, GOES HRIT, Elektro-L, SSDV, WEFAX, KAIT, KAFT files, Argos, CubeSat, LoRa satellites, Orbcomm, Globalstar, GOES DCS, analog TV/MAC/FPV pictures.
Views: Live (picture growing, constellation), Gallery = Conversations = Passes/Products (thumbnail strip, pass identity from `passes_ui.cpp`: satellite, AOS/LOS, max elevation), Log (packets/frames per pass), Stats (SNR per pass), Map (projection via gallery), Rules (record IQ and CADU per scheduled pass via `sched.h`).

**Type S, Service or multiplex.** DVB-T/T2, DVB-C, DVB-S/S2, ISDB-T/S, ATSC 1/3, DTMB.
Views: Player/Live, Services (common table), Guide, Recordings, Streams (TS), Stats (quality over time), Conversations = Reception sessions (one per tune: mode, freq, start/end, lock time, min/mean/max quality, services seen, files made), Rules (record service, series, event).

**Type O, Beacon or observation.** NDB, VOR/ILS, DME, DGPS, eLoran, time stations, e-Czas, GNSS, SBAS, SARSAT, ionospheric sounders, radio astronomy, meteor scatter.
Views: Live, Heard (heard beacon/station table: ident, freq, first/last heard, best level, ident confirmed), Log, Conversations = Observations/Sessions (fix session, SBAS stream, time-decode run, distress alert grouped from repeated bursts), Stats (drift, availability), Map where positions, Rules.

**Type X, Tool.** Workbench, Spectrum explorer/analyser, PSK demodulator, Mode Builder, Plugins, Signal Lab, Sweep scanner.
Views: tool tabs stay, but every tool gets the same Export menu, tr() names, and a saved Analysis session (section 2.5). Plugin and Mode Builder frames feed events and msgdb.

### 1.5 Advanced settings, empty states, help

- Anything expert (demod tuning, raw frames, tree views, Settings tabs, FIC/FIG, symbol dumps, decoder thresholds) sits under tab 8 Advanced or a collapsed "Advanced" header on the control panel. No tuning in the default flow (feedback-ease-of-use).
- NEW `emptyState(modeId)`: when a list is empty, show one sentence what the mode is listening for, what frequency/antenna is needed, and a "?" button. Uses `app/ui/learn.cpp` for text; no mode may show an empty table with no explanation.
- Every new control gets `tr()` and a tooltip; every tab gets a "?" help entry in learn.cpp.
- No limit on bandwidth or sample rate is introduced by any component; the session layer is independent of rate.

## 2. The Conversation model

One core model, called `Session` in code, shown as "Conversation" where it is a call or thread and as Track, Pass, Programme, Flight or Observation elsewhere. It is the generalisation of `voice::CallEntry`/`CallInfo`, `trunk::CallRecord`, `IridiumCall`, `Qo100Conversation`, `MarineVhfCall`, `AirbandActivity`, `AcarsLog` buckets and `ContactGraph` completion state. It does not replace per-mode decoders; they fill it.

### 2.1 The record

NEW `core/include/dect2/session/session.h` (namespace `dect2::sess`), plain struct, `results_io.h` friendly (no pointers):

```
struct Session {
  uint64_t id;            // stable: hash(mode, kind, key, startWall), survives restarts
  uint16_t mode;          // mode number (docs/MODE-NUMBERS.md)
  uint8_t  kind;          // Call, Thread, Track, Pass, Programme, Device, Reception, Observation, Alert, Analysis
  char     key[48];       // natural key: "TG 1234/slot1", "capcode 1234567", "ICAO 4CA7B1", "NORAD 25338", "MMSI ..."
  char     who[2][40];    // who: source and target (ids), aliases in aliasSrc/aliasDst
  char     aliasSrc[32], aliasDst[32];
  int64_t  startWall, endWall;   // UTC ms; endWall==0 means active
  uint32_t durationMs;
  float    levelDb, levelPeakDb, snrDb, errRate;  // quality summary
  uint64_t freqHz; int32_t carrierId; int16_t slot; // where
  double   lat, lon; float altM;                  // last position, NaN if none
  uint8_t  verdict;       // clear / encrypted / unreadable / test (mirrors voice::Verdict)
  uint8_t  flags;         // emergency, lateEntry, priority, watched, starred
  uint32_t eventCount;    // messages / frames / transmissions folded in
  uint32_t audioIdx, imageIdx, textIdx;  // links into side tables
  char     tags[64];      // comma separated
  char     note[128];     // user note (kept in the sidecar, not in the ring)
};
struct SessionLink { uint64_t sessionId; uint8_t type; /*Audio,Image,Message,Position,Transcript,Iq,Telemetry*/ char ref[160]; int64_t wall; };
```

Rules for the model:
- A mode's decoder calls `SessionStore::begin(key, ...)`, `update(id, ...)`, `end(id, verdict)` and `link(id, type, ref)`. The store lives in the engine, one per receiver (NEW `core/include/dect2/session/session_store.h`, `core/src/session/session_store.cpp`), and the sessions travel as a list field of each mode's `*_tel.h` struct so `rx_json.cpp`, the remote API and the UI see them.
- Gap/grouping rules are per mode and live in the decoder side (NEW `session_rules.h`): voice = hang time; Airband = same channel and quiet gap N seconds, joined by callsign (transcript); Marine = ch16 call plus the working channel hop (DSC/MMSI match); ACARS = registration plus msgNum/msgSeq with ETB/ETX reassembly; pager = capcode plus 10 min window with repeat merge; HFDL = logon to logoff (`hfdl_rx.cpp` `acIcao` map); VDL2 = AVLC link (XID to disconnect); APRS = pair plus message number plus ack; LoRaWAN = DevEUI/DevAddr join to uplink/ack; DSC distress = alert plus relays plus acknowledgements by `distressMmsi`; passes = AOS to LOS from `passes_ui.cpp`; tracks = same ICAO/MMSI/serial across gaps shorter than a configurable limit.
- Encrypted or unreadable content creates a Session with `verdict = encrypted` and metadata only. Never audio, never text.

### 2.2 Persistence

Three layers, all existing mechanisms:
1. Hot ring in memory per receiver (cap raised to 10,000 sessions; the old 100/300 caps become display windows). Persisted across mode change with `DECT2_RESULT_SLOT` (core/src/results/engine_results.cpp) via `results_io.h`/`results_fields.h` (regenerate with tools/gen_results_fields.py). All missing modes get slots (list in section 5, Wave 1).
2. Cold store: append-only JSONL per mode per day, `<dataDir>/sessions/<mode>/YYYY-MM-DD.jsonl`, written by an `AutoSaver`-style writer (extend `core/include/dect2/common/msg_log.h`). Each line is one Session plus its links. Notes and tags are edited by writing a small sidecar `notes.jsonl` (id to note/tags) so the day files stay append-only. Retention by `recmgr` cleanup rule (days and total MB; default 90 days, 2 GB, owner can change).
3. Index: each session start/end is emitted as an `au::Event` (core/include/dect2/automation/event.h) of type `session.<kind>` with mode, freq, ident (key), text (summary) and extra (JSON of the record). `app/panels/msgdb_ui.cpp` taps it into `msgdb::Record`. msgdb gets one added column `session_id` (core/include/dect2/msgdb/msgdb.h `Record` and `Filter`), so View > Messages can show a thread and jump to the mode's Conversations tab. msgdb keeps refusing voice audio and encrypted content; for voice it stores only the metadata Record (who/when/duration), never text from transcripts unless the channel is unencrypted analog and transcribe is on.
Per-mode event names also stay (acars.message etc.); mode events that do not exist yet are added in core/src/automation/catalog.cpp in Wave 1 and 2 (list in section 5).

### 2.3 Audio, images, messages, positions linked

- Audio: `SessionLink{Audio}` points at the file written by the recorder hook (section 3.5); `voice::CallEntry.file` becomes this link.
- Images: gallery keys (`gallery::publish` key such as `meteor-<start>`); SSDV/WEFAX/KAIT/Meteor/HRPT/GOES publish and link.
- Messages: ids into the mode's Log table and msgdb Record ids.
- Positions: LRRP, NMEA, D-PRS, M17 positions, ADS-B points; kept as a track in `results_io` (time-stamped, replacing lat/lon-only trails) so GPX/KML export works.
- Transcript: `TranscriptEntry` gets `sessionId` and `audioRef` (core/include/dect2/transcribe/transcript.h); text only for unencrypted analog voice and only when transcribe is enabled by the user.
- IQ: a `recmgr::Meta` entry with SigMF, linked as `Iq`.

### 2.4 The shared UI

NEW `app/ui/kit/session_list.h/.cpp` and `session_detail.h/.cpp`, generalised from `app/voice/voice_monitor_ui.cpp` (history tab, `vmsearch` search box, filters). See section 4.

### 2.5 Analysis sessions for tools

Kind `Analysis`: file/IQ path, selection, demod parameters, measurement, bit-pattern hits, notes, as one JSON next to the recording; reopened by Workbench, Signal Lab, Protocol Lab, PSK demodulator and Mode Builder (hand-off buttons).

### 2.6 Session bundle

"Export session" zips: session JSON, CSV of its log, audio, images, positions as KML/GPX, transcript, notes, and a key-free metadata file. Follows `.onairbackup`/preset conventions (core/include/dect2/backup/backup.h). User keys and secrets are never included.

### 2.7 Privacy defaults in the model

- Identifiers that are personal data (BLE/Wi-Fi addresses, ANT+ device ids and heart rate, NFC UIDs, DECT/phone identities, FLARM no-track aircraft) are not written to the cold store unless the user enables "Keep identifiers" for that mode; default stores hashed or aggregated values. BLE trackers: identifiers are never written anywhere (bletrk_book.h rule stays).
- FLARM aircraft with the no-track/stealth flag are never recorded.
- Redaction toggle for ACARS text (`acarsRedact`) is applied consistently to table, export and cold store, with a visible switch.

## 3. Auto-record

### 3.1 Rules

NEW `core/include/dect2/session/record_rules.h`; evaluated by the session store when a session begins, updates and ends. A rule = trigger plus action plus scope:

Triggers (any can combine with AND):
- by id: talkgroup, radio id, callsign, MMSI, ICAO, capcode, DevAddr, BLE kind (not address), PTT id, tone/code (CTCSS/DCS/5-tone), station (FM PS/PI, DAB service)
- by tag/category: P25 `TgEntry` tag/category, call kind (emergency, private, all-call, data)
- by keyword: in text, transcript, DLS/RadioText/RT+, message body (watchlist Kind::Words engine)
- by level: C/N or RSSI above X, error rate below Y, duration above Z
- by event: new device/PAN/cell/tower, new talkgroup, emergency squawk 7500/7600/7700, DSC distress, SAME event code (TOR, SVR, ...), TA flag, satellite AOS
- by schedule: time windows and cron through `core/include/dect2/sched/sched.h` (once/daily/cron/satellite-pass jobs)
- Default rule when the user turns Record on: "everything unencrypted that squelch opened" for voice modes, "all messages" to the daily log for message modes. Nothing else is on by default.

Actions:
- Audio: per call/transmission/programme item (WAV, FLAC or MP3 via `core/include/dect2/audio/audio_rec.h`), pre-roll from `audio/clip_ring.h` (default 3 s), hang time as the mode defines.
- Log: append to the daily JSONL and text log (`msglog::AutoSaver`).
- Save object: image, file, slide, LOT/MOT object, FIS-B product, NAVTEX text, chart (existing savers: `NavtexSaver`, WEFAX archive, KAFT folder, gallery).
- IQ clip: `AudioClipRing` style IQ ring plus `recmgr` SigMF entry, only when a rule asks (IQ is large). Length default 10 s, max configurable.
- Track: save positions as GPX/KML at session end.
- Notify: watchlist channels (desktop, sound, ntfy, Telegram, webhook) through `core/include/dect2/watchlist/watchlist.h`; new kinds Capcode, Talkgroup, Tone, Session.

### 3.2 Rule editor

One dialog (section 4.2), reachable from tab 7 and the header's recording indicator. Simple mode: a list of ready-made rules (Record everything, Record emergencies, Record only my list, Record this talkgroup (from a call row's context menu), Notify me); Advanced: free combination and schedule.

### 3.3 What is saved where

One folder tree, one naming scheme (replaces `recordings/`, `audio-recordings/`, `audio-recordings/marine-vhf/`, `<data>/goes/`, `dvbs_ip.pcap` fixed names):

```
<dataDir>/library/<mode>/<YYYY-MM-DD>/<HHMMSS>_<freq>_<key>_<alias>.<ext>
<dataDir>/library/<mode>/<YYYY-MM-DD>/<same name>.json      # sidecar (the Session record)
<dataDir>/sessions/<mode>/<YYYY-MM-DD>.jsonl                # cold store
```

Every file is registered in `core/include/dect2/sched/recmgr.h` (`Meta` tags and notes, search, trash with undo, SigMF export), so the Recording library (app/panels/recording_library_ui.cpp, recmgr_ui.cpp) shows one list for all modes. The folder is chosen once in Settings (a "..." chooser like Marine's), formats by one combo (WAV/FLAC/MP3) with tags (frequency, mode, date, ids, callsign) in all formats.

### 3.4 Limits and cleanup

- Disk guard from `sched.h`: stop recording at free space below a limit (default 2 GB or 5 percent) and say so in the header; never delete silently while recording.
- Cleanup rule via `recmgr` (max age days, max total MB, never delete starred or noted items). Defaults: audio 30 days or 5 GB, logs 90 days, IQ 7 days or 10 GB. Deleted items go to the trash with undo (existing recmgr trash).
- Per-rule caps: max call length (default 1 h), min length (0.5 s, as Marine), max IQ clips per hour.
- Cap disk of the cold store by retention days only; sessions are small.

### 3.5 Recorder hook

NEW `core/include/dect2/session/recorder_hook.h`: generalises `GeneralReceiver::setTransmissionRecording` (pre-roll, hang, min length, optional channel IQ, `transmissions.csv`, `recTags`) and `voice::Monitor` per-call WAV into one `RecorderHook` that any mode's open/close events drive. One implementation; General, Marine and Airband plug in; `voice::Monitor` keeps its policy lists (allow/mute/priority/hold/squelch) and calls the hook for storage. `audio_control_ui.cpp` Record button records the mix to the same library with the same naming (the fourth recorder is retired in Wave 2).

### 3.6 Legal and privacy notes (shown in the rule editor and docs)

- Only unencrypted traffic is recorded as content. Encrypted voice and data: metadata only (who/when/duration/verdict), never audio, never text. No attempt to crack or auto-decrypt.
- Decryption only with keys the user imported into the key store (`core/include/dect2/voicekeys/voice_keys.h`, `key_persist.h`, app/keys). Keys are never logged, never in session records, never in exports or bundles. Wireless M-Bus meter keys use the same store pattern.
- Voice codecs: patented codecs are not shipped. Voice modes record decoded audio only when the user has installed the codec (docs/CODECS.md, runtime loading); otherwise the session still tracks the call and may save the raw frames.
- The user is responsible for the laws of their country on receiving and recording; the first time a recording rule is switched on, a one-time notice says so (not a blocker). Transmit is never blocked by this system.
- Nothing is uploaded automatically. Rooms, swarm, feeds, uploads and exports happen only when the user triggers them. The session layer has no network code.
- Transcription (speech-to-text) is opt-in, offline, with an accuracy note.
- Personal-identifier modes: see 2.7.

## 4. Shared components (build once)

Each with reuse, effort (S under 1 day, M 2 to 5 days, L over a week) and first users. All NEW files live under app/ui/kit or core/include/dect2/session unless stated.

| # | Component | Reuses | Effort | First modes |
|---|-----------|--------|--------|-------------|
| 4.1 | `Session` model plus `SessionStore`, event emit, JSONL cold store, results slot, rx_json list | `voice_monitor.h`, `results_io.h`, `engine_results.cpp`, `msg_log.h`, `automation/event.h`, `rx_json.cpp` | L | DMR, P25 (migrate `voice::Monitor` onto it), then every type |
| 4.2 | Rule editor and rule engine | `watchlist.h`, `sched.h`, `automation/rules.h`, `voice::Settings` lists | M | Digital voice, Marine, Airband, General |
| 4.3 | Conversation list widget (table with search `vmsearch` pattern, sort, filter chips, group-by, star, multi-select, context menu "record this", wall time) | `app/voice/voice_monitor_ui.cpp`, ImGui sortable table (`ism_ui.cpp deviceView`) | M | DMR, NXDN, dPMR, D-STAR, M17, P25 2 |
| 4.4 | Conversation detail pane (timeline, play with scrub, notes, tags, linked messages/images/positions, transcript, "show file", export bundle) | `voice_monitor_ui.cpp`, `acars_ui.cpp` detail pane, `recording_library_ui.cpp` | M | Same as 4.3, then ACARS/VDL2/HFDL |
| 4.5 | Recorder hook | `general_rx.cpp`, `audio_rec.h`, `clip_ring.h`, `recmgr.h` | M | General, Marine, Airband |
| 4.6 | Exporter (`exportMenu()`): CSV/JSON/text/KML/GPX/GeoJSON/audio/bundle | `results_export.cpp`, `exporters.h`, `msglog_ui.h` | S | All; first Airband, Marine, APRS, KA |
| 4.7 | Statistics strip and timeline (counts, airtime per hour, top-N talkgroups, quality over time, sparkline) | `ImPlot` use in airband `spectrumView`, ADS-B `statsView`, `FmTelemetry` history | M | Digital voice, Airband, ADS-B, Marine |
| 4.8 | Search box helper (text + field filter + time range, lowercase `lwMatch` pattern, saved filters) | `lwFilter`, `vmsearch`, `dscMatches`, msgdb `Filter` | S | All tables |
| 4.9 | Status header `modeHeader()` and `ModeFrame` tab skeleton | `*Summary()` helpers, `subNav`, `mode_table.cpp` | M | All; pilot DMR and Airband |
| 4.10 | Empty state plus "?" help hook plus tr() lint | `learn.cpp`, `tour.cpp` | S | All |
| 4.11 | Entity roster (per radio/talkgroup/callsign/device pages: first/last seen, count, airtime, last site/position, labels, name import from CSV like P25 `TalkgroupList` / RadioID.net) | `trunk.h TalkgroupList`, `ShipDb`, `acars_log.h` | M | Digital voice, trunking, D-STAR/M17, ISM |
| 4.12 | Map session layer (tracks from Session positions, KML export) | `app/map`, `adsbmap`, `dmr_map.cpp`, `track_hub.h` | M | DMR/D-STAR/M17 maps unified, AIS, drones |
| 4.13 | Pass object (satellite, AOS/LOS, max elevation) joined to sessions | `passes_ui.cpp`, SGP4, `sched.h` | M | Meteor, HRPT, LoRa sat, Argos, CubeSat, Iridium |
| 4.14 | Event catalog additions and msgdb `session_id` column | `catalog.cpp`, `msgdb.h`, `msgdb_ui.cpp` | S per family | Per wave |
| 4.15 | Reception session for service modes (TV) and quality logger | `msglog::AutoSaver`, common telemetry block | M | DVB-T/T2, DVB-S, ATSC 3.0 |
| 4.16 | Services panel (TV) and Programme log (broadcast) | `tv.cpp` cards, `scan_outputs.cpp`, `recorder.h`; DAB DLS / RDS RT+ / HD PSD feed | M each | DVB, then DAB/FM/HD/DRM |

Dependency order: 4.1 first (all else needs it), 4.8 and 4.6 and 4.10 in parallel (small, independent of 4.1), then 4.9, 4.3, 4.4, 4.5, 4.2, then the rest.

## 5. Per-group plans and rollout waves

Principle: framework first, then the modes the most people use, quick wins before big items, and each wave unblocks the next. Quick wins are listed per group; everything not in a wave later is Wave 5 backlog.

### Wave 0: Framework (blocks everything)
- 4.1 Session model/store, JSONL, results slot, rx_json list. Also raise silent caps and show a "N rows dropped" note.
- 4.8 search box helper, 4.6 `exportMenu()`, 4.10 empty state/help/tr() lint, 4.9 header and tab skeleton.
- Add `session_id` to msgdb; catalog entries `session.*`.
- Pilot on DMR (best data) behind the standard; migrate `voice::Monitor` to write Sessions (history 1000 in memory becomes the store; persistence of the existing history is the first visible win).
- Quick wins that cost nothing: P25 Phase 2 gets `voiceui::strip` (voice monitor already fed at core/src/p25p2/p25p2_rx.cpp:536); voice monitor history export CSV/JSON; open-recording and play per history row using `CallEntry.file`.

### Wave 1: Digital voice and trunking (largest user base, owner's example)
Quick wins: standardise sub-nav (Live, Calls, Messages, Map, Site) across DMR/NXDN/P25/dPMR/D-STAR/M17 (M17 Activity to Calls, DMR Table to Positions); wall time instead of signal seconds in DMR, MPT1327, LTR, EDACS; search/sort in each call table; `msglog_ui::bar` on every Messages tab; show hidden fields as columns (NXDN baud/BER/endSeen, DMR cc/privacy/lateEntry, dPMR channel/commMode, M17 type/enc/ext1/ext2, P25 key id); join DMR LRRP position to the call row; P25 Calls tab shows `trunk::CallRecord.audioPath` and Export CSV; fix TETRAPOL "screen only, never saved" label; `tr()` and help in MPT1327/LTR/EDACS/Fusion; remove the quadratic unit list in `nxdn_ui.cpp list()`.
Big items: 4.3/4.4 common Calls panel replacing per-mode tables; unify DmrCall/P25Call/NxdnCall/DpmrCall/DstarCall/M17Transmission/CallLog onto Session (one list, one set of columns); au events (`dmr.call`, `p25.call`, `nxdn.call`, ...); TETRA `TetraCall` from `tetra_slots.h/tetra_traffic.h` with Calls tab and CallInfo identities (encrypted: metadata only); TETRAPOL and EDACS follow through `voice::Monitor`; Fusion call list from header/terminator; rules engine (4.2) with talkgroup tags from `TgEntry`; roster pages (4.11) and name import (RadioID, RadioReference CSV); MPT1327/LTR/EDACS call detection and trunk follower modelled on `p25/trunk.h`; FreeDV station log and recording; Digital voice (auto) detection history with VoiceSignal fields.
Also in this wave: Iridium `IridiumCall` migrates to Session; Inmarsat all-carriers mode (208) voice carriers use Session (see section 6).

### Wave 2: Analog voice and recording unification
Quick wins: `msglog_ui::bar` on Airband Activity, Marine Call log, Tones History, General; Marine Reveal/Open file button and peak column; Tones: Level and DCS alias columns, store frequency in `ToneEvent` (fixes wrong frequency on save); Airband Channels table Calls and Last heard columns; tr() on raw literals; unify columns Time/Channel/Frequency/Length/Level/Recording; General Transmissions list built from the recorder events; folder chooser and one Format combo in General; Airband per-channel Rec checkbox via `setChannelTap`; Transcripts table Speaker and Duration columns.
Big items: 4.5 recorder hook; Airband/Marine/General/Tones on Session (exchange = channel plus callsign/MMSI); join transcripts, activity and recordings (`TranscriptEntry.sessionId/audioRef`); Marine DSC/AIS join (vessel names and position, distress forces record of the named working channel); persistence plus rx_json for the four modes; merge Audio control/Cleanup/Record popups into one Audio panel.

### Wave 3: Aviation and maritime (surveillance, data links)
Quick wins: ADS-B table filter/sort/search and hidden detail rows (roll, IAS, hazards, NIC/SIL); Save log for ACARS and HFDL reusing `vdl2log`; `msglog_ui::bar` on ACARS/VDL2/HFDL/VDL4/uplink; AcarsCat column and filter; consistent acarsRedact; VDL4 to subNav with Channels/Stations and wall time; UAT Map; Save buttons for uplink, ASTERIX, FLARM, UAT; KML/GPX per aircraft track; events for vdl4/uat/flarm/asterix/modeac/uplink. AIS filter/summary/columns, rolling !AIVDM log, rename Messages to Statistics, safety text list; DSC auto-save (on by default for distress), frequency column, sort, distress-only filter; NAVTEX search/subject filter/frequency stored, `navtex.message` event; WEFAX gallery shows its own metadata, `wefax.chart` event; VDES CSV export plus `vdes.asm` event.
Big items: Flight/Session record on `track_hub.h` plus `acars_log.h`; ACARS multi-block reassembly and downlink/uplink pairing; HFDL/VDL2 link sessions; persistent flight history (ADS-B/UAT/FLARM/ASTERIX; FLARM privacy kept); AIS vessel history with time-stamped tracks; DSC distress incident view with follow-up recording; aviation panel kit; NAVTEX structured messages.

### Wave 4: Broadcast radio and TV
Broadcast quick wins: programme log per mode (DLS/DL Plus, RT+/PS, HD PSD, DRM text, CDR text) with `msglog_ui::bar`; recently-played ring; HD LOT pictures and DAB slides to gallery; SAME pair ZCZC/NNNN into an alert lifecycle, event kind filter, home-county flag, bundled county names; show hidden fields (FM callSign, ert, EON); Record button with item naming; persistence for DRM/HDR/CDR/C-QUAM; Advanced tabs collapsed; rx_json cases.
Broadcast big items: programme log framework and auto-record per item; SAME voice clip plus notify; ensemble-wide DAB (record-all) later; object saver for HD/DRM/CDR; AM station identification (needs licence check for a schedule database).
TV quick wins: same sub-view names and shared header; Snapshot/Record for analog and MAC; export buttons for services, carriers, flows, layers; unique pcap/symbol file names; show hidden counters; sort/search for services and scan; TV recordings into `recmgr`; quality CSV logger; ARIB name conversion; `tv.*` events.
TV big items: reception session (4.15); common Services panel; recording rules (EPG keyword/genre/series, emergency flags, SCTE-35); analog/MAC capture to a video file; quality report for engineers; DVB-S IP/carrier intelligence store.

### Wave 5: Satellite, space, science, navigation, time
Quick wins: `msglog_ui::bar` for Aero, Orbcomm, Argos, SARSAT, Globalstar, LoRa sat, CubeSat, Horus, Sonde, Meteor scatter; missing results slots (inmc, argos, sarsat, goesdcs, qo100, ssdv, mscatter, astro); summary headers; text filter/sort for message tables; HRPT auto-save plus gallery publish; share GOES/Elektro product code; events (Aero, SARSAT, Argos, GOES DCS, Horus, CubeSat) and watchlist alerts (SARSAT, Inmarsat priority 3, sonde landed); Iridium call Play button; QO-100 callsign/snippet columns; Radiosonde/Horus CSV and GPX. Navigation: `msglog_ui::bar` on Time minutes, e-Czas, SBAS, NDB; fill empty DGPS/eLoran/DME sidebars; NDB history with time/kHz/level; GNSS fix CSV/GPX/NMEA/RINEX buttons; ident events.
Big items: Pass object (4.13); QO-100 and Iridium onto Session; auto-record per pass (IQ plus CADU via `sched.h`); Aero/Iridium aircraft into `acars_log`/`track_hub`; balloon flight record joining Horus/Sonde/SSDV; SARSAT beacon alert grouping; beacon database and band scan for NDB/VOR; GNSS session recorder; DGPS RTCM body decode (decoder work); Loran chain identification.

### Wave 6: HF digital, pagers/messaging, IoT, mesh, rail/drones/cell, tools
(The HF digital group audit was a duplicate entry with no content; HF digital must be audited with the same template before this wave. Its natural unit is the QSO via `map/contact_graph.h`, with ADIF in `ham_log_ui.cpp`.)
Pagers/APRS/Multitone quick wins: `msglog_ui::bar` on APRS and Multitone; pager sort/Type filter and fixed/damaged/readHow columns; capcode view first seen/pages per hour/CSV and name table; APRS stations columns (count, alt, course, speed, distance), type filter, Messages-only view; renames (Pages / APRS); tr() and help. Big items: capcode watch kind in watchlist; APRS message threads, weather, telemetry; persistence; rule-based record; Multitone sequence matching (`docs/PAGER_CCIR.md`; needs real recordings).
IoT quick wins: `msglog_ui::bar` on all logs; CSV button per main table; subNav for ANT+, DECT, NFC; show decoded-but-hidden fields; summary headers; tr() and "?" help; sort/filter; result slots for dect, wmbus, zwave, sigfox, nfc, esubghz; remove duplicate Z-Wave/Sigfox sub-views in ISM or link. Big items: generic Device session with `ism.reading`, `zigbee.device`, `wmbus.reading`, `sigfox.uplink`, `nfc.tag`, `esubghz.chat` events; time-series store for readings; PCAP writers (Zigbee, Wi-SUN, HaLow); DECT call-activity sessions (metadata only); Flipper participants; M-Bus user-key decrypt (key store); Sigfox device grouping.
Mesh/LoRa quick wins: Save on LoRa raw/LoRaWAN; chat Save/Auto-save; filters; surface hidden fields; summary header; unify tabs; carry replyId/senderTime/wantAck; configurable caps; LoRa events. Big items: thread/ack tracking; auto-record rules; mesh topology and delivery; persistence; GPX/KML/PCAP exports; merge the two LoRaWAN UIs; LoRa raw clustering.
Rail/drones/cell quick wins: Save button on NR, GSM-R, RID, DJI, BLE, Wi-Fi, LBJ, KLW/TDCS, EBICab; time columns for KLW/TDCS and LBJ; throttle `ble.device` event per address (flood risk); ebicab result slot; fix LBJ longitude/latitude label order; DJI map layer. Big items: drone flight records (RID plus DJI, KML/GPX per flight, serial alert); train tracking; NR SIB1 decode (decoder work); cell family unification; ETWS/CMAS text decode.
Tools quick wins: `msglog_ui::bar` on KA tables; Clear buttons; KAPP hash and UTC; KAFT default save folder; KAIT picture strip plus auto-save; KAAL stations-heard table; remove duplicate Export in Workbench. Big items: KA conversation layer (events kaal.call, kapp.message, kaft.session, kait.picture, kaws.chat, kaws.beacon, kavb.stream; add families to the msgdb tap mask at app/panels/msgdb_ui.cpp line 78); KAAL call log and recording through the shared strip; analysis session; spectrum tools merge; Plugin/Mode Builder events.

Each wave ends with: tests (section 8), docs/PATHWAY.md entry, graphify refresh, and owner review of screenshots before the next wave. Keep at most four to five workers at once (feedback-mac-load); one build at a time.

## 6. Honest limits

- Inmarsat voice: the Inmarsat all-carriers mode (208, docs/INMARSAT-REVAMP.md) and Aero decoders do not decode voice today (`notDecodedNote()`: C-channel voice unsupported, R/T channels not started). There is no public specification for the voice channel, and no demodulator exists. The framework will track the voice carriers and their calls anyway (carrier, assignment signalling units from `aero_su.h`, start/end, level, aircraft/ES where known) as Sessions with verdict "unreadable"; audio appears only if a demodulator and a user-installed codec are ever added (CodecHelp::AeroAmbe route). Inmarsat-C messages and Aero text are fully usable now.
- Encrypted voice (P25, DMR, NXDN, TETRA): metadata sessions only; audio only for unencrypted or user-key-decryptable calls.
- DECT voice stays undecoded; sessions are activity metadata from `DectBase.slotsMask`.
- Analog TV, FPV, MAC: there is no video recorder; "auto record" for these needs a small video writer (Wave 4, M).
- Globalstar is encrypted CDMA: sessions are activity events only. VDES VDE-TER content is not decoded: sessions are bursts, with IQ capture.
- Trunking follow for MPT1327/LTR needs a frequency plan from the user; EDACS layout is an assumption (`layoutAssumed`), so ids are marked unverified.
- Position and name enrichment that depends on databases (OpenCelliD, coast stations, NDB/VOR, TMC location tables, MW/SW schedules, county shapes, DCS directory) needs a licence check and a user import; none is bundled by this plan.
- Decoder work not covered by the framework: NR SIB1, DGPS RTCM bodies, ETWS/CMAS text, ATSC 3.0 MMTP/ESG, FIS-B weather, VDL4 payloads, CPDLC, EBICab payloads, Loran chain tables, TETRA SDS. Each is listed under its group as a separate decoder item; the framework simply shows what the decoder provides.
- Speech-to-text transcripts are only as good as the model; labelled as such; opt-in.
- Several audit claims ("verify" notes, columns not read line by line) are unconfirmed; the first task of each wave is a short code check before building on them. The audits were read-only, nothing was built or run.
- Real-world validation: modes marked simulation-only or generated-signal-only (KAWS, PSK, Multitone, VDES, some trunking) cannot validate presentation choices on live data.

## 7. Open questions for the owner

1. Retention defaults: audio 30 days / 5 GB, logs 90 days, IQ 7 days / 10 GB. Acceptable, or different?
2. Should the cold store (JSONL sessions) be on by default for all modes, or only when Record or Auto-save is switched on? Proposed: sessions on by default (metadata is small), audio and IQ off by default.
3. Personal identifiers (BLE, Wi-Fi, ANT+, NFC UID, DECT): default to hashed/aggregated as proposed, with a per-mode "Keep identifiers" switch?
4. Transcripts into msgdb: allow text from unencrypted analog voice (Airband/Marine) into View > Messages, with the voice-text flag? msgdb currently refuses voice content.
5. Tab rename is visible to every user (Calls to Conversations in some modes, Activity to Log). Keep the wording "Calls" for voice modes and "Threads" for message modes, with the generic name only internal? Proposed: yes, per-type user-facing names.
6. `voice::Monitor` migration: replace its in-memory history with the Session store in Wave 0 (more risk, best result) or keep both for one release and bridge?
7. Priority between waves: voice and trunking first as proposed, or aviation first?
8. Whether to run the HF digital audit (missing) before Wave 6, and whether FT8/WSPR QSO logging joins this framework through `ContactGraph`.
9. New watchlist kinds (Capcode, Talkgroup, Tone, Session): OK to add?
10. Video capture for analog TV/MAC/FPV: MP4/MJPEG writer, or numbered PNG as FPV does now?
11. Which data directory layout migration is acceptable (old `recordings/`, `audio-recordings/`)? Proposed: leave old files, index them in recmgr, write new ones to `library/`.
12. Release target: ship Wave 0 and 1 as 3.4, rest in 3.5 and later?

## 8. Test plan

### 8.1 Unit tests (tests/core, tests/app; names proposed)
- `test_session_model`: begin/update/end, id stability across restart (same key and startWall give the same id), gap grouping rules per mode (voice hang time, pager window, ACARS msgNum reassembly, HFDL logon/logoff, APRS ack pairing, DSC distress grouping, pass AOS/LOS), encrypted verdict stores no text or audio link, NaN position handling.
- `test_session_persist`: `results_io` round trip and signature guard; JSONL append and reload; sidecar notes merge; retention cleanup keeps starred/noted; corrupted line skipped.
- `test_record_rules`: trigger combinations, schedules, disk guard stops recording, per-rule caps, encrypted never records audio, default rule set, rule serialisation in presets.
- `test_recorder_hook`: pre-roll, hang, min length, file naming template, tags in WAV/FLAC/MP3, recmgr registration.
- `test_session_events`: each emitted `session.*` event has required fields; msgdb insert and `session_id` filter; voice content refused; throttle (BLE per-address).
- `test_export`: CSV/JSON/KML/GPX output for sessions; bundle excludes keys; redaction applied.
- `test_mode_numbers` stays green (tests/app/test_mode_numbers.cpp); add `test_tab_names` that every mode's tabs come from the standard list and are tr() keys.
- Per-mode adapters: replay existing fixtures (dvbt_real, DMR/P25/NXDN/dPMR vectors, ACARS/VDL2/HFDL/AIS vectors, generated signals) through the decoder and assert the sessions produced (counts, ids, durations).
- Real-world impairments (rule): run adapters with frequency offset, off-centre capture, wrap-around timestamps, reserved bits, all parameter combinations; sweep every mode of a group; never claim fixed from loopback.

### 8.2 GUI screenshots (no Zoom/screen capture of the user's screen)
- Run a renamed copy of the app (feedback-isolate-gui-test-settings: copy of dect2 under another process name) with fresh settings, `--hidden --mute --shot`, so real settings and the user's Zoom are untouched.
- Per mode type, one screenshot per standard tab and for the empty state, the header with recording on, the rule editor, the conversation detail. Downscale with `sips -Z 1000` before reading, read each image once per render (CLAUDE.local.md rule), crop regions with `sips -c`.
- Check in light and dark theme, narrow and wide windows (wide carriers table), with long names and non-ASCII text.
- Accessibility: tab order and tooltips; every new string present in the English table; run the tr() lint and the i18n check (docs/I18N.md).

### 8.3 Large-log performance
- Synthetic store with 100,000 sessions and 1,000,000 log rows per mode: list scrolling stays at 60 fps (clipped rows only), search returns in under 200 ms (indexed by id and time; free-text via msgdb FTS), sort under 300 ms, filter chips instant.
- Cold-store start-up: loading the last 7 days under 1 s without blocking the UI thread (load on a worker thread, show "loading").
- Write path: 1,000 session events per second sustained without dropping and without audio underruns; JSONL writer batched; event bus tap does not stall the decoder thread (bounded queue, drop counter shown).
- Memory ceiling: ring of 10,000 sessions per receiver under 50 MB; whole-span capture with 100+ carriers (wide carriers table) stays within the CPU budget (feedback-mac-load: measure on the Mac, one build at a time, run long tests in the background).
- CI: run unit tests via GitHub (`ci.yml`/`ci-full.yml` dispatch), not on the laptop (feedback-tests-on-github-not-laptop); Windows and APK build checked before release.

### 8.4 Acceptance per wave
Owner checklist: the same tab names and order across the wave's modes; every list survives a mode change and a restart; one click exports; one click starts a rule; no unexplained empty screens; no raw English literals outside tr(); docs/PATHWAY.md entry written.
