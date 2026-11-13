// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// track-book.test.js - the in-memory track store: merge rules, write-behind selection, deltas, expiry.
import test from "node:test";
import assert from "node:assert/strict";
import { TrackBook, sanitize, mergePath, headingFromTrail, PERSIST_GAP_MS, PERSIST_IDLE_GAP_MS, REMOVED_LOG_MS } from "../src/track-book.js";

const NOW = 1_800_000_000_000;
const ac = (o = {}) => ({ kind: "aircraft", id: "4CA123", label: "TST1", lat: 50, lon: 8, heading: 90, speed: 300, alt: 30000, age: 1, src: "adsb", ...o });
const feed = (book, raw, now) => book.accept(sanitize(raw, now), now);

test("a first fix is accepted and visible; the same fix again is accepted but not visible (no version bump)", () => {
  const b = new TrackBook(NOW);
  const r = feed(b, ac(), NOW);
  assert.equal(r.visible, true);
  const v = b.version;
  const r2 = feed(b, ac(), NOW + 2000);
  assert.ok(r2);
  assert.equal(r2.visible, false);
  assert.equal(b.version, v, "an identical report changes nothing a viewer could see");
  assert.equal(r2.t, NOW + 2000, "but the liveness time moves on");
});

test("an older fix loses against a fresher one", () => {
  const b = new TrackBook(NOW);
  feed(b, ac({ age: 0 }), NOW);
  assert.equal(feed(b, ac({ lat: 49, age: 20 }), NOW + 1000), null);
  assert.equal(b.rows.get("aircraft:4CA123").lat, 50);
});

test("meta merge: a weaker source keeps the better label; a squawk/extra/cat sticks when the update has none", () => {
  const b = new TrackBook(NOW);
  feed(b, ac({ squawk: "7700", cat: "A3", extra: "x" }), NOW);
  const r = feed(b, ac({ lat: 50.01, src: "acars", label: "4CA123" }), NOW + 3000);
  assert.equal(r.squawk, "7700");
  assert.equal(r.cat, "A3");
  assert.equal(r.src, "adsb");
  assert.equal(r.label, "TST1");
});

test("heading falls back to the stored trail, then to the previous heading", () => {
  const b = new TrackBook(NOW);
  feed(b, ac({ heading: undefined, lat: 50, lon: 8, path: [] }), NOW);
  const r = feed(b, ac({ heading: undefined, lat: 50.01, lon: 8, path: [] }), NOW + 3000);
  assert.ok(Math.abs(r.heading) < 1 || Math.abs(r.heading - 360) < 1, "north: " + r.heading);
  assert.equal(headingFromTrail([[8, 50]], 8, 50, null), null);
  assert.equal(headingFromTrail([], 8, 50, 123), 123);
});

test("mergePath keeps the longer history and appends the new point once", () => {
  const old = [[1, 1], [2, 2], [3, 3]];
  assert.deepEqual(mergePath(old, [[9, 9]], 9, 9), [[1, 1], [2, 2], [3, 3], [9, 9]]);
  assert.deepEqual(mergePath(old, [[1, 1], [2, 2], [3, 3], [4, 4]], 4, 4).length, 4);
  assert.equal(mergePath(null, [[5, 5]], 5, 5).length, 1);
});

test("takePersist: new rows at once, a moving row once per gap, a still row only as a heartbeat, a text change at once", () => {
  const b = new TrackBook(NOW);
  feed(b, ac(), NOW);
  let p = b.takePersist(NOW + 1000);
  assert.equal(p.upserts.length, 1, "new row");
  feed(b, ac({ lat: 50.1 }), NOW + 5000);
  assert.equal(b.takePersist(NOW + 6000).upserts.length, 0, "moved, but written only 5 s ago");
  assert.equal(b.takePersist(NOW + 1000 + PERSIST_GAP_MS).upserts.length, 1, "moving: after the gap");
  feed(b, ac({ lat: 50.1 }), NOW + 40000);                       // same position again
  assert.equal(b.takePersist(NOW + 41000 + PERSIST_GAP_MS).upserts.length, 0, "still: no write inside the idle gap");
  feed(b, ac({ lat: 50.1, squawk: "7700" }), NOW + 50000);
  assert.equal(b.takePersist(NOW + 50500).upserts.length, 1, "squawk changed: immediately");
  feed(b, ac({ lat: 50.1, squawk: "7700" }), NOW + 60000);
  assert.equal(b.takePersist(NOW + 51000 + PERSIST_IDLE_GAP_MS).upserts.length, 1, "heartbeat after the idle gap");
  assert.equal(b.pending, false);
});

