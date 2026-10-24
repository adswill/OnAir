// track-book.js - the hub's live tracks, kept in memory. Pure (no Cloudflare APIs), covered by test/track-book.test.js.
//
// Why: every SQLite row the Durable Object writes is billed, and the old hub did a SELECT plus an UPSERT (and a second
// index write) for every track of every POST. Now ingest only touches this Map; the hub writes the changed rows back in
// one transaction every FLUSH_MS (write-behind), at most once per PERSIST_GAP_MS per track. /live, the WebSocket and the
// delta endpoint are all served from here, so they cost no SQL reads at all.

import { cleanDoa, DOA_TTL_MS } from "./doa.js";
import { cleanBy, BY_SEARCH_LIMIT } from "./by-name.js";

export const KINDS = new Set([
  "aircraft", "vessel", "sonde", "aprs", "ham", "drone", "flarm", "sat", "buoy", "dsc", "station", "gnss", "doa",
]);

export const TTL_MS = {
  aircraft: 15 * 60e3,
  vessel: 30 * 60e3,
  sonde: 3 * 3600e3,
  aprs: 45 * 60e3,
  ham: 45 * 60e3,
  drone: 20 * 60e3,
  flarm: 15 * 60e3,
  sat: 20 * 60e3,
  gnss: 10 * 60e3,
  doa: DOA_TTL_MS,
  buoy: 2 * 3600e3,
  dsc: 60 * 60e3,
  station: 6 * 3600e3,
};

export function ttlFor(kind) {
  return TTL_MS[kind] || 20 * 60e3;
}

export const PERSIST_GAP_MS = 30_000;        // a moving track is written back at most this often
export const PERSIST_IDLE_GAP_MS = 300_000;  // a track that has not moved since its last write: only a heartbeat
export const REMOVED_LOG_MS = 10 * 60e3;     // how long removals stay available to ?since= deltas

/** Wall-clock ms of the position report (ingest time minus client age field). */
export function positionTimeMs(tMs, ageSec) {
  const a = Number(ageSec);
  if (Number.isFinite(a) && a > 0) return tMs - a * 1000;
  return tMs;
}

export function fresherPosition(incomingAge, prevRow, now) {
  if (!prevRow) return true;
  const inT = positionTimeMs(now, incomingAge);
  const prevT = positionTimeMs(prevRow.t, prevRow.age);
  return inT >= prevT - 500;
}

const SRC_RANK = {
  adsb: 40, ais: 38, flarm: 35, acars: 30, hfdl: 28, aprs: 25, ham: 25, sonde: 20, sat: 22, dsc: 18, buoy: 12,
};

/** Max position age (seconds) accepted at ingest - aligned with map UI stale colouring. */
const MAX_INGEST_AGE_SEC = {
  aircraft: { default: 45, adsb: 30, acars: 30, hfdl: 30, sat: 60 },
  vessel: { default: 90, ais: 90 },
  flarm: { default: 30 },
  drone: { default: 45 },
  aprs: { default: 120 },
  ham: { default: 120 },
  sonde: { default: 900 },
  sat: { default: 180 },
  gnss: { default: 120 },
  dsc: { default: 600 },
  buoy: { default: 7200 },
  station: { default: 3600 },
};

function maxIngestAgeSec(kind, src) {
  const spec = MAX_INGEST_AGE_SEC[kind];
  if (!spec) return 300;
  const s = String(src || "").toLowerCase();
  if (s && spec[s] != null) return spec[s];
  return spec.default ?? 300;
}

function normalizeIcao(s) {
  const x = String(s || "").toUpperCase().replace(/[^0-9A-F]/g, "");
  return x.length === 6 ? x : "";
}

function normalizeMmsi(s) {
  const x = String(s || "").replace(/\D/g, "");
  if (x.length >= 9) return x.slice(-9);
  return x.length ? x : "";
}

/** Stable client id before kind prefix (ICAO, MMSI, callsign, serial, ...). */
function canonicalClientId(kind, raw) {
  const rawId = String(raw.id || "").slice(0, 80);
  if (!rawId) return null;
  switch (kind) {
    case "aircraft": {
      const icao = normalizeIcao(raw.icao) || normalizeIcao(rawId);
      return icao || rawId;
    }
    case "vessel":
    case "dsc": {
      const mmsi = normalizeMmsi(raw.mmsi) || normalizeMmsi(rawId);
      return mmsi || rawId;
    }
    case "flarm": {
      const hex = normalizeIcao(raw.id) || normalizeIcao(rawId);
      return hex || rawId;
    }
    case "aprs":
    case "ham": {
      const cs = rawId.toUpperCase().replace(/[^A-Z0-9-]/g, "").slice(0, 12);
      return cs || rawId;
    }
    default:
      return rawId;
  }
}

