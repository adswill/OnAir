# OnAir live map (map.on-air.dev)

Cloudflare Worker + static page. The Worker (`src/index.js`) holds the `MapHub` Durable Object (live tracks, WebSocket, `/live`, `/ingest`),
proxies OSM tiles and serves `/meta` (aircraft type, airline and route from adsbdb, with airport coordinates). The page is in `public/`.

## Files
- `src/msgs.js` - ACARS messages per aircraft: sanitising, limits, merge (ingest `msgs` per aircraft track, at most 5 per track per post, 120 per station per minute, 20 kept per aircraft, 500 characters each, 48 h). `GET /msgs?id=<ICAO hex>` returns them; they are not part of `/live`. The card shows them with textContent only.
- `src/track-book.js` - the hub's live tracks in memory (merge rules, write-behind selection, deltas, expiry); `test/track-book.test.js`.
- `test/load-sim.js` - load simulator (see Cost model); only runs with `--run`.
- `src/doa.js` - direction finding: a `doa` track is a bearing line (station position, `heading` = bearing, `extra` = `{f: Hz, s: sigma deg, c: confidence}`, 10 min TTL, at most 4 per post, no trail); `computeFixes` intersects the newest line of each distinct station on one frequency (within 3 kHz) into a derived, never stored `fix` track (robust ML fit, 95 % ellipse in `extra` = `{a, b, o, n, r, f}`) that is added to `/live` and the WebSocket upserts. Fixes with a 95 % major axis over 300 km are not published.
- `src/by-name.js` - the optional public name (`by`) a station can send in the `/ingest` body: 2-24 chars of letters, digits, space and `_-.`, no web or e-mail addresses, a short reserved and banned list; the server re-checks it, keeps the last contributor per track (`by`, `heard_at`) and answers `GET /api/by/<name>` (case-insensitive prefix, `?exact=1`, at most 200 tracks). A bad name is dropped (data still taken) and the reply carries `by_error`. Names are never logged and no IP is stored. `test/by-name.test.js`.
- `src/jam.js` - GPS interference cells (aggregate only): `POST /jam` `{station, cell: 0.5, cells: [[latIdx, lonIdx, aircraft, degraded]]}` is checked cell by cell (anything else rejects the post, no ids), a station's newest post replaces its earlier one (30 s gap, 600 cells, 2 h), `GET /jam` returns the sum over stations without station ids and without cells under 3 aircraft; kept in one `meta` row (written at most once a minute). `public/jamlayer.js` turns the cells into the coloured squares of the "GPS interference (community)" layer. See `docs/gps-jamming-map.md`. `test/jam.test.js`.
- `src/index.js` - Worker + MapHub. `/meta` returns type, registration, airline, flight number and `route.origin/destination` (with lat/lon).
- `public/index.html`, `styles.css` - page shell and theme (dark tokens, drawer rail and bottom-sheet card on phones).
- `public/app.js` - map, layers, live feed, search (callsign / registration / hex / MMSI, `/` focuses it), stats bar, selection, `#hex=` deep links.
- `public/card.js` - the FlightRadar24-style info card (route, progress, estimated arrival, first seen / estimated departure, picture, data rows).
- `src/trail.js`, `src/trail-store.js` - pure trail rules (limits, thinning, request parsing) and the `TrailStore` that keeps them in the hub's SQLite (`trails` table).
- `public/trail.js` - merges the hub's trail with the points the page saw live.
- `public/geo.js` - pure maths: distance, ETA, progress, departure estimate, squawk meanings, formatting.
- `public/types.js` - pure mapping from ICAO type / ADS-B category / AIS type to an icon shape.
- `public/shapes.js`, `icons.js` - original hand-authored vector icons (SVG paths) and the lazy canvas rendering at device resolution; `icons.js` also holds the altitude colour scale and the image ids. `test/icon-sheet.html` shows every icon on a dark and a light map (serve `web/map` and open it).
- `public/airports.js` - ~290 major airports (fallback coordinates when the route lookup has none).
- `public/df.js` - map features for the DF layers: bearing line, wedge (one sigma) and station dot per `doa`, 95 % ellipse and marker per `fix`.
- `public/demo.js` - `?demo=1` synthetic data.
- `test/` - `node --test` tests for the pure modules.

## Preview offline (demo)
```
cd web/map/public && python3 -m http.server 8765
# open http://localhost:8765/?demo=1
```
The demo has ~60 synthetic aircraft, ships, sondes, APRS stations and satellites, clearly marked DEMO. Tiles load from openstreetmap.org when not on on-air.dev.

## Test
```
cd web/map && node --test test/
```

## Deploy
The owner runs `npx wrangler deploy` in `web/map`. Nothing here deploys automatically.

