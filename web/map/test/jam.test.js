// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

import { test } from "node:test";
import assert from "node:assert/strict";
import { sanitizeJam, JamBook, JAM_MIN_GAP_MS, JAM_TTL_MS, MAX_CELLS_PER_POST } from "../src/jam.js";
import { cellsToGeo, level } from "../public/jamlayer.js";

const post = (station, cells) => ({ station, cell: 0.5, cells });
const ST = (i) => "station-" + String(i).padStart(4, "0");

test("sanitize accepts clean cells", () => {
  const c = sanitizeJam(post(ST(1), [[100, 20, 5, 2], [101, 20, 3, 0]]));
  assert.deepEqual(c.cells, [[100, 20, 5, 2], [101, 20, 3, 0]]);
});

test("privacy: any extra field in a cell, an id or a position rejects the whole post", () => {
  assert.equal(sanitizeJam(post(ST(1), [[100, 20, 5, 2, "4CA123"]])), null);
  assert.equal(sanitizeJam(post(ST(1), [[100, 20, 5, "4CA123"]])), null);
  assert.equal(sanitizeJam(post(ST(1), [[100.25, 20, 5, 2]])), null);   // finer than the cell
  assert.equal(sanitizeJam(post(ST(1), [{ icao: "4CA123", li: 1, lo: 2, n: 3, bad: 1 }])), null);
});

test("sanitize limits: range, counts, duplicates, size, cell size, station id", () => {
  assert.equal(sanitizeJam(post(ST(1), [[400, 20, 5, 2]])), null);
  assert.equal(sanitizeJam(post(ST(1), [[10, 20, 5, 6]])), null);     // bad > n
  assert.equal(sanitizeJam(post(ST(1), [[10, 20, 0, 0]])), null);
  assert.equal(sanitizeJam(post(ST(1), [[10, 20, 9999, 0]])), null);
  assert.equal(sanitizeJam(post(ST(1), [[10, 20, 5, 1], [10, 20, 4, 1]])), null);
  const many = Array.from({ length: MAX_CELLS_PER_POST + 1 }, (_, i) => [i % 300, Math.floor(i / 300), 3, 0]);
  assert.equal(sanitizeJam(post(ST(1), many)), null);
  assert.equal(sanitizeJam({ station: ST(1), cell: 1, cells: [] }), null);
  assert.equal(sanitizeJam(post("x", [])), null);
  assert.equal(sanitizeJam(null), null);
});

test("book sums stations, replaces a station's earlier post, hides thin cells", () => {
  const b = new JamBook();
  let t = 1e9;
  assert.equal(b.accept(ST(1), [[100, 20, 2, 1]], t), "ok");
  assert.deepEqual(b.cells(t), []);                       // 2 aircraft: below the minimum
  assert.equal(b.accept(ST(2), [[100, 20, 2, 2], [5, 5, 10, 0]], t), "ok");
  assert.deepEqual(b.cells(t + 11e3), [[5, 5, 10, 0], [100, 20, 4, 3]]);
  t += JAM_MIN_GAP_MS + 1;
  assert.equal(b.accept(ST(1), [[100, 20, 6, 0]], t), "ok");   // replaces, does not add
  assert.deepEqual(b.cells(t + 11e3).find((c) => c[0] === 100), [100, 20, 8, 2]);
});

test("book: rate limit, expiry, station cap, persistence round trip", () => {
  const b = new JamBook();
  const t = 5e9;
  assert.equal(b.accept(ST(1), [[1, 1, 5, 5]], t), "ok");
  assert.equal(b.accept(ST(1), [[1, 1, 5, 5]], t + 1000), "slow");
  assert.equal(b.cells(t + JAM_TTL_MS + 1).length, 0);
  const c = new JamBook();
  c.accept(ST(2), [[3, 4, 7, 3]], t);
  const d = new JamBook();
  d.load(JSON.parse(JSON.stringify(c.toJSON())), t + 1000);
  assert.deepEqual(d.cells(t + 20e3), [[3, 4, 7, 3]]);
  const e = new JamBook();
  e.load([["bad id", t, [[1, 1, 1, 1]]], [ST(3), t, [[1, 1, 1, 9]]]], t);
  assert.equal(e.stations.size, 0);
});

test("published cells carry no station ids", () => {
  const b = new JamBook();
  b.accept(ST(1), [[1, 1, 5, 5]], 1e9);
  assert.ok(!JSON.stringify(b.cells(1e9)).includes("station"));
});

test("synthetic fleet near a jammer: the cells around it light up, far cells stay green", () => {
  const b = new JamBook();
  const cells = [];
  for (let li = 100; li < 110; li++) for (let lo = 40; lo < 50; lo++) {
    const d = Math.hypot(li - 105, lo - 45);
    const n = 8;
    const bad = d < 3 ? n : d < 5 ? 3 : 0;
    cells.push([li, lo, n, bad]);
  }
  b.accept(ST(1), cells, 1e9);
  const geo = cellsToGeo(b.cells(1e9));
  assert.equal(geo.features.length, 100);
  const at = (li, lo) => geo.features.find((f) => f.geometry.coordinates[0][0][1] === li * 0.5 && f.geometry.coordinates[0][0][0] === lo * 0.5);
  assert.equal(at(105, 45).properties.lv, 3);
  assert.equal(at(100, 40).properties.lv, 0);
  assert.equal(level(2, 2), -1);
  assert.equal(level(10, 3), 2);
});
