// Stats, unique counting, streaks, badge thresholds and the anti-cheat rules of src/badge-stats.js, with tracks made by the hub's own
// sanitize() (so the rows look exactly like the ones the hub credits).
import test from "node:test";
import assert from "node:assert/strict";
import { sanitize } from "../src/track-book.js";
import {
  BadgeBook, MemoryBadgeBackend, objectKey, rareKinds, weekKey, monthKey, solarHour, statValue, periodValue, currentStreak, evaluateBadges,
  emptyStats, showcase, publicProfile, HOURLY_CAP, MAX_STATIONS_PER_DAY,
} from "../src/badge-stats.js";
import { BADGES, tierFor, progressFor, BADGE_BY_ID } from "../public/badges/defs.js";
import { distKm, fromLocator, fieldOf, icaoCountry, mmsiCountry } from "../src/badge-geo.js";

const T0 = Date.UTC(2026, 10, 10, 12, 0, 0);   // Tue 10 Nov 2026 12:00 UTC
const ST = "station-abcdef12";
const HOME = { lat: 54.69, lon: 25.28 };       // Vilnius

function ac(hex, lat, lon, extra = {}) { return sanitize({ kind: "aircraft", id: hex, icao: hex, lat, lon, alt: 30000, speed: 450, src: "adsb", age: 0, ...extra }, T0, ST); }
function ship(mmsi, lat, lon, extra = {}) { return sanitize({ kind: "vessel", id: mmsi, mmsi, lat, lon, speed: 12, src: "ais", age: 0, ...extra }, T0, ST); }
function book() { const b = new BadgeBook(new MemoryBadgeBackend()); b.setSettings(1, "alice", { badges: true }); return b; }
function post(b, rows, now, more = {}) { return b.credit({ uid: 1, login: "alice", station: ST, rows, prev: new Map(), home: HOME, now, ...more }); }

test("geo helpers: distance, locators, fields, countries", () => {
  assert.ok(Math.abs(distKm(0, 0, 0, 1) - 111.19) < 0.1);
  const c = fromLocator("KO24pq");
  assert.ok(c && Math.abs(c.lat - 54.69) < 0.05 && Math.abs(c.lon - 25.29) < 0.1);
  assert.equal(fromLocator("ZZ99"), null);
  assert.equal(fromLocator("javascript:"), null);
  assert.equal(fieldOf(54.7, 25.3), "KO");
  assert.equal(icaoCountry("4B1234"), "CH");
  assert.equal(icaoCountry("A12345"), "US");
  assert.equal(icaoCountry("3C6444"), "DE");
  assert.equal(icaoCountry("F00000"), "");
  assert.equal(mmsiCountry("277123456"), "LT");
  assert.equal(mmsiCountry("970123456"), "");
});

test("object keys: real-looking ids only", () => {
  assert.deepEqual(objectKey(ac("3C6444", 50, 8)), { kind: "aircraft", key: "a:3C6444" });
  assert.equal(objectKey(ac("000000", 50, 8)), null);
  assert.equal(objectKey(ac("FFFFFF", 50, 8)), null);
  assert.equal(objectKey(ac("3C6444", 50, 8, { alt: 99000 })), null, "impossible altitude");
  assert.equal(objectKey(ac("3C6444", 50, 8, { speed: 3000 })), null, "impossible speed");
  assert.deepEqual(objectKey(ship("277123456", 55, 21)), { kind: "vessel", key: "v:277123456" });
  assert.equal(objectKey(ship("970123456", 55, 21)), null, "a SART is a rare catch, not a ship");
  assert.equal(objectKey(sanitize({ kind: "aprs", id: "LY2XX-9", lat: 54, lon: 25 }, T0, ST)).key, "p:LY2XX-9");
  assert.equal(objectKey(sanitize({ kind: "aprs", id: "NOCALL", lat: 54, lon: 25 }, T0, ST)), null, "a callsign has a digit");
  assert.equal(objectKey(sanitize({ kind: "station", id: "ST1", lat: 54, lon: 25 }, T0, ST)), null);
  assert.deepEqual(rareKinds(ac("3C6444", 50, 8, { squawk: "7700" })), ["sq7700"]);
  assert.deepEqual(rareKinds(ac("3C6444", 50, 8, { cat: "B2" })), ["balloon"]);
  assert.deepEqual(rareKinds(ship("972000001", 55, 21)), ["mob"]);
});

