// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// doa.test.js - bearing lines: ingest validation, the server-side fix where lines from different stations cross, outliers, freshness.
import test from "node:test";
import assert from "node:assert/strict";
import { sanitize } from "../src/index.js";
import { azimuth, distanceM, destination, computeFixes, fixLines, cleanDoa, DOA_TTL_MS, angDiff } from "../src/doa.js";

const NOW = 1_700_000_000_000;
const TX = { lat: 54.70, lon: 25.30 };

// deterministic gaussian
let seed = 7;
const rnd = () => { seed = (seed * 1664525 + 1013904223) % 4294967296; return (seed + 0.5) / 4294967296; };
const gauss = () => Math.sqrt(-2 * Math.log(rnd())) * Math.cos(2 * Math.PI * rnd());

function track(station, from, freq, sigma = 3, noise = 0, bias = 0, t = NOW) {
  const b = azimuth(from.lat, from.lon, TX.lat, TX.lon) + noise * gauss() + bias;
  return { id: `doa:${station}-${Math.round(freq / 1000)}`, lat: from.lat, lon: from.lon, heading: ((b % 360) + 360) % 360, t,
    extra: JSON.stringify({ f: freq, s: sigma, c: 0.8 }) };
}
const at = (brg, km) => { const [lon, lat] = destination(TX.lat, TX.lon, brg, km * 1000); return { lat, lon }; };

test("a bearing line is accepted, normalised and gets no trail", () => {
  const t = sanitize({ kind: "doa", lat: 54.7, lon: 25.3, heading: 370, freq: 433920000, sigma: 3, conf: 0.9, path: [[1, 2]], id: "spoof" }, NOW, "abcdef123456789");
  assert.ok(t);
  assert.equal(t.kind, "doa");
  assert.equal(t.heading, 10);
  assert.equal(t.id, "doa:abcdef123456-433920");
  assert.deepEqual(t.path, []);
  assert.deepEqual(JSON.parse(t.extra), { f: 433920000, s: 3, c: 0.9 });
});

test("bad bearing lines are refused", () => {
  assert.equal(sanitize({ kind: "doa", lat: 54.7, lon: 25.3, heading: 10, freq: 433920000 }, NOW, ""), null);   // no station
  assert.equal(sanitize({ kind: "doa", lat: 54.7, lon: 25.3, heading: 10 }, NOW, "abcdefgh"), null);              // no frequency
  assert.equal(sanitize({ kind: "doa", lat: 54.7, lon: 25.3, freq: 433920000 }, NOW, "abcdefgh"), null);          // no bearing
  assert.equal(sanitize({ kind: "doa", lat: 91, lon: 25.3, heading: 1, freq: 433920000 }, NOW, "abcdefgh"), null);
  assert.equal(sanitize({ kind: "doa", lat: 54.7, lon: 25.3, heading: 1, freq: 433920000, age: 3600 }, NOW, "abcdefgh"), null);
  assert.equal(cleanDoa({ heading: 1, freq: 5 }), null);
  const c = cleanDoa({ heading: 1, freq: 433e6, sigma: 0, conf: 7 });
  assert.equal(c.sigma, 0.5);
  assert.equal(c.conf, 1);
});

test("two stations make a fix at the transmitter", () => {
  const rows = [track("stationA", at(250, 20), 433920000), track("stationB", at(130, 25), 433920000)];
  const f = computeFixes(rows, NOW);
  assert.equal(f.length, 1);
  assert.equal(f[0].kind, "fix");
  assert.ok(distanceM(f[0].lat, f[0].lon, TX.lat, TX.lon) < 5);
  const x = JSON.parse(f[0].extra);
  assert.equal(x.n, 2);
  assert.ok(x.a >= x.b && x.a > 0);
});

test("lines on other frequencies, from the same station, or parallel give no fix", () => {
  assert.equal(computeFixes([track("stationA", at(250, 20), 433920000), track("stationB", at(130, 25), 144800000)], NOW).length, 0);
  assert.equal(computeFixes([track("stationA", at(250, 20), 433920000), track("stationA", at(130, 25), 433920000)], NOW).length, 0);
  const a = { lat: 54.7, lon: 25.0, heading: 90, t: NOW, id: "doa:stationA-100000", extra: JSON.stringify({ f: 100e6, s: 3 }) };
  const b = { lat: 54.8, lon: 25.0, heading: 90, t: NOW, id: "doa:stationB-100000", extra: JSON.stringify({ f: 100e6, s: 3 }) };
  assert.equal(computeFixes([a, b], NOW).length, 0);
});