function trackStorageId(kind, raw) {
  const id = canonicalClientId(kind, raw);
  if (!id) return null;
  return `${kind}:${id}`;
}

function srcRankOf(src) {
  return SRC_RANK[String(src || "").toLowerCase()] || 10;
}

/** When merging a fresher fix, keep the better label / source if the update is from a weaker link. */
export function mergeTrackMeta(prev, inc) {
  if (!prev) return inc;
  const out = { ...inc };
  const prevR = srcRankOf(prev.src);
  const incR = srcRankOf(inc.src);
  if (incR < prevR) {
    out.sub = prev.sub || inc.sub;
    out.src = prev.src || inc.src;
    const weakLabel = !inc.label || inc.label === inc.rawId || inc.label === inc.id;
    if (prev.label && weakLabel) out.label = prev.label;
  }
  if (prev.squawk && !inc.squawk) out.squawk = prev.squawk;
  if (prev.extra && !inc.extra) out.extra = prev.extra;
  if (prev.cat && !inc.cat) out.cat = prev.cat;
  return out;
}

export function sanitize(raw, now, station, by = "") {
  if (!raw || typeof raw !== "object") return null;
  const kind = String(raw.kind || "");
  if (!KINDS.has(kind)) return null;
  const lat = Number(raw.lat), lon = Number(raw.lon);
  if (!Number.isFinite(lat) || !Number.isFinite(lon) || lat < -90 || lat > 90 || lon < -180 || lon > 180) return null;
  if (lat === 0 && lon === 0) return null;
  if (kind === "doa") return sanitizeDoa(raw, now, station);
  const storageId = trackStorageId(kind, raw);
  if (!storageId) return null;
  const rawId = String(raw.id || "").slice(0, 80);
  const clientId = canonicalClientId(kind, raw) || rawId;
  const icao = kind === "aircraft" ? (normalizeIcao(raw.icao) || normalizeIcao(clientId)) : "";
  const mmsi = (kind === "vessel" || kind === "dsc") ? (normalizeMmsi(raw.mmsi) || normalizeMmsi(clientId)) : "";
  const alt = Number.isFinite(Number(raw.alt)) ? Number(raw.alt) : null;
  const heading = Number.isFinite(Number(raw.heading)) ? Number(raw.heading) : null;
  const speed = Number.isFinite(Number(raw.speed)) ? Number(raw.speed) : null;
  const vrate = Number.isFinite(Number(raw.vrate)) ? Number(raw.vrate) : null;
  const age = Number.isFinite(Number(raw.age)) ? Number(raw.age) : 0;
  if (age > maxIngestAgeSec(kind, raw.src)) return null;
  const squawk = raw.squawk == null || raw.squawk === "" ? null : String(raw.squawk).slice(0, 8);
  const path = [];
  if (Array.isArray(raw.path)) {
    for (const pt of raw.path.slice(-80)) {
      let la, lo;
      if (Array.isArray(pt) && pt.length >= 2) { la = Number(pt[0]); lo = Number(pt[1]); }
      else if (pt && typeof pt === "object") { la = Number(pt.lat); lo = Number(pt.lon); }
      else continue;
      if (Number.isFinite(la) && Number.isFinite(lo) && Math.abs(la) <= 90 && Math.abs(lo) <= 180) path.push([lo, la]);
    }
  }
  path.push([lon, lat]);
  return {
    id: storageId,
    kind,
    rawId: clientId,
    icao,
    mmsi,
    label: String(raw.label || rawId).slice(0, 48),
    sub: String(raw.sub || "").slice(0, 32),
    lat, lon, alt, heading, speed, vrate, age,
    est: raw.est === 1 || raw.est === true ? 1 : 0,
    dv: (raw.dv === 1 || raw.dv === true) && (speed != null || heading != null || vrate != null) ? 1 : 0,   // additive: old clients never send it
    squawk,
    src: String(raw.src || "").slice(0, 16),
    cat: String(raw.cat || "").slice(0, 48),
    extra: String(raw.extra || "").slice(0, 120),
    path: kind === "gnss" || kind === "doa" ? [] : path.slice(-80),   // no trails for navigation satellites
    by: kind === "doa" ? "" : cleanBy(by),   // optional public name of the sender; "" = anonymous. Already checked by the caller, checked again here
    t: now,
  };
}

