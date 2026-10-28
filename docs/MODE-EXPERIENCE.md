# Mode Experience Standard (design, 3.4, revision 2)

Status: design only. No code in this branch. Written from the group audits of 2026-10-28 (digital voice, analog voice, aviation, maritime, satellite and science, TV and video, broadcast radio, pagers and messaging, IoT/ISM, navigation and time, mesh/LoRa, rail/drones/wireless/cell, tools/custom) and the existing shared infrastructure. Every file named below was named in those audits; items marked NEW do not exist yet. Where an audit said "verify", the first task of the owning wave is to verify before building.

Revision 2 answers three critic reports (UX consistency, feasibility/performance, legal/privacy). Section 9 lists each change and where a request was declined with a reason.

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

Every mode has one primary type (some a secondary). The type decides which items the mode shows. The standard is a contract: same visible tabs, same names per type, same toolbar, so the user learns it once.

### 1.1 The common frame: at most four visible tabs

Implemented once as `ModeFrame` in app/ui/kit (NEW `app/ui/kit/mode_frame.h/.cpp`), called from each mode's `tab()` hook in app/panels/modeui.cpp. The frame is a `subNav` with at most four entries, so narrow ImGui panels always fit.

| Slot | Visible as | Contents |
|------|-----------|----------|
| 1 | Live | Right now only: now talking, picture being received, constellation, now playing, active tracks. Always the default tab. |
| 2 | Items tab (name per type, 1.2) | Home of everything heard: finished and in progress, one row per Session, active rows pinned on top. |
| 3 | Map | Only where positions exist. Shared layer app/map. |
| 4 | More (dropdown) | Log, Stats, Signal, Advanced. Collapsed by default. A beginner never needs it. |

Inside More: **Log** = raw events, frames and messages, with search, filter, wall time, Save and Auto-save (`msglog_ui::bar`); **Stats** = statistics strip and timeline; **Signal** = receiver diagnostics (existing `receiver()` content); **Advanced** = decoder tuning, raw frames, FIC/FIG trees, Settings tabs. Advanced is an entry in More, closed until opened, never a peer tab. Slot order and meaning never change. A mode with one thing to show keeps one tab and still gets the header and Save.

Tabs are `subNav` (not RadioButtons or `typo::beginTabItem`); ANT+, DECT, VDL4, UAT, NFC, Astro move to `subNav`. Names go through `tr()`.

Where each item lives (one rule, no overlap):
- **Live** = what is happening this second. A "now talking" row links to its Items row: clicking selects that Session in the Items tab.
- **Items tab** = the home of every finished or running thing. The user looks here for "the thing I just heard".
- **Log** = raw material under More. Example: DMR link-control messages and data frames are Log; the call they belong to is an Items row.

Examples: DMR (Live = slot 1 and 2 talking now; Calls = "TG 91 Worldwide, 12 s"; Log = LC and data frames). ACARS (Live = last message; Threads = aircraft 4CA7B1 with 5 messages; Log = every block). ADS-B (Live = table; Tracks = flights; Log = squawk changes).

### 1.2 User-facing names per type (decided)

"Session" and "Conversation" are internal names only. The user sees:

| Type | Items tab name | Other special views, mapped into the four slots |
|------|---------------|--------------------------------------------------|
| V Voice | Calls | Roster and Positions: detail pane and filter chips of Calls; voice strip in Live |
| M Message | Threads | Station and aircraft tables: inside Live |
| T Track | Tracks | Table with detail: Live; squawk and event log: More > Log |
| D Device | Devices | Frames: More > Log; join sessions: Devices detail |
| B Broadcast | Programmes | Services table (search, PTY, favourites) as a left pane of Live; slides and station card: Live |
| P Picture or pass | Passes | Gallery is the Passes tab (thumbnail strip is the list); packets per pass: More > Log |
| S Service | Services | Player: Live; Guide, Recordings: filter chips in Services; Streams: More > Advanced; TS and PSI events: More > Log (Type S has a Log under More); reception sessions later (3.5) |
| O Beacon or observation | Heard | Observation sessions later (3.5); Log under More |
| X Tool | tool tabs unchanged | Export menu and tr() names only; no Items tab |

Old names map: Activity, Call log, Calls, Messages, Packets, Frames become Items or Log as the data demands (Airband Activity becomes Log; DMR Calls stays Calls; M17 Activity becomes Calls).

### 1.3 Status header: one line with a priority rule

NEW `modeHeader()` in app/ui/kit, fed by the existing `*Summary()` helpers (`dmrSummary`, `airbandSummary`, `marineVhfSummary`, `tonesSummary`, `inmcSummary`, `aeroSummary`, `ridSummary`, `bletrkSummary`, `cellSummary`) and the common telemetry block (state, snrDb, blocksOk/blocksBad, dataValid).

Fields in priority order, dropped from the end as `ImGui::GetContentRegionAvail` shrinks: 1 lock lamp, 2 mode name, 3 Record button and state, 4 last heard, 5 counts (calls today, active now), 6 SNR, 7 rate, 8 freq and bandwidth. Hidden fields 6 to 8 move into the lamp tooltip. Below 360 px only 1 to 3 remain.

Relation to existing UI: the header replaces the per-mode summary strip at the top of each mode panel. It shows read-outs only (frequency is displayed, not edited), so it does not duplicate the toolbar controls. The old strip is removed in the same change that adds the header for a mode, so there is never a third copy.

### 1.4 Export: one obvious button, formats behind it

- Every Items row has a visible **Save** button (not only a context menu): it writes the primary file of that item (audio for a call, text for a thread, GPX for a track).
- The detail pane has one **Save this call** button (name per type) that writes the item's files plus a JSON sidecar into the library folder. A small drop-down beside it lists the formats: CSV, JSON, text, KML/GPX/GeoJSON (when lat/lon exist, via `xport::GeoTrack`), audio WAV/FLAC/MP3 (`audio_rec.h`), PNG (gallery), transcript TXT/SRT (`TranscriptStore`), PCAP (802.15.4, LoRaTap, 802.11ah where frames are kept).
- Above the list: **Export list...** (NEW `exportMenu()`, wraps `resexp::saveButton`, `xport::*`, `msglog_ui::bar`) offers only formats the data supports.
- The zip session bundle is later (3.5). Safety rules for every export: 2.7.

### 1.5 The two-click path (acceptance)

- **Record a talkgroup, channel, capcode or station**: one click on the row's **Keep** toggle ("Keep this talkgroup") creates a rule (3.2). If recording is off, the master Record toggle in the header is the single other click, needed once.
- **Master Record**: one toggle in the header. Default rule: everything unencrypted that squelch opened for voice modes, excluding the categories in 3.6. Clicking the indicator while on opens the Record rules dialog (not a tab).
- **Export a conversation**: row Save = 1 click; or select row, then **Save this call** = 2 clicks.
- **Play**: the row's Play button = 1 click.

### 1.6 Item title and columns (same rule for every type)

