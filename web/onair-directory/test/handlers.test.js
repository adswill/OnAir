// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Worker handlers against an in-memory stand-in for D1. The fake understands exactly the statements src/index.js issues (matched by their
// text), so a changed query shows up here as an unknown statement instead of silently passing.
import test from "node:test";
import assert from "node:assert/strict";
import { handle } from "../src/index.js";
import { canonicalize, toHex } from "../src/logic.js";

function fakeDB() {
  const t = { servers: new Map(), rl: new Map(), blocks: [], reports: [], n: 0 };
  const stmt = (sql) => {
    let args = [];
    const q = sql.replace(/\s+/g, " ").trim();
    const run = (mode) => {
      const a = args;
      let r;
      if (q.startsWith("INSERT INTO rl")) {
        const [k, win, , exp] = [a[0], a[1], 1, a[2]];
        const row = t.rl.get(k);
        if (row && row.win === win) row.n++; else t.rl.set(k, { win, n: 1, exp });
        r = { n: t.rl.get(k).n };
      } else if (q.startsWith("SELECT 1 AS x FROM blocks")) {
        const [id, pub, host] = a;
        r = t.blocks.find((b) => (b.kind === "id" && b.value === id) || (b.kind === "pub" && b.value === pub) || (b.kind === "host" && (host === b.value || host.endsWith("." + b.value)))) ? { x: 1 } : null;
      } else if (q === "SELECT * FROM servers WHERE id = ?1") r = t.servers.get(a[0]) || null;
      else if (q.startsWith("SELECT COUNT(*) AS n FROM servers")) r = { n: [...t.servers.values()].filter((s) => s.expires_at > a[0]).length };
      else if (q.startsWith("SELECT id FROM servers WHERE url")) r = [...t.servers.values()].find((s) => s.url === a[0] && s.id !== a[1]) || null;
      else if (q.startsWith("INSERT INTO servers")) {
        const [id, pub, name, url, host, loc, lat, lon, freq, radio, cap, users, ver, contact, descr, reach_opt, reach_ok, reach_at, interval_s, now, last_ts, expires_at] = a;
        const old = t.servers.get(id);
        t.servers.set(id, { id, pub, name, url, host, loc, lat, lon, freq, radio, cap, users, ver, contact, descr, reach_opt, reach_ok, reach_at, interval_s,
          first_seen: old ? old.first_seen : now, last_seen: now, last_ts, expires_at });
        r = { meta: { changes: 1 } };
      } else if (q.startsWith("SELECT * FROM servers WHERE expires_at > ?1 ORDER")) r = { results: [...t.servers.values()].filter((s) => s.expires_at > a[0]) };
      else if (q.startsWith("SELECT * FROM servers WHERE id = ?1 AND expires_at")) { const s = t.servers.get(a[0]); r = s && s.expires_at > a[1] ? s : null; }
      else if (q.startsWith("SELECT 1 AS x FROM servers WHERE id")) r = t.servers.has(a[0]) ? { x: 1 } : null;
      else if (q.startsWith("INSERT INTO reports")) { t.reports.push({ n: ++t.n, server_id: a[0], reason: a[1], text: a[2], at: a[3] }); r = { meta: { changes: 1 } }; }
      else if (q === "SELECT win FROM rl WHERE k = ?1 AND exp > ?2") { const row = t.rl.get(a[0]); r = row && row.exp > a[1] ? { win: row.win } : null; }
      else if (q.startsWith("INSERT OR REPLACE INTO rl")) { t.rl.set(a[0], { win: a[1], n: 1, exp: a[2] }); r = { meta: { changes: 1 } }; }
      else if (q === "DELETE FROM servers WHERE id = ?1") r = { meta: { changes: t.servers.delete(a[0]) ? 1 : 0 } };
      else if (q.startsWith("INSERT OR REPLACE INTO blocks")) { t.blocks.push({ kind: a[0], value: a[1], note: a[2], at: a[3] }); r = { meta: { changes: 1 } }; }
      else if (q.startsWith("DELETE FROM servers WHERE host = ?1")) {
        let c = 0;
        for (const [k, s] of t.servers) if (s.host === a[0] || s.host.endsWith("." + a[0])) { t.servers.delete(k); c++; }
        r = { meta: { changes: c } };
      } else if (/^DELETE FROM servers WHERE (id|pub) = \?1$/.test(q)) {
        const col = q.includes("pub") ? "pub" : "id";
        let c = 0;
        for (const [k, s] of t.servers) if (s[col] === a[0]) { t.servers.delete(k); c++; }
        r = { meta: { changes: c } };
      } else if (q.startsWith("DELETE FROM blocks")) { t.blocks = t.blocks.filter((b) => !(b.kind === a[0] && b.value === a[1])); r = { meta: { changes: 1 } }; }
      else if (q.startsWith("SELECT * FROM reports")) r = { results: [...t.reports].reverse() };
      else if (q.startsWith("DELETE FROM reports")) { t.reports = t.reports.filter((x) => x.n !== a[0]); r = { meta: { changes: 1 } }; }
      else throw new Error("fake D1: unknown statement: " + q);
      return r;
    };
    const s = {
      bind(...x) { args = x; return s; },
      first: async () => run("first"),
      all: async () => run("all"),
      run: async () => run("run"),
    };
    return s;
  };
  return { prepare: stmt, t };
}

