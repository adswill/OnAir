// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The Worker routes against real Room / Lobby objects on a fake namespace.
import { test } from "node:test";
import assert from "node:assert/strict";
import worker from "../src/index.js";
import { Room } from "../src/room.js";
import { Lobby } from "../src/lobby.js";

function ns(Cls, env) {
  const objs = new Map();
  return {
    idFromName: (n) => n,
    get: (id) => {
      if (!objs.has(id)) {
        const store = new Map();
        const ctx = { storage: { get: async (k) => store.get(k), put: async (k, v) => store.set(k, v), setAlarm: async () => {}, deleteAlarm: async () => {}, deleteAll: async () => store.clear() },
          blockConcurrencyWhile: (f) => f(), getWebSockets: () => [], acceptWebSocket() {} };
        objs.set(id, new Cls(ctx, env));
      }
      const o = objs.get(id);
      return { fetch: (u, init) => o.fetch(u instanceof Request ? u : new Request(u, init)) };
    },
  };
}
function mkEnv() {
  const env = { ROOM_SECRET: "test-secret-0123456789abcdef", ASSETS: { fetch: async (r) => new Response("asset " + new URL(r.url).pathname) } };
  env.ROOM = ns(Room, env); env.LOBBY = ns(Lobby, env);
  return env;
}
const call = (env, path, init) => worker.fetch(new Request("https://rooms.test" + path, init), env, { waitUntil() {} });

test("create needs consent, push needs the host token (header or body), tokens are not accepted for other rooms", async () => {
  const env = mkEnv();
  assert.equal((await call(env, "/api/create", { method: "POST", body: JSON.stringify({ title: "x" }) })).status, 400);
  const c = await (await call(env, "/api/create", { method: "POST", body: JSON.stringify({ consent: true, title: "My room", mode: "ADS-B", freq: 1090, transcript: false }) })).json();
  assert.match(c.id, /^[a-z0-9]{10}$/);
  assert.equal((await call(env, `/api/r/${c.id}/push`, { method: "POST", body: "{}" })).status, 401);
  assert.equal((await call(env, `/api/r/${c.id}/push`, { method: "POST", body: JSON.stringify({ tok: c.hostToken }) })).status, 200);
  assert.equal((await call(env, `/api/r/${c.id}/push`, { method: "POST", headers: { Authorization: "Bearer " + c.hostToken }, body: "{}" })).status, 200);
  const c2 = await (await call(env, "/api/create", { method: "POST", body: JSON.stringify({ consent: true }) })).json();
  assert.equal((await call(env, `/api/r/${c2.id}/push`, { method: "POST", body: JSON.stringify({ tok: c.hostToken }) })).status, 401);
  assert.equal((await call(env, `/api/r/${c.id}/info`)).status, 200);
});
test("host PIN login gives a host token; a wrong PIN does not", async () => {
  const env = mkEnv();
  const c = await (await call(env, "/api/create", { method: "POST", body: JSON.stringify({ consent: true }) })).json();
  assert.equal((await call(env, `/api/r/${c.id}/hostlogin`, { method: "POST", body: JSON.stringify({ pin: "nope" }) })).status, 403);
  const ok = await call(env, `/api/r/${c.id}/hostlogin`, { method: "POST", body: JSON.stringify({ pin: c.pin }) });
  assert.equal(ok.status, 200);
  const t = (await ok.json()).hostToken;
  assert.equal((await call(env, `/api/r/${c.id}/hostview`, { headers: { Authorization: "Bearer " + t } })).status, 200);
  assert.equal((await call(env, `/api/r/${c.id}/hostview`)).status, 401);
});
test("a visitor page path serves the room page; without ROOM_SECRET the worker refuses", async () => {
  const env = mkEnv();
  assert.match(await (await call(env, "/r/abcdefghjk")).text(), /room\.html/);
  assert.equal((await call({ ...env, ROOM_SECRET: "" }, "/api/rooms")).status, 503);
});
