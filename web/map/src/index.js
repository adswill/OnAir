import { validateBy, searchKey, BY_SEARCH_LIMIT } from "./by-name.js";
import { sanitizeList, mergeStored, takeBudget, MSG_TTL_MS, MSG_TOTAL_MAX, MSG_PER_INGEST } from "./msgs.js";
import { parseIngestBody } from "./lenient-json.js";
import { TrailStore, SqlBackend } from "./trail-store.js";
import { parseTrailRequest } from "./trail.js";
import { TrackBook, sanitize, positionTimeMs, TTL_MS } from "./track-book.js";
import { JamBook, sanitizeJam, CELL_DEG, JAM_TTL_MS, MIN_AIRCRAFT_PUBLISHED } from "./jam.js";
import { computeFixes, DOA_TTL_MS, MAX_DOA_PER_INGEST } from "./doa.js";


// sanitize is exported for the tests (it already was, from this file)
export { sanitize };

// ---- cost knobs (see README, "Cost model") -------------------------------------------------------------------------
const FLUSH_MS = 10_000;       // write-behind: changed tracks go to SQLite at most this often (one transaction)
const BROADCAST_MS = 1000;     // WebSocket upserts are batched for this long
const PRUNE_EVERY_MS = 600_000; // SQL clean-up (expired rows, orphan trails, caps) - never per request
const LIVE_EDGE_S = 3;         // /live is cached at the Cloudflare edge for this long (Worker, caches.default)
const LOAD_CACHE_MS = 5000;
const META_TTL_S = 86400;         // /meta (adsbdb lookups) in the edge cache
const META_MISS_TTL_S = 300;
const STATION_PERSIST_MS = 60_000;
const EXPIRE_EVERY_MS = 15_000;