/** A bearing line from a station that runs a direction finder: the track is the station position, heading is the bearing. */
function sanitizeDoa(raw, now, station) {
  const lat = Number(raw.lat), lon = Number(raw.lon);
  if (!station) return null;
  const d = cleanDoa(raw);
  if (!d) return null;
  const age = Number.isFinite(Number(raw.age)) ? Number(raw.age) : 0;
  if (age > DOA_TTL_MS / 1000) return null;
  return {
    id: "doa:" + String(station).slice(0, 12) + "-" + Math.round(d.freq / 1000), kind: "doa", rawId: String(station).slice(0, 12), icao: "", mmsi: "",
    label: (d.freq / 1e6).toFixed(3) + " MHz", sub: Math.round(d.bearing) + "\u00b0",
    lat, lon, alt: null, heading: d.bearing, speed: null, vrate: null, age, est: 0, dv: 0, squawk: null, src: "df", cat: "",
    extra: JSON.stringify({ f: d.freq, s: d.sigma, c: +d.conf.toFixed(2) }),
    path: [], t: now,
  };
}

function pathDistanceM(lon1, lat1, lon2, lat2) {
  const p = Math.PI / 180;
  const dLat = (lat2 - lat1) * p;
  const dLon = (lon2 - lon1) * p;
  const a = Math.sin(dLat / 2) ** 2
    + Math.cos(lat1 * p) * Math.cos(lat2 * p) * Math.sin(dLon / 2) ** 2;
  return 6371000 * 2 * Math.asin(Math.min(1, Math.sqrt(a)));
}

function bearingDeg(lon1, lat1, lon2, lat2) {
  const p = Math.PI / 180;
  const dLon = (lon2 - lon1) * p;
  const y = Math.sin(dLon) * Math.cos(lat2 * p);
  const x = Math.cos(lat1 * p) * Math.sin(lat2 * p) - Math.sin(lat1 * p) * Math.cos(lat2 * p) * Math.cos(dLon);
  return (Math.atan2(y, x) / p + 360) % 360;
}

/** Datalink fixes often have no heading field; use the stored trail (array of [lon, lat]). */
export function headingFromTrail(pathArr, lon, lat, reported) {
  if (reported != null && Number.isFinite(reported)) return reported;
  const path = Array.isArray(pathArr) ? pathArr.slice() : [];
  const lon2 = lon, lat2 = lat;
  if (!path.length || path[path.length - 1][0] !== lon2 || path[path.length - 1][1] !== lat2) path.push([lon2, lat2]);
  if (path.length < 2) return null;
  const minM = 15;
  for (let i = path.length - 2; i >= 0; i--) {
    const [lon1, lat1] = path[i];
    if (pathDistanceM(lon1, lat1, lon2, lat2) >= minM) return bearingDeg(lon1, lat1, lon2, lat2);
  }
  const [lon0, lat0] = path[0];
  if (pathDistanceM(lon0, lat0, lon2, lat2) >= minM) return bearingDeg(lon0, lat0, lon2, lat2);
  return null;
}

/** Merge the stored path with the one the station sent (arrays of [lon, lat]); returns a new array of at most 80 points. */
export function mergePath(oldArr, incoming, lon, lat) {
  const old = Array.isArray(oldArr) ? oldArr : [];
  let neu = Array.isArray(incoming) ? incoming.slice() : [];
  if (neu.length <= 1 && old.length >= 2) neu = old.slice();
  else if (old.length > neu.length) neu = old.slice();
  const last = neu[neu.length - 1];
  if (!last || last[0] !== lon || last[1] !== lat) neu.push([lon, lat]);
  return neu.slice(-80);
}

