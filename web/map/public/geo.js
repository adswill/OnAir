// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// geo.js - pure geometry, flight-progress and formatting helpers (no DOM, no network).
// Used by the aircraft card and covered by web/map/test/geo.test.js.

const R_NM = 3440.065; // mean Earth radius in nautical miles

/** Great-circle distance in nautical miles between two lat/lon points (degrees). */
export function distanceNm(lat1, lon1, lat2, lon2) {
  const p = Math.PI / 180;
  const dLat = (lat2 - lat1) * p;
  const dLon = (lon2 - lon1) * p;
  const a = Math.sin(dLat / 2) ** 2 + Math.cos(lat1 * p) * Math.cos(lat2 * p) * Math.sin(dLon / 2) ** 2;
  return R_NM * 2 * Math.asin(Math.min(1, Math.sqrt(a)));
}

/** Initial great-circle bearing in degrees true (0-360) from point 1 to point 2. */
export function bearing(lat1, lon1, lat2, lon2) {
  const p = Math.PI / 180;
  const dLon = (lon2 - lon1) * p;
  const y = Math.sin(dLon) * Math.cos(lat2 * p);
  const x = Math.cos(lat1 * p) * Math.sin(lat2 * p) - Math.sin(lat1 * p) * Math.cos(lat2 * p) * Math.cos(dLon);
  return (Math.atan2(y, x) / p + 360) % 360;
}

/** True when a fix looks like an aircraft on the ground (very low and slow). */
export function isOnGround(altFt, speedKt) {
  if (altFt == null || !Number.isFinite(altFt)) return false;
  return altFt <= 100 && (speedKt == null || speedKt < 60);
}

/**
 * Estimated time of arrival from remaining great-circle distance and ground speed.
 * Returns null when the speed is unusable (<80 kt), the plane is on the ground, or the result is absurd (>20 h).
 * `now` is injectable for tests.
 */
export function eta({ lat, lon, altFt, speedKt }, dest, now = Date.now()) {
  if (!dest || !Number.isFinite(dest.lat) || !Number.isFinite(dest.lon)) return null;
  if (!Number.isFinite(speedKt) || speedKt < 80) return null;
  if (isOnGround(altFt, speedKt)) return null;
  const remainingNm = distanceNm(lat, lon, dest.lat, dest.lon);
  const minutes = (remainingNm / speedKt) * 60;
  if (!Number.isFinite(minutes) || minutes > 20 * 60) return null;
  return { remainingNm, minutes, atMs: now + minutes * 60e3 };
}

/** Flight progress between origin and destination: distances (nm) and fraction 0..1 of the route flown. */
export function progress(origin, dest, lat, lon) {
  if (!origin || !dest) return null;
  const totalNm = distanceNm(origin.lat, origin.lon, dest.lat, dest.lon);
  const flownNm = distanceNm(origin.lat, origin.lon, lat, lon);
  const remainingNm = distanceNm(lat, lon, dest.lat, dest.lon);
  if (!(totalNm > 1)) return null;
  // flown + remaining >= total (detours); use the ratio so the bar never jumps outside 0..1
  const frac = Math.max(0, Math.min(1, flownNm / (flownNm + remainingNm)));
  return { totalNm, flownNm, remainingNm, frac };
}

/**
 * Departure estimate from the first sighting. `first` = {ms, altFt, lat, lon}.
 * Only when first seen on the ground or below 3000 ft within 30 km of the departure airport:
 * on the ground -> take-off ~ first sighting; airborne low -> subtract the climb time at ~1800 ft/min.
 * Returns {ms, estimated:true} or null.
 */
export function estimateDeparture(first, origin) {
  if (!first || !origin || !Number.isFinite(first.ms)) return null;
  if (!Number.isFinite(first.altFt) || first.altFt >= 3000) return null;
  const km = distanceNm(first.lat, first.lon, origin.lat, origin.lon) * 1.852;
  if (!(km <= 30)) return null;
  const climbMin = first.altFt > 100 ? first.altFt / 1800 : 0;
  return { ms: first.ms - climbMin * 60e3, estimated: true };
}

/** Meaning of a transponder code; `emergency` marks 7500/7600/7700. */
export function squawkMeaning(code) {
  const c = String(code == null ? "" : code).padStart(4, "0");
  switch (c) {
    case "7500": return { code: c, text: "Hijack", emergency: true };
    case "7600": return { code: c, text: "Radio failure", emergency: true };
    case "7700": return { code: c, text: "General emergency", emergency: true };
    case "7000": return { code: c, text: "VFR conspicuity (Europe)", emergency: false };
    case "1200": return { code: c, text: "VFR (North America)", emergency: false };
    case "2000": return { code: c, text: "No ATC code assigned (oceanic/IFR)", emergency: false };
    default: return { code: c, text: "", emergency: false };
  }
}

/** "1 h 12 min", "35 min", "<1 min". */
export function formatDuration(minutes) {
  if (!Number.isFinite(minutes) || minutes < 0) return "";
  const m = Math.round(minutes);
  if (m < 1) return "<1 min";
  if (m < 60) return m + " min";
  const h = Math.floor(m / 60);
  return h + " h " + String(m % 60).padStart(2, "0") + " min";
}

/** Vertical-rate arrow glyph: up, down, or level (|fpm| < 100). */
export function vrateArrow(fpm) {
  if (!Number.isFinite(fpm) || Math.abs(fpm) < 100) return "→";
  return fpm > 0 ? "↑" : "↓";
}

/** Country flag emoji from an ISO 3166 alpha-2 code; empty when invalid. */
export function flagEmoji(iso2) {
  const s = String(iso2 || "").toUpperCase();
  if (!/^[A-Z]{2}$/.test(s)) return "";
  return String.fromCodePoint(0x1F1E6 + s.charCodeAt(0) - 65, 0x1F1E6 + s.charCodeAt(1) - 65);
}

/** Local HH:MM for a wall-clock ms value. */
export function hhmm(ms) {
  return new Date(ms).toLocaleTimeString([], { hour: "2-digit", minute: "2-digit" });
}

/** Airline-style callsign split: "BAW123" -> {airline:"BAW", number:"123"}; null when not that shape. */
export function splitCallsign(cs) {
  const m = String(cs || "").trim().toUpperCase().match(/^([A-Z]{3})(\d{1,4}[A-Z]?)$/);
  return m ? { airline: m[1], number: m[2] } : null;
}

/** Stable pastel-free badge colour from a string (airline code). Returns hsl() string. */
export function badgeColor(s) {
  let h = 0;
  for (const ch of String(s || "?")) h = (h * 31 + ch.charCodeAt(0)) % 360;
  return `hsl(${h} 45% 38%)`;
}
