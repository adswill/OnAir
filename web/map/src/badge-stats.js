// badge-stats.js - per-user stats and badges, kept by the hub from what a signed-in user actually shares (docs/MAP-BADGES-4.0.md).
// Pure (no Cloudflare APIs): all storage goes through a backend object (src/badge-store.js has the SQLite one, the tests a memory one).
//
// What counts: each unique object once per user (aircraft by ICAO address, ships by MMSI, sondes by serial, APRS stations by callsign,
// satellites, ham contacts, DF fixes, ACARS messages); time shared (seconds between credited posts, at most 120 s per gap, per user, so
// two stations or a fast post rate do not count double); days, streaks, record range, countries and Maidenhead fields, rare catches.
// Anti-cheat: the track must have passed sanitize() (age, position, kind); ids must look real; per-user rate limits and hourly caps; a post
// that repeats one of the user's last posts earns nothing; a position that disagrees with what other stations report at the same moment
// earns nothing; range and rare catches need two reports of the same object at least 20 s apart that agree with the object's speed.

import { BADGES, tierFor, BADGE_BY_ID } from "../public/badges/defs.js";
import { distKm, fieldOf, icaoCountry, mmsiCountry } from "./badge-geo.js";

export const CREDIT_GAP_MS = 2000;          // at most one credited post per user per 2 s (more posts are accepted, they just earn nothing)
export const MAX_GAP_CREDIT_MS = 120_000;   // time credit for one gap between credited posts
export const SESSION_GAP_MS = 300_000;      // a gap longer than this starts a new session (the first post of a session earns 60 s)
export const FIRST_POST_CREDIT_MS = 60_000;
export const MAX_STATIONS_PER_DAY = 10;     // stations that may earn for one user in 24 h
export const CONFIRM_MIN_MS = 20_000;       // range / rare: the second report must come at least this long after the first ...
export const CONFIRM_MAX_MS = 15 * 60_000;  // ... and within this long
export const HOME_PAUSE_MS = 3600_000;      // range credit pauses this long after the station's home square moves > 30 km
export const SAVE_GAP_MS = 30_000;          // a user's row is written back at most this often (sooner after a new badge)
export const IDLE_DROP_MS = 2 * 3600_000;   // users not seen for this long leave memory (their rows are already written)

/** New unique objects one user may earn per hour, per kind (beyond it the object is not counted yet; a later hour can count it). */
export const HOURLY_CAP = { aircraft: 3000, vessel: 2000, aprs: 1500, ham: 3000, sat: 300, sonde: 60, acars: 2000, fix: 120, flarm: 500, drone: 200, buoy: 200, dsc: 300, gnss: 200 };
/** Farthest plausible direct reception per kind (km); a range above it is not credited. */
export const MAX_RANGE_KM = { aircraft: 650, vessel: 400, sonde: 800, aprs: 600, flarm: 250, drone: 30 };
/** Fastest plausible speed per kind (m/s) between two reports used for range or rare-catch confirmation. */
const VMAX = { aircraft: 350, vessel: 30, sonde: 150, aprs: 150, flarm: 120, drone: 50 };
/** Sources that are direct line-of-sight receptions (only these earn range). */
const DIRECT_SRC = { aircraft: ["adsb", "uat", "1090", "978"], vessel: ["ais"], sonde: ["sonde", ""], aprs: ["aprs"], flarm: ["flarm"], drone: ["rid", "adsb"] };
/** Another station's report this far away at the same moment makes the object a conflict (km). */
const CONFLICT_KM = { aircraft: 40, vessel: 10, sonde: 60, aprs: 30, flarm: 20 };

const COUNT_KINDS = ["aircraft", "vessel", "sonde", "aprs", "sat", "ham", "fix", "acars", "flarm", "drone", "buoy", "dsc", "gnss"];

