// iconsvg.test.js - the SVG sprite and the legend stay in step with shapes.js, and stay small.
import test from "node:test";
import assert from "node:assert/strict";
import { SHAPES } from "../public/shapes.js";
import { LEGEND, spriteSvg, iconSvg, symbolInner } from "../public/iconsvg.js";

test("the legend lists every shape exactly once, and only real shapes", () => {
  const seen = LEGEND.flatMap(([, rows]) => rows.map(([s]) => s));
  for (const s of seen) assert.ok(SHAPES[s], "unknown shape in the legend: " + s);
  assert.equal(new Set(seen).size, seen.length, "a shape is listed twice");
  const missing = Object.keys(SHAPES).filter((s) => !seen.includes(s));
  assert.deepEqual(missing, [], "shapes missing from the legend");
});

test("the sprite has one symbol per shape and is small", () => {
  const svg = spriteSvg();
  assert.equal((svg.match(/<symbol /g) || []).length, Object.keys(SHAPES).length);
  assert.ok(svg.length < 90_000, "sprite is " + svg.length + " bytes");
  assert.ok(!/<script|onload|javascript:/i.test(svg));
});

test("every shape has a body and well-formed path data (no NaN, no empty path)", () => {
  for (const [n, sh] of Object.entries(SHAPES)) {
    assert.ok(Array.isArray(sh.body) && sh.body.length, n + " has no body");
    for (const d of [...sh.body, ...(sh.detail || []), ...(sh.lines || []), ...(sh.disc || []), ...(sh.ring || []), sh.leader].filter(Boolean)) {
      assert.match(d, /^M/, n + ": " + d);
      assert.ok(!/NaN|undefined|Infinity/.test(d), n + ": " + d);
    }
    assert.ok(symbolInner(sh).includes("currentColor"));
  }
});

test("iconSvg: rotation only for heading-aware shapes, colour is sanitised, unknown shapes fall back", () => {
  assert.match(iconSvg("narrow", { rotate: 90 }), /rotate\(90deg\)/);
  assert.doesNotMatch(iconSvg("ham", { rotate: 90 }), /rotate/);
  assert.doesNotMatch(iconSvg("narrow", { color: 'red;"><script>' }), /<script/);
  assert.match(iconSvg("nope"), /#i-plane/);
  assert.match(iconSvg("sat", { state: "stale" }), /ico-stale/);
});
