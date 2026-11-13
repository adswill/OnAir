// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// df.test.js - the DF map features: wedge/line/station per bearing, ellipse + marker per fix, expiry and the kind switch.
import test from "node:test";
import assert from "node:assert/strict";
import { dfFeatures, ellipseRing, destination, DF_TTL_MS } from "../public/df.js";

const now = 1_700_000_000_000;
const doa = { id: "doa:a-433920", kind: "doa", lat: 54.7, lon: 25.3, heading: 90, label: "433.920 MHz", extra: JSON.stringify({ f: 433920000, s: 3 }), t: now };
const fix = { id: "fix:433920", kind: "fix", lat: 54.7, lon: 26, label: "433.920 MHz", extra: JSON.stringify({ a: 2000, b: 500, o: 90 }), t: now };

test("a bearing becomes a line, a wedge and a station dot", () => {
  const f = dfFeatures(new Map([[doa.id, doa]]), { doa: true, fix: true }, now);
  assert.deepEqual(f.map((x) => x.properties.kind).sort(), ["dfline", "dfstation", "dfwedge"]);
  const line = f.find((x) => x.properties.kind === "dfline").geometry.coordinates;
  assert.ok(line[1][0] > 26 && Math.abs(line[1][1] - 54.7) < 0.2, "the line runs east");
});

test("a fix becomes an ellipse and a marker; the ellipse axes are right", () => {
  const f = dfFeatures(new Map([[fix.id, fix]]), { doa: true, fix: true }, now);
  assert.deepEqual(f.map((x) => x.properties.kind).sort(), ["dfell", "dffix"]);
  const ring = ellipseRing(54.7, 26, 2000, 500, 90);
  const lons = ring.map((p) => p[0]), lats = ring.map((p) => p[1]);
  const w = (Math.max(...lons) - Math.min(...lons)) * 111195 * Math.cos(54.7 * Math.PI / 180), h = (Math.max(...lats) - Math.min(...lats)) * 111195;
  assert.ok(Math.abs(w - 4000) < 40 && Math.abs(h - 1000) < 40, `${w} x ${h}`);
});

test("expired entries and switched-off kinds are not drawn", () => {
  const m = new Map([[doa.id, doa], [fix.id, fix]]);
  assert.equal(dfFeatures(m, { doa: false, fix: false }, now).length, 0);
  assert.equal(dfFeatures(m, { doa: true, fix: true }, now + DF_TTL_MS + 1).length, 0);
  assert.equal(dfFeatures(m, { doa: false, fix: true }, now).length, 2);
});

test("destination goes the right way", () => {
  const [lon, lat] = destination(0, 0, 90, 111195);
  assert.ok(Math.abs(lon - 1) < 0.001 && Math.abs(lat) < 1e-9);
});