export function emptyStats() {
  const c = {};
  for (const k of COUNT_KINDS) c[k] = 0;
  return {
    v: 1, first: 0, shares: 0, secs: 0, night: 0, early: 0, lastAt: 0,
    days: 0, lastDay: -1, streak: 0, bestStreak: 0,
    c, objects: 0, rangeM: 0, rangeKind: "", rangeAt: 0,
    countries: [], fields: [], rare: {}, kinds: [],
    wk: { k: "", secs: 0, objects: 0, aircraft: 0, vessel: 0, rangeM: 0 },
    mo: { k: "", secs: 0, objects: 0, aircraft: 0, vessel: 0, rangeM: 0 },
    earned: {},
  };
}

// ---------------------------------------------------------------- time helpers

export function dayIndex(ms) { return Math.floor(ms / 86400000); }

/** ISO 8601 week key "2026-W46" (UTC). */
export function weekKey(ms) {
  const d = new Date(Math.floor(ms / 86400000) * 86400000);
  const wd = (d.getUTCDay() + 6) % 7;           // Monday 0
  d.setUTCDate(d.getUTCDate() - wd + 3);        // the Thursday of this week decides the year
  const y = d.getUTCFullYear();
  const jan4 = new Date(Date.UTC(y, 0, 4));
  const w = 1 + Math.round(((d - jan4) / 86400000 - 3 + ((jan4.getUTCDay() + 6) % 7)) / 7);
  return `${y}-W${String(w).padStart(2, "0")}`;
}
export function monthKey(ms) {
  const d = new Date(ms);
  return `${d.getUTCFullYear()}-${String(d.getUTCMonth() + 1).padStart(2, "0")}`;
}

/** Local solar hour (0..24) at a longitude: UTC hour + lon / 15. Needs no time zone and works anywhere on the map. */
export function solarHour(ms, lon) {
  const h = (ms / 3600000 + lon / 15) % 24;
  return h < 0 ? h + 24 : h;
}

// ---------------------------------------------------------------- per-object rules

const ICAO_RE = /^[0-9A-F]{6}$/;
const APRS_RE = /^(?=[A-Z0-9]*[0-9])[A-Z0-9]{3,7}(-[0-9]{1,2})?$/;

/**
 * The unique key an accepted track earns, or null when it earns nothing (not a counted kind, or an id that does not look real).
 * `row` is the sanitize() output (or the TrackBook row): kind, rawId, icao, mmsi, alt, speed, est.
 */
export function objectKey(row) {
  const kind = row.kind;
  const id = String(row.rawId || "");
  switch (kind) {
    case "aircraft": {
      const h = String(row.icao || "");
      if (!ICAO_RE.test(h) || h === "000000" || h === "FFFFFF") return null;
      if (row.alt != null && (row.alt < -1500 || row.alt > 65000)) return null;
      if (row.speed != null && (row.speed < 0 || row.speed > 1300)) return null;
      return { kind, key: "a:" + h };
    }
    case "vessel": {
      const m = String(row.mmsi || "");
      if (!/^[2-7]\d{8}$/.test(m)) return null;     // ship stations only (SART / MOB / EPIRB are rare catches, not ships)
      if (row.speed != null && (row.speed < 0 || row.speed > 70)) return null;
      return { kind, key: "v:" + m };
    }
    case "flarm": return ICAO_RE.test(id.toUpperCase()) ? { kind, key: "f:" + id.toUpperCase() } : null;
    case "sonde":
      if (!/^[A-Za-z0-9-]{4,16}$/.test(id)) return null;
      if (row.alt != null && (row.alt < -500 || row.alt > 45000)) return null;
      return { kind, key: "s:" + id.toUpperCase() };
    case "aprs": { const cs = id.toUpperCase(); return APRS_RE.test(cs) ? { kind, key: "p:" + cs } : null; }
    case "ham": return /^[A-Za-z0-9/_:.>-]{3,60}$/.test(id) ? { kind, key: "h:" + id.toUpperCase() } : null;
    case "sat": return /^[A-Za-z0-9 ._()-]{1,32}$/.test(id) ? { kind, key: "t:" + id.toUpperCase() } : null;
    case "gnss": return /^[A-Za-z0-9 ._-]{1,16}$/.test(id) ? { kind, key: "g:" + id.toUpperCase() } : null;
    case "dsc": { const m = String(row.mmsi || ""); return /^\d{9}$/.test(m) ? { kind, key: "d:" + m } : null; }
    case "drone": return /^[A-Za-z0-9 ._:-]{2,40}$/.test(id) ? { kind, key: "r:" + id.toUpperCase() } : null;
    case "buoy": return /^[A-Za-z0-9 ._:-]{1,40}$/.test(id) ? { kind, key: "b:" + id.toUpperCase() } : null;
    default: return null;   // station, doa (bearing lines count as a kind only), fix
  }
}