test("badge thresholds: tiers, progress and the data file", () => {
  const ids = new Set();
  for (const b of BADGES) {
    assert.ok(!ids.has(b.id), "unique id " + b.id); ids.add(b.id);
    assert.ok(b.name && b.desc && b.icon && b.stat);
    assert.ok(b.tiers.length >= 1 && b.tiers.length <= 4);
    for (let i = 1; i < b.tiers.length; i++) assert.ok(b.tiers[i] > b.tiers[i - 1], b.id + " ascending");
  }
  const p = BADGE_BY_ID.plane_spotter;
  assert.equal(tierFor(p, 99), 0);
  assert.equal(tierFor(p, 100), 1);
  assert.equal(tierFor(p, 9999), 2);
  assert.equal(tierFor(p, 100000), 4);
  assert.deepEqual(progressFor(p, 550), { tier: 1, next: 1000, frac: 0.5 });
  assert.equal(progressFor(p, 1e6).next, null);
  assert.equal(BADGE_BY_ID.multi_mode.tiers[0], 5, "multi-mode starts at five kinds");
  const s = emptyStats();
  s.c.aircraft = 1000; s.shares = 1;
  const got = evaluateBadges(s, T0);
  assert.deepEqual(got.filter((g) => g.id === "plane_spotter").map((g) => g.tier), [1, 2], "two tiers at once, each with a date");
  assert.ok(got.some((g) => g.id === "first_light"));
  assert.equal(evaluateBadges(s, T0 + 1).length, 0, "nothing twice");
  assert.equal(showcase(s), "plane_spotter:2");
});

test("unique counting: an object counts once, across posts and stations", () => {
  const b = book();
  let r = post(b, [ac("3C6444", 54.9, 25.5), ac("4B1234", 54.5, 25.0)], T0);
  assert.ok(r.credited);
  assert.ok(r.awards.some((a) => a.id === "first_light"));
  r = post(b, [ac("3C6444", 54.91, 25.51), ac("4B1234", 54.51, 25.01)], T0 + 4000);
  r = b.credit({ uid: 1, login: "alice", station: "station-second-1", rows: [ac("3C6444", 54.92, 25.52)], prev: new Map(), home: HOME, now: T0 + 8000 });
  const s = b.users.get(1).s;
  assert.equal(s.c.aircraft, 2);
  assert.equal(s.objects, 2);
  assert.deepEqual([...s.countries].sort(), ["CH", "DE"]);
  assert.deepEqual(s.fields, ["KO"]);
});

test("unique counting survives a restart (stored keys are read back)", () => {
  const db = new MemoryBadgeBackend();
  const b1 = new BadgeBook(db);
  b1.setSettings(1, "alice", { badges: true });
  b1.credit({ uid: 1, login: "alice", station: ST, rows: [ac("3C6444", 54.9, 25.5)], prev: new Map(), home: HOME, now: T0 });
  b1.flush(T0 + 1);
  const b2 = new BadgeBook(db);
  b2.credit({ uid: 1, login: "alice", station: ST, rows: [ac("3C6444", 54.95, 25.55), ac("4CA111", 54.8, 25.4)], prev: new Map(), home: HOME, now: T0 + 5000 });
  assert.equal(b2.users.get(1).s.c.aircraft, 2, "3C6444 was not counted again");
});

