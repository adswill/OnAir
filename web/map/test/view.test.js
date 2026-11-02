// view.test.js - layer groups, age fade / filter, shortcuts, sparkline geometry (public/view.js).
import test from "node:test";
import assert from "node:assert/strict";
import { LAYER_GROUPS, AGE_OPTIONS, STALE_SEC, staleSecOf, ageFade, passesAge, SHORTCUTS, sparkline, seriesOfTrail } from "../public/view.js";

test("every layer is in exactly one group", () => {
  const ids = LAYER_GROUPS.flatMap(([, l]) => l);
  assert.equal(new Set(ids).size, ids.length);
  for (const k of ["aircraft", "vessel", "sonde", "aprs", "ham", "drone", "flarm", "sat", "buoy", "dsc", "station", "gnss", "doa", "fix"]) assert.ok(ids.includes(k), k);
  for (const k of ids) assert.ok(STALE_SEC[k] > 0, "no stale limit for " + k);
});

test("age fade: full until stale, then down to 0.35, never below", () => {
  assert.equal(ageFade(0, 60), 1);
  assert.equal(ageFade(60, 60), 1);
  assert.ok(ageFade(120, 60) < 1 && ageFade(120, 60) > 0.35);
  assert.equal(ageFade(360, 60), 0.35);
  assert.equal(ageFade(99999, 60), 0.35);
  assert.equal(ageFade(NaN, 60), 1);
  assert.equal(staleSecOf("aircraft", 30), 30);
  assert.equal(staleSecOf("aircraft", undefined), 60);
  assert.equal(staleSecOf("nope"), 300);
});

test("age filter", () => {
  assert.equal(passesAge(5000, 0), true);
  assert.equal(passesAge(5000, ""), true);
  assert.equal(passesAge(119, 2), true);
  assert.equal(passesAge(121, 2), false);
  assert.deepEqual(AGE_OPTIONS[0], ["", "Any age"]);
});

test("shortcut table has unique keys", () => {
  const keys = SHORTCUTS.map(([k]) => k);
  assert.equal(new Set(keys).size, keys.length);
});

test("sparkline: needs two points, scales into the box, flat series sits in the middle", () => {
  assert.equal(sparkline([[0, 1]]), null);
  assert.equal(sparkline([[0, 1], [0, 2]]), null);
  const s = sparkline([[0, 100], [10, 300], [20, 200]], 100, 50, 0);
  assert.equal(s.min, 100); assert.equal(s.max, 300); assert.equal(s.last, 200);
  assert.match(s.line, /^M0\.0,50\.0L50\.0,0\.0L100\.0,25\.0$/);
  assert.ok(s.area.endsWith("Z"));
  assert.match(sparkline([[0, 5], [1, 5]], 100, 50, 0).line, /,25\.0/);
  assert.equal(sparkline([[0, NaN], [1, 2], [2, 3]]).n, 2);
});

test("series of a trail: altitude when present, else distance; nothing for a point", () => {
  const t0 = 1e12;
  const alt = [[t0, 50, 8, 1000, 90], [t0 + 1e4, 50.1, 8, 2000, 90], [t0 + 2e4, 50.2, 8, 3000, 90]];
  assert.equal(seriesOfTrail(alt).name, "Altitude");
  const flat = alt.map((p) => [p[0], p[1], p[2], null, 90]);
  const s = seriesOfTrail(flat);
  assert.equal(s.name, "Distance from first fix");
  assert.ok(s.points[2][1] > 20 && s.points[2][1] < 25, String(s.points[2][1]));
  assert.equal(seriesOfTrail([[t0, 50, 8, null, 0]]), null);
  assert.equal(seriesOfTrail(null), null);
});
