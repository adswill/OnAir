// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// df.js - map features for direction finding: bearing lines from the stations ("doa" tracks) and the fix markers with their 95 % ellipse ("fix" tracks).
// Pure (no DOM); covered by web/map/test/df.test.js.
const D2R = Math.PI / 180, R = 6371008.8;
export const DF_TTL_MS = 10 * 60e3;
export const LINE_KM = 120;     // length of a bearing line when no fix ends it

export function destination(lat, lon, bearing, distM) {
  const p1 = lat * D2R, l1 = lon * D2R, b = bearing * D2R, d = distM / R;
  const p2 = Math.asin(Math.sin(p1) * Math.cos(d) + Math.cos(p1) * Math.sin(d) * Math.cos(b));
  const l2 = l1 + Math.atan2(Math.sin(b) * Math.sin(d) * Math.cos(p1), Math.cos(d) - Math.sin(p1) * Math.sin(p2));
  return [((l2 / D2R + 540) % 360) - 180, p2 / D2R];
}

/** Closed polygon ring [lon, lat] of an ellipse: semi-axes a (along orientation o, degrees clockwise from north) and b, in metres. */
export function ellipseRing(lat, lon, a, b, o, steps = 48) {
  const ring = [];
  const th = o * D2R;
  const mN = R * D2R, mE = R * D2R * Math.max(0.01, Math.cos(lat * D2R));
  for (let i = 0; i <= steps; i++) {
    const t = (i / steps) * 2 * Math.PI;
    const u = a * Math.cos(t), v = b * Math.sin(t);                  // along / across the major axis
    const e = u * Math.sin(th) + v * Math.cos(th), n = u * Math.cos(th) - v * Math.sin(th);
    ring.push([lon + e / mE, lat + n / mN]);
  }
  return ring;
}

function parse(extra) { try { return JSON.parse(extra || "{}") || {}; } catch { return {}; } }

/** GeoJSON features for the DF layers; `show` is the on/off state per kind. */
export function dfFeatures(tracks, show, now) {
  const features = [];
  for (const t of tracks.values()) {
    if (t.kind !== "doa" && t.kind !== "fix") continue;
    if (!show[t.kind] || now - t.t > DF_TTL_MS) continue;
    const x = parse(t.extra);
    const fade = Math.max(0.25, 1 - (now - t.t) / DF_TTL_MS);
    if (t.kind === "doa" && Number.isFinite(t.heading)) {
      const sg = Math.min(30, Math.max(0.5, x.s || 5));
      const left = destination(t.lat, t.lon, t.heading - sg, LINE_KM * 1000), right = destination(t.lat, t.lon, t.heading + sg, LINE_KM * 1000);
      features.push({ type: "Feature", properties: { id: t.id, kind: "dfline", fade, label: t.label }, geometry: { type: "LineString", coordinates: [[t.lon, t.lat], destination(t.lat, t.lon, t.heading, LINE_KM * 1000)] } });
      features.push({ type: "Feature", properties: { id: t.id, kind: "dfwedge", fade }, geometry: { type: "Polygon", coordinates: [[[t.lon, t.lat], left, destination(t.lat, t.lon, t.heading, LINE_KM * 1000), right, [t.lon, t.lat]]] } });
      features.push({ type: "Feature", properties: { id: t.id, kind: "dfstation", fade }, geometry: { type: "Point", coordinates: [t.lon, t.lat] } });
    } else if (t.kind === "fix" && x.a > 0) {
      features.push({ type: "Feature", properties: { id: t.id, kind: "dfell", fade }, geometry: { type: "Polygon", coordinates: [ellipseRing(t.lat, t.lon, x.a, x.b, x.o || 0)] } });
      features.push({ type: "Feature", properties: { id: t.id, kind: "dffix", fade, label: t.label }, geometry: { type: "Point", coordinates: [t.lon, t.lat] } });
    }
  }
  return features;
}