Each Session has one human title built by its adapter (`Session::title`, 2.1): "DMR TG 91 Worldwide, 12 s", "Aircraft 4CA7B1 voice, 40 s", "Capcode 1234567, 3 pages". Default columns, at most six: Time (wall time, UTC/local via `timefmt`), Title, Who (source to target with alias), Duration, Quality (level/BER or SNR), Actions (Play, Keep, Save). Frequency, slot, flags, tags, notes and position sit behind a column chooser. Default sort: active first, then newest first. Active rows show a pulsing lamp and a growing duration; finished rows are static.

### 1.7 Tracked but not decodable (Inmarsat, Iridium voice, Globalstar, DECT, encrypted)

These Sessions show a lock or antenna icon and the sentence: "Tracked, cannot be heard: this signal is seen and timed, but its voice is not decoded (or is encrypted). Time and details are saved." The Play button is replaced by **Why?**, which opens the mode's "?" help (what is missing: demodulator, codec or key). Save still works (metadata). The empty state uses the same wording so the user never thinks the mode is broken, and the header shows "Voice: tracked only".

### 1.8 Type definitions

The type lists below describe the data each type needs. Where a list says "Conversations = X" read it as the Items tab of 1.2 (named X); tab names and slots there are overridden by 1.1 and 1.2.

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

### 1.9 ImGui layout

- Items tab: list on the left (about 60 percent), detail pane on the right, a draggable divider remembered per mode in the UI settings. Below 720 px the detail becomes a slide-over opened by selecting a row, with a Back button; below 360 px the list is a single column of two-line cards.
- The Record rules editor is a modal dialog (one at a time), opened from the header Record indicator or a row's "Rules..." entry. It is not a docked pane and not a tab.
- Every table uses `ImGuiListClipper` and sortable, resizable, scrolling table flags; no table builds rows outside the visible range.
- Keyboard: Up/Down moves selection, Space plays, Enter opens detail, K toggles Keep, S saves, / focuses search, Esc closes slide-over or dialog. Tab order: Live, Items, Map, More. Every control has a tooltip.
- Filter chips (Active, Starred, Encrypted, With audio, Today) are toggle buttons above the table; saved filters are named entries in a combo beside the search box, stored in the UI settings.

### 1.10 Advanced settings, empty states, help

- Anything expert (demod tuning, raw frames, tree views, Settings tabs, FIC/FIG, symbol dumps, decoder thresholds) sits under More > Advanced. No tuning in the default flow (feedback-ease-of-use).
- NEW `emptyState(modeId)`: an empty list shows one sentence on what the mode is listening for, which frequency/antenna is needed, and a "?" button (text in `app/ui/learn.cpp`). No empty table without explanation.
- Every new control gets `tr()` and a tooltip; every tab gets a "?" help entry in learn.cpp.
- No limit on bandwidth or sample rate is introduced by any component; the session layer is independent of rate.

## 2. The Conversation model

One core model, `Session` in code, shown to the user as Call, Thread, Track, Pass, Programme, Device or Observation (1.2). It generalises `voice::CallEntry`/`CallInfo`, `trunk::CallRecord`, `IridiumCall`, `Qo100Conversation`, `MarineVhfCall`, `AirbandActivity`, `AcarsLog` buckets and `ContactGraph` completion state. It does not replace per-mode decoders; they fill it.

Scope for 3.4: kinds Call, Thread, Track; DMR and Airband adapters; list, detail, one-click Keep and Save; export CSV/JSON/WAV. Kinds Pass, Programme, Device, Reception, Observation, Alert, Analysis, TV reception sessions and the zip session bundle are explicitly later (3.5 and after).

### 2.1 The record

NEW `core/include/dect2/session/session.h` (namespace `dect2::sess`). Variable text is `std::string` (results_io supports it), so long callsigns, DevEUI+DevAddr pairs and UTF-8 names are never cut.

```
struct Session {
  uint64_t id;            // (startDayUtc << 24) | per-day per-mode sequence, assigned once at begin()
  uint16_t mode;          // mode number (docs/MODE-NUMBERS.md)
  uint16_t receiver;      // receiver instance index (2.2a)
  uint8_t  kind;          // Call, Thread, Track (3.4); Pass, Programme, Device, Reception, Observation later
  std::string key;        // natural key: "TG 1234/slot1", "capcode 1234567", "ICAO 4CA7B1", "MMSI ..."
  std::string title;      // human title (1.6), built by the adapter
  std::string who[2], alias[2];   // source and target ids and aliases
  int64_t  startWall, endWall;    // UTC ms; endWall==0 means active
  uint32_t durationMs;
  float    levelDb, levelPeakDb, snrDb, errRate;
  uint64_t freqHz; int32_t carrierId; int16_t slot;   // -1 when not applicable
  double   lat, lon; float altM;                      // NaN if none
  uint8_t  verdict;       // clear / encrypted / unreadable / test (mirrors voice::Verdict)
  uint16_t flags;         // emergency, lateEntry, priority, watched, starred, recording, orphanClosed
  uint32_t eventCount;
};
struct SessionLink { uint64_t sessionId; uint8_t type; /*Audio,Image,Message,Position,Transcript,Iq,Telemetry*/ std::string ref; int64_t wall; };
```

Changes from revision 1: `note`, `tags`, `audioIdx/imageIdx/textIdx` are gone from the struct. Notes and tags live in the store's `notes` table (2.2); everything that points at files, images or messages is a row in the single `links` table (`SessionLink`).

Id rule: the id is assigned once at `begin()` from the UTC day and a monotonic per-day per-mode sequence kept by the store, so two sessions with the same key starting in the same millisecond never collide. If `startWall` is revised later (late entry, ACARS reassembly) the id does not change; only `startWall` does.

Rules for the model:
- A decoder calls `SessionStore::begin(key, ...)`, `update(id, ...)`, `end(id, verdict)` and `link(id, type, ref)`. These calls only enqueue; they never touch disk and never block (2.2a).
- Gap/grouping rules are per mode and live decoder-side (NEW `session_rules.h`): voice = hang time; Airband = same channel and quiet gap N seconds, joined by callsign (transcript); Marine = ch16 call plus the working channel hop (DSC/MMSI match); ACARS = registration plus msgNum/msgSeq with ETB/ETX reassembly; pager = capcode plus 10 min window with repeat merge; HFDL = logon to logoff (`hfdl_rx.cpp` `acIcao` map); VDL2 = AVLC link (XID to disconnect); APRS = pair plus message number plus ack; LoRaWAN = DevEUI/DevAddr join to uplink/ack; DSC distress = alert plus relays plus acknowledgements by `distressMmsi`; passes = AOS to LOS from `passes_ui.cpp`; tracks = same ICAO/MMSI/serial across gaps shorter than a configurable limit.
- Split and merge quality is a decoder-quality item: each adapter has an acceptance test under frequency offset and fading (8.1).
- Encrypted or unreadable content creates a Session with that verdict and metadata only. Never audio, text, raw frames or IQ (3.6).

### 2.2 Persistence: one indexed store, a small hot window

Revision 1 used append-only JSONL plus a 10,000-slot ring in the results blob. That is replaced.

