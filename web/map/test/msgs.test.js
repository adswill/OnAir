// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

import test from "node:test";
import assert from "node:assert/strict";
import {
  cleanText, redact, sanitizeMsg, sanitizeList, mergeStored, takeBudget, msgKey,
  MSG_TTL_MS, MSG_PER_TRACK, MSG_TEXT_MAX, MSG_STATION_PER_MIN, MSG_PER_TRACK_PER_INGEST,
} from "../src/msgs.js";

const NOW = 1_800_000_000_000;
const mk = (o = {}) => ({ t: NOW / 1000 - 10, m: "VDL2", f: 136.975, l: "H1", c: "crew", x: "HELLO CREW", ...o });

test("sanitising strips control characters and caps length", () => {
  assert.equal(cleanText("A\u0000B\u0007C‮D"), "A B C D");
  assert.equal(cleanText("x".repeat(900)).length, MSG_TEXT_MAX);
  assert.equal(cleanText("a\r\nb"), "a\nb");
  assert.equal(cleanText("😀".repeat(400)).length <= MSG_TEXT_MAX, true);
  assert.ok(!/[\ud800-\udbff]$/.test(cleanText("a" + "😀".repeat(300))));
});

test("page-hostile text is kept as plain text, not interpreted", () => {
  const m = sanitizeMsg(mk({ x: '<img src=x onerror=alert(1)> "q" \\ back' }), NOW);
  assert.equal(m.x, '<img src=x onerror=alert(1)> "q" \\ back');   // escaping is the page's job (textContent)
  assert.equal(JSON.parse(JSON.stringify(m)).x, m.x);
});

test("phone numbers and e-mail are removed", () => {
  assert.equal(redact("CALL +44 20 7946 0958 NOW"), "CALL [number] NOW");
  assert.equal(redact("MAIL crew.ops@airline.example PLS"), "MAIL [email] PLS");
  assert.equal(redact("FL350 REQ 1234"), "FL350 REQ 1234");
});

test("bad messages are dropped", () => {
  assert.equal(sanitizeMsg(null, NOW), null);
  assert.equal(sanitizeMsg(mk({ x: "   " }), NOW), null);
  assert.equal(sanitizeMsg(mk({ t: "nope" }), NOW), null);
  assert.equal(sanitizeMsg(mk({ t: (NOW - MSG_TTL_MS) / 1000 - 5 }), NOW), null);   // older than 48 h
  assert.equal(sanitizeMsg(mk({ t: NOW / 1000 + 3600 }), NOW), null);               // from the future
  assert.equal(sanitizeMsg(mk({ m: "evil", c: "zzz", f: -3 }), NOW).m, "OTHER");
  assert.equal(sanitizeMsg(mk({ c: "zzz" }), NOW).c, "other");
});

test("duplicates and per-track cap in one ingest", () => {
  const list = [mk(), mk(), ...Array.from({ length: 12 }, (_, i) => mk({ x: "T" + i }))];
  const out = sanitizeList(list, NOW);
  assert.equal(out.length, MSG_PER_TRACK_PER_INGEST);
  assert.equal(new Set(out.map(msgKey)).size, out.length);
  assert.deepEqual(sanitizeList("x", NOW), []);
});

test("stored list: newest first, deduped, capped, expired removed", () => {
  let stored = [];
  for (let k = 0; k < 8; k++) {
    stored = mergeStored(stored, Array.from({ length: 5 }, (_, i) => sanitizeMsg(mk({ t: NOW / 1000 - 1000 + k * 10 + i, x: `M${k}-${i}` }), NOW)), NOW);
  }
  assert.equal(stored.length, MSG_PER_TRACK);
  assert.ok(stored[0].t >= stored[stored.length - 1].t);
  const again = mergeStored(stored, [stored[0]], NOW);
  assert.equal(again.length, stored.length);
  assert.equal(mergeStored(stored, [], NOW + MSG_TTL_MS + 60e3).length, 0);
});

test("station rate limit", () => {
  const st = new Map();
  assert.equal(takeBudget(st, "s1", 100, NOW), 100);
  assert.equal(takeBudget(st, "s1", 100, NOW + 1000), MSG_STATION_PER_MIN - 100);
  assert.equal(takeBudget(st, "s1", 5, NOW + 2000), 0);
  assert.equal(takeBudget(st, "s2", 5, NOW + 2000), 5);
  assert.equal(takeBudget(st, "s1", 5, NOW + 70e3), 5);   // a minute later
});

test("transcripts of radio calls arrive as mode VOICE, cleaned like every other message", () => {
  const m = sanitizeMsg(mk({ m: "voice", c: "free", l: "", f: 118.7, x: "Lufthansa one two three, call +49 69 1234 5678 now" }), NOW);
  assert.equal(m.m, "VOICE");
  assert.equal(m.x, "Lufthansa one two three, call [number] now");
  assert.equal(m.f, 118.7);
});
