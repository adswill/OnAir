// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// msgs.js - ACARS messages attached to an aircraft: validation, sanitising, limits. Pure functions (tested in test/msgs.test.js).
// The text is public radio but may hold personal data, so it is cut hard here as well as in the app (the app drops phone numbers and
// e-mail addresses first). The page shows it with textContent only.

export const MSG_TTL_MS = 48 * 3600e3;
export const MSG_PER_TRACK = 20;          // stored per aircraft
export const MSG_TEXT_MAX = 500;
export const MSG_PER_TRACK_PER_INGEST = 5;
export const MSG_PER_INGEST = 60;
export const MSG_STATION_PER_MIN = 120;
export const MSG_TOTAL_MAX = 6000;        // rows in the whole table; the oldest go first

const CATS = new Set(["free", "crew", "ops", "company", "maint", "fault", "other"]);
const MODES = new Set(["ACARS", "VDL2", "HFDL", "SATCOM", "VOICE", "OTHER"]);   // VOICE: airband radio calls turned into text by the sender's speech engine (opt-in, public)

// C0/C1 controls (the newline stays), zero-width and bidi-override characters, line/paragraph separators, BOM become a space.
function stripCtl(x) {
  let o = "";
  for (let i = 0; i < x.length; i++) {
    const c = x.charCodeAt(i);
    const bad = (c < 32 && c !== 10) || (c >= 127 && c <= 159) || (c >= 0x200b && c <= 0x200f) || (c >= 0x2028 && c <= 0x202e)
      || (c >= 0x2066 && c <= 0x2069) || c === 0xfeff || c === 0xfffe || c === 0xffff;
    o += bad ? " " : x[i];
  }
  return o;
}

/** Control characters out (line breaks and tabs become spaces, except that \n stays as one newline), length cap. */
export function cleanText(s, max = MSG_TEXT_MAX) {
  let x = String(s == null ? "" : s);
  x = x.replace(/\r\n?/g, "\n");
  x = stripCtl(x);
  x = x.replace(/ {3,}/g, "  ").replace(/\n{3,}/g, "\n\n");
  x = x.trim();
  if (x.length > max) x = x.slice(0, max);
  // do not leave half a surrogate pair at the cut
  if (/[\ud800-\udbff]$/.test(x)) x = x.slice(0, -1);
  return x;
}

/** Server-side second pass over what the app already removed. */
export function redact(s) {
  return String(s)
    .replace(/[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}/g, "[email]")
    .replace(/(?:\+|00)?\d[\d ()./-]{7,}\d/g, (m) => (m.replace(/\D/g, "").length >= 8 ? "[number]" : m));
}

/** One message from the wire, or null. */
export function sanitizeMsg(raw, now) {
  if (!raw || typeof raw !== "object") return null;
  let t = Number(raw.t);
  if (!Number.isFinite(t)) return null;
  t = Math.floor(t) * 1000;                       // seconds on the wire
  if (t > now + 5 * 60e3 || t < now - MSG_TTL_MS) return null;
  const text = redact(cleanText(raw.x));
  if (!text) return null;
  let mode = String(raw.m || "OTHER").toUpperCase().replace(/[^A-Z0-9]/g, "").slice(0, 8);
  if (!MODES.has(mode)) mode = "OTHER";
  const f = Number(raw.f);
  const freq = Number.isFinite(f) && f > 0 && f < 100000 ? Math.round(f * 1000) / 1000 : null;   // MHz
  const label = cleanText(raw.l, 4).replace(/\n/g, "");
  let cat = String(raw.c || "other").toLowerCase();
  if (!CATS.has(cat)) cat = "other";
  return { t, m: mode, f: freq, l: label, c: cat, x: text };
}

export function msgKey(m) {
  // dedupe: the same label and text within the same aircraft (a repeat of a block, or two receivers hearing it)
  let h = 5381;
  const s = m.l + "|" + m.x;
  for (let i = 0; i < s.length; i++) h = ((h * 33) ^ s.charCodeAt(i)) >>> 0;
  return h.toString(16) + ":" + s.length;
}

/** Per-station rate limiter state: returns how many messages may be taken now (and counts them). */
export function takeBudget(state, station, want, now) {
  const w = (state.get(station) || []).filter((t) => now - t < 60e3);
  const room = Math.max(0, MSG_STATION_PER_MIN - w.length);
  const n = Math.min(room, want);
  for (let i = 0; i < n; i++) w.push(now);
  state.set(station, w);
  if (state.size > 2000) for (const [k, v] of state) if (!v.length || now - v[v.length - 1] > 60e3) state.delete(k);
  return n;
}

/** The messages of one track in an ingest: sanitised, deduped within the batch, at most MSG_PER_TRACK_PER_INGEST. */
export function sanitizeList(list, now) {
  if (!Array.isArray(list)) return [];
  const out = [], seen = new Set();
  for (const r of list.slice(0, MSG_PER_TRACK_PER_INGEST * 2)) {
    const m = sanitizeMsg(r, now);
    if (!m) continue;
    const k = msgKey(m);
    if (seen.has(k)) continue;
    seen.add(k);
    out.push(m);
    if (out.length >= MSG_PER_TRACK_PER_INGEST) break;
  }
  return out;
}

/** Merge into the stored list (newest first): drops duplicates and expired ones, keeps MSG_PER_TRACK. Returns the new list. */
export function mergeStored(stored, incoming, now) {
  const keys = new Set(), all = [];
  for (const m of [...incoming, ...stored]) {
    if (now - m.t > MSG_TTL_MS) continue;
    const k = msgKey(m);
    if (keys.has(k)) continue;
    keys.add(k);
    all.push(m);
  }
  all.sort((a, b) => b.t - a.t);
  return all.slice(0, MSG_PER_TRACK);
}