1. **Session database (cold store, indexed).** NEW `core/src/session/session_db.cpp`: a SQLite file `<dataDir>/sessions/sessions.db`, using the same SQLite library as `core/src/msgdb/msgdb.cpp` but its own file and its own limits. It never shares the 1,000,000 row / 512 MB cap of `msgdb::Db`. Tables: `sessions` (primary key id; indexes on (mode, startWall), (mode, key), (startWall)), `links`, `notes` (id to note/tags, editable in place), and FTS5 over title/who/alias/key. **Upsert by id**: begin inserts with endWall=0; update and end replace the row, so there are no duplicate begin/end records. **Crash recovery**: the store writes a heartbeat; on open, rows with endWall=0 and a stale heartbeat are closed with `orphanClosed` and endWall = last update time. JSONL is an export format only (2.6). Limits: per-mode row quota (default 200,000, oldest finished unstarred first) and a total size cap (default 2 GB, lowerable); reaching a cap is announced in the header and never touches library audio.
2. **Hot window in the results slot.** `DECT2_RESULT_SLOT` (core/src/results/engine_results.cpp) keeps only the most recent window (default 200 sessions per mode, about today's 100-300 caps). `Engine::saveResults` re-serialises and hashes every slot's blob every 30 s under `resIoMu_`; a 200-row window keeps that cost unchanged. Older sessions load on demand from the database. A Session struct change invalidates the signature and drops the window only; the database is the truth and refills it, so nothing is lost.
3. **Query API, not whole-list telemetry.** A mode's `*_tel.h` struct (for example `DmrTelemetry::callLog`) carries only the window and a `sessionGen` counter. The UI asks `SessionStore::query(mode, offset, limit, filter, sort)` and re-queries only when `sessionGen` changed, keeping a cached page for the `ImGuiListClipper`. `rx_json.cpp` and the remote API serve a window (last N, N <= 200) or a paged query, never 10,000 rows. Filtering and sorting run in SQLite on a reader thread; the UI thread never sorts a large list.
4. **Index and events.** When a Session ends (not on every update) the store emits one rate-limited `au::Event` of type `session.<kind>` with mode, freq, ident (key) and text (title). `extra` carries only ids and counts, never aliases, tags, notes or text. `app/panels/msgdb_ui.cpp` taps it into `msgdb::Record`; msgdb gets a `session_id` column (`Record`, `Filter`) so View > Messages can jump to the mode's Items tab. msgdb stays voice-content-free and gets a per-family row quota (new `Limits` field) so a chatty family cannot evict the others from the shared cap. Anything put into `extra` passes the redaction rules of 2.7.

#### 2.2a Threads, ownership, bus and families

- **Ownership.** `voice::Monitor` is a process-global singleton per mode name; the SessionStore key is (mode, receiver instance). For 3.4 the Monitor is a bridge, not replaced: it keeps its history and policy lists, and its call open/close hooks call `SessionStore::begin/end` (enqueue only). Replacement is a 3.5 decision after the DMR pilot is proven. With two receivers on one mode (wide capture, multi-SDR, KAAL) each has its own key; the Items tab shows the receiver the panel is bound to, with an "all receivers" chip.
- **Writer thread.** One writer thread and a bounded queue (default 8192 items) with a drop counter, shown in More > Signal and as a header badge when non-zero. Decoder and audio threads only enqueue; no SQLite, file I/O or recmgr registration ever happens under the Monitor mutex. Commits every 250 ms or 256 items.
- **Event bus.** `automation::Engine` has `kQueueMax = 2048` and silently drops events when full (core/src/automation/engine.cpp:415). Persistence never goes through the bus. Only the end-of-session summary goes on it, rate limited (default 50 per second per mode); `dropped_` is surfaced per family in the UI.
- **Families (Wave 0a, first task).** `au::Family` (core/include/dect2/automation/engine.h:49) has 18 entries and engine.cpp builds the mask as `uint32_t` with `1u << f` (lines 290-295); `app/panels/msgdb_ui.cpp` line 78 builds the tap mask the same way. Widen to `uint64_t`/`1ull << f`, update `names[]`, `familyOf`, `wants()` and the tap mask, add a static_assert `kFamilies <= 64`, and add new events under a few families (voice, tv, broadcast, iot, mesh, cell, kaal, session) rather than one per mode.

### 2.3 Audio, images, messages, positions linked

- Audio: `SessionLink{Audio}` points at the file written by the recorder hook (3.5); `voice::CallEntry.file` becomes this link.
- Images: gallery keys (`gallery::publish` key such as `meteor-<start>`); later wave.
- Messages: ids into the mode's Log table and msgdb Record ids.
- Positions: LRRP, NMEA, D-PRS, M17 positions, ADS-B points; kept as a time-stamped track in the links table so GPX/KML export works. History for personal-position modes is opt-in (2.7).
- Transcript: `TranscriptEntry` gets `sessionId` and `audioRef` (core/include/dect2/transcribe/transcript.h); text only for unencrypted analog voice and only when the user enabled transcribe; not copied to msgdb unless the user opts in per mode (section 7, decided).
- IQ: a `recmgr::Meta` entry with SigMF, linked as `Iq`; only by explicit rule (3.1).

### 2.4 The shared UI

NEW `app/ui/kit/session_list.h/.cpp` and `session_detail.h/.cpp`, generalised from `app/voice/voice_monitor_ui.cpp` (history tab, `vmsearch` box, filters). Today that code calls `Monitor::history(query,max)` and copies under the mutex each frame; the new list uses the generation-counter cache of 2.2. Notes and tags are edited in the detail pane (stored in the `notes` table); they are not part of the row and not in exports by default (2.7).

### 2.5 Later: Analysis sessions, Alert kind, Reception sessions

Deferred to 3.5 or later: kinds Analysis (tools), Alert, and TV Reception sessions (4.15). Tools in 3.4 only get the Export menu and tr() names.

### 2.6 Export formats and the bundle

3.4: CSV, JSON, JSONL (export only), text, KML/GPX, WAV (FLAC and MP3 after), per-item Save. Later: the zip session bundle (session JSON, CSV, audio, images, positions, transcript, notes), following `.onairbackup` conventions (core/include/dect2/backup/backup.h), with the safe defaults of 2.7. Sessions are excluded from `.onairbackup` unless the user ticks "Include session history".

### 2.7 Privacy, safety and export defaults

- **Logging is opt-in and visible.** The session database is created and written only when the user turns Record or "Keep history" on, per mode. Off, the in-memory list (hot window, lost on mode change as today) still works. When persistence is on the header shows "Logging: ON" and a first-run notice explains what is saved and where. This withdraws the revision 1 proposal of default-on logging.
- **Metadata first.** The store holds ids, times, quality and titles. Message bodies (pager text, APRS and LoRa chat, ACARS text) are not stored unless the user opts in per mode ("Keep message text").
- **Identifiers.** Personal or tracking-capable identifiers are stored hashed with a mandatory per-install random salt (128 bit, created at first use, never exported), or aggregated, unless the user enables "Keep identifiers" for that mode. Covered: BLE/Wi-Fi addresses, ANT+ ids and heart rate, NFC UIDs, DECT and phone identities, TPMS and tyre-sensor ids, keyless-entry ids, Wireless M-Bus meter ids and readings, Remote ID and DJI operator/pilot location, DMR/P25/NXDN/dPMR radio ids when paired with a position (LRRP), pager capcodes, satellite terminal ids (Iridium, Inmarsat), and aliases imported from RadioID or RadioReference. BLE trackers: identifiers are never written anywhere (`bletrk_book.h` rule stays). FLARM aircraft with the no-track flag are never recorded. The mode's More > Advanced shows the switch "Keep real ids" and its state; while off, rows show a stable short tag derived from the salted hash (for example "dev 7F3A"), so devices stay distinguishable in the list.
- **No tracking of people.** The entity roster (4.11) is per mode and per receiver. There is no cross-mode identity merge, no "person" entity and no automatic following of one id across modes. Notes, tags and labels are excluded from exports and bundles by default. A rule or watch-list entry combining a single id with a position-bearing mode (LRRP, APRS, AIS, Remote ID, DJI, satellite terminals) shows a visible warning in the rule editor and is never created by default. Position history for those modes is opt-in with a 30-day maximum (ordinary logs: 90).
- **Redaction.** `acarsRedact` becomes one "Redact message text" switch for pager text, APRS messages, LoRa chat, ACARS text and notifications, applied the same in table, export, store and notifications.
- **Export safety.** CSV, JSON and SRT writers escape cells that start with =, +, - or @ (leading apostrophe in CSV) and strip control characters, because radio text is attacker-controlled. Exports and the later bundle have an **Anonymise** option (strip ids, aliases, notes, receiver location): on by default for bundles, off for a plain CSV the user explicitly saves. Keys and key ids are never exported.
- **Storage hygiene.** The database, sidecars and library files are created owner-only (0600/0700 on POSIX, user ACL on Windows). A one-time warning appears when `dataDir` is in a synced folder (iCloud, Dropbox, OneDrive, Google Drive, detected by path). Settings has **Delete all data for this mode** and **Delete everything** (with a confirm). Trash is bounded: 7 days by default, then permanent.
- All retention numbers are maxima the user can lower or switch off.

## 3. Auto-record

### 3.1 Rules

NEW `core/include/dect2/session/record_rules.h`, evaluated by the session store when a session begins, updates and ends. A rule = trigger plus action plus scope.

3.4 triggers (simple): everything unencrypted that squelch opened; a talkgroup, channel, capcode, callsign, MMSI or ICAO (one click from a row's Keep toggle); emergency flag. Later (3.5): AND combinations, level/error-rate/duration, keyword in text/transcript (watchlist Kind::Words), events (new device, squawk 7500/7600/7700, DSC distress, SAME code, TA flag, satellite AOS), schedules and cron through `core/include/dect2/sched/sched.h`.

Default rule when the user turns Record on: voice modes: "everything unencrypted that squelch opened" **excluding** the categories of 3.6 that carry private calls (telephony-like categories stay off until the user adds them); message modes: nothing but the session metadata (bodies need "Keep message text", 2.7). Nothing else is on by default.

Actions:
- **Audio**: per call/transmission/programme item (WAV first; FLAC/MP3 via `core/include/dect2/audio/audio_rec.h`), pre-roll from `audio/clip_ring.h` (default 3 s; the ring is mono 16-bit audio at the output rate), hang time as the mode defines. Audio is never saved for encrypted or unreadable verdicts.
- **Log**: sessions go to the database (2.2); the plain-text daily log uses `msglog::AutoSaver`. `msg_log.h` is a header-only text-line writer with fixed columns that opens per append; it is not extended for sessions. A high-rate writer is the SessionStore writer thread (2.2a).
- **Save object**: image, file, slide, LOT/MOT object, FIS-B product, NAVTEX text, chart (existing savers: `NavtexSaver`, WEFAX archive, KAFT folder, gallery). Later.
- **IQ clip** (later, explicit rule only): no IQ ring exists. `AudioClipRing` is audio only. Design: IQ is recorded post-trigger (from the trigger onward, default 10 s) at the channel rate of a decoded carrier (the `<name>_48k.cf32` style baseband `GeneralReceiver` already writes), or per pass; there is no wide-band pre-roll (10 Msps cf32 is about 80 MB/s). The rule editor shows the estimated size (rate x 8 bytes x seconds) and warns when it exceeds the free space or a configurable budget (default 512 MB per clip); it never refuses. IQ is never saved for encrypted or unreadable verdicts, and the rule shows a notice that IQ also contains neighbouring channels.
- **Track**: save positions as GPX/KML at session end (opt-in for personal-position modes).
- **Notify**: watchlist channels (desktop, sound, ntfy, Telegram, webhook) through `core/include/dect2/watchlist/watchlist.h`; new kinds Capcode, Talkgroup, Tone, Session (approved by default as watch kinds, section 7). Notify payload rules: 3.7.

### 3.2 Rule editor and the Keep toggle

A modal dialog (1.9) opened from the header Record indicator or a row's "Rules...". Simple mode: ready-made rules (Record everything unencrypted, Record emergencies, Record only my list, Keep this talkgroup, Notify me); Advanced: free combination and schedule (3.5). A row's Keep toggle is the visible form of "Keep this talkgroup/channel/capcode". The editor shows a pre-flight disk estimate: expected files per day from the last 24 h of sessions and the free space.

### 3.3 What is saved where

One folder tree, one naming scheme (replaces `recordings/`, `audio-recordings/`, `audio-recordings/marine-vhf/`, `<data>/goes/`, `dvbs_ip.pcap` fixed names):

```
<dataDir>/library/<mode>/<YYYY-MM-DD>/<HHMMSS>_<freq>_<key>_<alias>.<ext>
<dataDir>/library/<mode>/<YYYY-MM-DD>/<same name>.json      # sidecar, written only when "Save sidecar" is on (default on for Save this call, off for auto-record)
<dataDir>/sessions/sessions.db                              # the session database (2.2)
```

Files are registered in `core/include/dect2/sched/recmgr.h` (`Meta` tags and notes, search, trash with undo, SigMF export). **Index, not scan:** `recmgr::scan` walks the tree (maxDepth 4) on each library refresh, which stalls with thousands of files. The library UI (app/panels/recording_library_ui.cpp, recmgr_ui.cpp) reads the `links` table of the session database as its index and scans only for files not in it (imports, old folders). Auto-record writes no per-call sidecar by default (the database row is the metadata), halving the file count. The folder is chosen once in Settings (a "..." chooser like Marine's), format by one combo with tags in all formats.

### 3.4 Limits and cleanup

- **Recorder budget.** Maximum concurrent encoders (default 8) and open files (default 64, well below the macOS default of 256 descriptors). Encoding runs on a recorder worker pool, never on the decoder thread; WAV is written directly, FLAC/MP3 encode on the pool. When the cap is reached the session is still tracked and a visible "N calls not recorded (busy)" counter appears in the header. Recording is never silently dropped.
- **Disk guard** from `sched.h`: stop at free space below a limit (default 2 GB or 5 percent) and say so in the header.
- **Automatic cleanup is off by default.** The user may set a maximum age or total size for audio, IQ and logs in Settings; when a limit is reached the app warns and asks before deleting (a list of what would go), never deleting silently. Starred and noted items are never deleted. Deleted items go to the trash with undo (7 days, 2.7).
- **Total size cap** across library audio, IQ and the session database is shown in Settings (default advice: keep below 20 percent of the disk; a warning, not a limit); small laptops and Android/APK show the actual free space beside it.
- Per-rule caps: max call length (default 1 h), min length (0.5 s, as Marine), max IQ clips per hour.

### 3.5 Recorder hook

NEW `core/include/dect2/session/recorder_hook.h`: generalises `GeneralReceiver::setTransmissionRecording` (pre-roll, hang, min length, optional channel IQ, `transmissions.csv`, `recTags`) and `voice::Monitor` per-call WAV into one `RecorderHook` driven by any mode's open/close events. General, Marine and Airband plug in; `voice::Monitor` keeps its policy lists (allow/mute/priority/hold/squelch) and calls the hook for storage. `audio_control_ui.cpp` Record button records the mix to the same library with the same naming (the fourth recorder is retired in Wave 2).

### 3.6 Legal and privacy notes (shown in the rule editor and docs)

- Only unencrypted traffic is recorded as content. Encrypted voice and data: metadata only (who/when/duration/verdict); never audio, text, raw frames or IQ. No attempt to crack or auto-decrypt.
- Decryption only with keys the user imported into the key store (`core/include/dect2/voicekeys/voice_keys.h`, `key_persist.h`, app/keys). Keys are never logged, never in session records, never in exports or bundles.
- Patented codecs are not shipped. Voice modes record decoded audio only when the user has installed the codec (docs/CODECS.md). If no codec is installed the call is still tracked; raw frames may be saved **only for verdict clear** calls, never for encrypted or unreadable.
- **Per-category notice.** Laws differ for telephony, cellular, cordless, satellite-phone and pager interception, and for recording versus listening. Categories: telephony and cordless (DECT, PMR446 and baby-monitor style analogue), cellular, satellite voice (Iridium, Inmarsat), pagers (text bodies), and private business radio (DMR/P25/NXDN/dPMR/TETRA users). The first time a rule includes a category, a notice names the category, says the user is responsible for the law of their country, and stores an acknowledgement with the rule (rule field `ackCategory`, timestamp). It is shown again when the system region setting changes. It is never a blocker, never touches transmit, and the acknowledgement is one button.
- Categories that carry private calls (Iridium/Inmarsat voice, DECT, cellular, cordless, pager text) are excluded from the default Record rule until the user adds them deliberately.
- Nothing is uploaded automatically: Rooms, swarm, feeds, uploads and exports happen only on a user action. The session layer itself has no network code; the only paths that leave the machine are the Notify action (3.7), the remote API (3.7), and user exports.
- Transcription (speech-to-text) is opt-in, offline, with an accuracy note.
- Personal-identifier modes: 2.7.

### 3.7 Network egress

- **Notify** (ntfy, Telegram, webhook): the default payload is minimal text (mode, kind, time, title with ids hashed or omitted), no content, no location. Adding ids, text or position is a per-rule switch shown in the rule editor with a warning that it leaves the machine. Triggered only by an explicit user rule.
- **au events to webhooks, rx_json and the remote API**: sanitised payload (2.2 point 4: ids and counts only; no alias, tags, notes, text), authentication required, localhost-only by default; exposing it beyond localhost is an explicit Settings switch.
- Anything that leaves the machine is listed in the rule editor line "Sends to: ..." so the user sees it before enabling.

## 4. Shared components (build once)

Each with reuse, effort (S under 1 day, M 2 to 5 days, L over a week, XL several weeks) and first users. All NEW files live under app/ui/kit or core/include/dect2/session unless stated.

| # | Component | Reuses | Effort | First modes |
|---|-----------|--------|--------|-------------|
| 4.0 | Automation family mask widened to 64 bits (2.2a) | `engine.h`, `engine.cpp`, `msgdb_ui.cpp` | S | all (prerequisite) |
| 4.1a | `Session` model, `SessionStore` (writer thread, SQLite db, upsert, crash recovery, query API, `sessionGen`) | `msgdb.cpp` (SQLite use), `results_io.h`, `engine_results.cpp` | XL (3 to 4 weeks) | DMR via the Monitor bridge |
| 4.1b | Results window, rx_json window, regenerated `results_fields.h`, Android bridge header update, msgdb `session_id` and per-family quota, `session.*` summary events | `rx_json.cpp`, tools/gen_results_fields.py, `msgdb.h`, `catalog.cpp` | L | DMR |
| 4.2 | Rule editor and rule engine (3.4 triggers first) | `watchlist.h`, `sched.h`, `automation/rules.h`, `voice::Settings` lists | M | DMR, Airband |
| 4.3 | Session list widget (clipper, search, sort, chips, Keep/Save buttons, wall time) | `voice_monitor_ui.cpp`, `ism_ui.cpp deviceView` | M | DMR, Airband |
| 4.4 | Session detail pane (timeline, play with scrub, notes, tags, linked items, Save this call) | `voice_monitor_ui.cpp`, `acars_ui.cpp` detail pane, `recording_library_ui.cpp` | M | DMR, Airband |
| 4.5 | Recorder hook, worker pool, budgets, incremental library index | `general_rx.cpp`, `audio_rec.h`, `clip_ring.h`, `recmgr.h` | L | Airband, General, Marine |
| 4.6 | Exporter `exportMenu()` with CSV/JSON escaping and Anonymise | `results_export.cpp`, `exporters.h`, `msglog_ui.h` | S | all; first Airband, Marine, APRS, KA |
| 4.7 | Statistics strip and timeline | ImPlot in airband `spectrumView`, ADS-B `statsView`, `FmTelemetry` history | M | digital voice, Airband |
| 4.8 | Search helper (text + field filter + time range, saved filters) | `lwFilter`, `vmsearch`, `dscMatches`, msgdb `Filter` | S | all tables |
| 4.9 | `modeHeader()` and `ModeFrame` skeleton | `*Summary()` helpers, `subNav`, `mode_table.cpp` | M | DMR, Airband first |
| 4.10 | Empty state, "?" help hook, tr() lint | `learn.cpp`, `tour.cpp` | S | all |
| 4.11 | Entity roster (per mode, no cross-mode merge; name import from CSV) | `trunk.h TalkgroupList`, `ShipDb`, `acars_log.h` | M | later wave 1 |
| 4.12 | Map session layer | `app/map`, `adsbmap`, `dmr_map.cpp`, `track_hub.h` | M | later |
| 4.13 | Pass object joined to sessions | `passes_ui.cpp`, SGP4, `sched.h` | M | later (Wave 5) |
| 4.14 | Event catalog additions per family | `catalog.cpp`, `msgdb_ui.cpp` | S per family | per wave |
| 4.15 | Reception session for service modes and quality logger | common telemetry block | M | later (3.5) |
| 4.16 | Services panel (TV), Programme log (broadcast) | `tv.cpp`, `scan_outputs.cpp`, `recorder.h` | M each | later |

Dependency order: 4.0, then 4.1a; 4.6, 4.8, 4.10 in parallel from the start; then 4.1b, 4.9, 4.3, 4.4, 4.5, 4.2; the rest in later waves. The `*Summary()` helpers differ in signature per mode (not audited line by line): the first task of 4.9 is a one-page table of their signatures for the pilot modes only.

## 5. Per-group plans and rollout waves

Principle: framework first, pilot on two modes (DMR and Airband), then the modes the most people use. Quick wins before big items; each wave unblocks the next. Tab renames happen once, after the Items list exists (Wave 1), not in a quick-win round before it.

### Wave 0a: Store and pilot core (3.4, blocks everything; estimated 4 to 6 weeks)
- 4.0 family mask widening (first task).
- 4.1a `Session`, `SessionStore`, SQLite db, writer thread, query API, crash recovery; unit and soak tests (8.3).
- 4.1b DMR adapter through the `voice::Monitor` bridge; results window, rx_json window, `results_fields.h` regeneration, Android bridge header update (explicit task), msgdb `session_id` and per-family quota, `session.call` summary event.
- 4.8, 4.6, 4.10 as small parallel tasks.

### Wave 0b: Frame and pilot UI (3.4, estimated 2 to 3 weeks after 0a starts)
- 4.9 `ModeFrame` and `modeHeader()`, 4.3 and 4.4 list and detail, Keep and Save buttons, Record toggle, 4.2 simple rule editor, Airband adapter, `RecorderHook` minimal (WAV) for Airband and DMR. Acceptance: screenshots of the two pilot modes (header, Items tab, detail, rule dialog, narrow window, empty state) reviewed by the owner before any roll-out to other modes.
- Quick wins that cost nothing: P25 Phase 2 gets `voiceui::strip` (voice monitor already fed at core/src/p25p2/p25p2_rx.cpp:536); voice monitor history export CSV/JSON; open-recording and play per history row using `CallEntry.file`.
- Whether Wave 1 also ships in 3.4 is decided after the pilot review; the default plan is 3.4 = Wave 0a + 0b, Wave 1 in 3.5.

### Wave 1: Digital voice and trunking (largest user base, owner's example)
Quick wins: standardise the Items tab as Calls (Live, Calls, Map, More; renamed once, after the Items list exists) across DMR/NXDN/P25/dPMR/D-STAR/M17 (M17 Activity to Calls, DMR Table to Positions); wall time instead of signal seconds in DMR, MPT1327, LTR, EDACS; search/sort in each call table; `msglog_ui::bar` on every Messages tab; show hidden fields as columns (NXDN baud/BER/endSeen, DMR cc/privacy/lateEntry, dPMR channel/commMode, M17 type/enc/ext1/ext2, P25 key id); join DMR LRRP position to the call row; P25 Calls tab shows `trunk::CallRecord.audioPath` and Export CSV; fix TETRAPOL "screen only, never saved" label; `tr()` and help in MPT1327/LTR/EDACS/Fusion; remove the quadratic unit list in `nxdn_ui.cpp list()`.
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
(BLOCKED until the HF digital audit is run with the same template; the group audit was a duplicate entry with no content. Its natural unit is the QSO via `map/contact_graph.h`, with ADIF in `ham_log_ui.cpp`.)
Pagers/APRS/Multitone quick wins: `msglog_ui::bar` on APRS and Multitone; pager sort/Type filter and fixed/damaged/readHow columns; capcode view first seen/pages per hour/CSV and name table; APRS stations columns (count, alt, course, speed, distance), type filter, Messages-only view; renames (Pages / APRS); tr() and help. Big items: capcode watch kind in watchlist; APRS message threads, weather, telemetry; persistence; rule-based record; Multitone sequence matching (`docs/PAGER_CCIR.md`; needs real recordings).
IoT quick wins: `msglog_ui::bar` on all logs; CSV button per main table; subNav for ANT+, DECT, NFC; show decoded-but-hidden fields; summary headers; tr() and "?" help; sort/filter; result slots for dect, wmbus, zwave, sigfox, nfc, esubghz; remove duplicate Z-Wave/Sigfox sub-views in ISM or link. Big items: generic Device session with `ism.reading`, `zigbee.device`, `wmbus.reading`, `sigfox.uplink`, `nfc.tag`, `esubghz.chat` events; time-series store for readings; PCAP writers (Zigbee, Wi-SUN, HaLow); DECT call-activity sessions (metadata only); Flipper participants; M-Bus user-key decrypt (key store); Sigfox device grouping.
Mesh/LoRa quick wins: Save on LoRa raw/LoRaWAN; chat Save/Auto-save; filters; surface hidden fields; summary header; unify tabs; carry replyId/senderTime/wantAck; configurable caps; LoRa events. Big items: thread/ack tracking; auto-record rules; mesh topology and delivery; persistence; GPX/KML/PCAP exports; merge the two LoRaWAN UIs; LoRa raw clustering.
Rail/drones/cell quick wins: Save button on NR, GSM-R, RID, DJI, BLE, Wi-Fi, LBJ, KLW/TDCS, EBICab; time columns for KLW/TDCS and LBJ; throttle `ble.device` event per address (flood risk); ebicab result slot; fix LBJ longitude/latitude label order; DJI map layer. Big items: drone flight records (RID plus DJI, KML/GPX per flight, serial alert); train tracking; NR SIB1 decode (decoder work); cell family unification; ETWS/CMAS text decode.
Tools quick wins: `msglog_ui::bar` on KA tables; Clear buttons; KAPP hash and UTC; KAFT default save folder; KAIT picture strip plus auto-save; KAAL stations-heard table; remove duplicate Export in Workbench. Big items: KA conversation layer (events kaal.call, kapp.message, kaft.session, kait.picture, kaws.chat, kaws.beacon, kavb.stream; add families to the msgdb tap mask at app/panels/msgdb_ui.cpp line 78); KAAL call log and recording through the shared strip; analysis session; spectrum tools merge; Plugin/Mode Builder events.


Each wave ends with: tests (section 8), docs/PATHWAY.md entry, graphify refresh, and owner review of screenshots before the next wave. Keep at most four to five workers at once (feedback-mac-load); one build at a time.

Note on cross-repo tasks: modes missing from `DECT2_RESULT_SLOT` also need `rx_json` cases, regenerated `results_fields.h` (tools/gen_results_fields.py) and Android bridge header updates; these are listed as explicit tasks in every wave that adds slots, not "quick wins".

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
- Wave 6 is blocked on the missing HF digital audit: run it with the same template before committing Wave 6 scope.
- Session split/merge under real-world fading and offset can make one call two Sessions or two calls one; the adapters carry measured acceptance numbers (8.1) and the Items tab offers a manual "Merge with previous" later.

## 7. Decisions (formerly open questions)

Decided in this revision so Wave 0 is implementable; the owner may overrule any line.

1. Retention: audio, IQ and log cleanup are off by default; the user sets maxima; the app warns and asks before deleting (3.4). Cold store caps: 200,000 sessions per mode, 2 GB total.
2. Session persistence is **opt-in** (Record or "Keep history" per mode), with a visible "Logging: ON" header state and a first-run notice (2.7). Not default-on.
3. Identifier hashing with a mandatory per-install random salt, plus a per-mode "Keep real ids" switch (2.7). Not a question any more.
4. Transcript text stays **out of msgdb** unless the user opts in per mode; then it carries the voice-text flag.
5. Per-type names (Calls, Threads, Tracks, Passes, Programmes, Devices, Services, Heard); "Conversation" internal only (1.2).
6. `voice::Monitor` is bridged in 3.4, replaced only after the DMR pilot (2.2a).
7. Wave order: voice and trunking first, aviation second.
8. HF digital audit runs before Wave 6; FT8/WSPR QSO logging joins through `ContactGraph` afterwards.
9. New watchlist kinds Capcode, Talkgroup, Tone, Session: approved, with the warning of 2.7 on single-id plus position rules.
10. Video capture for analog TV/MAC/FPV: MJPEG or numbered PNG (as FPV does now). No MP4 with H.264 or AAC; no patented encoder is shipped. TS pass-through for DVB recording stays.
11. Data directory: old folders are left in place and indexed by `recmgr`; new files go to `library/`.
12. Release: 3.4 = Wave 0a + 0b (store, DMR and Airband pilot, one-click Keep and Save). Wave 1 follows in 3.5 unless the pilot review shows it is cheap enough to include.

## 8. Test plan

### 8.1 Unit tests (new directory tests/session, wired with `add_test` in CMakeLists.txt like the other mode test directories; names proposed)
- `test_session_model`: begin/update/end, id assignment (same-ms same-key sessions get different ids, id stable when startWall is revised), gap grouping per mode (voice hang time, pager window, ACARS msgNum reassembly, HFDL logon/logoff, APRS ack pairing, DSC distress grouping, pass AOS/LOS), encrypted verdict stores no text/audio/IQ link, NaN position handling, UTF-8 keys never cut.
- `test_session_db`: upsert, no duplicate rows, crash recovery (kill mid-write, reopen, orphans closed), mid-line truncated/corrupt file recovery, per-mode quota, size cap, notes table edit in place, FTS search, query paging and sort.
- `test_session_store_threads`: writer thread with bounded queue, drop counter, enqueue never blocks (timed), no I/O under the Monitor mutex; run under TSAN in CI.
- `test_family_mask`: 64-bit mask, `*` matching, tap mask, static_assert on `kFamilies`.
- `test_record_rules`: 3.4 triggers, default rule excludes private-call categories, disk guard, per-rule caps, encrypted never records audio/raw/IQ, category acknowledgement stored, rule serialisation in presets.
- `test_recorder_hook`: pre-roll, hang, min length, naming template, tags in WAV, concurrent-encoder cap and drop counter, registration in the `links` table.
- `test_session_events`: summary event fields, `extra` carries no alias/tags/notes/text, msgdb `session_id` filter, per-family quota, voice content refused, throttle (BLE per address), webhook payload sanitised.
- `test_export`: CSV/JSON/KML/GPX output; CSV formula escape and control-character strip; Anonymise; keys never present; redaction applied; sessions excluded from `.onairbackup` by default.
- `test_privacy`: salted hash stable within an install and different across installs; "Keep real ids" off shows short tags; file permissions 0600; Delete all data removes rows, sidecars and files.
- `test_mode_numbers` stays green (tests/app/test_mode_numbers.cpp); add `test_tab_names` (every mode's tabs from the standard, at most four visible, tr() keys).
- Per-mode adapters (DMR, Airband first): replay existing fixtures (DMR vectors, airband recordings, generated signals) through the decoder and assert the sessions produced (counts, ids, durations).
- **Real-world impairments (rule)**: run adapters with frequency offset, off-centre capture, wrap-around timestamps, reserved bits, all parameter combinations, fading; sweep every mode of a group; never claim fixed from loopback. Acceptance numbers for each voice adapter: with +/- 2 kHz offset and Rayleigh fading at 10 dB SNR the call count must equal the clean count within +/- 5 percent and no call may be split by a fade shorter than the hang time. These numbers are set on the DMR pilot and recorded in the test.

### 8.2 GUI screenshots (no Zoom/screen capture of the user's screen)
- Run a renamed copy of the app (feedback-isolate-gui-test-settings) with fresh settings, `--hidden --mute --shot`.
- **Wave 0b acceptance**: header, Items tab, detail pane, rule dialog and empty state of the two pilot modes (DMR, Airband), wide and narrow (360 px, 720 px), light and dark, with long names and non-ASCII text; owner review before roll-out to other modes. Downscale with `sips -Z 1000`, read each image once per render, crop with `sips -c`.
- Accessibility: tab order and tooltips; every new string present in the English table; run the tr() lint and the i18n check (docs/I18N.md).

### 8.3 Large-log performance (named harness, runs on GitHub CI)
- `bench_session_db` (tests/session/bench_session_db.cpp, a CTest label `perf`): synthetic 100,000 sessions in the SQLite store: indexed query of one page under 20 ms, FTS search under 200 ms, filtered sort under 300 ms measured on the reader thread; the UI list is verified to never copy more than one page per `sessionGen` change.
- `soak_session_writer`: 1,000 session events per second for 10 minutes with simulated decoder threads and a fake audio callback: no enqueue longer than 1 ms, drop counter 0 at the nominal rate, drop counter exact above the queue limit, no audio underrun in the fake callback.
- Cold start: opening the database and loading the hot window under 1 s on a worker thread, UI shows "loading".
- The ring is no longer 10,000 rows in memory: the memory ceiling is the hot window (200 rows) plus one cached page per open list, under 5 MB per receiver; a wide capture with 100+ carriers stays within the CPU budget (feedback-mac-load: measure on the Mac only for the final check, one build at a time, long tests in the background).
- CI: run via GitHub (`ci.yml`/`ci-full.yml` dispatch), not locally (feedback-tests-on-github-not-laptop); a TSAN job covers `test_session_store_threads`; Windows and APK build checked before release.

### 8.4 Acceptance per wave
Owner checklist: the same visible tabs and names across the wave's modes (at most four); every list survives a mode change (hot window) and, with Keep history on, a restart; Record, Keep and Save are one click as in 1.5; no unexplained empty screens; no raw English literals outside tr(); docs/PATHWAY.md entry written.

## 9. Revision 2: what changed

Answers to the three critic reports. "Declined" items are justified.

- **UX**: tabs reduced to four visible with a More dropdown and Advanced collapsed (1.1); Live / Items / Log homes defined with examples and a Live-to-row link; per-type names decided with a mapping table including Type S (1.2); header priority and relation to toolbar (1.3); one obvious Save and Keep, click counts (1.4, 1.5); notes moved to the detail pane and "Notes and Rules" removed, the rule editor is a dialog opened from the header (1.9, 3.2); title rule, six default columns, sort, active vs finished (1.6); "tracked, cannot be heard" wording and state (1.7); ImGui layout, narrow behaviour, ListClipper, keyboard (1.9); scope cut to Call/Thread/Track, DMR and Airband, no Analysis/Alert/TV Reception/zip bundle in 3.4 (2); screenshots of the two pilots as acceptance (8.2); open questions 5, 6, 12 decided (7). Declined: none.
- **Feasibility**: SQLite store with upsert and crash recovery replaces JSONL, JSONL is export only (2.2); hot window instead of a 10,000-slot blob, query API with generation counter, rx_json windows (2.2); writer thread with bounded queue, bus carries summaries only (2.2a); family mask widened to 64 bits as the first task (4.0, 2.2a); Monitor bridge and (mode, receiver) key (2.2a); Session uses `std::string`, collision-safe ids, single links table (2.1); IQ design honest about `clip_ring.h` being audio-only, post-trigger only, size warning, never refuse (3.1); recorder budgets, incremental index, size caps (3.3, 3.4); Wave 0 split into 0a/0b with larger estimates and cross-repo tasks listed (4, 5); msgdb quota and redaction of session events (2.2); named perf harness, soak, TSAN, corruption tests, split/merge acceptance numbers (8); HF digital audit gate and tab renaming after the list exists (5, 6). Partly declined: the SQLite store lives beside msgdb rather than inside it, so the shared 1M-row cap is untouched; the "Merge with previous" action is later.
- **Legal and privacy**: persistence is opt-in with visible state and a first-run notice (2.7); real network-egress section and sanitised payloads, localhost default (3.7); anti-stalking rules, no cross-mode merge, shorter position retention, warnings on single-id plus position rules (2.7); identifier list extended and salted hashing mandatory (2.7); per-category legal notice with stored acknowledgement and default rule excluding private-call categories (3.6); raw frames and IQ never for encrypted/unreadable (3.1, 3.6); Anonymise, formula escaping, backup exclusion (2.7); 0600 permissions, synced-folder warning, delete-all, bounded trash (2.7); transcripts out of msgdb by default and MJPEG/PNG instead of MP4 (7); general redaction switch (2.7).

## 10. First build steps (for the implementation workflow)

Each item is a branch-sized work item with files, structs and tests. Order matters; items 5 to 7 can run in parallel with 3 and 4 once 2 is merged. No item needs a hardware radio.

1. **Family mask to 64 bits.** Files: core/include/dect2/automation/engine.h (`Family`, `wants`), core/src/automation/engine.cpp (mask building at lines 290-295, `names[]`, `familyOf`), app/panels/msgdb_ui.cpp (line 78 tap mask). Add families `voice`, `session` (more later). Test: `tests/session/test_family_mask.cpp` (`*`, per-family, tap mask, static_assert on `kFamilies <= 64`). Effort S.
2. **Session model and ids.** Files: core/include/dect2/session/session.h (`Session`, `SessionLink`, enums Kind/Verdict/Flags), core/src/session/session_id.cpp (day plus sequence). Test: `tests/session/test_session_model.cpp` (id uniqueness, id stable on startWall revision, NaN position, UTF-8 keys). Effort S.
3. **SessionDb (SQLite) with upsert and crash recovery.** Files: core/include/dect2/session/session_db.h, core/src/session/session_db.cpp (tables sessions/links/notes, FTS5, indexes, per-mode quota, size cap, heartbeat, owner-only file mode). Tests: `tests/session/test_session_db.cpp` (upsert no duplicates, kill and reopen closes orphans, truncated file recovery, paging, sort, FTS, quota). Effort L.
4. **SessionStore with writer thread.** Files: core/include/dect2/session/session_store.h, core/src/session/session_store.cpp (`begin/update/end/link/query`, `sessionGen`, bounded queue 8192, drop counter, batch commit, key = (mode, receiver)). Tests: `tests/session/test_session_store_threads.cpp` (enqueue never blocks, drop counter exact, TSAN job in .github/workflows/ci.yml), `tests/session/soak_session_writer.cpp` (1,000 events/s). Effort L.
5. **Privacy primitives.** Files: core/include/dect2/session/privacy.h, core/src/session/privacy.cpp (per-install salt in the key store folder, salted hash, short tag, "Keep real ids" setting, generalised redaction switch extending `acarsRedact`, CSV/JSON/SRT escaping helpers, owner-only file creation helper). Tests: `tests/session/test_privacy.cpp`, `tests/session/test_export.cpp` (formula escape, control characters, Anonymise). Effort M.
6. **Search helper, export menu, empty state.** Files: app/ui/kit/search_box.h/.cpp (4.8), app/ui/kit/export_menu.h/.cpp (4.6, wrapping `resexp::saveButton`, `xport::GeoTrack`, `msglog_ui::bar`), app/ui/kit/empty_state.h/.cpp (4.10, texts in app/ui/learn.cpp). Tests: tests/session/test_export.cpp (shared), tr() lint run. Effort M.
7. **DMR adapter through the Monitor bridge.** Files: core/include/dect2/voice/voice_monitor.h and its .cpp (open/close hooks calling `SessionStore::begin/end`, no I/O under `mu_`), core/include/dect2/session/session_rules.h and core/src/dmr/dmr_session.cpp (title "DMR TG <id> <alias>, <s>", hang-time grouping), DmrTelemetry (core/include/dect2/dmr/dmr_tel.h) gets `sessionGen` and the 200-row window. Tests: `tests/session/test_dmr_sessions.cpp` replaying existing DMR vectors; impairment sweep (offset, fading) with the acceptance numbers of 8.1. Effort L.
8. **Results window, rx_json window, msgdb column.** Files: core/src/results/engine_results.cpp (window slot), tools/gen_results_fields.py then core/include/dect2/results/results_fields.h, core/src/rx_json.cpp (window or paged query, sanitised, localhost default), core/include/dect2/msgdb/msgdb.h and core/src/msgdb/msgdb.cpp (`session_id`, per-family quota), core/src/automation/catalog.cpp (`session.call`), Android bridge header. Tests: `tests/session/test_session_events.cpp`. Effort L.
9. **ModeFrame, header, list, detail for DMR.** Files: app/ui/kit/mode_frame.h/.cpp, mode_header.h/.cpp, session_list.h/.cpp, session_detail.h/.cpp; wire in app/panels/modeui.cpp and app/voice/voice_monitor_ui.cpp (Calls tab, Keep, Play, Save, Save this call, Record toggle, tracked-only state). Tests: tests/app/test_tab_names.cpp, GUI screenshots per 8.2. Effort L.
10. **Airband adapter and minimal RecorderHook.** Files: core/include/dect2/session/recorder_hook.h, core/src/session/recorder_hook.cpp (WAV, pre-roll from `AudioClipRing`, concurrent-encoder cap, drop counter, `links` registration), airband adapter next to the airband receiver, simple rule editor dialog (app/ui/kit/rule_editor.h/.cpp with the 3.4 triggers and the category acknowledgement). Tests: `tests/session/test_recorder_hook.cpp`, `tests/session/test_record_rules.cpp`. Effort L.

After step 10: owner reviews the pilot screenshots (8.2); then Wave 1 roll-out to NXDN, dPMR, D-STAR, M17, P25.