/** Rare-catch keys of one track (see RARE in public/badges/defs.js). */
export function rareKinds(row) {
  const out = [];
  if (row.kind === "aircraft") {
    const sq = String(row.squawk || "");
    if (sq === "7500" || sq === "7600" || sq === "7700") out.push("sq" + sq);
    const cat = String(row.cat || "").toUpperCase();
    if (cat === "B1") out.push("glider");
    else if (cat === "B2") out.push("balloon");
    else if (cat === "B3") out.push("para");
    else if (cat === "B7") out.push("space");
  } else if (row.kind === "vessel" || row.kind === "dsc") {
    const m = String(row.mmsi || "");
    if (/^970\d{6}$/.test(m)) out.push("sart");
    else if (/^972\d{6}$/.test(m)) out.push("mob");
    else if (/^974\d{6}$/.test(m)) out.push("epirb");
  }
  return out;
}

function isDirect(row) {
  const list = DIRECT_SRC[row.kind];
  if (!list || row.est) return false;
  return list.includes(String(row.src || "").toLowerCase());
}

/** A small hash of a post's object positions: the same post sent again (a replay or a duplicate) has the same value. */
export function postFingerprint(rows) {
  let h1 = 0x811c9dc5, h2 = 0x01000193;
  const parts = rows.map((r) => `${r.id}|${Math.round(r.lat * 1e5)}|${Math.round(r.lon * 1e5)}|${r.alt == null ? "" : Math.round(r.alt)}`).sort();
  for (const p of parts) for (let i = 0; i < p.length; i++) {
    const c = p.charCodeAt(i);
    h1 = Math.imul(h1 ^ c, 16777619) >>> 0;
    h2 = Math.imul(h2 ^ c, 2246822519) >>> 0;
  }
  return h1.toString(36) + "." + h2.toString(36) + "." + parts.length;
}

// ---------------------------------------------------------------- values the badges and boards read

export function statValue(s, stat) {
  switch (stat) {
    case "shares": return s.shares > 0 ? 1 : 0;
    case "hours": return s.secs / 3600;
    case "days": return s.days;
    case "bestStreak": return s.bestStreak;
    case "rangeKm": return s.rangeM / 1000;
    case "countries": return s.countries.length;
    case "fields": return s.fields.length;
    case "nightHours": return s.night / 3600;
    case "earlyHours": return s.early / 3600;
    case "rare": return Object.keys(s.rare).length;
    case "kinds": return s.kinds.length;
    case "objects": return s.objects;
    default: return s.c[stat] || 0;
  }
}

/** A board's value for a period ("week", "month", "all"); a stale week or month counts 0. */
export function periodValue(s, stat, period, now) {
  if (period === "all") return statValue(s, stat);
  const p = period === "week" ? s.wk : s.mo;
  const cur = period === "week" ? weekKey(now) : monthKey(now);
  if (!p || p.k !== cur) return 0;
  switch (stat) {
    case "hours": return p.secs / 3600;
    case "objects": return p.objects;
    case "aircraft": return p.aircraft;
    case "vessel": return p.vessel;
    case "rangeKm": return p.rangeM / 1000;
    default: return 0;
  }
}

/** The current daily streak (0 when the last share was before yesterday, UTC). */
export function currentStreak(s, now) {
  const d = dayIndex(now);
  return s.lastDay === d || s.lastDay === d - 1 ? s.streak : 0;
}

/** Highest tier earned per badge: { id: tier }. */
export function earnedTiers(s) {
  const o = {};
  for (const [id, list] of Object.entries(s.earned)) if (list.length) o[id] = list[list.length - 1][0];
  return o;
}

