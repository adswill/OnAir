// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The real MapHub ingest path (src/index.js) with sign-in: a fake Durable Object context (SQL calls are recorded and answer nothing, the
// badge storage is swapped for the memory backend), so the code under test is the hub's own ingest, by-name and reply logic.
import test from "node:test";
import assert from "node:assert/strict";
import { createHmac } from "node:crypto";
import { MapHub } from "../src/index.js";
import { MemoryBadgeBackend, BadgeBook } from "../src/badge-stats.js";

const SECRET = "hub-test-secret-0123456789";
function marketToken(uid, login) {
  const now = Math.floor(Date.now() / 1000);
  const p = Buffer.from(JSON.stringify({ uid, login, iat: now - 5, exp: now + 3600 })).toString("base64url");
  return p + "." + createHmac("sha256", SECRET).update(p).digest("base64url");
}
function fakeCtx() {
  const cursor = { toArray: () => [], rowsWritten: 0 };
  return {
    storage: { sql: { exec: () => cursor }, transactionSync: (fn) => fn(), setAlarm: () => {} },
    blockConcurrencyWhile: (fn) => fn(),
    getWebSockets: () => [],
  };
}
async function hub() {
  const h = new MapHub(fakeCtx(), { SESSION_SECRET: SECRET });
  await h.ready;
  const db = new MemoryBadgeBackend();
  h.badges.db = db;
  h.badges.book = new BadgeBook(db);
  h.badges.claims.clear();
  return h;
}
let n = 0;
const station = () => "station-test-" + String(++n).padStart(4, "0");
function ingest(h, body, token) {
  const headers = { "Content-Type": "application/json" };
  if (token) headers.Authorization = "Bearer " + token;
  return h.fetch(new Request("https://map.on-air.dev/ingest", { method: "POST", headers, body: JSON.stringify(body) })).then((r) => r.json());
}
const ac = (hex, lat, lon) => ({ kind: "aircraft", id: hex, icao: hex, lat, lon, alt: 30000, speed: 420, src: "adsb", age: 1 });

test("signed out: the reply and the data are exactly as before", async () => {
  const h = await hub();
  const r = await ingest(h, { station: station(), tracks: [ac("3C6444", 50, 8)] });
  assert.equal(r.ok, true);
  assert.equal(r.n, 1);
  assert.ok(!("account" in r) && !("awards" in r) && !("account_error" in r));
  assert.equal(h.badges.book.users.size, 0, "nothing is kept for anonymous posts");
});

test("signed in with the marketplace token: the post counts and new badges come back", async () => {
  const h = await hub();
  const tok = marketToken(7, "alice");
  const me = await h.fetch(new Request("https://map.on-air.dev/api/me/settings", { method: "POST", headers: { Authorization: "Bearer " + tok }, body: JSON.stringify({ badges: true, public: true, by: "AliceRx" }) }));
  assert.equal(me.status, 200);
  const st = station();
  const r = await ingest(h, { station: st, by: "AliceRx", home: "KO24pq", tracks: [ac("3C6444", 54.9, 25.5), ac("4B1234", 54.8, 25.4)] }, tok);
  assert.equal(r.n, 2);
  assert.equal(r.account.login, "alice");
  assert.ok(r.awards.some((a) => a.id === "first_light"));
  assert.equal(h.badges.book.users.get(7).s.c.aircraft, 2);
  // the next post carries the showcase badge on the track (name linked + public)
  await new Promise((res) => setTimeout(res, 2100));
  await ingest(h, { station: st, by: "AliceRx", tracks: [ac("3C6444", 54.91, 25.51)] }, tok);
  const row = h.book.rows.get("aircraft:3C6444");
  assert.equal(JSON.parse(h.book.wire(row)).bb, "first_light:1:alice");
  // nothing about the home square is kept
  assert.ok(!JSON.stringify([...h.badges.db.users.values()]).includes("KO24"));
});

test("a bad token: the data is still taken and the reply says why", async () => {
  const h = await hub();
  const r = await ingest(h, { station: station(), tracks: [ac("3C6444", 50, 8)] }, marketToken(7, "alice").slice(0, -3) + "AAA");
  assert.equal(r.n, 1);
  assert.match(r.account_error, /sign-in/);
});

test("a linked map name is only for its account", async () => {
  const h = await hub();
  const tok = marketToken(7, "alice");
  await h.fetch(new Request("https://map.on-air.dev/api/me/settings", { method: "POST", headers: { Authorization: "Bearer " + tok }, body: JSON.stringify({ by: "AliceRx" }) }));
  const anon = await ingest(h, { station: station(), by: "alicerx", tracks: [ac("3C6444", 50, 8)] });
  assert.equal(anon.n, 1);
  assert.match(anon.by_error, /linked to an OnAir account/);
  assert.equal(h.book.rows.get("aircraft:3C6444").by, "", "posted without the name");
  const other = await ingest(h, { station: station(), by: "Someone", tracks: [ac("4B1234", 50, 8)] });
  assert.equal(other.by_error, undefined, "unlinked names work as before");
});
