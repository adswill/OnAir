// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// lib.js - pure helpers for Live rooms: signed tokens, input cleaning, rate limits, light moderation. Tested in test/lib.test.js.
// Everything a visitor or host sends is cut and cleaned here; the pages show text with textContent only.

export const LIMITS = {
  viewersPerRoom: 50,
  liveRooms: 200,
  createPerIpPerHour: 3,
  hostTokenTtlMs: 12 * 3600e3,   // a room never lives longer than this
  idleExpireMs: 10 * 60e3,       // no host push for this long: the room ends
  wfBins: 512,
  wfLinesPerPush: 4,
  wfBackfill: 60,                // lines kept for a viewer who joins late
  textsPerPush: 20,
  textMax: 300,
  chatMax: 200,
  chatGapMs: 2000,               // one chat line per viewer per 2 s
  roomChatPerSec: 10,
  chatKeep: 100,
  reportGapMs: 30000,
  reportsKeep: 20,
  pushBodyMax: 16 * 1024,
};

const enc = new TextEncoder();

export function b64uEncode(bytes) {
  let s = "";
  for (const b of bytes) s += String.fromCharCode(b);
  return btoa(s).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "");
}
export function b64uDecode(str) {
  const s = String(str).replace(/-/g, "+").replace(/_/g, "/");
  const bin = atob(s + "=".repeat((4 - (s.length % 4)) % 4));
  const out = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i);
  return out;
}

async function hmacKey(secret) {
  return crypto.subtle.importKey("raw", enc.encode(String(secret)), { name: "HMAC", hash: "SHA-256" }, false, ["sign", "verify"]);
}

/** token = base64url(JSON payload) + "." + base64url(HMAC-SHA256). Payload: {k: kind, r: roomId, v?: viewerId, e: expiry ms}. */
export async function makeToken(secret, payload) {
  if (!secret || String(secret).length < 16) throw new Error("ROOM_SECRET missing or too short");
  const p = b64uEncode(enc.encode(JSON.stringify(payload)));
  const sig = new Uint8Array(await crypto.subtle.sign("HMAC", await hmacKey(secret), enc.encode(p)));
  return p + "." + b64uEncode(sig);
}

/** The payload when the signature and the expiry are good, else null. */
export async function verifyToken(secret, token, now = Date.now()) {
  try {
    if (!secret || typeof token !== "string" || token.length > 600) return null;
    const [p, s] = token.split(".");
    if (!p || !s) return null;
    const ok = await crypto.subtle.verify("HMAC", await hmacKey(secret), b64uDecode(s), enc.encode(p));
    if (!ok) return null;
    const obj = JSON.parse(new TextDecoder().decode(b64uDecode(p)));
    if (!obj || typeof obj.e !== "number" || obj.e < now) return null;
    return obj;
  } catch { return null; }
}

export async function hashIp(secret, ip) {
  const d = new Uint8Array(await crypto.subtle.sign("HMAC", await hmacKey(secret), enc.encode("ip|" + ip)));
  return b64uEncode(d.slice(0, 12));
}

export const ID_CHARS = "abcdefghjkmnpqrstuvwxyz23456789";   // no look-alikes
export function randomId(n = 10) {
  const b = crypto.getRandomValues(new Uint8Array(n));
  let s = "";
  for (const x of b) s += ID_CHARS[x % ID_CHARS.length];
  return s;
}
export const validRoomId = (s) => typeof s === "string" && /^[a-hj-km-np-z2-9]{10}$/.test(s);

