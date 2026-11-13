// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// lobby.js - one small Durable Object: the list of live rooms and the room-creation limits. Memory plus one storage key.
import { LIMITS, WindowLimit } from "./lib.js";

const STALE_MS = 3 * 60_000;
const json = (o, status = 200) => new Response(JSON.stringify(o), { status, headers: { "Content-Type": "application/json; charset=utf-8", "Cache-Control": "no-store" } });

export class Lobby {
  constructor(ctx, env) {
    this.ctx = ctx;
    this.rooms = new Map();        // id -> {id,title,mode,freq,viewers,transcript,t}
    this.createLimit = new Map();  // ip hash -> WindowLimit
  }
  prune(now) { for (const [id, r] of this.rooms) if (now - r.t > STALE_MS) this.rooms.delete(id); }

  async fetch(request) {
    const p = new URL(request.url).pathname, now = Date.now();
    this.prune(now);
    if (p === "/list") {
      const list = [...this.rooms.values()].sort((a, b) => b.viewers - a.viewers).slice(0, 100)
        .map(({ id, title, mode, freq, viewers, transcript }) => ({ id, title, mode, freq, viewers, transcript }));
      return json({ rooms: list });
    }
    if (p === "/reserve" && request.method === "POST") {   // body: {ip}. Counts the room at once so a burst cannot pass the cap
      const { ip, id, title, mode, freq, transcript } = await request.json();
      let lim = this.createLimit.get(ip);
      if (!lim) { lim = new WindowLimit(LIMITS.createPerIpPerHour, 3600e3); this.createLimit.set(ip, lim); if (this.createLimit.size > 2000) this.createLimit.clear(); }
      if (this.rooms.size >= LIMITS.liveRooms) return json({ error: "full" }, 503);
      if (!lim.allow(now)) return json({ error: "rate" }, 429);
      this.rooms.set(id, { id, title, mode, freq, viewers: 0, transcript: !!transcript, t: now });
      return json({ ok: true });
    }
    if (p === "/beat" && request.method === "POST") {
      const m = await request.json();
      if (this.rooms.has(m.id)) this.rooms.set(m.id, { id: m.id, title: m.title, mode: m.mode, freq: m.freq, viewers: m.viewers | 0, transcript: !!m.transcript, t: now });
      return json({ ok: true });
    }
    if (p === "/gone" && request.method === "POST") { this.rooms.delete((await request.json()).id); return json({ ok: true }); }
    return json({ error: "not found" }, 404);
  }
}
