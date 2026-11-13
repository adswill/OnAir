// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// room.js - one Durable Object per room. The host POSTs batched pushes (about one every 2 s); viewers hold hibernating WebSockets.
// Cost notes are in docs/live-rooms.md: outgoing WebSocket messages are not billed per message, incoming ones count 20:1.
import { LIMITS, WindowLimit, makeToken, verifyToken, randomId, cleanNick, cleanChat, cleanTitle, cleanMode, cleanFreqMhz, parsePush, hashIp } from "./lib.js";

const ALARM_MS = 30_000;
const json = (o, status = 200) => new Response(JSON.stringify(o), { status, headers: { "Content-Type": "application/json; charset=utf-8", "Cache-Control": "no-store" } });

export class Room {
  constructor(ctx, env) {
    this.ctx = ctx;
    this.env = env;
    this.meta = null;                 // {id, title, mode, freq, transcript, createdAt, lastPush, ended}
    this.mod = { banned: [], bannedIp: [], muted: [] };   // viewer ids / ip hashes; stored (changes are rare)
    this.wf = [];                     // last wfBackfill lines (memory only)
    this.texts = [];                  // last decoded texts
    this.chat = [];                   // {seq, vid, nick, text}
    this.chatSeq = 0;
    this.reports = [];
    this.chatBucket = new WindowLimit(LIMITS.roomChatPerSec, 1000);
    this.viewerGap = new Map();       // viewer id -> last chat ms
    this.reportGap = new Map();
    this.lastV = 0;
    this.pinTries = new WindowLimit(5, 60_000);
    ctx.blockConcurrencyWhile(async () => {
      this.meta = (await ctx.storage.get("meta")) || null;
      this.mod = (await ctx.storage.get("mod")) || this.mod;
    });
  }

  async fetch(request) {
    const url = new URL(request.url);
    const p = url.pathname;
    if (p === "/init" && request.method === "POST") return this.init(await request.json());
    if (!this.meta || this.meta.ended) return json({ error: "gone" }, 404);
    if (p === "/ws") return this.openSocket(request);
    if (p === "/hostview") return json({ viewers: this.ctx.getWebSockets().length, chat: this.chat.slice(-LIMITS.chatKeep).map(pubChat), reports: this.reports });
    if (p === "/hostcheck" && request.method === "POST") {   // the host PIN shown in the app; five tries a minute
      if (!this.pinTries.allow()) return json({ error: "slow" }, 429);
      const b = await request.json().catch(() => ({}));
      const h = await hashIp(this.env.ROOM_SECRET, "pin|" + this.meta.id + "|" + String(b.pin || "").slice(0, 16));
      return json({ ok: !!this.meta.pinHash && h === this.meta.pinHash });
    }
    if (p === "/info") return json(this.publicMeta());
    if (request.method !== "POST") return json({ error: "method" }, 405);
    if (p === "/push") return this.push(request);
    if (p === "/mod") return this.moderate(request);
    if (p === "/end") { await this.end("host ended"); return json({ ok: true }); }
    return json({ error: "not found" }, 404);
  }

  publicMeta() {
    const m = this.meta;
    return { id: m.id, title: m.title, mode: m.mode, freq: m.freq, transcript: m.transcript, viewers: this.ctx.getWebSockets().length, bins: LIMITS.wfBins };
  }

  async init({ id, title, mode, freq, transcript, pinHash }) {
    if (this.meta) return json({ error: "exists" }, 409);
    const now = Date.now();
    this.meta = { id, title: cleanTitle(title), mode: cleanMode(mode), freq: cleanFreqMhz(freq), transcript: !!transcript, pinHash: String(pinHash || ""), createdAt: now, lastPush: now, ended: false };
    await this.ctx.storage.put("meta", this.meta);
    await this.ctx.storage.setAlarm(now + ALARM_MS);
    return json({ ok: true });
  }