// ---- text cleaning --------------------------------------------------------------------------------------------------
function stripCtl(x) {
  let o = "";
  for (let i = 0; i < x.length; i++) {
    const c = x.charCodeAt(i);
    const bad = c < 32 || (c >= 127 && c <= 159) || (c >= 0x200b && c <= 0x200f) || (c >= 0x2028 && c <= 0x202e)
      || (c >= 0x2066 && c <= 0x2069) || c === 0xfeff || c === 0xfffe || c === 0xffff;
    o += bad ? " " : x[i];
  }
  return o;
}
export function cleanText(s, max) {
  const x = stripCtl(String(s == null ? "" : s)).replace(/\s+/g, " ").trim();
  return Array.from(x).slice(0, max).join("");
}
/** Phone numbers and e-mail addresses out (the same rule as the map's ACARS messages). */
export function scrubPersonal(s) {
  return String(s)
    .replace(/[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}/g, "[email]")
    .replace(/\+?\d[\d ().-]{7,}\d/g, "[number]");
}
export function cleanNick(s) {
  const x = cleanText(s, 20).replace(/[^\p{L}\p{N} _-]/gu, "").trim();
  return x || "guest";
}
export const cleanTitle = (s) => cleanText(s, 60) || "Untitled room";
export const cleanMode = (s) => cleanText(s, 24);
export function cleanFreqMhz(v) {
  const n = Number(v);
  return Number.isFinite(n) && n > 0 && n < 1e6 ? Math.round(n * 1e6) / 1e6 : 0;
}

const BAD = ["fuck", "shit", "cunt", "nigger", "nigga", "faggot", "bitch", "asshole", "retard"];
const LEET = { 0: "o", 1: "i", 3: "e", 4: "a", 5: "s", 7: "t", "@": "a", $: "s" };
/** Profanity-light: a short word list is starred out; links are removed. Not a substitute for the host's mute / ban. */
export function moderate(s) {
  let out = String(s).replace(/(https?:\/\/|www\.)\S+/gi, "[link]");
  const norm = Array.from(out).map((c) => LEET[c] ?? c).join("").toLowerCase();
  const chars = Array.from(out);
  for (const w of BAD) {
    let i = norm.indexOf(w);
    while (i >= 0) {
      for (let k = 0; k < w.length; k++) chars[i + k] = "*";
      i = norm.indexOf(w, i + w.length);
    }
  }
  return chars.join("");
}
export const cleanChat = (s) => moderate(scrubPersonal(cleanText(s, LIMITS.chatMax)));
export const cleanHostText = (s) => scrubPersonal(cleanText(s, LIMITS.textMax));

// ---- waterfall lines ------------------------------------------------------------------------------------------------
/** The host sends each line as base64 of exactly 512 bytes (one byte per bin). Returns a Uint8Array, or null. */
export function parseWfLine(b64) {
  try {
    if (typeof b64 !== "string" || b64.length > 800) return null;
    const b = b64uDecode(b64.replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, ""));
    return b.length === LIMITS.wfBins ? b : null;
  } catch { return null; }
}

// ---- rate limits ----------------------------------------------------------------------------------------------------
/** At most `perWindow` events per `windowMs`. */
export class WindowLimit {
  constructor(perWindow, windowMs) { this.n = perWindow; this.w = windowMs; this.t = []; }
  allow(now = Date.now()) {
    while (this.t.length && this.t[0] <= now - this.w) this.t.shift();
    if (this.t.length >= this.n) return false;
    this.t.push(now);
    return true;
  }
}

/** Parse a host push body. Returns a cleaned object; unknown fields are dropped. */
export function parsePush(body, transcriptAllowed) {
  const o = body && typeof body === "object" ? body : {};
  const lines = [];
  if (Array.isArray(o.wf)) for (const l of o.wf.slice(0, LIMITS.wfLinesPerPush)) { const p = parseWfLine(l); if (p) lines.push(p); }
  const texts = [];
  if (transcriptAllowed && Array.isArray(o.msgs))
    for (const m of o.msgs.slice(0, LIMITS.textsPerPush)) { const t = cleanHostText(m); if (t) texts.push(t); }
  return {
    lines, texts,
    title: o.title == null ? null : cleanTitle(o.title),
    mode: o.mode == null ? null : cleanMode(o.mode),
    freq: o.freq == null ? null : cleanFreqMhz(o.freq),
    chatSeen: Number.isInteger(o.cs) && o.cs >= 0 ? o.cs : 0,
  };
}
