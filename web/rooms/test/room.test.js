// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The Room and Lobby objects against a fake Durable Object state (no workerd needed).
import { test } from "node:test";
import assert from "node:assert/strict";
import { Room, wfFrame } from "../src/room.js";
import { Lobby } from "../src/lobby.js";
import { makeToken, LIMITS } from "../src/lib.js";

class FakeWS {
  constructor() { this.sent = []; this.att = null; this.closed = false; }
  send(d) { this.sent.push(d); }
  close() { this.closed = true; }
  serializeAttachment(a) { this.att = a; }
  deserializeAttachment() { return this.att; }
  json() { return this.sent.filter((x) => typeof x === "string").map((x) => JSON.parse(x)); }
}
function mkRoom() {
  const store = new Map(), socks = [];
  const ctx = {
    storage: { get: async (k) => store.get(k), put: async (k, v) => { store.set(k, v); }, setAlarm: async () => {}, deleteAlarm: async () => {}, deleteAll: async () => store.clear() },
    blockConcurrencyWhile: (f) => f(), getWebSockets: () => socks.filter((s) => !s.closed), acceptWebSocket: (s) => socks.push(s),
  };
  const env = { ROOM_SECRET: "test-secret-0123456789abcdef", LOBBY: { idFromName: () => 1, get: () => ({ fetch: async () => new Response("{}") }) } };
  const r = new Room(ctx, env);
  return { r, socks, env };
}
const post = (r, path, body) => r.fetch(new Request("https://room" + path, { method: "POST", body: JSON.stringify(body) }));
async function join(r, socks, nick) {
  const ws = new FakeWS(); ws.att = { ip: "x", t: Date.now(), id: null }; socks.push(ws);
  await r.webSocketMessage(ws, JSON.stringify({ t: "hello", nick }));
  return ws;
}
const line = (v) => Buffer.alloc(512, v).toString("base64");