  // ---- host side ----
  async push(request) {
    const raw = await request.text();
    if (raw.length > LIMITS.pushBodyMax) return json({ error: "too large" }, 413);
    let body; try { body = JSON.parse(raw); } catch { return json({ error: "bad json" }, 400); }
    const m = this.meta, now = Date.now();
    const q = parsePush(body, m.transcript);
    m.lastPush = now;
    let metaChanged = false;
    if (q.title && q.title !== m.title) { m.title = q.title; metaChanged = true; }
    if (q.mode != null && q.mode !== m.mode) { m.mode = q.mode; metaChanged = true; }
    if (q.freq && q.freq !== m.freq) { m.freq = q.freq; metaChanged = true; }
    if (metaChanged) await this.ctx.storage.put("meta", m);
    if (q.lines.length) {
      for (const l of q.lines) this.wf.push(l);
      if (this.wf.length > LIMITS.wfBackfill) this.wf.splice(0, this.wf.length - LIMITS.wfBackfill);
      this.broadcast(wfFrame(q.lines));
    }
    if (q.texts.length) {
      this.texts.push(...q.texts);
      if (this.texts.length > 30) this.texts.splice(0, this.texts.length - 30);
      this.broadcast(JSON.stringify({ t: "texts", items: q.texts }));
    }
    if (metaChanged) this.broadcast(JSON.stringify({ t: "meta", ...this.publicMeta() }));
    return json({
      ok: true,
      viewers: this.ctx.getWebSockets().length,
    });
  }

  async moderate(request) {
    let b; try { b = await request.json(); } catch { return json({ error: "bad json" }, 400); }
    const vid = typeof b.vid === "string" ? b.vid.slice(0, 20) : "";
    const act = b.action;
    if (!vid || !["mute", "unmute", "kick", "ban"].includes(act)) return json({ error: "bad request" }, 400);
    const targets = this.ctx.getWebSockets().filter((w) => (w.deserializeAttachment() || {}).id === vid);
    if (act === "mute") { if (!this.mod.muted.includes(vid)) this.mod.muted.push(vid); }
    if (act === "unmute") this.mod.muted = this.mod.muted.filter((x) => x !== vid);
    if (act === "ban") {
      if (!this.mod.banned.includes(vid)) this.mod.banned.push(vid);
      for (const w of targets) { const ip = (w.deserializeAttachment() || {}).ip; if (ip && !this.mod.bannedIp.includes(ip)) this.mod.bannedIp.push(ip); }
    }
    this.mod.muted = this.mod.muted.slice(-500); this.mod.banned = this.mod.banned.slice(-500); this.mod.bannedIp = this.mod.bannedIp.slice(-500);
    await this.ctx.storage.put("mod", this.mod);
    if (act === "kick" || act === "ban") for (const w of targets) { try { w.send(JSON.stringify({ t: "kicked" })); w.close(4003, "removed"); } catch {} }
    if (act === "mute") for (const w of targets) { try { w.send(JSON.stringify({ t: "muted" })); } catch {} }
    return json({ ok: true, n: targets.length });
  }

  // ---- viewer side ----
  async openSocket(request) {
    if (request.headers.get("Upgrade") !== "websocket") return json({ error: "upgrade" }, 426);
    if (this.ctx.getWebSockets().length >= LIMITS.viewersPerRoom) return json({ error: "room full" }, 503);
    const ip = await hashIp(this.env.ROOM_SECRET, request.headers.get("CF-Connecting-IP") || "?");
    if (this.mod.bannedIp.includes(ip)) return json({ error: "banned" }, 403);
    const pair = new WebSocketPair();
    this.ctx.acceptWebSocket(pair[1]);
    pair[1].serializeAttachment({ ip, t: Date.now(), id: null });
    return new Response(null, { status: 101, webSocket: pair[0] });
  }

