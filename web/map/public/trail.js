// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// trail.js - merge the trail the hub keeps with the points this page saw live (pure, tested in node).
// A point is [t_ms, lat, lon, alt|null, heading|null], the same shape the hub serves.

export const MAX_CLIENT_POINTS = 1000;
const SAME_FIX_MS = 1500; // the hub and the page stamp the same fix within ~1 s of each other

/** Union of both lists, oldest first. Same timestamp (or two fixes within 1.5 s at the same spot) counts once; the server copy wins. */
export function mergeTrails(server, live) {
  const all = [];
  for (const p of server || []) if (valid(p)) all.push({ p, s: 1 });
  for (const p of live || []) if (valid(p)) all.push({ p, s: 0 });
  all.sort((a, b) => a.p[0] - b.p[0] || b.s - a.s);
  const out = [];
  for (const { p } of all) {
    const last = out[out.length - 1];
    if (last && (p[0] === last[0] || (p[0] - last[0] < SAME_FIX_MS && p[1] === last[1] && p[2] === last[2]))) continue;
    out.push(p);
  }
  return out.length > MAX_CLIENT_POINTS ? out.slice(-MAX_CLIENT_POINTS) : out;
}

function valid(p) {
  return Array.isArray(p) && Number.isFinite(p[0]) && Number.isFinite(p[1]) && Number.isFinite(p[2]);
}

/** Append one live fix, skipping repeats of the last position. Returns a new array. */
export function appendLive(points, t, lat, lon, alt, hdg) {
  const last = points && points[points.length - 1];
  if (last && (t <= last[0] || (last[1] === lat && last[2] === lon))) return points || [];
  const next = (points || []).concat([[t, lat, lon, alt ?? null, hdg ?? null]]);
  return next.length > MAX_CLIENT_POINTS ? next.slice(-MAX_CLIENT_POINTS) : next;
}

/** [lon,lat] coordinate list for MapLibre. */
export function toLine(points) {
  return (points || []).map((p) => [p[2], p[1]]);
}
