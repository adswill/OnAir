// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// onair-rooms Worker: routes, token checks, edge cache for the room list. Not deployed (see docs/live-rooms.md).
import { LIMITS, makeToken, verifyToken, randomId, ID_CHARS, validRoomId, cleanTitle, cleanMode, cleanFreqMhz, hashIp } from "./lib.js";
export { Room } from "./room.js";
export { Lobby } from "./lobby.js";

const json = (o, status = 200) => new Response(JSON.stringify(o), { status, headers: { "Content-Type": "application/json; charset=utf-8", "Cache-Control": "no-store" } });
const LIST_CACHE_S = 10;

async function hostAuth(env, t, id) {
  const tok = await verifyToken(env.ROOM_SECRET, t);
  return !!(tok && tok.k === "host" && tok.r === id);
}

export default {
  async fetch(request, env, ctx) {
    const url = new URL(request.url);
    const p = url.pathname;
    if (!env.ROOM_SECRET) return json({ error: "not configured" }, 503);
    const lobby = env.LOBBY.get(env.LOBBY.idFromName("main"));

    if (p === "/api/rooms" && request.method === "GET") {
      const key = new Request(url.origin + "/api/rooms");
      const hit = await caches.default.match(key);
      if (hit) return hit;
      const res = new Response(await (await lobby.fetch("https://lobby/list")).text(), {
        headers: { "Content-Type": "application/json; charset=utf-8", "Cache-Control": `public, max-age=${LIST_CACHE_S}` },
      });
      ctx.waitUntil(caches.default.put(key, res.clone()));
      return res;
    }

    if (p === "/api/create" && request.method === "POST") {
      let b; try { b = await request.json(); } catch { return json({ error: "bad json" }, 400); }
      if (b.consent !== true) return json({ error: "consent required" }, 400);   // the app sends this only after the explicit consent text
      const id = randomId(10);
      const room = { id, title: cleanTitle(b.title), mode: cleanMode(b.mode), freq: cleanFreqMhz(b.freq), transcript: b.transcript === true };
      const ip = await hashIp(env.ROOM_SECRET, request.headers.get("CF-Connecting-IP") || "?");
      const r = await lobby.fetch("https://lobby/reserve", { method: "POST", body: JSON.stringify({ ip, ...room }) });
      if (!r.ok) return json(await r.json(), r.status);
      const pin = randomId(8);   // shown in the app; typed on the room page to moderate (never put in a URL)
      const pinHash = await hashIp(env.ROOM_SECRET, "pin|" + id + "|" + pin);
      const stub = env.ROOM.get(env.ROOM.idFromName(id));
      const init = await stub.fetch("https://room/init", { method: "POST", body: JSON.stringify({ ...room, pinHash }) });
      if (!init.ok) return json({ error: "init" }, 500);
      const hostToken = await makeToken(env.ROOM_SECRET, { k: "host", r: id, e: Date.now() + LIMITS.hostTokenTtlMs });
      return json({ id, hostToken, pin, viewUrl: `${url.origin}/r/${id}`, maxViewers: LIMITS.viewersPerRoom });
    }

    let m = p.match(/^\/api\/r\/([a-z0-9]+)\/(ws|info|push|mod|end|hostlogin|hostview)$/);
    if (m) {
      const [, id, action] = m;
      if (!validRoomId(id)) return json({ error: "not found" }, 404);
      if (action === "hostlogin" && request.method === "POST") {
        let b; try { b = await request.json(); } catch { return json({ error: "bad json" }, 400); }
        const r = await env.ROOM.get(env.ROOM.idFromName(id)).fetch("https://room/hostcheck", { method: "POST", body: JSON.stringify({ pin: b.pin }) });
        if (r.status === 429) return json({ error: "slow" }, 429);
        if (!r.ok || !(await r.json()).ok) return json({ error: "wrong pin" }, 403);
        return json({ hostToken: await makeToken(env.ROOM_SECRET, { k: "host", r: id, e: Date.now() + 6 * 3600e3 }) });
      }
      if (action === "ws") {   // the browser's Origin must be this site (no cross-site WebSocket hijacking)
        const o = request.headers.get("Origin");
        if (o && o !== url.origin) return json({ error: "origin" }, 403);
      }
      let forward = request;
      if (action === "push" || action === "mod" || action === "end" || action === "hostview") {
        // The host token travels in the Authorization header (the page) or in the JSON body as "tok" (the app); never in a URL.
        const h = request.headers.get("Authorization") || "";
        let t = h.startsWith("Bearer ") ? h.slice(7) : "";
        let text = null;
        if (request.method === "POST") {
          text = await request.text();
          if (text.length > LIMITS.pushBodyMax) return json({ error: "too large" }, 413);
          if (!t) { try { t = String(JSON.parse(text).tok || ""); } catch {} }
        }
        if (!(await hostAuth(env, t, id))) return json({ error: "unauthorised" }, 401);
        forward = new Request("https://room/" + action, text === null ? { method: request.method } : { method: "POST", body: text });
      } else forward = new Request("https://room/" + action, request);
      return env.ROOM.get(env.ROOM.idFromName(id)).fetch(forward);
    }

    m = p.match(/^\/r\/([a-z0-9]+)$/);
    if (m && validRoomId(m[1])) return env.ASSETS.fetch(new Request(url.origin + "/room.html", request));
    return env.ASSETS.fetch(request);
  },
};