/** Award every tier newly reached; returns [{ id, tier, at }]. */
export function evaluateBadges(s, now) {
  const out = [];
  for (const b of BADGES) {
    const tier = tierFor(b, statValue(s, b.stat));
    const list = s.earned[b.id] || [];
    const have = list.length ? list[list.length - 1][0] : 0;
    for (let t = have + 1; t <= tier; t++) { list.push([t, now]); out.push({ id: b.id, tier: t, at: now }); }
    if (list.length) s.earned[b.id] = list;
  }
  return out;
}

/** The badge shown next to the user's name on the map: their highest tier, the rarest badge on a tie. "id:tier" or "". */
export function showcase(s) {
  let best = "", bestTier = 0, bestIdx = -1;
  const tiers = earnedTiers(s);
  BADGES.forEach((b, i) => {
    const t = tiers[b.id] || 0;
    if (t > bestTier || (t === bestTier && t > 0 && i > bestIdx)) { best = b.id; bestTier = t; bestIdx = i; }
  });
  return best ? `${best}:${bestTier}` : "";
}

// ---------------------------------------------------------------- the book

export class MemoryBadgeBackend {
  constructor() { this.users = new Map(); this.seen = new Map(); this.tokens = new Map(); }
  tokenGet(h) { const t = this.tokens.get(h); return t ? { ...t } : null; }
  tokenPut(h, uid, kind, exp) { if (!this.tokens.has(h)) this.tokens.set(h, { uid, kind, exp, revoked: 0 }); }
  tokenRevoke(h) { const t = this.tokens.get(h); if (t) t.revoked = 1; }
  tokenRevokeUser(uid, kind) { for (const t of this.tokens.values()) if (t.uid === uid && t.kind === kind) t.revoked = 1; }
  tokenCount(uid, kind, nowSec) { let n = 0; for (const t of this.tokens.values()) if (t.uid === uid && t.kind === kind && !t.revoked && t.exp >= nowSec) n++; return n; }
  tokenPrune(nowSec) { for (const [h, t] of this.tokens) if (t.exp < nowSec - 86400) this.tokens.delete(h); }
  loadUser(uid) { const u = this.users.get(uid); return u ? JSON.parse(JSON.stringify(u)) : null; }
  saveUser(u) { this.users.set(u.uid, JSON.parse(JSON.stringify({ uid: u.uid, login: u.login, public: u.public, badges: u.badges, hidden: u.hidden, by: u.by, appsBefore: u.appsBefore, s: u.s }))); }
  deleteUser(uid) { this.users.delete(uid); this.seen.delete(uid); for (const [h, t] of this.tokens) if (t.uid === uid) this.tokens.delete(h); }
  loadSeen(uid) { return [...(this.seen.get(uid) || [])]; }
  addSeen(uid, keys) { let s = this.seen.get(uid); if (!s) { s = new Set(); this.seen.set(uid, s); } for (const k of keys) s.add(k); }
  claims() { const o = []; for (const u of this.users.values()) if (u.by) o.push([u.by, u.uid]); return o; }
  findLogin(lc) { for (const u of this.users.values()) if (u.login.toLowerCase() === lc) return u.uid; return 0; }
  boardRows() { return [...this.users.values()].filter((u) => u.public && u.badges && !u.hidden).map((u) => JSON.parse(JSON.stringify(u))); }
}

export class BadgeBook {
  constructor(backend) {
    this.db = backend;
    this.users = new Map();     // uid -> user (with runtime fields prefixed _)
  }

  /** The user, loaded or made (badges and public profile both off until the user turns them on). */
  user(uid, login) {
    let u = this.users.get(uid);
    if (!u) {
      const r = this.db.loadUser(uid);
      u = r ? { ...r, s: { ...emptyStats(), ...(r.s || {}) } }
        : { uid, login: login || "", public: false, badges: false, hidden: false, by: "", appsBefore: 0, s: emptyStats(), _new: true };
      Object.assign(u, { _seen: null, _pendingSeen: [], _last: new Map(), _cand: new Map(), _fps: [], _creditAt: 0, _hour: { h: -1, n: {} },
        _stations: new Map(), _home: null, _homePause: 0, _dirty: !!u._new, _savedAt: 0, _touched: 0, _urgent: false });
      this.users.set(uid, u);
    }
    if (login && u.login !== login) { u.login = login; u._dirty = true; }
    return u;
  }

