// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// trail-store.js - keeps the bounded position history per track. All SQL lives in SqlBackend so the
// logic in TrailStore can be tested against an in-memory backend (test/trail-store.test.js).
import {
  TRAIL_RULES, GLOBAL_TRAIL_CAP, kindOfId, ruleFor, shouldKeep, wirePoint, overflowCount, MAX_POINTS_PER_TRACK,
} from "./trail.js";

export class SqlBackend {
  constructor(sql) {
    this.sql = sql;
    // CREATE IF NOT EXISTS inside the Durable Object: no wrangler migration is needed for a new table.
    sql.exec(`
      CREATE TABLE IF NOT EXISTS trails (
        id TEXT NOT NULL,
        kind TEXT NOT NULL,
        t INTEGER NOT NULL,
        lat REAL NOT NULL,
        lon REAL NOT NULL,
        alt REAL,
        hdg REAL
      );
      CREATE INDEX IF NOT EXISTS idx_trails_id_t ON trails(id, t);
      DROP INDEX IF EXISTS idx_trails_kind_t;
    `);
    // The (kind, t) index was dropped: it cost one more billed row written for every trail point. Expiry by kind now scans
    // the table once per prune (every few minutes); rows read are billed at a thousandth of rows written.
  }
  last(id) {
    const r = this.sql.exec("SELECT t, lat, lon, hdg FROM trails WHERE id = ? ORDER BY t DESC LIMIT 1", id).toArray()[0];
    return r ? { t: r.t, lat: r.lat, lon: r.lon, hdg: r.hdg } : null;
  }
  count(id) {
    const r = this.sql.exec("SELECT COUNT(*) AS n FROM trails WHERE id = ?", id).toArray()[0];
    return r ? r.n : 0;
  }
  insert(id, kind, p) {
    this.sql.exec("INSERT INTO trails (id, kind, t, lat, lon, alt, hdg) VALUES (?, ?, ?, ?, ?, ?, ?)",
      id, kind, p.t, p.lat, p.lon, p.alt, p.hdg);
  }
  deleteOldest(id, n) {
    this.sql.exec("DELETE FROM trails WHERE rowid IN (SELECT rowid FROM trails WHERE id = ? ORDER BY t LIMIT ?)", id, n);
  }
  deleteBefore(kind, cutoff) {
    this.sql.exec("DELETE FROM trails WHERE kind = ? AND t < ?", kind, cutoff);
  }
  /** One DELETE for every kind: rules = { kind: { windowMs } }. */
  deleteExpired(rules, now) {
    const when = Object.entries(rules).map(([k, r]) => `WHEN '${k}' THEN ${Math.round(r.windowMs)}`).join(" ");
    this.sql.exec(`DELETE FROM trails WHERE t < ? - CASE kind ${when} ELSE 0 END`, now);
  }
  deleteOrphans() {
    this.sql.exec("DELETE FROM trails WHERE id NOT IN (SELECT id FROM tracks)");
  }
  total() {
    const r = this.sql.exec("SELECT COUNT(*) AS n FROM trails").toArray()[0];
    return r ? r.n : 0;
  }
  deleteOldestGlobal(n) {
    this.sql.exec("DELETE FROM trails WHERE rowid IN (SELECT rowid FROM trails ORDER BY t LIMIT ?)", n);
  }
  read(id, limit) {
    // newest `limit` rows, returned oldest first
    return this.sql.exec(
      "SELECT t, lat, lon, alt, hdg FROM (SELECT t, lat, lon, alt, hdg FROM trails WHERE id = ? ORDER BY t DESC LIMIT ?) ORDER BY t",
      id, limit,
    ).toArray();
  }
  readAll() {
    return this.sql.exec("SELECT id, t, lat, lon, alt, hdg FROM trails ORDER BY id, t").toArray();
  }
}

export class TrailStore {
  /** buffer: collect inserts in memory and write them in one batch on flush() (the hub does this inside its flush transaction). */
  constructor(backend, { buffer = false } = {}) {
    this.db = backend;
    this.buffer = buffer;
    this.pending = [];         // [{ id, kind, row }]
    this.trim = new Map();     // id -> rows to delete from the front after the pending inserts
    this.cache = new Map(); // id -> { last, n, seen }
  }

  state(id, now) {
    let s = this.cache.get(id);
    if (!s) {
      s = { last: this.db.last(id), n: 0, seen: now };
      s.n = s.last ? this.db.count(id) : 0;
      this.cache.set(id, s);
    }
    s.seen = now;
    return s;
  }

  /** Record one accepted fix: p = {t, lat, lon, alt, hdg}. Returns true when a row was written. */
  record(id, p, now) {
    const kind = kindOfId(id);
    const rule = ruleFor(kind);
    if (!rule) return false;
    if (p.t < now - rule.windowMs) return false;
    const s = this.state(id, now);
    if (!shouldKeep(s.last, p, rule.stepMs)) return false;
    const row = { t: p.t, lat: p.lat, lon: p.lon, alt: p.alt ?? null, hdg: p.hdg ?? null };
    if (this.buffer) this.pending.push({ id, kind, row }); else this.db.insert(id, kind, row);
    s.last = row;
    s.n += 1;
    const over = overflowCount(s.n, rule.max);
    if (over) {
      if (this.buffer) this.trim.set(id, (this.trim.get(id) || 0) + over); else this.db.deleteOldest(id, over);
      s.n -= over;
    }
    return true;
  }

  /** Write what record() collected (no-op when not buffering). Call inside one storage transaction. */
  flush() {
    if (!this.pending.length && !this.trim.size) return 0;
    const n = this.pending.length;
    for (const { id, kind, row } of this.pending) this.db.insert(id, kind, row);
    this.pending = [];
    for (const [id, over] of this.trim) this.db.deleteOldest(id, over);
    this.trim.clear();
    return n;
  }

  /** Called from the hub's periodic prune: expiry by kind window, orphans (track gone), global cap, cache cleanup. */
  prune(now) {
    this.flush();
    if (this.db.deleteExpired) this.db.deleteExpired(TRAIL_RULES, now);
    else for (const kind of Object.keys(TRAIL_RULES)) this.db.deleteBefore(kind, now - TRAIL_RULES[kind].windowMs);
    this.db.deleteOrphans();
    const extra = this.db.total() - GLOBAL_TRAIL_CAP;
    if (extra > 0) this.db.deleteOldestGlobal(extra + Math.ceil(GLOBAL_TRAIL_CAP * 0.05));
    // Counts may have changed under the cache (expiry, orphans): clear it, entries are re-read lazily on the next fix.
    // CACHE_MS only matters between prunes, which run every ~45 s, so it is simply that interval.
    this.cache.clear();
  }

  /** { id: [wire points] } for the given ids (unknown / empty ids are omitted). */
  get(ids) {
    this.flush();
    const out = {};
    for (const id of ids) {
      const rows = this.db.read(id, MAX_POINTS_PER_TRACK);
      if (rows.length) out[id] = rows.map((r) => wirePoint({ t: r.t, lat: r.lat, lon: r.lon, alt: r.alt, hdg: r.hdg }));
    }
    return out;
  }

  /** Every trail, grouped by id (for /live?trails=1). */
  all() {
    this.flush();
    const out = {};
    for (const r of this.db.readAll()) (out[r.id] || (out[r.id] = [])).push(wirePoint({ t: r.t, lat: r.lat, lon: r.lon, alt: r.alt, hdg: r.hdg }));
    return out;
  }
}