const seed = new Uint8Array(32).map((_, i) => i + 40);
const key = await crypto.subtle.importKey("pkcs8", Buffer.concat([Buffer.from("302e020100300506032b657004220420", "hex"), Buffer.from(seed)]), { name: "Ed25519" }, true, ["sign"]);
const PUB = Buffer.from((await crypto.subtle.exportKey("jwk", key)).x, "base64url").toString("hex");
const seed2 = new Uint8Array(32).map((_, i) => i + 90);
const key2 = await crypto.subtle.importKey("pkcs8", Buffer.concat([Buffer.from("302e020100300506032b657004220420", "hex"), Buffer.from(seed2)]), { name: "Ed25519" }, true, ["sign"]);
const PUB2 = Buffer.from((await crypto.subtle.exportKey("jwk", key2)).x, "base64url").toString("hex");

let NOW = 1790000000;
const deps = (over = {}) => ({ now: () => NOW, resolvesPublic: async () => true, probeReachable: async () => true, ...over });
const mkEnv = () => ({ DB: fakeDB(), ADMIN_TOKEN: "admintoken-for-tests", IP_SALT: "s", MAX_SERVERS: "3", ASSETS: { fetch: async () => new Response("asset") } });

async function signed(rec, k = key) {
  const body = { v: 1, ...rec };
  const sig = Buffer.from(await crypto.subtle.sign({ name: "Ed25519" }, k, new TextEncoder().encode(canonicalize(body)))).toString("hex");
  return { ...body, sig };
}
const base = (o = {}) => ({ action: "heartbeat", id: "srv-one-0001", pub: PUB, ts: NOW, name: "One", url: "https://one.example.org/", loc: "54.69,25.28", freq: [[88e6, 108e6]],
  radio: "HackRF", cap: 10, users: 2, interval: 300, ver: "3.0", contact: "", desc: "d", reach: 0, ...o });
const post = (env, path, body, ip = "1.1.1.1", d = deps()) => handle(new Request("https://dir.test" + path, { method: "POST", headers: { "content-type": "application/json", "cf-connecting-ip": ip }, body: JSON.stringify(body) }), env, {}, d);
const get = (env, path, init = {}, d = deps()) => handle(new Request("https://dir.test" + path, init), env, {}, d);
const beat = async (env, o = {}, k = key, ip, d) => post(env, "/api/v1/heartbeat", await signed(base(o), k), ip, d);

test("first heartbeat registers, later ones update, listing shows it", async () => {
  NOW = 1790000000;
  const env = mkEnv();
  let r = await beat(env);
  assert.equal(r.status, 200);
  assert.equal((await r.json()).new, true);
  NOW += 60;
  r = await beat(env, { ts: NOW, users: 5 });
  assert.equal(r.status, 200);
  assert.equal((await r.json()).new, false);
  const list = await (await get(env, "/api/v1/servers")).json();
  assert.equal(list.count, 1);
  assert.equal(list.servers[0].users, 5);
  assert.equal(list.servers[0].pub, undefined);
  assert.equal(list.servers[0].lat, 54.69);
  const one = await get(env, "/api/v1/servers/srv-one-0001");
  assert.equal(one.status, 200);
  assert.equal(one.headers.get("access-control-allow-origin"), "*");
  assert.equal((await get(env, "/api/v1/servers/nonexistent1")).status, 404);
});

