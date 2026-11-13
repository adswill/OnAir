// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// OnAir relay on Cloudflare Workers (docs/ONAIR-WEB-RELAY.md protocol v1, docs/CF-RELAY-3.8.md).
//   relay.on-air.dev/relay/v1/host     the desktop app's host link (WebSocket) -> a new Room Durable Object
//   relay.on-air.dev/relay/v1/status   {"v":1,"ok":true}
//   relay.on-air.dev/api/v1/servers    the opt-in public list
//   <label>-r.on-air.dev/...           a shared radio: the Room that holds the label's host link
import { OFFLINE } from './lib.js';
export { Room } from './room.js';
export { Registry } from './registry.js';

const json = (o, extra) => new Response(JSON.stringify(o), { headers: { 'content-type': 'application/json', 'cache-control': 'no-store', ...(extra || {}) } });
const text = (status, body, extra) => new Response(body, { status, headers: { 'content-type': 'text/plain', 'cache-control': 'no-store', ...(extra || {}) } });
const offline = () => new Response(OFFLINE, { status: 503, headers: { 'content-type': 'text/html; charset=utf-8', 'cache-control': 'no-store', 'retry-after': '30' } });

// Per isolate, a few seconds: which room holds an address, and the address bans. Saves a registry call per page asset.
const ptr = new Map(); let bans = { t: 0, set: new Set() };
const PTR_MS = 10000, BANS_MS = 30000;

function timingSafeEq(a, b) {
  if (typeof a !== 'string' || typeof b !== 'string' || a.length !== b.length || !a.length) return false;
  let d = 0; for (let i = 0; i < a.length; i++) d |= a.charCodeAt(i) ^ b.charCodeAt(i); return d === 0;
}

export default {
  async fetch(request, env, ctx) {
    const url = new URL(request.url);
    const dev = env.RELAY_DEV_LOCAL === '1';
    // the real listener address: Cloudflare sets CF-Connecting-IP and a browser cannot override it at the edge
    const ip = (request.headers.get('cf-connecting-ip') || (dev ? '127.0.0.1' : '')).slice(0, 64) || '0.0.0.0';
    const hostName = ((dev && request.headers.get('host')) || url.host).toLowerCase();
    const bare = hostName.replace(/:\d+$/, '');
    const main = (env.RELAY_MAIN_HOST || 'relay.on-air.dev').toLowerCase();
    const reg = env.REGISTRY.get(env.REGISTRY.idFromName('global'));

    if (bare === main) {
      const p = url.pathname;
      if (p === '/relay/v1/host') {
        if ((request.headers.get('upgrade') || '').toLowerCase() !== 'websocket') return text(400, 'WebSocket upgrade required\n');
        const g = await reg.gate(ip);
        if (!g.ok) return text(g.status, g.message + '\n', g.status === 429 ? { 'retry-after': '30' } : {});
        const room = env.ROOM.get(env.ROOM.newUniqueId());
        const h = new Headers(request.headers); h.set('x-relay-kind', 'host'); h.set('x-relay-ip', ip);
        return room.fetch(new Request(request, { headers: h }));
      }
      if (p === '/relay/v1/status') return json({ v: 1, ok: true });
      if (p === '/api/v1/servers') return json({ v: 1, servers: await reg.list() }, { 'access-control-allow-origin': '*' });
      if (p.startsWith('/relay/v1/admin/') && request.method === 'POST') {
        const t = /^bearer\s+(\S+)$/i.exec(request.headers.get('authorization') || '');
        if (!env.ADMIN_TOKEN || env.ADMIN_TOKEN.length < 24 || !t || !timingSafeEq(t[1], env.ADMIN_TOKEN)) return text(403, 'no\n');
        let j = {}; try { j = await request.json(); } catch { /* empty */ }
        const on = !p.endsWith('/unban');
        if (typeof j.id === 'string') await reg.ban('id', j.id.slice(0, 64), on);
        if (typeof j.ip === 'string') await reg.ban('ip', j.ip.slice(0, 64), on);
        bans.t = 0;
        return json({ ok: true });
      }
      return text(200, 'OnAir relay\n');
    }

    const suffix = (env.RELAY_LABEL_SUFFIX || '-r.on-air.dev').toLowerCase();
    if (!bare.endsWith(suffix)) return text(404, 'unknown host\n');
    const label = bare.slice(0, -suffix.length);
    if (!/^[a-z2-7]{10}$/.test(label)) return text(404, 'unknown host\n');

    const now = Date.now();
    if (now - bans.t > BANS_MS) { try { bans = { t: now, set: new Set(await reg.ipBans()) }; } catch { /* keep the old list */ } }
    if (bans.set.has(ip)) return text(403, 'banned\n');

    const h = new Headers(request.headers);
    h.set('x-relay-ip', ip); h.set('x-relay-kind', 'browser'); h.set('x-relay-host', hostName);
    const canRetry = request.method === 'GET' || request.method === 'HEAD';
    for (let attempt = 0; attempt < 2; attempt++) {
      let p = ptr.get(label);
      if (!p || now - p.t > PTR_MS || attempt > 0) { p = { t: Date.now(), room: await reg.lookup(label) }; ptr.set(label, p); if (ptr.size > 5000) ptr.clear(); }
      if (!p.room) return offline();
      const res = await env.ROOM.get(env.ROOM.idFromString(p.room)).fetch(new Request(request, { headers: h }));
      if (res.headers.get('x-relay-gone') === '1' && canRetry && attempt === 0) { ptr.delete(label); continue; }   // the cached room lost its link: ask again once
      if (res.headers.get('x-relay-gone') === '1') { const r = new Response(res.body, res); r.headers.delete('x-relay-gone'); return r; }
      return res;
    }
    return offline();
  },
};