test("expiry removes the row, queues a delete only when it was ever written, and logs the removal for deltas", () => {
  const b = new TrackBook(NOW);
  feed(b, ac({ id: "AAAAAA" }), NOW);
  feed(b, ac({ id: "BBBBBB" }), NOW);
  b.takePersist(NOW + 1);                         // both written
  feed(b, ac({ id: "CCCCCC" }), NOW + 1000);      // never written
  const since = b.version;
  assert.equal(b.expire(NOW + 16 * 60e3 + 2000), 3 - 0, "all older than the 15 min aircraft TTL");
  assert.equal(b.size, 0);
  const p = b.takePersist(NOW + 17 * 60e3);
  assert.deepEqual(p.deletes.sort(), ["aircraft:AAAAAA", "aircraft:BBBBBB"], "CCCCCC was never stored");
  const d = b.deltaParts(since);
  assert.deepEqual(d.removed.sort(), ["aircraft:AAAAAA", "aircraft:BBBBBB", "aircraft:CCCCCC"]);
});

test("deltas: only changed rows after `since`; unknown or too-old versions fall back to a full snapshot (null)", () => {
  const b = new TrackBook(NOW);
  feed(b, ac({ id: "AAAAAA" }), NOW);
  const v1 = b.version;
  feed(b, ac({ id: "BBBBBB" }), NOW + 1000);
  feed(b, ac({ id: "AAAAAA", lat: 51 }), NOW + 4000);
  const d = b.deltaParts(v1);
  assert.equal(d.count, 2);
  assert.equal(JSON.parse(d.tracks).length, 2);
  assert.equal(b.deltaParts(b.version).count, 0, "up to date");
  assert.equal(b.deltaParts(b.version + 5), null, "from the future (another hub life)");
  assert.equal(b.deltaParts(NOW - 5), null, "before the log began");
  assert.equal(b.deltaParts(NaN), null);
  // removals older than the log window raise the floor
  feed(b, ac({ id: "CCCCCC" }), NOW + 5000);
  b.expire(NOW + 5000 + 16 * 60e3);
  b.expire(NOW + 5000 + 16 * 60e3 + REMOVED_LOG_MS + 1);
  assert.equal(b.deltaParts(v1), null, "log no longer complete for a client that old");
});

test("tracksJson is cached per version and equals the rows' wire JSON", () => {
  const b = new TrackBook(NOW);
  feed(b, ac({ id: "AAAAAA" }), NOW);
  const a = b.tracksJson();
  assert.equal(a, b.tracksJson());
  feed(b, ac({ id: "AAAAAA", lat: 52 }), NOW + 4000);
  const j = JSON.parse(b.tracksJson());
  assert.equal(j[0].lat, 52);
  assert.deepEqual(Object.keys(j[0]), ["id", "kind", "label", "sub", "lat", "lon", "alt", "heading", "speed", "extra", "t", "squawk", "cat", "vrate", "src", "age", "est", "dv"]);
});

test("load() restores rows as already persisted and keeps the path", () => {
  const b = new TrackBook(NOW);
  b.load([{ id: "aircraft:AAAAAA", kind: "aircraft", label: "X", sub: "", lat: 1, lon: 2, alt: 3, heading: 4, speed: 5, extra: "", t: NOW, squawk: null, cat: "", vrate: null, src: "adsb", age: 0, path: "[[2,1],[2.1,1.1]]", est: 0 }]);
  assert.equal(b.size, 1);
  assert.equal(b.rows.get("aircraft:AAAAAA").path.length, 2);
  assert.equal(b.takePersist(NOW + 1000).upserts.length, 0, "nothing to write back");
});

test("derived-kinematics flag (dv) is sanitised, carried over, and a bearing line (doa) is accepted with its own id and no path", () => {
  const b = new TrackBook(NOW);
  assert.equal(sanitize(ac({ dv: 1, speed: undefined, heading: undefined }), NOW).dv, 0, "dv needs a value to describe");
  feed(b, ac({ dv: 1 }), NOW);
  const r = feed(b, ac({ lat: 50.01, heading: undefined, speed: undefined, dv: 0 }), NOW + 4000);
  assert.equal(r.dv, 1, "kept speed/heading from the previous derived values stay marked derived");
  assert.equal(JSON.parse(b.wire(r)).dv, 1);
  const d = sanitize({ kind: "doa", id: "x", lat: 54.7, lon: 25.3, heading: 120, freq: 433920000, sigma: 3, conf: 0.8 }, NOW, "station-abc1");
  assert.equal(d.kind, "doa");
  assert.deepEqual(d.path, []);
  assert.equal(sanitize({ kind: "doa", lat: 54.7, lon: 25.3, heading: 120, freq: 433920000 }, NOW), null, "needs a station");
  const row = b.accept(d, NOW);
  assert.ok(row);
  assert.equal(b.doaRows().length, 1);
  const v = b.version;
  assert.equal(b.deltaParts(v - 1), null, "a changed bearing forces a full snapshot (fix markers are derived)");
  assert.ok(b.deltaParts(v));
});