test("entries expire after three missed heartbeats", async () => {
  NOW = 1790000000;
  const env = mkEnv();
  await beat(env);
  NOW += 899;
  assert.equal((await (await get(env, "/api/v1/servers")).json()).count, 1);
  NOW += 2;
  assert.equal((await (await get(env, "/api/v1/servers")).json()).count, 0);
  assert.equal((await get(env, "/api/v1/servers/srv-one-0001")).status, 404);
});

test("signature, key and replay rules", async () => {
  NOW = 1790000000;
  const env = mkEnv();
  assert.equal((await beat(env)).status, 200);
  // wrong signature (signed by another key but claiming the first pub)
  NOW += 60;
  let r = await post(env, "/api/v1/heartbeat", await signed(base({ ts: NOW }), key2));
  assert.equal(r.status, 401);
  // tampered field after signing
  const ok = await signed(base({ ts: NOW }));
  r = await post(env, "/api/v1/heartbeat", { ...ok, users: 9 });
  assert.equal(r.status, 401);
  // someone else claims the id with their own key: refused, the first key stays
  r = await beat(env, { ts: NOW, pub: PUB2 }, key2);
  assert.equal(r.status, 403);
  // replay of an old request
  r = await post(env, "/api/v1/heartbeat", await signed(base({ ts: NOW - 60 })));
  assert.equal(r.status, 409);
  // clock far off
  r = await beat(env, { ts: NOW + 4000 });
  assert.equal(r.status, 400);
  // bad content type / body / size
  r = await handle(new Request("https://dir.test/api/v1/heartbeat", { method: "POST", headers: { "content-type": "text/plain" }, body: "x" }), env, {}, deps());
  assert.equal(r.status, 415);
  r = await handle(new Request("https://dir.test/api/v1/heartbeat", { method: "POST", headers: { "content-type": "application/json" }, body: "{bad" }), env, {}, deps());
  assert.equal(r.status, 400);
  r = await handle(new Request("https://dir.test/api/v1/heartbeat", { method: "POST", headers: { "content-type": "application/json" }, body: "x".repeat(5000) }), env, {}, deps());
  assert.equal(r.status, 413);
});

test("url rules: private literal, non-public DNS, duplicate url", async () => {
  NOW = 1790000000;
  const env = mkEnv();
  assert.equal((await beat(env, { url: "https://10.0.0.5/" })).status, 400);
  assert.equal((await beat(env, { url: "https://one.example.org/" }, key, "2.2.2.2", deps({ resolvesPublic: async () => false }))).status, 422);
  assert.equal((await beat(env)).status, 200);
  NOW += 60;
  // another server tries to list the same url
  const other = await signed(base({ id: "srv-two-0002", pub: PUB2, ts: NOW }), key2);
  assert.equal((await post(env, "/api/v1/heartbeat", other)).status, 409);
});

test("rate limits: per key, per IP, new registrations per IP, capacity", async () => {
  NOW = 1790000000;
  let env = mkEnv();
  const codes = [];
  for (let i = 0; i < 10; i++) { NOW += 1; codes.push((await beat(env, { ts: NOW })).status); }
  assert.deepEqual(codes.slice(0, 8), [200, 200, 200, 200, 200, 200, 200, 200]);
  assert.deepEqual(codes.slice(8), [429, 429]);          // 8 per id per 5 minutes
  env = mkEnv();
  let last = 0;
  for (let i = 0; i < 70; i++) { last = (await post(env, "/api/v1/heartbeat", { junk: i }, "9.9.9.9")).status; }
  assert.equal(last, 429);                                // 60 requests per minute per address
  // capacity (MAX_SERVERS=3) and 5 new registrations per address per day
  env = mkEnv();
  NOW = 1790100000;
  const mk = async (n, ip) => {
    const k = await crypto.subtle.importKey("pkcs8", Buffer.concat([Buffer.from("302e020100300506032b657004220420", "hex"), Buffer.from(new Uint8Array(32).fill(n))]), { name: "Ed25519" }, true, ["sign"]);
    const pub = Buffer.from((await crypto.subtle.exportKey("jwk", k)).x, "base64url").toString("hex");
    return post(env, "/api/v1/heartbeat", await signed(base({ id: `srv-num-000${n}`, pub, ts: NOW, url: `https://s${n}.example.org/` }), k), ip);
  };
  assert.equal((await mk(1, "3.3.3.3")).status, 200);
  assert.equal((await mk(2, "3.3.3.4")).status, 200);
  assert.equal((await mk(3, "3.3.3.5")).status, 200);
  assert.equal((await mk(4, "3.3.3.6")).status, 503);
});