  async webSocketMessage(ws, message) {
    if (typeof message !== "string" || message.length > 1024) { ws.close(1009, "too big"); return; }
    let m; try { m = JSON.parse(message); } catch { return; }
    const att = ws.deserializeAttachment() || {};
    const now = Date.now();
    if (m.t === "hello") {
      if (att.id) return;
      let vid = null;
      const tok = await verifyToken(this.env.ROOM_SECRET, m.vt, now);
      if (tok && tok.k === "viewer" && tok.r === this.meta.id && typeof tok.v === "string") vid = tok.v;
      if (!vid) vid = randomId(8);
      if (this.mod.banned.includes(vid)) { ws.send(JSON.stringify({ t: "kicked" })); ws.close(4003, "banned"); return; }
      const nick = cleanNick(m.nick);
      ws.serializeAttachment({ ...att, id: vid, nick });
      const vt = await makeToken(this.env.ROOM_SECRET, { k: "viewer", r: this.meta.id, v: vid, e: now + LIMITS.hostTokenTtlMs });
      ws.send(JSON.stringify({ t: "welcome", vt, id: vid, nick, muted: this.mod.muted.includes(vid), meta: this.publicMeta(), chat: this.chat.slice(-30).map(pubChat), texts: this.texts }));
      if (this.wf.length) ws.send(wfFrame(this.wf));
      if (now - this.lastV > 5000) { this.lastV = now; this.broadcast(JSON.stringify({ t: "v", n: this.ctx.getWebSockets().length })); }
      return;
    }
    if (!att.id) return;   // not yet said hello
    if (m.t === "nick") { ws.serializeAttachment({ ...att, nick: cleanNick(m.nick) }); return; }
    if (m.t === "chat") {
      if (this.mod.muted.includes(att.id)) { ws.send(JSON.stringify({ t: "muted" })); return; }
      const last = this.viewerGap.get(att.id) || 0;
      if (now - last < LIMITS.chatGapMs) { ws.send(JSON.stringify({ t: "slow" })); return; }
      if (!this.chatBucket.allow(now)) { ws.send(JSON.stringify({ t: "slow" })); return; }
      const text = cleanChat(m.text);
      if (!text) return;
      this.viewerGap.set(att.id, now);
      if (this.viewerGap.size > 400) this.viewerGap.clear();
      const c = { seq: ++this.chatSeq, vid: att.id, nick: att.nick, text };
      this.chat.push(c);
      if (this.chat.length > LIMITS.chatKeep) this.chat.shift();
      this.broadcast(JSON.stringify({ t: "chat", ...pubChat(c) }));
    } else if (m.t === "report") {
      const last = this.reportGap.get(att.id) || 0;
      if (now - last < LIMITS.reportGapMs) return;
      this.reportGap.set(att.id, now);
      if (this.reportGap.size > 400) this.reportGap.clear();
      const c = this.chat.find((x) => x.seq === m.seq);
      if (c && this.reports.length < LIMITS.reportsKeep) this.reports.push({ seq: c.seq, vid: c.vid, nick: c.nick, text: c.text, by: att.id });
      ws.send(JSON.stringify({ t: "reported" }));
    }
  }

  webSocketClose(ws) { try { ws.close(); } catch {} }
  webSocketError() {}

  broadcast(data) {
    for (const w of this.ctx.getWebSockets()) {
      if (!(w.deserializeAttachment() || {}).id) continue;
      try { w.send(data); } catch {}
    }
  }

  async alarm() {
    if (!this.meta || this.meta.ended) return;
    const now = Date.now();
    if (now - this.meta.lastPush > LIMITS.idleExpireMs) return this.end("host stopped");
    if (now - this.meta.createdAt > LIMITS.hostTokenTtlMs) return this.end("time limit");
    for (const w of this.ctx.getWebSockets()) {   // sockets that never said hello
      const a = w.deserializeAttachment() || {};
      if (!a.id && now - (a.t || 0) > 15_000) { try { w.close(4000, "no hello"); } catch {} }
    }
    this.broadcast(JSON.stringify({ t: "v", n: this.ctx.getWebSockets().length }));
    try {   // heartbeat for the room list (one small request a minute)
      await this.env.LOBBY.get(this.env.LOBBY.idFromName("main")).fetch("https://lobby/beat", { method: "POST", body: JSON.stringify(this.publicMeta()) });
    } catch {}
    await this.ctx.storage.setAlarm(now + ALARM_MS);
  }

  async end(why) {
    if (!this.meta || this.meta.ended) return;
    this.meta.ended = true;
    this.broadcast(JSON.stringify({ t: "end", why }));
    for (const w of this.ctx.getWebSockets()) { try { w.close(1000, "ended"); } catch {} }
    try { await this.env.LOBBY.get(this.env.LOBBY.idFromName("main")).fetch("https://lobby/gone", { method: "POST", body: JSON.stringify({ id: this.meta.id }) }); } catch {}
    await this.ctx.storage.deleteAlarm();
    await this.ctx.storage.deleteAll();   // nothing is kept after a room ends
    this.meta = { ...this.meta, ended: true };
  }
}

const pubChat = (c) => ({ seq: c.seq, id: c.vid, nick: c.nick, text: c.text });

/** Binary frame: byte 1, byte n (lines), then n * 512 bytes. */
export function wfFrame(lines) {
  const n = Math.min(255, lines.length);
  const out = new Uint8Array(2 + n * LIMITS.wfBins);
  out[0] = 1; out[1] = n;
  for (let i = 0; i < n; i++) out.set(lines[i], 2 + i * LIMITS.wfBins);
  return out.buffer;
}