function cors(res) {
  const h = new Headers(res.headers);
  h.set("Access-Control-Allow-Origin", "*");
  h.set("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  h.set("Access-Control-Allow-Headers", "Content-Type, If-None-Match");
  h.set("Access-Control-Expose-Headers", "ETag");
  return new Response(res.body, { status: res.status, headers: h });
}

function json(obj, status = 200) {
  return cors(new Response(JSON.stringify(obj), {
    status,
    headers: { "Content-Type": "application/json; charset=utf-8", "Cache-Control": "no-store" },
  }));
}

/** Soft capacity - public load meter only (not CPU). High caps so a small community stays in single digits. */
const LOAD_CAP = {
  tracks: 15000,
  stations: 500,
  viewers: 2000,
  ingestPerMin: 12000,
  throttlePerMin: 400,
};

function pruneTimeWindow(arr, now, windowMs = 60e3) {
  const cut = now - windowMs;
  let i = 0;
  while (i < arr.length && arr[i] < cut) i++;
  if (i) arr.splice(0, i);
}

function serverLoadPct({ tracks, stations, viewers, ingestPerMin, throttlePerMin }) {
  const ratios = [
    tracks / LOAD_CAP.tracks,
    stations / LOAD_CAP.stations,
    viewers / LOAD_CAP.viewers,
    ingestPerMin / LOAD_CAP.ingestPerMin,
    throttlePerMin / LOAD_CAP.throttlePerMin,
  ];
  const peak = Math.max(0, ...ratios);
  return Math.round(Math.min(100, peak * 100));
}

/** SQL CASE giving each kind's TTL, so one DELETE expires every kind (one table scan instead of one per kind). */
const TTL_CASE = "CASE kind " + Object.entries(TTL_MS).map(([k, v]) => `WHEN '${k}' THEN ${v}`).join(" ") + " ELSE 1200000 END";

export class MapHub {
  constructor(ctx, env) {
    this.ctx = ctx;
    this.env = env;
    this.ingestAt = [];
    this.throttleAt = [];
    this.lastLoadBroadcast = 0;
    this.lastPrune = Date.now();
    this.lastExpire = 0;
    this.book = new TrackBook(Date.now());
    this.jam = new JamBook();
    this.jamDirty = false;
    this.jamWroteAt = 0;
    this.stationMap = new Map();      // id -> { last, n, wAt, dirty }
    this.msgCache = new Map();        // aircraft id -> { list, dirty }
    this.msgBudget = new Map();       // station -> times of recent accepted messages (rate limit)
    this.pendingUpserts = new Map();  // id -> row, waiting for the next WebSocket batch
    this.broadcastTimer = null;
    this.alarmAt = 0;
    this.loadCache = null;
    this.loadCacheAt = 0;
    this.ready = this.ctx.blockConcurrencyWhile(() => this.init());
  }

  async init() {
    const sql = this.ctx.storage.sql;
    sql.exec(`
      CREATE TABLE IF NOT EXISTS tracks (
        id TEXT PRIMARY KEY,
        kind TEXT NOT NULL,
        label TEXT,
        sub TEXT,
        lat REAL NOT NULL,
        lon REAL NOT NULL,
        alt REAL,
        heading REAL,
        speed REAL,
        extra TEXT,
        t INTEGER NOT NULL
      );
      CREATE TABLE IF NOT EXISTS stations (
        id TEXT PRIMARY KEY,
        last INTEGER NOT NULL,
        n INTEGER NOT NULL
      );
    `);
    const cols = sql.exec("PRAGMA table_info(tracks)").toArray().map((r) => r.name);
    const add = (name, typ) => {
      if (!cols.includes(name)) sql.exec(`ALTER TABLE tracks ADD COLUMN ${name} ${typ}`);
    };
    add("squawk", "TEXT");
    add("cat", "TEXT");
    add("vrate", "REAL");
    add("src", "TEXT");
    add("age", "REAL");
    add("path", "TEXT");
    add("dv", "INTEGER");    // 1: speed / heading / vertical rate are derived from position reports (or estimated), not broadcast
    add("by", "TEXT");       // optional public name of the station that heard it last (empty: anonymous)
    add("est", "INTEGER");   // 1: the position is an estimate (dead reckoning) the app sent, not a received fix
    sql.exec(`
      CREATE TABLE IF NOT EXISTS meta (
        k TEXT PRIMARY KEY,
        json TEXT NOT NULL,
        t INTEGER NOT NULL
      );
      CREATE TABLE IF NOT EXISTS msgs (
        id TEXT PRIMARY KEY,
        json TEXT NOT NULL,
        t INTEGER NOT NULL
      );
    `);
    // (kind, t) index dropped: it cost an extra billed row for every track write; expiry is one scan per prune instead.
    sql.exec("DROP INDEX IF EXISTS idx_tracks_kind_t");
    // Position history per track (see trail.js for the limits). Inserts are buffered and written inside flush().
    this.trails = new TrailStore(new SqlBackend(sql), { buffer: true });
    // The whole live picture is read once per start; from here on SQLite is write-behind storage, not the source.
    const now = Date.now();
    this.book.load(sql.exec(
      "SELECT id, kind, label, sub, lat, lon, alt, heading, speed, extra, t, squawk, cat, vrate, src, age, path, est, dv, by FROM tracks",
    ).toArray());
    this.book.expire(now);
    try {
      const jr = sql.exec("SELECT json FROM meta WHERE k = 'jam'").toArray()[0];
      if (jr) this.jam.load(JSON.parse(jr.json), now);
    } catch { /* a bad row only empties the GPS layer */ }
    for (const r of sql.exec("SELECT id, last, n FROM stations").toArray()) this.stationMap.set(r.id, { last: r.last, n: r.n, wAt: now, dirty: false });
    if (typeof WebSocketRequestResponsePair === "function" && this.ctx.setWebSocketAutoResponse) {
      // a client "ping" is answered by the runtime without waking the object
      this.ctx.setWebSocketAutoResponse(new WebSocketRequestResponsePair("ping", "pong"));
    }
    if (this.book.pending || this.book.size) this.armFlush(now);
  }

  // ---------- load meter ----------

  touchLoad(now, kind) {
    if (kind === "throttle") this.throttleAt.push(now);
    else this.ingestAt.push(now);
    pruneTimeWindow(this.ingestAt, now);
    pruneTimeWindow(this.throttleAt, now);
  }

  loadMetrics(now) {
    pruneTimeWindow(this.ingestAt, now);
    pruneTimeWindow(this.throttleAt, now);
    const viewers = this.ctx.getWebSockets().length;
    const ingestPerMin = this.ingestAt.length;
    const throttlePerMin = this.throttleAt.length;
    if (!this.loadCache || now - this.loadCacheAt >= LOAD_CACHE_MS) {
      let stations = 0;
      for (const s of this.stationMap.values()) if (s.last > now - 30 * 60e3) stations++;
      this.loadCache = { tracks: this.book.size, stations };
      this.loadCacheAt = now;
    }
    const c = this.loadCache;
    const pct = serverLoadPct({ tracks: c.tracks, stations: c.stations, viewers, ingestPerMin, throttlePerMin });
    return { pct, tracks: c.tracks, stations: c.stations, viewers, ingestPerMin, throttlePerMin };
  }

  maybeBroadcastLoad(now) {
    if (now - this.lastLoadBroadcast < 12000) return;
    this.lastLoadBroadcast = now;
    this.broadcast({ type: "load", load: this.loadMetrics(now) });
  }

  // ---------- write-behind ----------

  armFlush(now) {
    if (this.alarmAt > now) return;
    this.alarmAt = now + FLUSH_MS;
    this.ctx.storage.setAlarm(this.alarmAt);
  }

  async alarm() {
    await this.ready;
    this.alarmAt = 0;
    this.flush(Date.now());
  }

  /** One transaction: changed tracks, removed tracks, stations, ACARS messages, buffered trail points (+ prune when due). */
  flush(now) {
    this.book.expire(now);
    this.lastExpire = now;
    const { upserts, deletes } = this.book.takePersist(now);
    const sql = this.ctx.storage.sql;
    const stations = [];
    for (const [id, s] of this.stationMap) if (s.dirty && now - s.wAt >= STATION_PERSIST_MS) stations.push([id, s]);
    const msgs = [];
    for (const [id, m] of this.msgCache) if (m.dirty) msgs.push([id, m]);
    this.ctx.storage.transactionSync(() => {
      for (const t of upserts) {
        sql.exec(
          `INSERT INTO tracks (id, kind, label, sub, lat, lon, alt, heading, speed, extra, t, squawk, cat, vrate, src, age, path, est, dv, by)
           VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
           ON CONFLICT(id) DO UPDATE SET
             kind=excluded.kind, label=excluded.label, sub=excluded.sub,
             lat=excluded.lat, lon=excluded.lon, alt=excluded.alt,
             heading=excluded.heading, speed=excluded.speed, extra=excluded.extra, t=excluded.t,
             squawk=excluded.squawk, cat=excluded.cat, vrate=excluded.vrate, src=excluded.src,
             age=excluded.age, path=excluded.path, est=excluded.est, dv=excluded.dv, by=excluded.by`,
          t.id, t.kind, t.label, t.sub, t.lat, t.lon, t.alt, t.heading, t.speed, t.extra, t.t,
          t.squawk, t.cat, t.vrate, t.src, t.age, JSON.stringify(t.path), t.est ? 1 : 0, t.dv ? 1 : 0, t.by || "",
        );
      }
      for (let i = 0; i < deletes.length; i += 90) {
        const chunk = deletes.slice(i, i + 90);
        sql.exec(`DELETE FROM tracks WHERE id IN (${chunk.map(() => "?").join(",")})`, ...chunk);
      }
      for (const [id, s] of stations) {
        sql.exec("INSERT INTO stations (id, last, n) VALUES (?, ?, ?) ON CONFLICT(id) DO UPDATE SET last=excluded.last, n=excluded.n", id, s.last, s.n);
        s.dirty = false; s.wAt = now;
      }
      for (const [id, m] of msgs) {
        if (m.list.length) sql.exec("INSERT INTO msgs (id, json, t) VALUES (?, ?, ?) ON CONFLICT(id) DO UPDATE SET json=excluded.json, t=excluded.t", id, JSON.stringify(m.list), m.list[0].t);
        m.dirty = false;
      }
      this.trails.flush();
      if (this.jamDirty && now - this.jamWroteAt >= 60_000) {   // one row, at most once a minute
        sql.exec("INSERT INTO meta (k, json, t) VALUES ('jam', ?, ?) ON CONFLICT(k) DO UPDATE SET json=excluded.json, t=excluded.t", JSON.stringify(this.jam.toJSON()), now);
        this.jamDirty = false; this.jamWroteAt = now;
      }
      if (now - this.lastPrune >= PRUNE_EVERY_MS) this.prune(now);
    });
    if (this.book.pending || this.dirtyStations() || msgs.length || this.jamDirty) this.armFlush(now);
  }

  dirtyStations() {
    for (const s of this.stationMap.values()) if (s.dirty) return true;
    return false;
  }

  /** SQL clean-up. Runs from flush() every PRUNE_EVERY_MS: bulk deletes, each one table scan (rows read are almost free). */
  prune(now) {
    const sql = this.ctx.storage.sql;
    this.pruneMsgs(now);
    sql.exec(`DELETE FROM tracks WHERE t < ? - ${TTL_CASE}`, now);   // safety net; the book already expires in memory
    sql.exec("DELETE FROM stations WHERE last < ?", now - 24 * 3600e3);
    for (const [id, s] of this.stationMap) if (s.last < now - 24 * 3600e3) this.stationMap.delete(id);
    this.trails.prune(now); // after the tracks: trails of expired tracks go with them
    this.msgCache.clear();
    this.lastPrune = now;
  }

  pruneMsgs(now) {
    const sql = this.ctx.storage.sql;
    sql.exec("DELETE FROM msgs WHERE t < ?", now - MSG_TTL_MS);
    const n = sql.exec("SELECT COUNT(*) AS n FROM msgs").toArray()[0];
    if (n && n.n > MSG_TOTAL_MAX) {
      sql.exec("DELETE FROM msgs WHERE id IN (SELECT id FROM msgs ORDER BY t ASC LIMIT ?)", n.n - MSG_TOTAL_MAX);
    }
  }

  // ---------- requests ----------

  async fetch(req) {
    await this.ready;
    const url = new URL(req.url);
    if (req.method === "OPTIONS") return cors(new Response(null, { status: 204 }));
    if (url.pathname === "/ingest" && req.method === "POST") return this.ingest(req);
    if (url.pathname === "/live" && req.method === "GET") return this.live(url, req);
    if ((url.pathname === "/trail" || url.pathname === "/trails") && req.method === "GET") return this.trailsReq(url);
    if (url.pathname === "/meta" && req.method === "GET") return this.meta(req);
    if (url.pathname === "/msgs" && req.method === "GET") return this.msgs(req);
    if (url.pathname === "/jam" && req.method === "POST") return this.jamPost(req);
    if (url.pathname === "/jam" && req.method === "GET") return this.jamGet();
    if (url.pathname.startsWith("/api/by/") && req.method === "GET") return this.byUser(url);
    if (url.pathname === "/ws") return this.ws(req);
    return json({ error: "not found" }, 404);
  }

  async ingest(req) {
    let body;
    try {
      body = parseIngestBody(await req.text());
    } catch {
      return json({ error: "bad json" }, 400);
    }
    const station = String(body.station || "").slice(0, 64);
    if (!/^[a-zA-Z0-9_-]{8,64}$/.test(station)) return json({ error: "station" }, 400);
    const tracks = Array.isArray(body.tracks) ? body.tracks : [];
    if (tracks.length > 250) return json({ error: "too many" }, 413);

    const now = Date.now();
    let st = this.stationMap.get(station);
    if (st && now - st.last < 1500) {
      this.touchLoad(now, "throttle");
      this.maybeBroadcastLoad(now);
      return json({ error: "slow down" }, 429);
    }

    // optional public name (body.by): checked here, never logged; a bad one is dropped (the data is still taken) and the reply says why
    const byCheck = validateBy(typeof body.by === "string" ? body.by.slice(0, 80) : null);
    const by = byCheck.ok ? byCheck.name : "";
    let kept = 0;
    let msgBudgetLeft = MSG_PER_INGEST;
    let doaLeft = MAX_DOA_PER_INGEST;
    let sawDoa = false;
    for (const raw of tracks) {
      if (raw && raw.kind === "doa") { if (doaLeft-- <= 0) continue; }
      const t = sanitize(raw, now, station, by);
      if (!t) continue;
      const row = this.book.accept(t, now);
      if (!row) continue;
      kept++;
      this.trails.record(row.id, { t: positionTimeMs(now, row.age), lat: row.lat, lon: row.lon, alt: row.alt, hdg: row.heading }, now);
      if (row.visible) this.pendingUpserts.set(row.id, row);
      if (row.kind === "doa") sawDoa = true;
      if (row.kind === "aircraft" && Array.isArray(raw.msgs) && raw.msgs.length && msgBudgetLeft > 0) {
        let list = sanitizeList(raw.msgs, now).slice(0, msgBudgetLeft);
        list = list.slice(0, takeBudget(this.msgBudget, station, list.length, now));
        msgBudgetLeft -= list.length;
        if (list.length) this.storeMsgs(row.id, list, now);
      }
    }
    if (st) { st.last = now; st.n++; st.dirty = true; }
    else { st = { last: now, n: 1, wAt: 0, dirty: true }; this.stationMap.set(station, st); }
    if (sawDoa) for (const f of this.currentFixes(now)) this.pendingUpserts.set(f.id, f);   // derived fix markers go out with the bearings
    this.touchLoad(now, "ingest");
    this.loadCache = null;
    this.armFlush(now);
    this.scheduleBroadcast();
    this.maybeBroadcastLoad(now);
    const reply = { ok: true, n: kept, load: this.loadMetrics(now) };
    if (!byCheck.ok) reply.by_error = byCheck.message;
    return json(reply);
  }

  // ---------- GPS interference cells (aggregate only, see jam.js) ----------

  async jamPost(req) {
    let body;
    try { body = JSON.parse(await req.text()); } catch { return json({ error: "bad json" }, 400); }
    const clean = sanitizeJam(body);
    if (!clean) return json({ error: "bad cells" }, 400);
    const now = Date.now();
    const r = this.jam.accept(clean.station, clean.cells, now);
    if (r === "slow") return json({ error: "slow down" }, 429);
    if (r === "full") return json({ error: "busy" }, 503);
    this.jamDirty = true;
    this.armFlush(now);
    return json({ ok: true, n: clean.cells.length });
  }

  jamGet() {
    const now = Date.now();
    return json({ cell: CELL_DEG, ttl: JAM_TTL_MS / 1000, min: MIN_AIRCRAFT_PUBLISHED, now, cells: this.jam.cells(now) });
  }

  /** Fix markers where bearing lines from different stations on one frequency cross (derived from memory, never stored). */
  currentFixes(now) {
    const rows = this.book.doaRows().filter((r) => r.t > now - DOA_TTL_MS);
    return rows.length > 1 ? computeFixes(rows, now).map((f) => ({ est: 0, dv: 0, _fix: true, ...f })) : [];
  }

  /** The cached tracks array with the current fix markers appended. */
  tracksWithFixes(now) {
    const tj = this.book.tracksJson();
    const fixes = this.currentFixes(now);
    if (!fixes.length) return tj;
    const extra = fixes.map((f) => JSON.stringify(this.fixWire(f))).join(",");
    return tj.length > 2 ? tj.slice(0, -1) + "," + extra + "]" : "[" + extra + "]";
  }

  fixWire(t) {
    return {
      id: t.id, kind: t.kind, label: t.label, sub: t.sub, lat: t.lat, lon: t.lon, alt: t.alt, heading: t.heading, speed: t.speed,
      extra: t.extra, t: t.t, squawk: t.squawk, cat: t.cat, vrate: t.vrate, src: t.src, age: t.age, est: t.est ? 1 : 0, dv: t.dv ? 1 : 0,
    };
  }

  /** GET /api/by/<name>[?exact=1]: the tracks whose last contributor is that name (case-insensitive, prefix unless exact), at most 200. */
  byUser(url) {
    const raw = url.pathname.slice("/api/by/".length);
    const key = searchKey(raw);
    if (!key) return json({ error: "The name is empty or has characters a name cannot have.", tracks: [], users: [] }, 400);
    const v = validateBy(key);
    if (!v.ok && v.code !== "length") return json({ error: v.message, tracks: [], users: [] }, 400);
    const rows = this.book.byUser(key, url.searchParams.get("exact") !== "1", BY_SEARCH_LIMIT);
    const counts = new Map();
    for (const r of rows) counts.set(r.by, (counts.get(r.by) || 0) + 1);
    const users = [...counts].map(([name, n]) => ({ name, n })).sort((a, b) => b.n - a.n || a.name.localeCompare(b.name));
    return json({ users, tracks: rows.map((r) => JSON.parse(this.book.wire(r))), limit: BY_SEARCH_LIMIT });
  }

  // ---------- ACARS messages (cached in memory, written back with the flush) ----------

  msgEntry(id) {
    let e = this.msgCache.get(id);
    if (!e) {
      const row = this.ctx.storage.sql.exec("SELECT json FROM msgs WHERE id = ?", id).toArray()[0];
      let old = [];
      if (row) { try { old = JSON.parse(row.json); } catch { old = []; } }
      e = { list: Array.isArray(old) ? old : [], dirty: false };
      this.msgCache.set(id, e);
    }
    return e;
  }

  storeMsgs(id, list, now) {
    const e = this.msgEntry(id);
    const merged = mergeStored(e.list, list, now);
    if (!merged.length) return;
    e.list = merged;
    e.dirty = true;
  }

  /** GET /msgs?id=<ICAO hex or track id>: the stored ACARS messages of one aircraft (newest first). Not part of /live. */
  async msgs(req) {
    const url = new URL(req.url);
    const raw = String(url.searchParams.get("id") || "").slice(0, 90);
    const hex = raw.replace(/^aircraft:/, "").toUpperCase().replace(/[^0-9A-F]/g, "");
    const id = hex.length === 6 && /^(aircraft:)?[0-9A-Fa-f]{6}$/.test(raw) ? "aircraft:" + hex : "aircraft:" + raw.replace(/^aircraft:/, "");
    if (id.length < 10) return json({ error: "query" }, 400);
    const now = Date.now();
    const list = this.msgEntry(id).list.filter((m) => now - m.t <= MSG_TTL_MS);
    return json({ id, msgs: list });
  }

  // ---------- /live ----------

  expireIfDue(now) {
    if (now - this.lastExpire < EXPIRE_EVERY_MS) return;
    this.lastExpire = now;
    this.book.expire(now);
    if (this.book.pending) this.armFlush(now);
  }

  /**
   * /live: the full JSON as before (plus `version`). `?since=<version>` returns only what changed
   * ({ delta: true, tracks: [changed], removed: [ids], version }) or the full snapshot when `since` is unknown or too old.
   * `?trails=1` adds each track's history.
   */
  live(url, req) {
    const now = Date.now();
    this.expireIfDue(now);
    const book = this.book;
    const lm = this.loadMetrics(now);
    const load = JSON.stringify(lm);
    const stations = lm.stations;
    const sinceRaw = url.searchParams.get("since");
    const hdr = { "Content-Type": "application/json; charset=utf-8" };
    if (sinceRaw != null) {
      const parts = book.deltaParts(Number(sinceRaw));
      if (parts) {
        if (Number(sinceRaw) === book.version) {
          return cors(new Response(`{"now":${now},"version":${book.version},"delta":true,"stations":${stations},"tracks":[],"removed":[],"load":${load}}`,
            { headers: { ...hdr, "Cache-Control": "no-store" } }));
        }
        return cors(new Response(`{"now":${now},"version":${book.version},"delta":true,"stations":${stations},"tracks":${parts.tracks},"removed":${JSON.stringify(parts.removed)},"load":${load}}`,
          { headers: { ...hdr, "Cache-Control": "no-store" } }));
      }
    }
    if (url.searchParams.get("trails") === "1") {
      const all = this.trails.all();
      const tracks = JSON.parse(this.tracksWithFixes(now)).map((t) => (all[t.id] ? { ...t, trail: all[t.id] } : t));
      return cors(new Response(JSON.stringify({ now, version: book.version, stations, tracks, load: JSON.parse(load) }), { headers: { ...hdr, "Cache-Control": "no-store" } }));
    }
    const etag = `W/"${book.version}"`;
    if (req && req.headers.get("If-None-Match") === etag) {
      return cors(new Response(null, { status: 304, headers: { ETag: etag, "Cache-Control": `public, max-age=${LIVE_EDGE_S}` } }));
    }
    return cors(new Response(`{"now":${now},"version":${book.version},"stations":${stations},"tracks":${this.tracksWithFixes(now)},"load":${load}}`,
      { headers: { ...hdr, "Cache-Control": `public, max-age=${LIVE_EDGE_S}`, ETag: etag } }));
  }

  /** GET /trail?id=<track id> or /trails?ids=a,b,c -> { trails: { id: [points] } } (one request for every visible track). */
  trailsReq(url) {
    const q = parseTrailRequest(url.searchParams);
    if (!q) return json({ error: "query" }, 400);
    return json({ now: Date.now(), trails: this.trails.get(q.ids) });
  }

  async meta(req) {
    const url = new URL(req.url);
    const icao = String(url.searchParams.get("icao") || "").toUpperCase().replace(/[^0-9A-F]/g, "").slice(0, 6);
    const cs = String(url.searchParams.get("cs") || "").toUpperCase().replace(/[^A-Z0-9]/g, "").slice(0, 8);
    if (icao.length !== 6 && cs.length < 3) return json({ error: "query" }, 400);
    const k = "v2:" + (icao || "-") + ":" + (cs || "-");
    const now = Date.now();
    const hit = this.ctx.storage.sql.exec("SELECT json, t FROM meta WHERE k = ?", k).toArray()[0];
    if (hit && now - hit.t < 6 * 3600e3) {
      try { return json(JSON.parse(hit.json)); } catch { /* fetch again */ }
    }
    const hdr = { headers: { "User-Agent": "OnAirMap/1.0 (https://on-air.dev; map@on-air.dev)" } };
    const jobs = [];
    if (icao.length === 6) jobs.push(fetch("https://api.adsbdb.com/v0/aircraft/" + icao, hdr).then((r) => r.ok ? r.json() : null).catch(() => null));
    else jobs.push(Promise.resolve(null));
    if (cs.length >= 3) jobs.push(fetch("https://api.adsbdb.com/v0/callsign/" + cs, hdr).then((r) => r.ok ? r.json() : null).catch(() => null));
    else jobs.push(Promise.resolve(null));
    const [ac, fl] = await Promise.all(jobs);
    const out = buildMeta(ac, fl);
    this.ctx.storage.sql.exec(
      "INSERT INTO meta (k, json, t) VALUES (?, ?, ?) ON CONFLICT(k) DO UPDATE SET json=excluded.json, t=excluded.t",
      k, JSON.stringify(out), now,
    );
    return json(out);
  }

  // ---------- WebSocket (Hibernation API: acceptWebSocket + handlers, so idle viewers do not keep the object billed) ----------

  async ws(req) {
    if (req.headers.get("Upgrade") !== "websocket") return json({ error: "ws" }, 426);
    const pair = new WebSocketPair();
    const client = pair[0];
    const server = pair[1];
    this.ctx.acceptWebSocket(server);
    const now = Date.now();
    this.expireIfDue(now);
    const book = this.book;
    const lm = this.loadMetrics(now);
    server.send(`{"type":"hello","now":${now},"version":${book.version},"stations":${lm.stations},"tracks":${this.tracksWithFixes(now)},"load":${JSON.stringify(lm)}}`);
    this.maybeBroadcastLoad(now);
    return new Response(null, { status: 101, webSocket: client });
  }

  webSocketMessage() {}
  webSocketClose(ws, code) {
    try { ws.close(code >= 1000 && code < 5000 && code !== 1005 && code !== 1006 ? code : 1000, "closing"); } catch { /* already closed */ }
  }
  webSocketError() {}

  scheduleBroadcast() {
    if (this.broadcastTimer || !this.pendingUpserts.size) return;
    if (!this.ctx.getWebSockets().length) { this.pendingUpserts.clear(); return; }  // nobody to tell: no work
    this.broadcastTimer = setTimeout(() => { this.broadcastTimer = null; this.sendUpserts(); }, BROADCAST_MS);
  }

  sendUpserts() {
    if (!this.pendingUpserts.size) return;
    const parts = [];
    for (const row of this.pendingUpserts.values()) {
      if (row._fix) parts.push(JSON.stringify(this.fixWire(row)));
      else if (this.book.rows.get(row.id) === row) parts.push(this.book.wire(row));
    }
    this.pendingUpserts.clear();
    if (parts.length) this.sendAll(`{"type":"upsert","tracks":[${parts.join(",")}]}`);
  }

  sendAll(s) {
    for (const ws of this.ctx.getWebSockets()) {
      try { ws.send(s); } catch { /* closed */ }
    }
  }

  broadcast(msg) {
    const sockets = this.ctx.getWebSockets();
    if (!sockets.length) return;
    const s = JSON.stringify(msg);   // one serialisation for every socket
    for (const ws of sockets) {
      try { ws.send(s); } catch { /* closed */ }
    }
  }
}

async function osmTile(z, x, y) {
  const z0 = z | 0, x0 = x | 0, y0 = y | 0;
  if (!(z0 >= 0 && z0 <= 18) || x0 < 0 || y0 < 0) return new Response("bad tile", { status: 400 });
  const n = 1 << z0;
  if (x0 >= n || y0 >= n) return new Response("bad tile", { status: 400 });
  const upstream = `https://tile.openstreetmap.org/${z0}/${x0}/${y0}.png`;
  const r = await fetch(upstream, {
    headers: { "User-Agent": "OnAirMap/1.0 (https://on-air.dev; map@on-air.dev)" },
    cf: { cacheTtl: 86400, cacheEverything: true },
  });
  if (!r.ok) return new Response("tile", { status: r.status });
  const h = new Headers(r.headers);
  h.set("Cache-Control", "public, max-age=86400");
  h.set("Access-Control-Allow-Origin", "*");
  return new Response(r.body, { status: 200, headers: h });
}

/** Airport block from an adsbdb flightroute leg; coordinates let the browser compute distance and ETA. */
function airportOut(a) {
  if (!a) return null;
  const lat = Number(a.latitude), lon = Number(a.longitude);
  return {
    iata: String(a.iata_code || "").slice(0, 3),
    icao: String(a.icao_code || "").slice(0, 4),
    name: String(a.name || "").slice(0, 60),
    city: String(a.municipality || "").slice(0, 48),
    country: String(a.country_name || "").slice(0, 40),
    iso: String(a.country_iso_name || "").slice(0, 2),
    lat: Number.isFinite(lat) ? lat : null,
    lon: Number.isFinite(lon) ? lon : null,
  };
}

/** Compact metadata the card needs, from the adsbdb aircraft and flightroute responses (either may be null). */
function buildMeta(ac, fl) {
  const out = {
    type: "", icaoType: "", registration: "", operator: "", origin: "", destination: "", originName: "", destinationName: "",
    manufacturer: "", flightNumber: "", airline: null, route: null,
  };
  const a = ac && ac.response && ac.response.aircraft;
  if (a) {
    out.icaoType = String(a.icao_type || "").slice(0, 8);
    const man = String(a.manufacturer || "").trim();
    const typ = String(a.type || "").trim();
    out.manufacturer = man.slice(0, 40);
    out.type = [man, typ].filter(Boolean).join(" ").slice(0, 80) || out.icaoType;
    out.registration = String(a.registration || "").slice(0, 12);
    out.operator = String(a.registered_owner || "").slice(0, 48);
  }
  const r = fl && fl.response && fl.response.flightroute;
  if (r) {
    out.origin = String((r.origin && (r.origin.iata_code || r.origin.icao_code)) || "").slice(0, 4);
    out.destination = String((r.destination && (r.destination.iata_code || r.destination.icao_code)) || "").slice(0, 4);
    out.originName = String((r.origin && (r.origin.municipality || r.origin.name)) || "").slice(0, 48);
    out.destinationName = String((r.destination && (r.destination.municipality || r.destination.name)) || "").slice(0, 48);
    out.route = { origin: airportOut(r.origin), destination: airportOut(r.destination) };
    out.flightNumber = String(r.callsign_iata || "").slice(0, 8);
    if (r.airline) {
      out.airline = {
        name: String(r.airline.name || "").slice(0, 48),
        iata: String(r.airline.iata || "").slice(0, 3),
        icao: String(r.airline.icao || "").slice(0, 4),
        iso: String(r.airline.country_iso || "").slice(0, 2),
      };
      if (!out.operator && r.airline.name) out.operator = String(r.airline.name).slice(0, 48);
    }
  }
  return out;
}

async function lookupAircraft(req) {
  const url = new URL(req.url);
  const icao = String(url.searchParams.get("icao") || "").toUpperCase().replace(/[^0-9A-F]/g, "").slice(0, 6);
  const cs = String(url.searchParams.get("cs") || "").toUpperCase().replace(/[^A-Z0-9]/g, "").slice(0, 8);
  if (icao.length !== 6 && cs.length < 3) return json({ error: "query" }, 400);
  const cacheKey = new Request("https://map.on-air.dev/meta-cache/v2/" + (icao || "-") + "/" + (cs || "-"));
  const cached = await caches.default.match(cacheKey);
  if (cached) return cors(cached);
  const hdr = { headers: { "User-Agent": "OnAirMap/1.0 (https://on-air.dev; map@on-air.dev)" } };
  const jobs = [];
  if (icao.length === 6) jobs.push(fetch("https://api.adsbdb.com/v0/aircraft/" + icao, hdr).then((r) => r.ok ? r.json() : null).catch(() => null));
  else jobs.push(Promise.resolve(null));
  if (cs.length >= 3) jobs.push(fetch("https://api.adsbdb.com/v0/callsign/" + cs, hdr).then((r) => r.ok ? r.json() : null).catch(() => null));
  else jobs.push(Promise.resolve(null));
  const [ac, fl] = await Promise.all(jobs);
  const out = buildMeta(ac, fl);
  const res = json(out);
  // aircraft data rarely changes: a day. When adsbdb knew nothing (or failed) only 5 minutes, so a hiccup is not remembered.
  const ttl = ac || fl ? META_TTL_S : META_MISS_TTL_S;
  const store = new Response(JSON.stringify(out), {
    status: 200,
    headers: { "Content-Type": "application/json; charset=utf-8", "Cache-Control": `public, max-age=${ttl}` },
  });
  await caches.default.put(cacheKey, store);
  return res;
}


/**
 * GET /live through the Cloudflare edge cache (caches.default): one Durable Object call per LIVE_EDGE_S per data centre,
 * however many viewers poll. ETag / If-None-Match answers 304 without a body. `?since=` and `?trails=1` go to the hub.
 */
async function cachedLive(req, env, ctx, url) {
  const key = new Request(url.origin + "/live", { method: "GET" });
  let res = await caches.default.match(key);
  if (!res) {
    res = await env.MAP.get(env.MAP.idFromName("hub-v2")).fetch(new Request(url.origin + "/live", { method: "GET" }));
    if (res.status === 200) ctx.waitUntil(caches.default.put(key, res.clone()));
  }
  const etag = res.headers.get("ETag");
  if (etag && req.headers.get("If-None-Match") === etag) {
    return cors(new Response(null, { status: 304, headers: { ETag: etag, "Cache-Control": `public, max-age=${LIVE_EDGE_S}` } }));
  }
  return cors(res);
}

export default {
  async fetch(req, env, ctx) {
    const url = new URL(req.url);
    const tile = url.pathname.match(/^\/tiles\/(\d+)\/(\d+)\/(\d+)\.png$/);
    if (tile) return osmTile(+tile[1], +tile[2], +tile[3]);
    if (url.pathname === "/meta") {
      if (req.method === "OPTIONS") return cors(new Response(null, { status: 204 }));
      if (req.method === "GET") return lookupAircraft(req);
    }
    if (url.pathname === "/live" && req.method === "GET" && !url.searchParams.has("since") && url.searchParams.get("trails") !== "1") {
      return cachedLive(req, env, ctx, url);
    }
    const api = url.pathname === "/ingest" || url.pathname === "/live" || url.pathname === "/msgs" || url.pathname === "/ws" || url.pathname === "/jam"
      || url.pathname === "/trail" || url.pathname === "/trails" || url.pathname.startsWith("/api/by/");
    if (api) {
      const id = env.MAP.idFromName("hub-v2");
      return env.MAP.get(id).fetch(req);
    }
    return env.ASSETS.fetch(req);
  },
};
