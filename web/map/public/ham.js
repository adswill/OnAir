// ham.js - pure helpers for the "Ham contacts" section: band from frequency, parsing the contact hints a ham track may carry in
// its `extra` (JSON text or object: {m: mode, f: Hz, b: band, to: callsign, tlat, tlon}), building contacts (station -> peer),
// filtering by band / mode / age, colour by mode. No DOM, no ingest changes: tracks that carry no hints simply give no contact.

const BANDS = [
  ["160m", 1.8e6, 2e6], ["80m", 3.5e6, 4e6], ["60m", 5.25e6, 5.45e6], ["40m", 7e6, 7.3e6], ["30m", 10.1e6, 10.15e6],
  ["20m", 14e6, 14.35e6], ["17m", 18.068e6, 18.168e6], ["15m", 21e6, 21.45e6], ["12m", 24.89e6, 24.99e6],
  ["10m", 28e6, 29.7e6], ["6m", 50e6, 54e6], ["2m", 144e6, 148e6], ["70cm", 420e6, 450e6],
];
export const BAND_NAMES = BANDS.map((b) => b[0]);

export function bandOf(hz) {
  const f = Number(hz);
  if (!Number.isFinite(f)) return "";
  for (const [n, lo, hi] of BANDS) if (f >= lo && f <= hi) return n;
  return "";
}

const MODE_COLOURS = { FT8: "#f0b840", FT4: "#e0902c", WSPR: "#5aa0f0", JS8: "#d070e0", QSO: "#6ccc7a", OTHER: "#9aa0a6" };
export const MODE_NAMES = Object.keys(MODE_COLOURS);

/** Normalised mode: FT8/FT4/WSPR/JS8, SSB/CW/etc. count as QSO, anything else OTHER. */
export function normMode(m) {
  const s = String(m || "").toUpperCase().replace(/[^A-Z0-9]/g, "");
  if (!s) return "OTHER";
  if (s.startsWith("FT8")) return "FT8";
  if (s.startsWith("FT4")) return "FT4";
  if (s.startsWith("WSPR")) return "WSPR";
  if (s.startsWith("JS8")) return "JS8";
  if (["SSB", "USB", "LSB", "CW", "AM", "FM", "QSO", "RTTY", "PSK31", "PSK"].includes(s)) return "QSO";
  return "OTHER";
}
export function modeColour(m) { return MODE_COLOURS[normMode(m)]; }

/** The contact hints of a track, or null when it has none. Never throws on odd input. */
export function hamHints(t) {
  let x = t && t.extra;
  if (typeof x === "string") { try { x = JSON.parse(x); } catch { return null; } }
  if (!x || typeof x !== "object") return null;
  const to = typeof x.to === "string" ? x.to.trim().toUpperCase().slice(0, 16) : "";
  const tlat = Number(x.tlat), tlon = Number(x.tlon);
  const hasPos = x.tlat != null && x.tlon != null && Number.isFinite(tlat) && Number.isFinite(tlon) && Math.abs(tlat) <= 90 && Math.abs(tlon) <= 180;
  if (!to && !hasPos) return null;
  const f = Number(x.f);
  return { to, pos: hasPos ? [tlon, tlat] : null, mode: normMode(x.m), freq: Number.isFinite(f) ? f : null, band: x.b ? String(x.b) : bandOf(f) };
}

/** Contacts from ham tracks. `peerPos(callsign)` may supply a position for a peer that is itself a known track. */
export function buildContacts(tracksIn, nowMs = Date.now(), peerPos = null) {
  // the page passes Map.values(): an iterator, which is spent after one pass (this used to leave every contact list empty)
  const tracks = Array.isArray(tracksIn) ? tracksIn : [...tracksIn];
  const byCall = new Map();
  for (const t of tracks) if (t.kind === "ham" && t.label) byCall.set(String(t.label).toUpperCase(), t);
  const out = [];
  for (const t of tracks) {
    if (t.kind !== "ham") continue;
    const h = hamHints(t);
    if (!h) continue;
    // the hub stamps a track with its arrival time and keeps the report's age beside it: a contact was heard `age` seconds before that
    const heardMs = t.t - (Number(t.age) > 0 ? Number(t.age) * 1000 : 0);
    let pos = h.pos;
    if (!pos && h.to) {
      const p = byCall.get(h.to);
      if (p && Number.isFinite(p.lon) && Number.isFinite(p.lat)) pos = [p.lon, p.lat];
      else if (peerPos) pos = peerPos(h.to) || null;
    }
    out.push({ id: t.id, from: String(t.label || t.id), to: h.to, a: [t.lon, t.lat], b: pos, mode: h.mode, band: h.band, freq: h.freq, t: heardMs, ageMin: Math.max(0, (nowMs - heardMs) / 60000) });
  }
  out.sort((x, y) => x.ageMin - y.ageMin);
  return out;
}

/** filters: {bands: Set|null, modes: Set|null, maxAgeMin: number|null}. An empty/absent set means all. */
export function filterContacts(list, f = {}) {
  return list.filter((c) =>
    (!f.bands || !f.bands.size || f.bands.has(c.band)) &&
    (!f.modes || !f.modes.size || f.modes.has(c.mode)) &&
    (f.maxAgeMin == null || c.ageMin <= f.maxAgeMin));
}

/** GeoJSON lines for contacts that have both ends. A line crossing the antimeridian is drawn the short way by shifting the far end. */
export function contactFeatures(list) {
  const feats = [];
  for (const c of list) {
    if (!c.b) continue;
    const [lo1, la1] = c.a;
    let [lo2, la2] = c.b;
    if (lo2 - lo1 > 180) lo2 -= 360; else if (lo1 - lo2 > 180) lo2 += 360;
    feats.push({ type: "Feature", properties: { id: c.id, mode: c.mode, color: modeColour(c.mode), band: c.band }, geometry: { type: "LineString", coordinates: [[lo1, la1], [lo2, la2]] } });
  }
  return feats;
}
