// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// derived.test.js - the additive "dv" flag: speed, heading and vertical rate derived from position reports by the app.
import test from "node:test";
import assert from "node:assert/strict";
import { sanitize } from "../src/index.js";

const NOW = 1_700_000_000_000;
const ST = "abcdef123456789";
const base = { kind: "aircraft", id: "4B1234", icao: "4B1234", lat: 47.1, lon: 8.5, age: 5, label: "SWR123", sub: "VDL2", src: "acars" };

test("an old client (no dv field) is stored as measured", () => {
  const t = sanitize({ ...base, speed: 452, heading: 90 }, NOW, ST);
  assert.ok(t);
  assert.equal(t.dv, 0);
  assert.equal(t.speed, 452);
});

test("dv:1 with a speed is kept; true is accepted too", () => {
  assert.equal(sanitize({ ...base, speed: 452.4, heading: 91, vrate: -640, dv: 1 }, NOW, ST).dv, 1);
  assert.equal(sanitize({ ...base, speed: 452, dv: true }, NOW, ST).dv, 1);
});

test("dv without any speed, heading or vertical rate is dropped", () => {
  assert.equal(sanitize({ ...base, dv: 1 }, NOW, ST).dv, 0);
});

test("junk dv values are not a flag", () => {
  for (const dv of ["1", 2, "yes", null, {}, [1]]) assert.equal(sanitize({ ...base, speed: 300, dv }, NOW, ST).dv, 0, JSON.stringify(dv));
});

test("derived figures keep their numbers, with the usual validation", () => {
  const t = sanitize({ ...base, speed: "abc", heading: 120, dv: 1 }, NOW, ST);
  assert.equal(t.speed, null);
  assert.equal(t.heading, 120);
  assert.equal(t.dv, 1);
});