  seenSet(u) {
    if (!u._seen) u._seen = new Set(this.db.loadSeen(u.uid));
    return u._seen;
  }

  /**
   * Credit one ingest post. `rows`: the tracks the hub accepted from it (sanitize() output, before TrackBook.accept()); `prev`: Map id ->
   * the hub's row for that id before this post (or undefined); `home`: { lat, lon } of the station's locator square or null; `extraKeys`:
   * [{ kind: "acars" | "fix", key }] (ACARS messages and DF fixes found by the hub for this post).
   * Returns { credited, reason, awards: [{ id, tier, at }] }.
   */
  credit({ uid, login, station, rows, prev, home, extraKeys = [], now }) {
    const u = this.user(uid, login);
    u._touched = now;
    if (!u.badges || u.hidden) return { credited: false, reason: u.hidden ? "hidden" : "badges_off", awards: [] };
    if (now - u._creditAt < CREDIT_GAP_MS) return { credited: false, reason: "rate", awards: [] };
    // stations per user per day
    for (const [st, at] of u._stations) if (now - at > 86400000) u._stations.delete(st);
    if (!u._stations.has(station) && u._stations.size >= MAX_STATIONS_PER_DAY) return { credited: false, reason: "stations", awards: [] };
    // the same post again (a replay or a duplicate): nothing
    const fp = postFingerprint(rows);
    if (rows.length && u._fps.includes(fp)) return { credited: false, reason: "duplicate", awards: [] };

    // conflicts: another station reports the object far away right now
    let checked = 0, conflicts = 0;
    const bad = new Set();
    for (const r of rows) {
      const p = prev && prev.get(r.id);
      const lim = CONFLICT_KM[r.kind];
      if (!p || !lim || r.est || p.est || now - p.t > 30_000) continue;
      const mine = u._last.get(r.id);
      if (mine && mine.lat === p.lat && mine.lon === p.lon) continue;   // that row is our own last report
      checked++;
      if (distKm(p.lat, p.lon, r.lat, r.lon) > lim) { conflicts++; bad.add(r.id); }
    }
    if (checked >= 3 && conflicts * 2 > checked) return { credited: false, reason: "conflict", awards: [] };

    // fresh: at least one object is new to this user or has moved since this user's last report of it
    let fresh = false;
    for (const r of rows) {
      if (bad.has(r.id)) continue;
      const mine = u._last.get(r.id);
      if (!mine || mine.lat !== r.lat || mine.lon !== r.lon || mine.alt !== r.alt) { fresh = true; break; }
    }
    if (!fresh && !extraKeys.length) return { credited: false, reason: "stale", awards: [] };

    const s = u.s;
    rollPeriods(s, now);
    u._creditAt = now;
    u._stations.set(station, now);
    u._fps.push(fp); if (u._fps.length > 6) u._fps.shift();
    if (!s.first) s.first = now;
    s.shares++;

    // time: the gap since the last credited post (per user, so several stations do not add up)
    const gap = s.lastAt && now - s.lastAt <= SESSION_GAP_MS ? Math.min(now - s.lastAt, MAX_GAP_CREDIT_MS) : FIRST_POST_CREDIT_MS;
    const sec = gap / 1000;
    s.secs += sec; s.wk.secs += sec; s.mo.secs += sec;
    s.lastAt = now;
    const lon = home ? home.lon : meanLon(rows);
    if (lon != null) {
      const h = solarHour(now, lon);
      if (h < 4) s.night += sec; else if (h < 7) s.early += sec;
    }
    // days and streaks (UTC days)
    const d = dayIndex(now);
    if (d !== s.lastDay) {
      s.days++;
      s.streak = s.lastDay === d - 1 ? s.streak + 1 : 1;
      s.bestStreak = Math.max(s.bestStreak, s.streak);
      s.lastDay = d;
    }

    // home square: a move of more than 30 km pauses range credit for an hour (so a moved "home" cannot claim a range at once)
    if (home) {
      if (u._home && distKm(u._home.lat, u._home.lon, home.lat, home.lon) > 30) { u._homePause = now + HOME_PAUSE_MS; u._cand.clear(); }
      u._home = home;
    }

    // hourly caps
    const hr = Math.floor(now / 3600000);
    if (u._hour.h !== hr) u._hour = { h: hr, n: {} };
    const seen = this.seenSet(u);
    const kinds = new Set(s.kinds);
    const countries = new Set(s.countries), fields = new Set(s.fields);
    const countNew = (kind, key) => {
      if (seen.has(key)) return false;
      const cap = HOURLY_CAP[kind] ?? 200;
      if ((u._hour.n[kind] || 0) >= cap) return false;
      u._hour.n[kind] = (u._hour.n[kind] || 0) + 1;
      seen.add(key); u._pendingSeen.push(key);
      s.c[kind] = (s.c[kind] || 0) + 1;
      s.objects++; s.wk.objects++; s.mo.objects++;
      if (kind === "aircraft" || kind === "vessel") { s.wk[kind]++; s.mo[kind]++; }
      return true;
    };

    for (const r of rows) {
      if (r.kind === "station") continue;
      if (r.kind === "doa") { kinds.add("doa"); continue; }
      if (bad.has(r.id)) continue;
      const ok = objectKey(r);
      u._last.set(r.id, { lat: r.lat, lon: r.lon, alt: r.alt, t: now });
      if (!ok) continue;
      kinds.add(ok.kind);
      if (countNew(ok.kind, ok.key)) {
        const cc = r.kind === "aircraft" ? icaoCountry(r.icao) : r.kind === "vessel" ? mmsiCountry(r.mmsi) : "";
        if (cc) countries.add(cc);
      }
      if (!r.est) fields.add(fieldOf(r.lat, r.lon));
      // range and rare catches need a confirming second report
      const rare = rareKinds(r);
      const direct = home && isDirect(r) && now >= u._homePause;
      if (!direct && !rare.length) continue;
      const c = u._cand.get(r.id);
      const confirmed = c && now - c.t >= CONFIRM_MIN_MS && now - c.t <= CONFIRM_MAX_MS
        && distKm(c.lat, c.lon, r.lat, r.lon) * 1000 / ((now - c.t) / 1000) <= (VMAX[r.kind] || 150);
      if (!c || now - c.t > CONFIRM_MAX_MS) u._cand.set(r.id, { lat: r.lat, lon: r.lon, t: now, km: direct ? distKm(home.lat, home.lon, r.lat, r.lon) : 0 });
      if (!confirmed) continue;
      for (const k of rare) if (!s.rare[k]) s.rare[k] = now;
      if (direct) {
        const km = Math.min(c.km || Infinity, distKm(home.lat, home.lon, r.lat, r.lon));
        if (Number.isFinite(km) && km <= (MAX_RANGE_KM[r.kind] || 0)) {
          const m = Math.round(km * 1000);
          if (m > s.rangeM) { s.rangeM = m; s.rangeKind = r.kind; s.rangeAt = now; }
          if (m > s.wk.rangeM) s.wk.rangeM = m;
          if (m > s.mo.rangeM) s.mo.rangeM = m;
        }
      }
    }
    for (const e of extraKeys) {
      kinds.add(e.kind);
      countNew(e.kind, e.key);
    }
    if (u._last.size > 6000) { const drop = u._last.size - 5000; let i = 0; for (const k of u._last.keys()) { if (i++ >= drop) break; u._last.delete(k); } }
    if (u._cand.size > 2000) u._cand.clear();
    s.kinds = [...kinds].slice(0, 32);
    s.countries = [...countries].slice(0, 300);
    s.fields = [...fields].slice(0, 324);
    const awards = evaluateBadges(s, now);
    u._dirty = true;
    if (awards.length) u._urgent = true;
    return { credited: true, reason: "", awards };
  }