test("stale lines are ignored and the freshest line of a station is used", () => {
  const old = track("stationA", at(250, 20), 433920000, 3, 0, 40, NOW - DOA_TTL_MS - 1000);
  assert.equal(computeFixes([old, track("stationB", at(130, 25), 433920000)], NOW).length, 0);
  const rows = [track("stationA", at(250, 20), 433920000, 3, 0, 40, NOW - 120000), track("stationA", at(250, 20), 433920000, 3, 0, 0, NOW), track("stationB", at(130, 25), 433920000)];
  rows[0].id = "doa:stationA-433920"; rows[1].id = "doa:stationA-433920";   // same key in the store: only one survives there; both here, newest wins
  const f = computeFixes(rows, NOW);
  assert.equal(f.length, 1);
  assert.ok(distanceM(f[0].lat, f[0].lon, TX.lat, TX.lon) < 5);
});

test("nearby frequencies are one transmitter, far ones are two", () => {
  const rows = [track("stationA", at(250, 20), 433920000), track("stationB", at(130, 25), 433921500), track("stationC", at(10, 18), 433920500)];
  const f = computeFixes(rows, NOW);
  assert.equal(f.length, 1);
  assert.equal(JSON.parse(f[0].extra).n, 3);
});

test("an outlier among four bearings is rejected", () => {
  const rows = [250, 130, 10, 330].map((b, i) => track("station" + i, at(b, 22), 433920000, 3, 0.5, i === 2 ? 55 : 0));
  const f = computeFixes(rows, NOW);
  assert.equal(f.length, 1);
  assert.equal(JSON.parse(f[0].extra).n, 3);
  assert.ok(distanceM(f[0].lat, f[0].lon, TX.lat, TX.lon) < 1500);
});

test("ellipse coverage and median error with noisy bearings", () => {
  const errs = []; let inside = 0, total = 0;
  for (let k = 0; k < 300; k++) {
    const lines = [0, 1, 2].map((i) => {
      const from = at(((120 * i + 360 * rnd() / 3) % 360), 18 + 15 * rnd());
      const b = azimuth(from.lat, from.lon, TX.lat, TX.lon) + 3 * gauss();
      return { lat: from.lat, lon: from.lon, bearing: (b + 360) % 360, sigma: 3 };
    });
    const f = fixLines(lines);
    if (!f) continue;
    total++;
    const dN = (TX.lat - f.lat) * 111195, dE = angDiff(TX.lon, f.lon) * 111195 * Math.cos(f.lat * Math.PI / 180);
    errs.push(Math.hypot(dE, dN));
    const th = f.o * Math.PI / 180, u = dE * Math.sin(th) + dN * Math.cos(th), v = dE * Math.cos(th) - dN * Math.sin(th);
    if ((u / f.a) ** 2 + (v / f.b) ** 2 <= 1) inside++;
  }
  errs.sort((x, y) => x - y);
  const cov = inside / total;
  console.log(`3 stations, sigma 3 deg: median error ${Math.round(errs[errs.length >> 1])} m, 95% ellipse holds ${(cov * 100).toFixed(0)}% of ${total} trials`);
  assert.ok(total > 280);
  assert.ok(cov > 0.85 && cov < 0.995, "coverage " + cov);
  assert.ok(errs[errs.length >> 1] < 3000);
});

test("a fix across the antimeridian", () => {
  const tx = { lat: 10.4, lon: -179.95 };
  const mk = (st, la, lo) => ({ id: `doa:${st}-100000`, lat: la, lon: lo, heading: azimuth(la, lo, tx.lat, tx.lon), t: NOW, extra: JSON.stringify({ f: 100e6, s: 3 }) });
  const f = computeFixes([mk("stationA", 10, 179.8), mk("stationB", 10.2, -179.8)], NOW);
  assert.equal(f.length, 1);
  assert.ok(distanceM(f[0].lat, f[0].lon, tx.lat, tx.lon) < 50);
});
