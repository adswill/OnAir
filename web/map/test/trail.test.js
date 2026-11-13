// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// trail.test.js - pure trail rules (hub side) and the client merge (node --test, no deps).
import test from "node:test";
import assert from "node:assert/strict";
import { shouldKeep, headingDelta, distanceM, overflowCount, parseTrailRequest, wirePoint, kindOfId, ruleFor, MAX_IDS } from "../src/trail.js";
import { mergeTrails, appendLive, toLine } from "../public/trail.js";

const P = (t, lat, lon, hdg = 90) => ({ t, lat, lon, hdg });

test("thinning: first point kept, too soon or too close dropped", () => {
  assert.equal(shouldKeep(null, P(0, 0, 0)), true);
  assert.equal(shouldKeep(P(0, 25, 55), P(3000, 25.01, 55)), false, "under 5 s");
  assert.equal(shouldKeep(P(0, 25, 55), P(10000, 25.0001, 55)), false, "under 100 m");
  assert.equal(shouldKeep(P(0, 25, 55), P(10000, 25.01, 55)), true, "1.1 km");
  assert.equal(shouldKeep(P(5000, 25, 55), P(5000, 26, 55)), false, "same or older timestamp");
});

test("thinning: a heading change keeps a close point (but not sooner than 5 s)", () => {
  assert.equal(shouldKeep(P(0, 25, 55, 90), P(10000, 25.0001, 55, 120)), true);
  assert.equal(shouldKeep(P(0, 25, 55, 90), P(2000, 25.0001, 55, 120)), false);
  assert.equal(headingDelta(350, 10), 20);
  assert.equal(headingDelta(null, 10), 0);
});

test("distance and overflow", () => {
  const d = distanceM(25, 55, 25.01, 55);
  assert.ok(d > 1100 && d < 1120, String(d));
  assert.equal(overflowCount(300, 300), 0);
  assert.equal(overflowCount(330, 300), 0, "inside the 10 % slack");
  assert.equal(overflowCount(331, 300), 31);
});

test("rules by kind", () => {
  assert.equal(ruleFor("aircraft").max, 300);
  assert.equal(ruleFor("aircraft").windowMs, 3600e3);
  assert.equal(ruleFor("vessel").max, 200);
  assert.equal(ruleFor("sonde").max, 1000);
  assert.equal(ruleFor("sat"), null);
  assert.equal(kindOfId("aircraft:4B1A2C"), "aircraft");
  assert.equal(kindOfId("nocolon"), "");
});

test("wire point is compact and rounded", () => {
  assert.deepEqual(wirePoint({ t: 5, lat: 25.123456789, lon: 55.1, alt: 35000.4, hdg: 89.6 }), [5, 25.12346, 55.1, 35000, 90]);
  assert.deepEqual(wirePoint({ t: 5, lat: 1, lon: 2 }), [5, 1, 2, null, null]);
});

test("request parsing: ids, repeats, dedupe, unknown kinds and sats dropped, limit", () => {
  const q = parseTrailRequest(new URLSearchParams("ids=aircraft:AAAAAA,vessel:123456789,aircraft:AAAAAA,sat:x,junk,"));
  assert.deepEqual(q.ids, ["aircraft:AAAAAA", "vessel:123456789"]);
  assert.deepEqual(parseTrailRequest(new URLSearchParams("id=sonde:S1&id=aprs:N0CALL-9")).ids, ["sonde:S1", "aprs:N0CALL-9"]);
  assert.equal(parseTrailRequest(new URLSearchParams("")), null);
  assert.equal(parseTrailRequest(new URLSearchParams("id=sat:ISS")), null);
  const many = Array.from({ length: 200 }, (_, i) => "aircraft:" + i).join(",");
  assert.equal(parseTrailRequest(new URLSearchParams({ ids: many })).ids.length, MAX_IDS);
});

test("merge: dedupes by timestamp, orders, server copy wins", () => {
  const server = [[1000, 1, 1, 100, 90], [2000, 1, 2, 100, 90], [3000, 1, 3, 100, 90]];
  const live = [[3000, 1, 3, null, null], [4000, 1, 4, null, null], [500, 1, 0, null, null]];
  const m = mergeTrails(server, live);
  assert.deepEqual(m.map((p) => p[0]), [500, 1000, 2000, 3000, 4000]);
  assert.equal(m[3][3], 100);
});

test("merge: same spot within 1.5 s counts once; bad points ignored; capped", () => {
  const m = mergeTrails([[1000, 1, 1, null, null]], [[1800, 1, 1, null, null], [2000, 2, 2, null, null], ["x"], null]);
  assert.equal(m.length, 2);
  const big = Array.from({ length: 1500 }, (_, i) => [i * 10000, 1, i, null, null]);
  assert.equal(mergeTrails(big, []).length, 1000);
  assert.deepEqual(mergeTrails(null, undefined), []);
});

test("appendLive skips repeats and old stamps; toLine gives lon,lat", () => {
  let pts = appendLive([], 1000, 1, 2, 300, 10);
  pts = appendLive(pts, 2000, 1, 2);
  pts = appendLive(pts, 500, 5, 5);
  pts = appendLive(pts, 3000, 3, 4);
  assert.equal(pts.length, 2);
  assert.deepEqual(toLine(pts), [[2, 1], [4, 3]]);
});