## Trails (kept by the hub, so a refresh keeps them)
- Each accepted fix with a position is stored in the `trails` table (created with `CREATE TABLE IF NOT EXISTS` inside the Durable Object: no wrangler migration needed). Rows: time, lat, lon, alt, heading.
- Thinning: a point is kept only if >= 5 s and >= 100 m after the last kept one, or >= 5 s after it with a heading change of 8 degrees or more (keeps corners).
- Limits per track: aircraft / FLARM / drones 60 min or 300 points; ships 6 h / 200; sondes 4 h / 1000 (a whole flight); APRS, amateur, DSC 6 h / 100; buoys 6 h / 50; satellites and stations none. Trails go when their track expires; the whole table is capped at 150000 rows (oldest first).
- Writes happen in the same synchronous run as the track upsert (one storage transaction); the last kept point per track is cached in memory and re-read after a restart or prune.
- `GET /trail?id=<track id>` or `GET /trails?ids=a,b,c` (max 60) -> `{ now, trails: { id: [[t_ms, lat, lon, alt, heading], ...] } }`, oldest first.
- `GET /live?trails=1` adds `trail` (same points) to each track that has one. Plain `/live` is unchanged, so the Android app, the Discord bot and the desktop app are not affected.
- The page fetches `/trail` when a track is selected (also from a `#hex=` link after a refresh) or the Track toggle is turned on, and merges it with fixes seen live (deduped by timestamp). The Track toggle is remembered in localStorage.

## Notes
- The aircraft photo is requested from the visitor's browser (Planespotters public API, no key) only after a click, with credit; it falls back to a generic illustration.
- ETA is estimated in the browser from great-circle distance and ground speed; it is hidden when the speed is unusable, the plane is on the ground or the destination position is unknown.
- "Departed about" is only shown when the aircraft was first seen (by this page) on the ground or below 3000 ft within 30 km of the departure airport.
- The hub does not carry geometric altitude, Mach/IAS or a "seen by N stations" count, so those rows are not shown.

## Cost model (what Cloudflare bills, and what the hub does about it)
Prices as read from the Cloudflare docs (Workers Paid): Durable Objects "Requests: 1 million/month included, $0.15 per million";
"Duration (GB-s, 128 MB allocation): 400,000 GB-s/month, $12.50 per million GB-s", billed on the allocated memory "regardless of actual usage";
"SQLite rows read: first 25 billion/month, $0.001 per million"; "rows written: first 50 million/month, $1.00 per million" (deletes count as
rows written); incoming WebSocket messages count at 20:1. Workers: "10 million included per month, +$0.30 per additional million";
"Requests to static assets are free and unlimited." Rows written is the expensive one by far.

What the hub does:
- **In-memory tracks, write-behind** (`TrackBook`). Ingest touches no SQL. Every `FLUSH_MS` (10 s, an alarm) changed tracks are written in ONE transaction, a moving track at most every 30 s, a still one every 5 min, a changed label/squawk at once. Deletes are bulk. SQLite is read once per start.
- No more `(kind, t)` indexes on `tracks` and `trails` (each cost an extra billed row per write); expiry is one `DELETE` with a `CASE` per prune, every 10 min (`PRUNE_EVERY_MS`), not per request.
- Trails: points are buffered and inserted in the flush transaction; minimum spacing per kind (`stepMs` in `trail.js`: aircraft 10 s, ships 30 s, APRS 60 s) which is about `window / cap`, so the drawn line is the same. Corners still count after 5 s.
- Stations and ACARS messages are cached in memory and written in the flush (stations at most once a minute).
- `/live` is built from memory (cached JSON per track, cached array per version), has an `ETag`, and the Worker serves it from the edge cache (`caches.default`, `LIVE_EDGE_S` = 3 s): 1000 viewers cost one Durable Object call per 3 s per data centre. `/meta` is cached 24 h (5 min when adsbdb knew nothing).
- `GET /live?since=<version>` returns `{ delta: true, version, tracks: [changed], removed: [ids] }` or, when the version is unknown/too old (10 min log, or the hub restarted), the normal full body. Every full body now also has `version` (additive). The page polls with it when the WebSocket is down and takes a full snapshot every 5 min. Plain `/live` is unchanged for the Android app and the Discord bot.
- WebSockets use the Hibernation API (`acceptWebSocket`, handlers on the class); a `ping` from the client is answered by the runtime without waking the object. Fan-out serialises once, batches upserts for 1 s, and does nothing when nobody is connected. WebSocket upserts no longer rely on alarms (`setTimeout`), so alarms are 6 per minute.
- Duration: with stations posting every few seconds the object is never idle, so it cannot hibernate; 128 MB x 24 h x 30 d is about 324,000 GB-s, inside the 400,000 included. It only matters for a quiet hub, where hibernation now lets it sleep.