  /** Users to write back now (and their new unique keys); marks them saved. */
  takeDirty(now) {
    const out = [];
    for (const u of this.users.values()) {
      if (!u._dirty) continue;
      if (!u._urgent && now - u._savedAt < SAVE_GAP_MS) continue;
      out.push({ user: u, seen: u._pendingSeen });
      u._pendingSeen = []; u._dirty = false; u._urgent = false; u._savedAt = now; delete u._new;
    }
    return out;
  }

  /** Write back through the backend (used by the tests and by the hub's flush inside one transaction). */
  flush(now) {
    for (const { user, seen } of this.takeDirty(now)) {
      this.db.saveUser(user);
      if (seen.length) this.db.addSeen(user.uid, seen);
    }
  }

  /** Forget idle users (call after a flush, so nothing is lost). */
  dropIdle(now) {
    for (const [uid, u] of this.users) if (!u._dirty && now - u._touched > IDLE_DROP_MS) this.users.delete(uid);
  }

  /** Settings change from the profile page or the app. Returns the user. */
  setSettings(uid, login, { badges, public: pub, by }) {
    const u = this.user(uid, login);
    if (typeof badges === "boolean") u.badges = badges;
    if (typeof pub === "boolean") u.public = pub;
    if (typeof by === "string") u.by = by;
    u._dirty = true; u._urgent = true;
    return u;
  }

