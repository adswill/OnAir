// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

import test from "node:test";
import assert from "node:assert/strict";
import { verifySignature } from "../src/verify.js";
import { handle } from "../src/index.js";
import { summarize, statsFields, buildEmbed } from "../src/stats.js";

const toHex = (b) => Buffer.from(b).toString("hex");
const kp = await crypto.subtle.generateKey({ name: "Ed25519" }, true, ["sign", "verify"]);
const pubHex = toHex(await crypto.subtle.exportKey("raw", kp.publicKey));
const sign = async (ts, body) =>
  toHex(await crypto.subtle.sign({ name: "Ed25519" }, kp.privateKey, new TextEncoder().encode(ts + body)));
const env = { DISCORD_PUBLIC_KEY: pubHex, DISCORD_APP_ID: "1" };
const ctx = { waitUntil() {} };
const post = async (body, sig, ts = "1700000000") =>
  new Request("https://x/", {
    method: "POST", body,
    headers: { "X-Signature-Ed25519": sig ?? (await sign(ts, body)), "X-Signature-Timestamp": ts },
  });

test("valid signature verifies, tampered fails", async () => {
  const sig = await sign("123", "hello");
  assert.equal(await verifySignature(pubHex, sig, "123", "hello"), true);
  assert.equal(await verifySignature(pubHex, sig, "123", "hellp"), false);
  assert.equal(await verifySignature(pubHex, "zz", "123", "hello"), false);
});

test("PING gets PONG", async () => {
  const res = await handle(await post(JSON.stringify({ type: 1 })), env, ctx);
  assert.equal(res.status, 200);
  assert.deepEqual(await res.json(), { type: 1 });
});

test("bad signature gets 401", async () => {
  const res = await handle(await post(JSON.stringify({ type: 1 }), "00".repeat(64)), env, ctx);
  assert.equal(res.status, 401);
});

test("unknown command is ephemeral; /map defers", async () => {
  let res = await handle(await post(JSON.stringify({ type: 2, data: { name: "nope" } })), env, ctx);
  assert.equal((await res.json()).data.flags, 64);
  let waited = 0;
  res = await handle(await post(JSON.stringify({ type: 2, token: "t", data: { name: "stats" } })), env,
    { waitUntil() { waited++; } }, async () => new Response("{}"));
  assert.equal((await res.json()).type, 5);
  assert.equal(waited, 1);
});

test("stats formatting from mocked /live", () => {
  const live = {
    stations: 7,
    tracks: [{ kind: "aircraft" }, { kind: "aircraft" }, { kind: "sonde" }, { kind: "vessel" }, { kind: "drone" }],
    load: { pct: 3, viewers: 4, ingestPerMin: 120, throttlePerMin: 0 },
  };
  const s = summarize(live);
  assert.equal(s.markers, 5);
  const f = Object.fromEntries(statsFields(s).map((x) => [x.name, x.value]));
  assert.equal(f.Aircraft, "2");
  assert.equal(f["APRS stations"], "0");
  assert.equal(f.Drones, "1");
  assert.equal(f.FLARM, undefined);
  assert.equal(f["Live viewers"], "4");
  assert.match(f.Ingest, /120 posts\/min \(2\.0\/s\)/);
  assert.equal(f["Server load"], "3%");
  const e = buildEmbed(s, { shotAgeSec: 12, withImage: true });
  assert.match(e.footer.text, /cached 12 s ago/);
  assert.equal(e.image.url, "attachment://map.png");
});