test("no credit for duplicated or replayed posts, or for a faster post rate", () => {
  const b = book();
  const rows = [ac("3C6444", 54.9, 25.5)];
  assert.ok(post(b, rows, T0).credited);
  assert.equal(post(b, rows, T0 + 500).reason, "rate", "2 s between credited posts");
  assert.equal(post(b, rows, T0 + 5000).reason, "duplicate", "the same post again");
  const s = b.users.get(1).s;
  assert.equal(s.shares, 1);
  assert.equal(s.secs, 60);
  // a stale post (nothing moved) after a different one earns no time either
  assert.ok(post(b, [ac("3C6444", 54.91, 25.5), ac("4CA111", 54.0, 24.0)], T0 + 10000).credited);
  assert.equal(post(b, [ac("4CA111", 54.0, 24.0), ac("3C6444", 54.91, 25.5), ac("4CA111", 54.0, 24.0)].slice(0, 2), T0 + 14000).reason, "duplicate");
});

test("hours: the gap between credited posts, at most 120 s, per user", () => {
  const b = book();
  let lat = 54.0;
  for (let i = 0; i < 900; i++) post(b, [ac("3C6444", (lat += 0.001), 25)], T0 + i * 4000);   // one hour at 4 s
  const s = b.users.get(1).s;
  assert.ok(Math.abs(s.secs - (60 + 899 * 4)) < 1, "about an hour: " + s.secs);
  post(b, [ac("3C6444", (lat += 0.001), 25)], T0 + 900 * 4000 + 200_000);   // a 200 s gap counts 120 s
  assert.ok(Math.abs(s.secs - (60 + 899 * 4 + 120)) < 1);
  post(b, [ac("3C6444", (lat += 0.001), 25)], T0 + 900 * 4000 + 200_000 + 3600_000);   // a new session: 60 s
  assert.ok(Math.abs(s.secs - (60 + 899 * 4 + 120 + 60)) < 1);
});

test("days and streaks (UTC days)", () => {
  const b = book();
  const D = 86400000;
  let lat = 54;
  for (const d of [0, 1, 2, 4, 5]) post(b, [ac("3C6444", (lat += 0.01), 25)], T0 + d * D);
  const s = b.users.get(1).s;
  assert.equal(s.days, 5);
  assert.equal(s.bestStreak, 3);
  assert.equal(s.streak, 2);
  assert.equal(currentStreak(s, T0 + 5 * D), 2);
  assert.equal(currentStreak(s, T0 + 6 * D), 2, "yesterday still counts");
  assert.equal(currentStreak(s, T0 + 7 * D), 0);
  post(b, [ac("3C6444", (lat += 0.01), 25)], T0 + 5 * D + 3600_000);   // same day again: no change
  assert.equal(s.days, 5);
});

test("range needs two agreeing reports, a direct source, a plausible distance and a settled home", () => {
  const b = book();
  // 300 km east of Vilnius
  post(b, [ac("3C6444", 54.69, 29.94)], T0);
  let s = b.users.get(1).s;
  assert.equal(s.rangeM, 0, "one report is not enough");
  post(b, [ac("3C6444", 54.69, 29.99)], T0 + 30_000);   // 3.2 km in 30 s = 107 m/s
  assert.ok(s.rangeM > 295_000 && s.rangeM < 305_000, "range " + s.rangeM);
  assert.equal(s.rangeKind, "aircraft");
  // a teleport is not confirmed
  post(b, [ac("4CA111", 54.69, 31.0)], T0 + 40_000);
  post(b, [ac("4CA111", 54.69, 33.0)], T0 + 70_000);   // 128 km in 30 s
  assert.ok(s.rangeM < 305_000);
  // beyond what a receiver can hear
  post(b, [ac("4CA222", 54.69, 37.0)], T0 + 80_000);
  post(b, [ac("4CA222", 54.69, 37.01)], T0 + 110_000);
  assert.ok(s.rangeM < 305_000, "750 km is not credited");
  // an estimated position or an indirect source earns nothing
  post(b, [ac("4CA333", 54.69, 30.5, { src: "hfdl" })], T0 + 120_000);
  post(b, [ac("4CA333", 54.69, 30.51, { src: "hfdl" })], T0 + 150_000);
  assert.ok(s.rangeM < 305_000);
  // moving "home" 500 km pauses range for an hour
  const far = { lat: 54.69, lon: 33.0 };
  post(b, [ac("4CA444", 54.69, 39.5)], T0 + 160_000, { home: far });
  post(b, [ac("4CA444", 54.69, 39.51)], T0 + 190_000, { home: far });
  assert.ok(s.rangeM < 305_000, "paused after the home square moved");
});