Tune: the constants at the top of `src/index.js` (`FLUSH_MS`, `BROADCAST_MS`, `PRUNE_EVERY_MS`, `LIVE_EDGE_S`, `META_TTL_S`), `PERSIST_GAP_MS` in `track-book.js`, `stepMs` per kind in `trail.js`. If the process is evicted, at most the last ~10 s of new tracks and ~30 s of movement are lost.

### Measured (test/load-sim.js, 60 s, 70 stations x 150-300 tracks every 2-5 s, 200 /live pollers every 5 s, 50 WebSockets, real workerd + DO SQLite)
Run: `node test/load-sim.js --run --tree <checkout> --secs 60` (starts `wrangler dev --local`, no login; the wrapper counts DO fetches/alarms and the `rowsRead`/`rowsWritten` the SQL cursors report).

| per minute | before | after |
|---|---|---|
| ingest POSTs | 1197 | 1194 |
| Durable Object requests (fetch + alarm) | 3690 | 1220 |
| SQL statements | 248,000 | 8,900 |
| SQL rows written | 192,143 | 15,200 |
| SQL rows read | 1,955,748 | 2,641 |
| CPU ms per DO request (wrangler+workerd, ps) | 15.7 | 21.7 (fewer, bigger requests; per minute 58,077 -> 26,469) |
| ingest latency p50 / p95 (ms, local) | 14.7 / 135 | 5.5 / 13.9 |
| /live latency p50 / p95 (ms, local) | 45 / 146 | 7.3 / 18 |

Monthly (x 43,200 min), billing units beyond the included amounts: DO requests 159M -> 53M ($23.8 -> $7.8); rows written 8.30 B -> 657 M ($8,251 -> $607); rows read 84 B -> 0.1 B ($60 -> $0). The scenario is a stress test (about 1,400 distinct moving targets); trail points are now about 80 % of the remaining writes, and the next step would be one JSON blob per track persisted per flush instead of one row per point. The Worker request count (ingest + /live, about 3,600/min = $43/month beyond 10 M) is not changed by caching: reduce it with fewer polls and longer app post intervals.

### App side (not changed, read only)
`app/map/map_send.cpp` posts at most every 4 s (`kMinInterval`) and every 60 s as a heartbeat, up to 250 tracks, each with up to 60 path points: about 130-230 KB per post with 250 tracks. The hub only needs the path for a track it has not seen yet (it keeps its own), so sending `path` only on a track's first post and every Nth post would cut the upload by roughly 80 %; the hub already ignores a one-point path (`mergePath`). This needs a native build and test, so it is left for a separate change. zlib deflate is not available in the app (`inflate.h` is a reader only).

## Icons
- 50 hand-drawn shapes in `shapes.js` (nothing copied from other maps or icon sets): 14 aircraft families seen from above (narrow-body, wide twin, four-engine, regional jet, business jet, turboprop, light single, light twin, helicopter with rotor disc, fighter, delta, transport, glider, balloon, airship), UAVs (quad with rotor discs, Remote ID quad with a status ring, fixed wing), a generic dart for unknown aircraft, ground vehicle and obstacle, 13 ship hulls by AIS type (cargo, tanker, passenger, high-speed twin hull, tug, fishing, sailing, pleasure, military, SAR, pilot, dredger, other) each with a dashed course-over-ground vector ahead of the bow, radiosonde (balloon, cord, parachute, payload), satellite and GNSS satellite (coloured per constellation), APRS types (car, truck, house, weather station, digipeater, balloon, boat), amateur, buoy, DSC, station.
- Choice of shape (`types.js`, tested in `test/types.test.js`): ICAO type code from the card's aircraft lookup, then ADS-B emitter category (A1-A7, B1-B6, C1-C5), military hex, ICAO wake class, then speed / altitude (jet cruise = airliner, slow and low = light aircraft), else the generic dart. Ships by AIS code or text; size from a length in the text ("183 m") or the type's typical size.
- Drawing: crisp 1.2 px outline, faint sheen on the fill, readable at 16-28 px; sprites are made at 3x, once per (shape, colour, theme) and handed to MapLibre, which packs them into one GPU atlas and draws every track in ONE symbol layer (no per-marker DOM). With more than 150 tracks only those near the screen are handed to the renderer (re-evaluated on move). Dark / light variants follow `prefers-color-scheme` (outline colour, label colours). Stale tracks are grey; the selected track gets a ring (no glow) and a label; callsigns show only for the selected track and from zoom 7.5 (collisions are dropped by the renderer).
- Heading changes ease toward the new course by the shortest arc (no jumps); `prefers-reduced-motion` snaps.
- Filters sheet: "Colour aircraft by altitude" (remembered) with a legend 0-45,000 ft; the colours are quantised to 2,000 ft bands so the atlas stays small.
- `?view=lat,lon,zoom` opens the map at a place (used for screenshots; works for shared links too).