/** Fields whose change is visible to a viewer: they bump the version and go out as an upsert. */
const VISIBLE = ["by", "lat", "lon", "alt", "heading", "speed", "label", "sub", "squawk", "cat", "src", "extra", "est", "dv", "vrate"];
/** Fields whose change makes the row worth writing back at once (not only on the periodic heartbeat). */
const SIG = ["by", "kind", "label", "sub", "squawk", "cat", "src", "extra", "est", "dv"];
const sigOf = (r) => SIG.map((k) => r[k]).join("\u0001");

export class TrackBook {
  constructor(now = Date.now()) {
    this.rows = new Map();          // id -> row (the wire fields + path + bookkeeping)
    this.version = now;             // monotonic; starts at the clock so a restarted hub never reuses an old number
    this.removed = new Map();       // id -> version at which it was removed (for deltas)
    this.removedFloor = this.version; // deltas from a version below this cannot be answered (the log is incomplete there)
    this.dirty = new Set();         // ids to write back
    this.pendingDeletes = new Set();
    this.doaIds = new Set();        // ids of bearing lines; fixes are derived from them
    this.doaVersion = this.version; // version of the last change to any bearing line (fixes change with it)
    this._tj = null;
    this._tjV = -1;
  }

  /** Load rows from storage at start (rows from SELECT * FROM tracks). They count as already persisted. */
  load(dbRows) {
    for (const r of dbRows) {
      let path = [];
      try { path = r.path ? JSON.parse(r.path) : []; } catch { path = []; }
      const row = {
        id: r.id, kind: r.kind, label: r.label, sub: r.sub, lat: r.lat, lon: r.lon, alt: r.alt, heading: r.heading,
        speed: r.speed, extra: r.extra, t: r.t, squawk: r.squawk, cat: r.cat, vrate: r.vrate, src: r.src, age: r.age,
        est: r.est ? 1 : 0, dv: r.dv ? 1 : 0, by: r.by || "", path: Array.isArray(path) ? path : [], ver: this.version, json: null, w: null,
      };
      row.w = { at: r.t, lat: row.lat, lon: row.lon, sig: sigOf(row) };
      this.rows.set(row.id, row);
      if (row.kind === "doa") this.doaIds.add(row.id);
    }
  }

  get size() { return this.rows.size; }

  /**
   * Take one sanitized track (sanitize() output). Returns the row when it was accepted (`row.visible` says whether a
   * viewer would see a difference), or null when an older fix lost against the one already held.
   */
  accept(t, now) {
    const prev = this.rows.get(t.id);
    if (!fresherPosition(t.age, prev, now)) return null;
    const merged = prev ? mergeTrackMeta(prev, t) : { ...t };
    if (t.kind === "gnss" || t.kind === "doa") merged.path = [];
    else merged.path = mergePath(prev && prev.path, merged.path, merged.lon, merged.lat);
    const trailHdg = t.kind === "gnss" || t.kind === "doa" ? null : headingFromTrail(merged.path, merged.lon, merged.lat, merged.heading);
    if (trailHdg != null) merged.heading = trailHdg;
    else if (merged.heading == null && prev && prev.heading != null) { merged.heading = prev.heading; if (prev.dv) merged.dv = 1; }
    if (merged.speed == null && prev && prev.speed != null) { merged.speed = prev.speed; if (prev.dv) merged.dv = 1; }
    let visible = !prev;
    if (prev) for (const k of VISIBLE) if (prev[k] !== merged[k]) { visible = true; break; }
    const row = prev || { id: t.id, ver: 0, json: null, w: null };
    for (const k of ["kind", "label", "sub", "lat", "lon", "alt", "heading", "speed", "extra", "t", "squawk", "cat", "vrate", "src", "age", "est", "dv", "path"]) row[k] = merged[k];
    row.by = merged.by || "";
    row.est = row.est ? 1 : 0;
    row.dv = row.dv ? 1 : 0;
    if (visible) { row.ver = ++this.version; row.json = null; if (row.kind === "doa") this.doaVersion = this.version; }
    if (row.kind === "doa") this.doaIds.add(row.id);
    this.removed.delete(row.id);
    this.pendingDeletes.delete(row.id);
    this.rows.set(row.id, row);
    this.dirty.add(row.id);
    row.visible = visible;
    return row;
  }