test("rare catches need two reports, and count kinds once", () => {
  const b = book();
  post(b, [ac("3C6444", 54.9, 25.5, { squawk: "7700" })], T0);
  const s = b.users.get(1).s;
  assert.equal(Object.keys(s.rare).length, 0);
  post(b, [ac("3C6444", 54.91, 25.51, { squawk: "7700" })], T0 + 25_000);
  assert.deepEqual(Object.keys(s.rare), ["sq7700"]);
  post(b, [ac("3C6444", 54.92, 25.52, { squawk: "7700" })], T0 + 60_000);
  assert.equal(statValue(s, "rare"), 1);
  assert.equal(tierFor(BADGE_BY_ID.rare_catch, statValue(s, "rare")), 1);
});

test("conflicts with other stations: the object earns nothing; a mostly conflicting post earns nothing at all", () => {
  const b = book();
  const prev = new Map([["aircraft:3C6444", { lat: 50.0, lon: 8.0, t: T0 - 5000, est: 0 }]]);
  let r = post(b, [ac("3C6444", 54.9, 25.5), ac("4B1234", 54.5, 25.0)], T0, { prev });
  assert.ok(r.credited);
  assert.equal(b.users.get(1).s.c.aircraft, 1, "only the agreeing one");
  const prev3 = new Map(["4CA001", "4CA002", "4CA003"].map((h) => ["aircraft:" + h, { lat: 40, lon: 0, t: T0 + 4000, est: 0 }]));
  r = post(b, [ac("4CA001", 54.9, 25.5), ac("4CA002", 54.8, 25.4), ac("4CA003", 54.7, 25.3)], T0 + 10_000, { prev: prev3 });
  assert.equal(r.reason, "conflict");
  // our own last report as the hub's row is not a conflict
  r = post(b, [ac("4B1234", 54.6, 25.1)], T0 + 20_000, { prev: new Map([["aircraft:4B1234", { lat: 54.5, lon: 25.0, t: T0 + 15_000, est: 0 }]]) });
  assert.ok(r.credited);
});

test("rate limits: hourly caps per kind and stations per day", () => {
  const b = book();
  const cap = HOURLY_CAP.sonde;
  for (let i = 0; i < cap + 20; i += 10) {
    const rows = [];
    for (let j = 0; j < 10; j++) rows.push(sanitize({ kind: "sonde", id: "S" + String(1000 + i + j), lat: 50 + j * 0.01, lon: 10, alt: 9000, src: "sonde" }, T0, ST));
    post(b, rows, T0 + i * 1000);
  }
  assert.equal(b.users.get(1).s.c.sonde, cap);
  for (let k = 0; k < MAX_STATIONS_PER_DAY + 2; k++) {
    const r = b.credit({ uid: 1, login: "alice", station: "station-many-" + k, rows: [ac("4CA" + String(100 + k), 54, 25 + k * 0.1)], prev: new Map(), home: HOME, now: T0 + 200_000 + k * 3000 });
    if (k >= MAX_STATIONS_PER_DAY - 1) assert.equal(r.reason, "stations");   // the first station already used one slot
  }
});

test("opt-in: nothing is kept while badges are off; hidden users earn nothing", () => {
  const b = new BadgeBook(new MemoryBadgeBackend());
  const r = b.credit({ uid: 2, login: "bob", station: ST, rows: [ac("3C6444", 54.9, 25.5)], prev: new Map(), home: HOME, now: T0 });
  assert.equal(r.reason, "badges_off");
  assert.equal(b.users.get(2).s.objects, 0);
  b.setSettings(2, "bob", { badges: true });
  b.users.get(2).hidden = true;
  assert.equal(b.credit({ uid: 2, login: "bob", station: ST, rows: [ac("3C6444", 54.9, 25.5)], prev: new Map(), home: HOME, now: T0 + 5000 }).reason, "hidden");
});

