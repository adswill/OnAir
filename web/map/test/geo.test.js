// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// geo.test.js - tests for the pure helpers in public/geo.js and public/airports.js (node --test, no deps).
import test from "node:test";
import assert from "node:assert/strict";
import {
  distanceNm, bearing, eta, progress, estimateDeparture, squawkMeaning, formatDuration, vrateArrow, flagEmoji, splitCallsign, isOnGround,
} from "../public/geo.js";
import { airportByCode, AIRPORT_COUNT } from "../public/airports.js";

test("great-circle distance LHR-JFK is about 2990 nm", () => {
  const d = distanceNm(51.47, -0.46, 40.64, -73.78);
  assert.ok(d > 2960 && d < 3020, String(d));
  assert.equal(distanceNm(10, 10, 10, 10), 0);
});

test("bearing due east on the equator is 90", () => {
  assert.ok(Math.abs(bearing(0, 0, 0, 10) - 90) < 1e-6);
});

test("ETA: remaining distance / speed, local clock injected", () => {
  const dest = { lat: 0, lon: 6 }; // 360 nm along the equator
  const e = eta({ lat: 0, lon: 0, altFt: 35000, speedKt: 360 }, dest, 1_000_000);
  assert.ok(Math.abs(e.minutes - 60) < 0.5);
  assert.ok(Math.abs(e.atMs - (1_000_000 + 3600e3)) < 40e3);
});

test("ETA hidden for unusable speed, ground, missing destination, absurd duration", () => {
  const dest = { lat: 0, lon: 6 };
  assert.equal(eta({ lat: 0, lon: 0, altFt: 35000, speedKt: 20 }, dest), null);
  assert.equal(eta({ lat: 0, lon: 0, altFt: 35000, speedKt: null }, dest), null);
  assert.equal(eta({ lat: 0, lon: 0, altFt: 0, speedKt: 30 }, dest), null);
  assert.equal(eta({ lat: 0, lon: 0, altFt: 35000, speedKt: 300 }, null), null);
  assert.equal(eta({ lat: 0, lon: 0, altFt: 35000, speedKt: 90 }, { lat: 0, lon: 170 }), null);
});

test("on-ground rule", () => {
  assert.equal(isOnGround(50, 10), true);
  assert.equal(isOnGround(50, 150), false);
  assert.equal(isOnGround(null, 0), false);
});

test("progress fraction halfway", () => {
  const p = progress({ lat: 0, lon: 0 }, { lat: 0, lon: 10 }, 0, 5);
  assert.ok(Math.abs(p.frac - 0.5) < 1e-3);
  assert.equal(progress(null, { lat: 0, lon: 1 }, 0, 0), null);
});

test("departure estimate: on the ground near origin, low climb, too high, too far", () => {
  const o = { lat: 50.03, lon: 8.57 };
  const base = { ms: 10_000_000, lat: 50.04, lon: 8.58 };
  assert.equal(estimateDeparture({ ...base, altFt: 50 }, o).ms, 10_000_000);
  const climb = estimateDeparture({ ...base, altFt: 1800 }, o);
  assert.equal(climb.ms, 10_000_000 - 60e3); // 1800 ft at 1800 ft/min = 1 min
  assert.equal(climb.estimated, true);
  assert.equal(estimateDeparture({ ...base, altFt: 3500 }, o), null);
  assert.equal(estimateDeparture({ ms: 1, altFt: 100, lat: 52, lon: 8.57 }, o), null);
  assert.equal(estimateDeparture(null, o), null);
});

test("squawk meanings", () => {
  assert.deepEqual(squawkMeaning("7700"), { code: "7700", text: "General emergency", emergency: true });
  assert.equal(squawkMeaning(7500).emergency, true);
  assert.equal(squawkMeaning("7600").text, "Radio failure");
  assert.equal(squawkMeaning("1200").emergency, false);
  assert.equal(squawkMeaning(12).code, "0012");
});

test("formatting helpers", () => {
  assert.equal(formatDuration(72), "1 h 12 min");
  assert.equal(formatDuration(35), "35 min");
  assert.equal(formatDuration(0.2), "<1 min");
  assert.equal(vrateArrow(1500), "↑");
  assert.equal(vrateArrow(-900), "↓");
  assert.equal(vrateArrow(20), "→");
  assert.equal(flagEmoji("gb"), "\u{1F1EC}\u{1F1E7}");
  assert.equal(flagEmoji("x"), "");
  assert.deepEqual(splitCallsign("baw117"), { airline: "BAW", number: "117" });
  assert.equal(splitCallsign("N172SP"), null);
});

test("airport table: lookup by either code, plausible size", () => {
  assert.equal(airportByCode("EGLL").iata, "LHR");
  assert.equal(airportByCode("jfk").icao, "KJFK");
  assert.equal(airportByCode("ZZZZ"), null);
  assert.ok(AIRPORT_COUNT >= 250);
});