test("signed delete by the owner only", async () => {
  NOW = 1790000000;
  const env = mkEnv();
  await beat(env);
  NOW += 60;
  const del = (o, k) => signed({ action: "delete", id: "srv-one-0001", pub: PUB, ts: NOW, ...o }, k);
  assert.equal((await post(env, "/api/v1/heartbeat", await del({}, key2))).status, 401);
  assert.equal((await post(env, "/api/v1/heartbeat", await del({}, key))).status, 200);
  assert.equal((await (await get(env, "/api/v1/servers")).json()).count, 0);
  NOW += 60;
  assert.equal((await post(env, "/api/v1/heartbeat", await del({ ts: NOW }, key))).status, 404);
});

test("a heartbeat captured before a delete cannot be replayed after it", async () => {
  NOW = 1790000000;
  const env = mkEnv();
  await beat(env);
  const captured = await signed({ action: "heartbeat", id: "srv-one-0001", pub: PUB, ts: NOW + 30, name: "n", url: "https://one.example.org:8073/", loc: "", freq: [], radio: "", cap: 0, users: 0, ver: "3.0", contact: "", desc: "", reach: 0, interval: 60 }, key);
  NOW += 60;
  const del = await signed({ action: "delete", id: "srv-one-0001", pub: PUB, ts: NOW }, key);
  assert.equal((await post(env, "/api/v1/heartbeat", del)).status, 200);
  NOW += 30;
  const r = await post(env, "/api/v1/heartbeat", captured);
  assert.equal(r.status, 409);
  assert.equal((await (await get(env, "/api/v1/servers")).json()).count, 0);
});

test("reachability is opt-in and only boolean", async () => {
  NOW = 1790000000;
  const env = mkEnv();
  let probed = 0;
  const d = deps({ probeReachable: async () => { probed++; return true; } });
  await beat(env, { reach: 0 }, key, "1.1.1.1", d);
  assert.equal(probed, 0);
  assert.equal((await (await get(env, "/api/v1/servers")).json()).servers[0].reachable, null);
  NOW += 60;
  await beat(env, { reach: 1, ts: NOW }, key, "1.1.1.1", d);
  assert.equal(probed, 1);
  assert.equal((await (await get(env, "/api/v1/servers")).json()).servers[0].reachable, true);
  NOW += 60;
  await beat(env, { reach: 1, ts: NOW }, key, "1.1.1.1", d);
  assert.equal(probed, 1);                                // at most once an hour
  NOW += 3600;
  await beat(env, { reach: 1, ts: NOW }, key, "1.1.1.1", deps({ now: () => NOW, probeReachable: async () => false }));
  assert.equal((await (await get(env, "/api/v1/servers")).json()).servers[0].reachable, false);
});

test("list filters through the API", async () => {
  NOW = 1790000000;
  const env = mkEnv();
  await beat(env);
  assert.equal((await (await get(env, "/api/v1/servers?band=fm")).json()).count, 1);
  assert.equal((await (await get(env, "/api/v1/servers?band=hf")).json()).count, 0);
  assert.equal((await (await get(env, "/api/v1/servers?lat=54&lon=25&radius_km=100")).json()).count, 1);
  assert.equal((await (await get(env, "/api/v1/servers?lat=0&lon=0&radius_km=100")).json()).count, 0);
  assert.equal((await (await get(env, "/api/v1/servers?q=hackrf")).json()).count, 1);
  const o = await get(env, "/api/v1/servers", { method: "OPTIONS" });
  assert.equal(o.status, 204);
  assert.equal(o.headers.get("access-control-allow-origin"), "*");
  assert.equal((await get(env, "/index.html")).status, 200);       // non-API paths go to the static assets
});

