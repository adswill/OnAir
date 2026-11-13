// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

import { test } from "node:test";
import assert from "node:assert/strict";
import { makeToken, verifyToken, cleanNick, cleanText, cleanChat, moderate, scrubPersonal, parseWfLine, parsePush, WindowLimit, validRoomId, randomId, LIMITS, b64uEncode } from "../src/lib.js";

const SECRET = "test-secret-0123456789abcdef";

test("tokens verify, expire and reject tampering", async () => {
  const t = await makeToken(SECRET, { k: "host", r: "abc", e: Date.now() + 1000 });
  assert.equal((await verifyToken(SECRET, t)).k, "host");
  assert.equal(await verifyToken("other-secret-0123456789", t), null);
  assert.equal(await verifyToken(SECRET, t.slice(0, -2) + "xx"), null);
  const [p, s] = t.split(".");
  const forged = b64uEncode(new TextEncoder().encode(JSON.stringify({ k: "host", r: "zzz", e: Date.now() + 1e6 }))) + "." + s;
  assert.equal(await verifyToken(SECRET, forged), null);
  const old = await makeToken(SECRET, { k: "host", r: "abc", e: 1000 });
  assert.equal(await verifyToken(SECRET, old), null);
  assert.equal(await verifyToken(SECRET, "garbage"), null);
  assert.equal(await verifyToken(SECRET, 42), null);
});
test("a short secret is refused", async () => { await assert.rejects(makeToken("short", { e: 1 })); });

test("nick and text cleaning", () => {
  assert.equal(cleanNick("<script>alert(1)</script>"), "scriptalert1sc");
  assert.equal(cleanNick("   "), "guest");
  assert.equal(cleanNick("a".repeat(100)).length, 20);
  assert.equal(cleanText("a\u0000b‮c\nd", 50), "a b c d");
  assert.equal(Array.from(cleanText("x".repeat(500), 300)).length, 300);
});
test("personal data and links are removed from chat", () => {
  assert.equal(scrubPersonal("mail me at bob@example.com or +44 20 7946 0958"), "mail me at [email] or [number]");
  assert.equal(cleanChat("see https://evil.example/x now"), "see [link] now");
});
test("profanity-light stars a short list, also with look-alike digits", () => {
  assert.equal(moderate("what the fuck"), "what the ****");
  assert.equal(moderate("sh1t happens"), "**** happens");
  assert.equal(moderate("a normal sentence"), "a normal sentence");
});
test("waterfall lines must be exactly 512 bytes", () => {
  const ok = Buffer.alloc(512, 7).toString("base64");
  assert.equal(parseWfLine(ok).length, 512);
  assert.equal(parseWfLine(Buffer.alloc(511).toString("base64")), null);
  assert.equal(parseWfLine(Buffer.alloc(513).toString("base64")), null);
  assert.equal(parseWfLine(5), null);
  assert.equal(parseWfLine("!!!not base64!!!"), null);
});
test("a push is capped and the decoded text only passes when the host opted in", () => {
  const ok = Buffer.alloc(512, 1).toString("base64");
  const body = { wf: Array(10).fill(ok), msgs: ["hello <b>x</b>", "call 0044 20 7946 0958 now", ...Array(50).fill("y")], title: "T".repeat(200), freq: 522, cs: 3 };
  const off = parsePush(body, false);
  assert.equal(off.texts.length, 0);
  assert.equal(off.lines.length, LIMITS.wfLinesPerPush);
  const on = parsePush(body, true);
  assert.equal(on.texts.length, LIMITS.textsPerPush);
  assert.match(on.texts[1], /\[number\]/);
  assert.equal(on.title.length, 60);
  assert.equal(on.freq, 522);
  assert.equal(on.chatSeen, 3);
  assert.deepEqual(parsePush(null, true).lines, []);
});
test("window limit", () => {
  const w = new WindowLimit(2, 1000);
  assert.ok(w.allow(0)); assert.ok(w.allow(10)); assert.ok(!w.allow(20)); assert.ok(w.allow(1001));
});
test("room ids", () => {
  const id = randomId(10);
  assert.ok(validRoomId(id));
  assert.ok(!validRoomId("../etc/passwd")); assert.ok(!validRoomId("ABCDEFGHJK")); assert.ok(!validRoomId(""));
});
