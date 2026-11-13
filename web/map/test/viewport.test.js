// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

import test from "node:test";
import assert from "node:assert/strict";
import { peekHeight, fullMaxHeight, freeArea, isClear, centreOffset, settleState } from "../public/viewport.js";

test("peek is about 31% of a phone and clamped", () => {
  assert.equal(peekHeight(812), 252);
  assert.equal(peekHeight(390), 150);       // landscape: floor
  assert.equal(peekHeight(1400), 300);      // tall: ceiling
  assert.ok(peekHeight(200) <= 100);        // never more than half
});
test("full cap", () => { assert.equal(fullMaxHeight(812), 585); assert.equal(fullMaxHeight(390), 234); });
test("free area above the sheet", () => {
  const a = freeArea(812, 40, 252);
  assert.deepEqual([a.top, a.bottom, a.height, a.centerY], [40, 560, 520, 300]);
});
test("marker under the sheet is not clear, one above is", () => {
  const a = freeArea(812, 40, 252);
  assert.equal(isClear(600, a), false);
  assert.equal(isClear(300, a), true);
  assert.equal(isClear(700, a), false);
});
test("centreOffset moves a marker to the middle of the free area", () => {
  const a = freeArea(812, 40, 252);
  assert.deepEqual(centreOffset(187, 406, 375, a), [0, 106]);
  assert.deepEqual(centreOffset(100, 300, 375, a), [-87, 0]);
});
test("settleState", () => {
  assert.equal(settleState("peek", -60), "full");
  assert.equal(settleState("full", 60), "peek");
  assert.equal(settleState("peek", -5), "peek");
});