  /** Drop tracks past their TTL (in memory now, in storage at the next flush). Returns how many went. */
  expire(now) {
    let n = 0;
    for (const [id, row] of this.rows) {
      if (now - row.t > ttlFor(row.kind)) {
        this.rows.delete(id);
        this.dirty.delete(id);
        this.removed.set(id, { v: ++this.version, at: now });
        if (row.kind === "doa") { this.doaIds.delete(id); this.doaVersion = this.version; }
        if (row.w) this.pendingDeletes.add(id);
        n++;
      }
    }
    // forget old removals; a client older than the log gets a full snapshot instead of a delta
    for (const [id, r] of this.removed) {
      if (now - r.at > REMOVED_LOG_MS) { this.removed.delete(id); this.removedFloor = Math.max(this.removedFloor, r.v); }
    }
    return n;
  }

  /** The JSON of one row as /live sends it (cached until the row changes). */
  wire(row) {
    if (row.json) return row.json;
    row.json = JSON.stringify({
      id: row.id, kind: row.kind, label: row.label, sub: row.sub,
      lat: row.lat, lon: row.lon, alt: row.alt, heading: row.heading, speed: row.speed,
      extra: row.extra, t: row.t, squawk: row.squawk, cat: row.cat, vrate: row.vrate,
      src: row.src, age: row.age, est: row.est ? 1 : 0, dv: row.dv ? 1 : 0,
      ...(row.by ? { by: row.by, heard_at: row.t } : {}),   // last writer wins: the sender of the newest report
    });
    return row.json;
  }

  /** Bearing-line rows in the shape computeFixes() wants. */
  doaRows() {
    const out = [];
    for (const id of this.doaIds) { const r = this.rows.get(id); if (r) out.push({ id: r.id, lat: r.lat, lon: r.lon, heading: r.heading, extra: r.extra, t: r.t }); }
    return out;
  }

  /** `[...]` of every track, cached per version. */
  tracksJson() {
    if (this._tj !== null && this._tjV === this.version) return this._tj;
    const parts = [];
    for (const row of this.rows.values()) parts.push(this.wire(row));
    this._tj = "[" + parts.join(",") + "]";
    this._tjV = this.version;
    return this._tj;
  }

  /** `[...]` of the rows changed after `since`, plus the ids removed after it; null when `since` is too old or from another hub life. */
  deltaParts(since) {
    if (!Number.isFinite(since) || since < this.removedFloor || since > this.version) return null;
    if (since < this.doaVersion) return null;   // derived fix markers changed: send a full snapshot
    const parts = [];
    for (const row of this.rows.values()) if (row.ver > since) parts.push(this.wire(row));
    const gone = [];
    for (const [id, r] of this.removed) if (r.v > since) gone.push(id);
    return { tracks: "[" + parts.join(",") + "]", removed: gone, count: parts.length };
  }

  /**
   * Rows to write back now and ids to delete. Writes a row when it is new, when a visible text field changed, or when
   * its last write is older than the gap (a moving track every 30 s, a still one every 5 min).
   */
  takePersist(now, gap = PERSIST_GAP_MS, idleGap = PERSIST_IDLE_GAP_MS) {
    const upserts = [];
    for (const id of this.dirty) {
      const row = this.rows.get(id);
      if (!row) { this.dirty.delete(id); continue; }
      const w = row.w;
      let need = !w;
      if (w && !need) {
        if (sigOf(row) !== w.sig) need = true;
        else {
          const moved = w.lat !== row.lat || w.lon !== row.lon;
          need = now - w.at >= (moved ? gap : idleGap);
        }
      }
      if (need) {
        upserts.push(row);
        row.w = { at: now, lat: row.lat, lon: row.lon, sig: sigOf(row) };
        this.dirty.delete(id);
      }
    }
    const deletes = [...this.pendingDeletes];
    this.pendingDeletes.clear();
    return { upserts, deletes };
  }

  /** Tracks whose last contributor is `key` (lower case; exact match, or prefix when `prefix`), newest first, at most `limit`. */
  byUser(key, prefix = true, limit = BY_SEARCH_LIMIT) {
    const out = [];
    for (const row of this.rows.values()) {
      const b = row.by ? row.by.toLowerCase() : "";
      if (b && (b === key || (prefix && b.startsWith(key)))) out.push(row);
    }
    out.sort((a, b) => b.t - a.t);
    return out.slice(0, limit);
  }

  /** True while something is waiting to be written (the hub keeps an alarm armed). */
  get pending() { return this.dirty.size > 0 || this.pendingDeletes.size > 0; }
}
