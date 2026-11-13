// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// gnss.test.js - the "gnss" track kind: accepted by ingest, no trail, stale reports refused.
import test from "node:test";
import assert from "node:assert/strict";
import { sanitize } from "../src/index.js";

test("a GNSS satellite is accepted and never gets a trail", () => {
  const t = sanitize({ kind: "gnss", id: "G12", label: "G12", sub: "GPS", lat: 41.5, lon: -20.25, alt: 20180, src: "gnss",
    path: [[1, 2], [3, 4]], extra: "ephemeris 1.2 h old" }, 1000);
  assert.ok(t);
  assert.equal(t.id, "gnss:G12");
  assert.equal(t.sub, "GPS");
  assert.deepEqual(t.path, []);
});

test("a stale GNSS position is refused, bad coordinates too", () => {
  assert.equal(sanitize({ kind: "gnss", id: "E05", lat: 1, lon: 2, age: 500 }, 1), null);
  assert.equal(sanitize({ kind: "gnss", id: "E05", lat: 91, lon: 2 }, 1), null);
  assert.equal(sanitize({ kind: "nope", id: "E05", lat: 1, lon: 2 }, 1), null);
});