test("reports: validated, rate limited, stored without addresses", async () => {
  NOW = 1790000000;
  const env = mkEnv();
  await beat(env);
  assert.equal((await post(env, "/api/v1/report", { id: "srv-one-0001", reason: "abuse", text: "bad\nthing" }, "4.4.4.4")).status, 200);
  assert.equal(env.DB.t.reports.length, 1);
  assert.equal(env.DB.t.reports[0].text, "bad thing");
  assert.equal(JSON.stringify(env.DB.t.reports).includes("4.4.4.4"), false);
  assert.equal((await post(env, "/api/v1/report", { id: "srv-one-0001", reason: "nonsense" }, "4.4.4.4")).status, 400);
  assert.equal((await post(env, "/api/v1/report", { id: "nosuchserver1", reason: "abuse" }, "4.4.4.4")).status, 404);
  let last = 0;
  for (let i = 0; i < 8; i++) last = (await post(env, "/api/v1/report", { id: "srv-one-0001", reason: "offline" }, "5.5.5.5")).status;
  assert.equal(last, 429);
});

test("admin: token required, removal, block list, reports", async () => {
  NOW = 1790000000;
  const env = mkEnv();
  await beat(env);
  await post(env, "/api/v1/report", { id: "srv-one-0001", reason: "abuse" }, "6.6.6.6");
  const adm = (method, path, token, body) => handle(new Request("https://dir.test" + path, { method, headers: { "content-type": "application/json", "cf-connecting-ip": "7.7.7.7", ...(token ? { authorization: "Bearer " + token } : {}) }, body: body ? JSON.stringify(body) : undefined }), env, {}, deps());
  assert.equal((await adm("GET", "/api/v1/admin/reports")).status, 401);
  assert.equal((await adm("GET", "/api/v1/admin/reports", "wrong")).status, 401);
  assert.equal((await adm("GET", "/api/v1/admin/reports", "admintoken-for-tests")).status, 200);
  assert.equal((await adm("POST", "/api/v1/admin/block", "admintoken-for-tests", { kind: "bogus", value: "x" })).status, 400);
  // blocking a host removes the server and refuses its next heartbeat
  const b = await adm("POST", "/api/v1/admin/block", "admintoken-for-tests", { kind: "host", value: "example.org", note: "abuse" });
  assert.equal((await b.json()).removed, 1);
  NOW += 60;
  assert.equal((await beat(env, { ts: NOW })).status, 403);
  assert.equal((await adm("DELETE", "/api/v1/admin/block?kind=host&value=example.org", "admintoken-for-tests")).status, 200);
  assert.equal((await beat(env, { ts: NOW + 1 })).status, 200);
  const rm = await adm("DELETE", "/api/v1/admin/servers/srv-one-0001", "admintoken-for-tests");
  assert.equal((await rm.json()).deleted, 1);
  // without a secret the admin API does not exist
  const bare = mkEnv();
  delete bare.ADMIN_TOKEN;
  const r = await handle(new Request("https://dir.test/api/v1/admin/reports", { headers: { authorization: "Bearer x" } }), bare, {}, deps());
  assert.equal(r.status, 404);
});

test("rate-limit keys are not a fixed hash of the address when IP_SALT is missing", async () => {
  const env = mkEnv();
  delete env.IP_SALT;
  await post(env, "/api/v1/heartbeat", { not: "a record" }, "9.9.9.9");
  const keys = [...env.DB.t.rl.keys()];
  assert.ok(keys.length > 0);
  const { sha256Hex } = await import("../src/crypto.js");
  const fixed = (await sha256Hex("unsalted|9.9.9.9")).slice(0, 24);
  assert.ok(keys.every((k) => !k.endsWith(fixed)), "key must not be sha256('unsalted|ip')");
});