test("ACARS messages, DF fixes and kinds for the multi-mode badge", () => {
  const b = book();
  const rows = [
    ac("3C6444", 54.9, 25.5), ship("277123456", 55.7, 21.1),
    sanitize({ kind: "sonde", id: "T1234567", lat: 54.6, lon: 25.1, alt: 12000, src: "sonde" }, T0, ST),
    sanitize({ kind: "aprs", id: "LY1AB-9", lat: 54.7, lon: 25.2, src: "aprs" }, T0, ST),
    sanitize({ kind: "sat", id: "NOAA 19", lat: 40, lon: 20, src: "sat" }, T0, ST),
  ];
  const extraKeys = [{ kind: "acars", key: "m:3C6444:123" }, { kind: "acars", key: "m:3C6444:123" }, { kind: "fix", key: "x:145500:1" }];
  const r = post(b, rows, T0, { extraKeys });
  const s = b.users.get(1).s;
  assert.equal(s.c.acars, 1, "the same message once");
  assert.equal(s.c.fix, 1);
  assert.equal(s.kinds.length, 7);
  assert.ok(r.awards.some((a) => a.id === "multi_mode" && a.tier === 1));
  assert.ok(r.awards.some((a) => a.id === "sonde_hunter" && a.tier === 1));
  assert.ok(r.awards.some((a) => a.id === "fox_hunter" && a.tier === 1));
});

test("periods: ISO weeks, months and the boards' period values", () => {
  assert.equal(weekKey(Date.UTC(2026, 10, 10)), "2026-W46");
  assert.equal(weekKey(Date.UTC(2027, 0, 1)), "2026-W53");
  assert.equal(weekKey(Date.UTC(2021, 0, 3)), "2020-W53");
  assert.equal(weekKey(Date.UTC(2024, 11, 30)), "2025-W01");
  assert.equal(monthKey(Date.UTC(2026, 10, 30, 23)), "2026-11");
  const b = book();
  post(b, [ac("3C6444", 54.9, 25.5)], T0);
  const s = b.users.get(1).s;
  assert.equal(periodValue(s, "objects", "week", T0), 1);
  assert.equal(periodValue(s, "objects", "week", T0 + 7 * 86400000), 0, "last week's count is not this week's");
  assert.equal(periodValue(s, "objects", "all", T0 + 7 * 86400000), 1);
  post(b, [ac("4CA111", 54.9, 25.5)], T0 + 7 * 86400000);
  assert.equal(s.wk.objects, 1, "rolled over");
  assert.equal(s.objects, 2);
});

test("night owl and early bird use local solar time", () => {
  assert.ok(Math.abs(solarHour(Date.UTC(2026, 0, 1, 0, 0), 90) - 6) < 1e-9);
  assert.ok(Math.abs(solarHour(Date.UTC(2026, 0, 1, 0, 0), -90) - 18) < 1e-9);
  const b = book();
  const night = Date.UTC(2026, 10, 10, 0, 30) - (25.28 / 15) * 3600_000;   // 00:30 local solar time in Vilnius
  post(b, [ac("3C6444", 54.9, 25.5)], night);
  const s = b.users.get(1).s;
  assert.equal(s.night, 60);
  assert.equal(s.early, 0);
});

test("public profile view", () => {
  const b = book();
  post(b, [ac("3C6444", 54.9, 25.5)], T0);
  const p = publicProfile(b.users.get(1), T0);
  assert.equal(p.login, "alice");
  assert.equal(p.stats.objects, 1);
  const fl = p.badges.find((x) => x.id === "first_light");
  assert.equal(fl.tier, 1);
  assert.equal(fl.earned[0].at, T0);
  assert.equal(p.badges.length, BADGES.length);
  assert.ok(!("home" in p.stats) && !JSON.stringify(p).includes("54.69"), "the home square is never part of the profile");
});
