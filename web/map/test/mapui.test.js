// mapui.test.js - ham contacts, clustering, search/filter and consent rules (pure modules).
import test from "node:test";
import assert from "node:assert/strict";
import { bandOf, normMode, hamHints, buildContacts, filterContacts, contactFeatures, modeColour } from "../public/ham.js";
import { clusterPoints, shouldCluster, cellDeg } from "../public/cluster.js";
import { searchTracks, passesFilter } from "../public/search.js";
import { KEY, readChoice, saveChoice, allowed, needsPrompt } from "../public/consent.js";

test("band and mode", () => {
  assert.equal(bandOf(14074000), "20m");
  assert.equal(bandOf(1e3), "");
  assert.equal(normMode("ft8"), "FT8");
  assert.equal(normMode("SSB"), "QSO");
  assert.equal(normMode("weird"), "OTHER");
  assert.notEqual(modeColour("FT8"), modeColour("WSPR"));
});

test("hints tolerate junk, contacts resolve peers by callsign", () => {
  assert.equal(hamHints({ extra: "not json" }), null);
  assert.equal(hamHints({ extra: null }), null);
  assert.equal(hamHints({ extra: { m: "FT8" } }), null);
  const now = 10_000_000;
  const tr = [
    { id: "ham:A", kind: "ham", label: "LY1AA", lon: 24, lat: 55, t: now - 60000, extra: JSON.stringify({ m: "FT8", f: 14074000, to: "DL1BB" }) },
    { id: "ham:B", kind: "ham", label: "DL1BB", lon: 8, lat: 50, t: now, extra: null },
    { id: "ham:C", kind: "ham", label: "W1AW", lon: -72, lat: 41, t: now - 3600e3, extra: { m: "WSPR", f: 7040000, tlat: 55, tlon: 24 } },
  ];
  const c = buildContacts(tr, now);
  assert.equal(c.length, 2);
  assert.deepEqual(c[0].b, [8, 50]);
  assert.equal(c[0].band, "20m");
  assert.equal(filterContacts(c, { modes: new Set(["WSPR"]) }).length, 1);
  assert.equal(filterContacts(c, { maxAgeMin: 5 }).length, 1);
  assert.equal(filterContacts(c, { bands: new Set(["40m"]) })[0].from, "W1AW");
  assert.equal(contactFeatures(c).length, 2);
});

test("antimeridian line goes the short way", () => {
  const f = contactFeatures([{ id: "x", mode: "FT8", band: "20m", a: [170, 0], b: [-170, 0] }]);
  assert.deepEqual(f[0].geometry.coordinates, [[170, 0], [190, 0]]);
});

test("clustering groups dense cells, keeps the selected one single", () => {
  const pts = [];
  for (let i = 0; i < 10; i++) pts.push({ id: "a" + i, kind: "aircraft", lon: 10 + i * 0.01, lat: 50 });
  pts.push({ id: "far", kind: "vessel", lon: -100, lat: 30 });
  const r = clusterPoints(pts, 3);
  assert.equal(r.clusters.length, 1);
  assert.equal(r.clusters[0].n, 10);
  assert.equal(r.singles.length, 1);
  const k = clusterPoints(pts, 3, { keep: (id) => id === "a0" });
  assert.equal(k.clusters[0].n, 9);
  assert.ok(k.singles.some((p) => p.id === "a0"));
  assert.ok(cellDeg(4) < cellDeg(3));
  assert.equal(shouldCluster(1000, 3), true);
  assert.equal(shouldCluster(1000, 7), false);
  assert.equal(shouldCluster(10, 3), false);
});

test("search ranks exact over prefix, matches MMSI/hex ids and 1-char is empty", () => {
  const tr = [
    { id: "aircraft:4CA1FB", kind: "aircraft", label: "RYR45AB", sub: "ADS-B" },
    { id: "vessel:244123456", kind: "vessel", label: "NORDIC", sub: "AIS" },
    { id: "aircraft:400000", kind: "aircraft", label: "RYR4", sub: "ADS-B" },
  ];
  assert.equal(searchTracks(tr, "r").length, 0);
  assert.equal(searchTracks(tr, "ryr4")[0].label, "RYR4");
  assert.equal(searchTracks(tr, "244123456")[0].label, "NORDIC");
  assert.equal(searchTracks(tr, "4ca1")[0].label, "RYR45AB");
  assert.equal(passesFilter({ kind: "aircraft", t: 0 }, { aircraft: true }, null, 1e9), true);
  assert.equal(passesFilter({ kind: "aircraft", t: 0 }, { aircraft: true }, 60, 1e9), false);
  assert.equal(passesFilter({ kind: "vessel", t: 0 }, { aircraft: true }, null), false);
});

test("consent: nothing before a choice, always is remembered, decline is not", () => {
  const mem = new Map();
  const st = { getItem: (k) => (mem.has(k) ? mem.get(k) : null), setItem: (k, v) => mem.set(k, v), removeItem: (k) => mem.delete(k) };
  assert.equal(allowed(readChoice(st), null), false);
  assert.equal(needsPrompt(readChoice(st), null), true);
  saveChoice(st, "always");
  assert.equal(mem.get(KEY), "always");
  assert.equal(allowed(readChoice(st), null), true);
  assert.equal(needsPrompt(readChoice(st), null), false);
  assert.equal(allowed("always", "no"), false);   // "Decline" from the footer link overrides for this view
  saveChoice(st, "no");
  assert.equal(mem.has(KEY), false);
  assert.equal(allowed(null, "yes"), true);
  const broken = { getItem() { throw new Error("blocked"); }, setItem() { throw new Error("blocked"); }, removeItem() { throw new Error("blocked"); } };
  assert.equal(readChoice(broken), null);
  saveChoice(broken, "always");
});

test("cluster cells are stable within a zoom level", () => {
  assert.equal(cellDeg(3.2), cellDeg(3.9));
  assert.ok(cellDeg(4.0) < cellDeg(3.9));
  const pts = Array.from({ length: 40 }, (_, i) => ({ id: "p" + i, kind: "air", lon: 10 + (i % 8) * 0.3, lat: 50 + Math.floor(i / 8) * 0.3 }));
  const a = clusterPoints(pts, 3.1), b = clusterPoints(pts, 3.8);
  assert.deepEqual(a.clusters, b.clusters);
});

import { searchUsers, byMatches } from "../public/search.js";
test("user search: prefix, case-insensitive, counts icons; byMatches filters", () => {
  const ts = [{ id: "a", by: "Anna" }, { id: "b", by: "anna" }, { id: "c", by: "Annabel" }, { id: "d" }, { id: "e", by: "Ben" }];
  assert.deepEqual(searchUsers(ts, "ANNA").map((u) => [u.name.toLowerCase(), u.n]), [["anna", 2], ["annabel", 1]]);
  assert.deepEqual(searchUsers(ts, "a"), []);
  assert.equal(ts.filter((t) => byMatches(t, "anna")).length, 3);
  assert.equal(ts.filter((t) => byMatches(t, "anna", false)).length, 2);
  assert.equal(byMatches({ id: "x" }, "anna"), false);
});