  /** Delete everything kept about a user (the "Delete my badge data" button). */
  forget(uid) {
    this.users.delete(uid);
    this.db.deleteUser(uid);
  }
}

function rollPeriods(s, now) {
  const wk = weekKey(now), mo = monthKey(now);
  if (s.wk.k !== wk) s.wk = { k: wk, secs: 0, objects: 0, aircraft: 0, vessel: 0, rangeM: 0 };
  if (s.mo.k !== mo) s.mo = { k: mo, secs: 0, objects: 0, aircraft: 0, vessel: 0, rangeM: 0 };
}

function meanLon(rows) {
  let x = 0, y = 0, n = 0;
  for (const r of rows) { if (r.kind === "station" || r.est) continue; x += Math.cos((r.lon * Math.PI) / 180); y += Math.sin((r.lon * Math.PI) / 180); n++; }
  return n ? (Math.atan2(y, x) * 180) / Math.PI : null;
}

/** The public view of a user: stats, badges with the date each tier was earned, progress. */
export function publicProfile(u, now) {
  const s = u.s;
  const badges = [];
  for (const b of BADGES) {
    const list = s.earned[b.id] || [];
    const value = statValue(s, b.stat);
    // name, description, icon and thresholds come along, so the app shows them without a copy of the data file
    badges.push({ id: b.id, name: b.name, desc: b.desc, icon: b.icon, unit: b.unit, tiers: b.tiers,
      tier: list.length ? list[list.length - 1][0] : 0, earned: list.map(([tier, at]) => ({ tier, at })), value: round2(value) });
  }
  return {
    login: u.login, by: u.by || "", first: s.first || 0,
    stats: {
      hours: round2(s.secs / 3600), days: s.days, streak: currentStreak(s, now), bestStreak: s.bestStreak,
      objects: s.objects, counts: { ...s.c }, rangeKm: round2(s.rangeM / 1000), rangeKind: s.rangeKind, rangeAt: s.rangeAt,
      countries: [...s.countries].sort(), fields: [...s.fields].sort(), rare: { ...s.rare }, kinds: [...s.kinds].sort(),
      nightHours: round2(s.night / 3600), earlyHours: round2(s.early / 3600),
      week: { hours: round2(periodValue(s, "hours", "week", now)), objects: periodValue(s, "objects", "week", now) },
      month: { hours: round2(periodValue(s, "hours", "month", now)), objects: periodValue(s, "objects", "month", now) },
    },
    badges, showcase: showcase(s),
  };
}

function round2(x) { return Math.round(x * 100) / 100; }

export { BADGE_BY_ID };
