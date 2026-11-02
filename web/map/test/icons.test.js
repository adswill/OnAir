// icons.test.js - pure parts of public/icons.js: image ids and the altitude colour scale.
import test from "node:test";
import assert from "node:assert/strict";
import { iconId, parseIconId, altitudeColor, altitudeGradientCss, ALT_STOPS, ALT_BAND_FT } from "../public/icons.js";

test("image ids round-trip, with a theme; older two-part ids mean the dark theme", () => {
  assert.equal(iconId("narrow", "#E6E8EA", "l"), "narrow|e6e8ea|l");
  assert.equal(iconId("narrow", "#e6e8ea"), "narrow|e6e8ea|d");
  assert.deepEqual(parseIconId("ship_cargo|8cb8eb|l"), ["ship_cargo", "#8cb8eb", "l"]);
  assert.deepEqual(parseIconId("narrow|e6e8ea"), ["narrow", "#e6e8ea", "d"]);
  assert.deepEqual(parseIconId("narrow"), ["narrow", "#c8cacc", "d"]);
});

test("altitude colour: stops are hit exactly, values between are blended, out of range is clamped, unknown is null", () => {
  assert.equal(altitudeColor(0), ALT_STOPS[0][1]);
  assert.equal(altitudeColor(45000), ALT_STOPS[6][1]);
  assert.equal(altitudeColor(80000), ALT_STOPS[6][1], "clamped");
  assert.equal(altitudeColor(-500), ALT_STOPS[0][1], "below sea level counts as 0");
  assert.equal(altitudeColor(null), null);
  assert.equal(altitudeColor(undefined), null);
  assert.equal(altitudeColor("abc"), null);
  assert.match(altitudeColor(12000), /^#[0-9a-f]{6}$/);
  assert.notEqual(altitudeColor(12000), altitudeColor(20000));
});

test("altitude colours are quantised so the sprite atlas stays small", () => {
  const set = new Set();
  for (let a = 0; a <= 50000; a += 100) set.add(altitudeColor(a));
  assert.ok(set.size <= 45000 / ALT_BAND_FT + 2, String(set.size));
  assert.equal(altitudeColor(5100), altitudeColor(5400), "same band");
});

test("legend gradient names every stop", () => {
  const css = altitudeGradientCss();
  for (const [, c] of ALT_STOPS) assert.ok(css.includes(c));
  assert.match(css, /^linear-gradient\(90deg,/);
});

test("icon ids carry an optional state (hover, stale) and stay backward compatible", async () => {
  const { parseIconIdFull, STATES } = await import("../public/icons.js");
  assert.equal(iconId("narrow", "#e6e8ea", "d", "h"), "narrow|e6e8ea|d|h");
  assert.equal(iconId("narrow", "#e6e8ea", "d"), "narrow|e6e8ea|d");
  assert.deepEqual(parseIconIdFull("narrow|e6e8ea|l|x"), ["narrow", "#e6e8ea", "l", "x"]);
  assert.deepEqual(parseIconIdFull("narrow|e6e8ea|l|zzz"), ["narrow", "#e6e8ea", "l", ""]);
  assert.deepEqual(parseIconId("narrow|e6e8ea|l|x"), ["narrow", "#e6e8ea", "l"]);
  assert.deepEqual(STATES, ["", "h", "x"]);
});
