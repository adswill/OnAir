// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// trail-store.test.js - TrailStore against an in-memory backend: caps, expiry, orphans, global cap, reads.
import test from "node:test";
import assert from "node:assert/strict";
import { TrailStore } from "../src/trail-store.js";
import { GLOBAL_TRAIL_CAP } from "../src/trail.js";

class Mem {
  constructor() { this.rows = []; this.tracks = new Set(); }
  last(id) { const r = this.rows.filter((x) => x.id === id).sort((a, b) => b.t - a.t)[0]; return r ? { ...r } : null; }
  count(id) { return this.rows.filter((x) => x.id === id).length; }
  insert(id, kind, p) { this.rows.push({ id, kind, ...p }); }
  deleteOldest(id, n) {
    const mine = this.rows.filter((x) => x.id === id).sort((a, b) => a.t - b.t).slice(0, n);
    this.rows = this.rows.filter((x) => !mine.includes(x));
  }
  deleteBefore(kind, cutoff) { this.rows = this.rows.filter((x) => !(x.kind === kind && x.t < cutoff)); }
  deleteOrphans() { this.rows = this.rows.filter((x) => this.tracks.has(x.id)); }
  total() { return this.rows.length; }
  deleteOldestGlobal(n) { const old = [...this.rows].sort((a, b) => a.t - b.t).slice(0, n); this.rows = this.rows.filter((x) => !old.includes(x)); }
  read(id, limit) { return this.rows.filter((x) => x.id === id).sort((a, b) => a.t - b.t).slice(-limit); }
  readAll() { return [...this.rows].sort((a, b) => (a.id < b.id ? -1 : a.id > b.id ? 1 : a.t - b.t)); }
}

const NOW = 1_800_000_000_000;
const fix = (i, step = 0.01) => ({ t: NOW - 3600e3 + i * 10000, lat: 25 + i * step, lon: 55, alt: 30000, hdg: 0 });

test("records spaced fixes, thins close ones, survives a restart (new store, same backend)", () => {
  const db = new Mem(); const s = new TrailStore(db);
  const id = "aircraft:ABC123";
  assert.equal(s.record(id, fix(0), NOW), true);
  assert.equal(s.record(id, { ...fix(1), lat: 25.00001 }, NOW), false, "within 100 m");
  assert.equal(s.record(id, fix(2), NOW), true);
  const s2 = new TrailStore(db); // DO restart: cache empty, state is re-read
  assert.equal(s2.record(id, fix(3), NOW), true);
  assert.equal(db.count(id), 3);
  assert.equal(s2.get([id])[id].length, 3);
});

test("per-track cap trims the oldest in batches", () => {
  const db = new Mem(); const s = new TrailStore(db);
  const id = "aircraft:CAP001";
  for (let i = 0; i < 400; i++) s.record(id, { ...fix(i), t: NOW - 3500e3 + i * 10000 }, NOW);
  const n = db.count(id);
  assert.ok(n <= 330 && n >= 300, String(n));
  assert.equal(db.read(id, 1000).at(-1).t, NOW - 3500e3 + 399 * 10000, "newest kept");
});

test("kinds without a trail (sat, station) store nothing; old fixes beyond the window refused", () => {
  const db = new Mem(); const s = new TrailStore(db);
  assert.equal(s.record("sat:ISS", fix(0), NOW), false);
  assert.equal(s.record("aircraft:OLD001", { ...fix(0), t: NOW - 2 * 3600e3 }, NOW), false);
  assert.equal(db.total(), 0);
});

test("prune: window expiry, orphans, global cap, then reads still work", () => {
  const db = new Mem(); const s = new TrailStore(db);
  db.tracks.add("aircraft:KEEP01");
  s.record("aircraft:KEEP01", fix(0), NOW);
  s.record("aircraft:KEEP01", { ...fix(50), t: NOW - 1000 }, NOW);
  s.record("aircraft:GONE01", fix(0), NOW); // track no longer in `tracks`
  s.prune(NOW);
  assert.equal(db.count("aircraft:GONE01"), 0);
  assert.equal(db.count("aircraft:KEEP01"), 2);
  s.prune(NOW + 2 * 3600e3); // both older than the 60 min aircraft window
  assert.equal(db.total(), 0);
  db.tracks.add("vessel:1");
  for (let i = 0; i < GLOBAL_TRAIL_CAP + 10; i++) db.rows.push({ id: "vessel:1", kind: "vessel", t: NOW - i, lat: 1, lon: 1, alt: null, hdg: null });
  s.prune(NOW);
  assert.ok(db.total() < GLOBAL_TRAIL_CAP);
});

test("get/all return compact points grouped by id", () => {
  const db = new Mem(); const s = new TrailStore(db);
  s.record("vessel:111", fix(0), NOW); s.record("vessel:111", fix(4), NOW); s.record("sonde:S1", fix(0), NOW);
  assert.deepEqual(Object.keys(s.all()).sort(), ["sonde:S1", "vessel:111"]);
  assert.equal(s.get(["vessel:111", "vessel:none"])["vessel:111"].length, 2);
  assert.equal(s.get(["vessel:none"])["vessel:none"], undefined);
});

test("buffered mode: nothing is written until flush(), then one batch; reads flush first; trims after the inserts", () => {
  const db = new Mem(); const s = new TrailStore(db, { buffer: true });
  const id = "aircraft:BUF001";
  for (let i = 0; i < 400; i++) s.record(id, { ...fix(i), t: NOW - 3500e3 + i * 10000 }, NOW);
  assert.equal(db.total(), 0, "no SQL yet");
  assert.equal(s.flush() > 300, true);
  assert.ok(db.count(id) <= 330 && db.count(id) >= 300, String(db.count(id)));
  assert.equal(s.flush(), 0, "second flush is empty");
  s.record(id, { ...fix(0), t: NOW + 900e3, lat: 40 }, NOW);
  assert.equal(s.get([id])[id].at(-1)[1], 40, "get() flushes the buffer first");
});

test("stepMs per kind: ships keep a point only every 30 s, corners still after 5 s", () => {
  const db = new Mem(); const s = new TrailStore(db);
  s.record("vessel:9", { t: NOW - 100e3, lat: 25, lon: 55, alt: null, hdg: 90 }, NOW);
  assert.equal(s.record("vessel:9", { t: NOW - 80e3, lat: 25.01, lon: 55, alt: null, hdg: 90 }, NOW), false, "20 s");
  assert.equal(s.record("vessel:9", { t: NOW - 60e3, lat: 25.01, lon: 55, alt: null, hdg: 90 }, NOW), true, "40 s");
  assert.equal(s.record("vessel:9", { t: NOW - 52e3, lat: 25.0101, lon: 55, alt: null, hdg: 130 }, NOW), true, "corner at 8 s");
});
