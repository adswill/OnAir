// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

import test from "node:test";
import assert from "node:assert/strict";
import { parseIngestBody } from "../src/lenient-json.js";

test("valid JSON is parsed as is", () => {
  assert.equal(parseIngestBody('{"a":[{"squawk":"0123"}]}').a[0].squawk, "0123");
});
test("an unquoted squawk with a leading zero is quoted", () => {
  const b = parseIngestBody('{"a":[{"id":"x","squawk":0123},{"squawk":7700,"alt":1}]}');
  assert.equal(b.a[0].squawk, "0123");
  assert.equal(b.a[1].squawk, "7700");
});
test("other broken JSON is still refused", () => {
  assert.throws(() => parseIngestBody('{"a":'));
});