test("push reaches viewers as a binary frame; text only when the transcript is allowed", async () => {
  const { r, socks } = mkRoom();
  await post(r, "/init", { id: "abcdefghjk", title: "Test", mode: "ADS-B", freq: 1090, transcript: false });
  const v = await join(r, socks, "ann");
  await post(r, "/push", { wf: [line(5), line(6)], msgs: ["secret text"] });
  const bin = v.sent.find((x) => typeof x !== "string");
  assert.equal(new Uint8Array(bin)[0], 1); assert.equal(new Uint8Array(bin)[1], 2); assert.equal(bin.byteLength, 2 + 2 * 512);
  assert.ok(!v.json().some((m) => m.t === "texts"));
});
test("chat: rate limit, mute, report, ban", async () => {
  const { r, socks } = mkRoom();
  await post(r, "/init", { id: "abcdefghjk", title: "T", mode: "FM", freq: 100, transcript: true });
  const a = await join(r, socks, "ann"), b = await join(r, socks, "bob");
  await r.webSocketMessage(a, JSON.stringify({ t: "chat", text: "hi <b>there</b>" }));
  await r.webSocketMessage(a, JSON.stringify({ t: "chat", text: "again" }));
  assert.ok(a.json().some((m) => m.t === "slow"));
  assert.equal(b.json().filter((m) => m.t === "chat").length, 1);
  assert.equal(b.json().find((m) => m.t === "chat").text, "hi <b>there</b>");   // stored as text; the page uses textContent
  await r.webSocketMessage(b, JSON.stringify({ t: "report", seq: 1 }));
  const res = await (await r.fetch(new Request("https://room/hostview"))).json();
  assert.equal(res.reports.length, 1); assert.equal(res.chat.length, 1);
  await post(r, "/mod", { action: "mute", vid: a.att.id });
  const n = b.json().length;
  a.sent.length = 0; a.att.id && (r.viewerGap.clear());
  await r.webSocketMessage(a, JSON.stringify({ t: "chat", text: "muted?" }));
  assert.equal(b.json().length, n);
  await post(r, "/mod", { action: "ban", vid: b.att.id });
  assert.ok(b.closed);
  const c = new FakeWS(); c.att = { ip: "x", t: 0, id: null }; socks.push(c);
  await r.webSocketMessage(c, JSON.stringify({ t: "hello", nick: "b2", vt: "garbage" }));
  assert.ok(c.att.id);   // a new anonymous viewer is fine; the banned id itself is refused below
  const re = new FakeWS(); re.att = { ip: "x", t: 0, id: null }; socks.push(re);
  r.mod.banned.push("zzzzzzzz");
  const tok = await makeToken("test-secret-0123456789abcdef", { k: "viewer", r: "abcdefghjk", v: "zzzzzzzz", e: Date.now() + 1e5 });
  await r.webSocketMessage(re, JSON.stringify({ t: "hello", nick: "x", vt: tok }));
  assert.ok(re.closed);
});
test("viewer cap", async () => {
  const { r, socks } = mkRoom();
  await post(r, "/init", { id: "abcdefghjk", title: "T", mode: "FM", freq: 100, transcript: false });
  for (let i = 0; i < LIMITS.viewersPerRoom; i++) socks.push(new FakeWS());
  const res = await r.fetch(new Request("https://room/ws", { headers: { Upgrade: "websocket" } }));
  assert.equal(res.status, 503);
});
test("a room that ends keeps nothing and refuses further traffic", async () => {
  const { r } = mkRoom();
  await post(r, "/init", { id: "abcdefghjk", title: "T", mode: "FM", freq: 100, transcript: false });
  await post(r, "/end", {});
  assert.equal((await post(r, "/push", {})).status, 404);
});
test("idle room expires on the alarm", async () => {
  const { r, socks } = mkRoom();
  await post(r, "/init", { id: "abcdefghjk", title: "T", mode: "FM", freq: 100, transcript: false });
  const v = await join(r, socks, "ann");
  r.meta.lastPush = Date.now() - LIMITS.idleExpireMs - 1;
  await r.alarm();
  assert.ok(v.json().some((m) => m.t === "end")); assert.ok(r.meta.ended);
});
test("lobby: per-IP creation limit and room cap", async () => {
  const l = new Lobby({}, {});
  const reserve = (ip, id) => l.fetch(new Request("https://l/reserve", { method: "POST", body: JSON.stringify({ ip, id, title: "t" }) }));
  for (let i = 0; i < LIMITS.createPerIpPerHour; i++) assert.equal((await reserve("ip1", "r" + i)).status, 200);
  assert.equal((await reserve("ip1", "rx")).status, 429);
  assert.equal((await reserve("ip2", "ry")).status, 200);
  const list = await (await l.fetch(new Request("https://l/list"))).json();
  assert.equal(list.rooms.length, LIMITS.createPerIpPerHour + 1);
});
test("wfFrame layout", () => {
  const f = new Uint8Array(wfFrame([new Uint8Array(512).fill(9)]));
  assert.equal(f[0], 1); assert.equal(f[1], 1); assert.equal(f[2], 9); assert.equal(f.length, 514);
});

test("host PIN check is hashed, wrong pins fail, and tries are limited", async () => {
  const { r, env } = mkRoom();
  const { hashIp } = await import("../src/lib.js");
  const pinHash = await hashIp(env.ROOM_SECRET, "pin|abcdefghjk|PIN12345");
  await post(r, "/init", { id: "abcdefghjk", title: "T", mode: "FM", freq: 100, transcript: false, pinHash });
  assert.equal((await (await post(r, "/hostcheck", { pin: "PIN12345" })).json()).ok, true);
  assert.equal((await (await post(r, "/hostcheck", { pin: "nope" })).json()).ok, false);
  for (let i = 0; i < 4; i++) await post(r, "/hostcheck", { pin: "x" });
  assert.equal((await post(r, "/hostcheck", { pin: "PIN12345" })).status, 429);
  assert.ok(!JSON.stringify(await (await r.fetch(new Request("https://room/info"))).json()).includes(pinHash));
});
