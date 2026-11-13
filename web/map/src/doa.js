// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// doa.js - bearing lines ("doa" tracks) shared by stations that run a direction finder, and the server-side fix where they cross.
// Pure functions (node --test test/doa.test.js). Spherical azimuths on the WGS-84 mean radius: the difference to the ellipsoid is far below
// the bearing error of any array (about 0.01 degrees over 500 km).

const D2R = Math.PI / 180;
const R_EARTH = 6371008.8;
export const DOA_TTL_MS = 10 * 60e3;
export const FREQ_CLUSTER_HZ = 3000;      // lines within 3 kHz of each other are the same transmitter
export const MAX_DOA_PER_INGEST = 4;
export const MAX_FIX_MAJOR_M = 300e3;     // a fix with a larger 95 % ellipse is useless and is not published

export const wrap360 = (d) => ((d % 360) + 360) % 360;
export const angDiff = (a, b) => { const d = wrap360(a - b); return d > 180 ? d - 360 : d; };

export function azimuth(lat1, lon1, lat2, lon2) {
  const p1 = lat1 * D2R, p2 = lat2 * D2R, dl = (lon2 - lon1) * D2R;
  return wrap360(Math.atan2(Math.sin(dl) * Math.cos(p2), Math.cos(p1) * Math.sin(p2) - Math.sin(p1) * Math.cos(p2) * Math.cos(dl)) / D2R);
}
export function distanceM(lat1, lon1, lat2, lon2) {
  const p1 = lat1 * D2R, p2 = lat2 * D2R;
  const a = Math.sin((p2 - p1) / 2) ** 2 + Math.cos(p1) * Math.cos(p2) * Math.sin((lon2 - lon1) * D2R / 2) ** 2;
  return 2 * R_EARTH * Math.asin(Math.min(1, Math.sqrt(a)));
}
export function destination(lat, lon, bearing, distM) {
  const p1 = lat * D2R, l1 = lon * D2R, b = bearing * D2R, d = distM / R_EARTH;
  const p2 = Math.asin(Math.sin(p1) * Math.cos(d) + Math.cos(p1) * Math.sin(d) * Math.cos(b));
  const l2 = l1 + Math.atan2(Math.sin(b) * Math.sin(d) * Math.cos(p1), Math.cos(d) - Math.sin(p1) * Math.sin(p2));
  return [((l2 / D2R + 540) % 360) - 180, p2 / D2R];   // [lon, lat]
}

/** Validate the DoA fields of an ingest record; returns {freq, sigma, conf} or null. */
export function cleanDoa(raw) {
  const bearing = Number(raw.heading != null ? raw.heading : raw.bearing);
  const freq = Number(raw.freq);
  if (!Number.isFinite(bearing) || !Number.isFinite(freq) || freq < 1e5 || freq > 1e11) return null;
  let sigma = Number(raw.sigma);
  if (!Number.isFinite(sigma)) sigma = 5;
  sigma = Math.min(45, Math.max(0.5, sigma));
  let conf = Number(raw.conf);
  if (!Number.isFinite(conf)) conf = 0;
  conf = Math.min(1, Math.max(0, conf));
  return { bearing: wrap360(bearing), freq: Math.round(freq), sigma, conf };
}

// ---------------------------------------------------------------- fixing
function localFrame(lat0, lon0) {
  return { lat0, lon0, mN: R_EARTH * D2R, mE: R_EARTH * D2R * Math.cos(lat0 * D2R) };
}
const toEN = (fr, lat, lon) => [angDiff(lon, fr.lon0) * fr.mE, (lat - fr.lat0) * fr.mN];
const toLL = (fr, e, n) => [fr.lat0 + n / fr.mN, fr.lon0 + e / fr.mE];

function ellipseFromCov(pee, pen, pnn, scale) {
  const tr = pee + pnn, det = pee * pnn - pen * pen;
  const disc = Math.sqrt(Math.max(0, tr * tr / 4 - det));
  const l1 = tr / 2 + disc, l2 = Math.max(0, tr / 2 - disc);
  const ang = 0.5 * Math.atan2(2 * pen, pnn - pee);
  return { a: scale * Math.sqrt(l1), b: scale * Math.sqrt(l2), o: ((ang / D2R) % 180 + 180) % 180 };
}

function intersectPair(a, b) {
  const fr = localFrame((a.lat + b.lat) / 2, a.lon + angDiff(b.lon, a.lon) / 2);
  const [ea, na] = toEN(fr, a.lat, a.lon), [eb, nb] = toEN(fr, b.lat, b.lon);
  const ua = Math.sin(a.bearing * D2R), va = Math.cos(a.bearing * D2R), ub = Math.sin(b.bearing * D2R), vb = Math.cos(b.bearing * D2R);
  const den = ua * vb - va * ub;
  if (Math.abs(den) < 1e-6) return null;
  const t = ((eb - ea) * vb - (nb - na) * ub) / den, s = ((eb - ea) * va - (nb - na) * ua) / den;
  if (t <= 0 || s <= 0) return null;
  return toLL(fr, ea + t * ua, na + t * va);
}

/** Maximum-likelihood (Gauss-Newton) fix from lines {lat, lon, bearing, sigma}; returns {lat, lon, a, b, o, rms} or null. */
export function fitLines(lines, start) {
  let [lat, lon] = start;
  let last = Infinity;
  for (let it = 0; it < 30; it++) {
    const fr = localFrame(lat, lon);
    let a11 = 0, a12 = 0, a22 = 0, g1 = 0, g2 = 0, cost = 0;
    for (const l of lines) {
      const [se, sn] = toEN(fr, l.lat, l.lon);
      const r2 = Math.max(1, se * se + sn * sn), w = 1 / ((l.sigma * D2R) ** 2);
      const res = angDiff(l.bearing, azimuth(l.lat, l.lon, lat, lon)) * D2R;
      const jE = -sn / r2, jN = se / r2;
      a11 += w * jE * jE; a12 += w * jE * jN; a22 += w * jN * jN; g1 += w * jE * res; g2 += w * jN * res; cost += w * res * res;
    }
    const det = a11 * a22 - a12 * a12;
    if (!(Math.abs(det) > 1e-30)) return null;
    const pee = a22 / det, pen = -a12 / det, pnn = a11 / det;
    const dE = pee * g1 + pen * g2, dN = pen * g1 + pnn * g2;
    [lat, lon] = toLL(fr, dE, dN);
    if (!Number.isFinite(lat) || !Number.isFinite(lon)) return null;
    if (Math.hypot(dE, dN) < 0.01 || Math.abs(last - cost) < 1e-12 * (1 + cost)) break;
    last = cost;
  }
  const fr = localFrame(lat, lon);
  let a11 = 0, a12 = 0, a22 = 0, rss = 0, ws = 0;
  for (const l of lines) {
    const [se, sn] = toEN(fr, l.lat, l.lon);
    const r2 = Math.max(1, se * se + sn * sn), w = 1 / ((l.sigma * D2R) ** 2);
    a11 += w * (sn * sn / (r2 * r2)); a12 += w * (-sn * se / (r2 * r2)); a22 += w * (se * se / (r2 * r2));
    const res = angDiff(l.bearing, azimuth(l.lat, l.lon, lat, lon));
    rss += res * res * w; ws += w;
  }
  const det = a11 * a22 - a12 * a12;
  if (!(Math.abs(det) > 1e-30)) return null;
  const e = ellipseFromCov(a22 / det, -a12 / det, a11 / det, Math.sqrt(5.991));
  return { lat, lon, a: e.a, b: e.b, o: e.o, rms: Math.sqrt(rss / ws) };
}

/** Robust fix: consensus over line pairs (inliers within max(3 sigma, 6 degrees)), then the ML fit on the inliers. */
export function fixLines(lines) {
  const n = lines.length;
  if (n < 2) return null;
  let best = null;
  for (let i = 0; i < n; i++) for (let j = i + 1; j < n; j++) {
    const p = intersectPair(lines[i], lines[j]);
    if (!p) continue;
    let cnt = 0, score = 0;
    const mask = [];
    for (let k = 0; k < n; k++) {
      const r = Math.abs(angDiff(lines[k].bearing, azimuth(lines[k].lat, lines[k].lon, p[0], p[1])));
      const ok = r <= Math.max(3 * lines[k].sigma, 6);
      mask.push(ok);
      if (ok) { cnt++; score += (r / lines[k].sigma) ** 2; }
    }
    if (!best || cnt > best.cnt || (cnt === best.cnt && score < best.score)) best = { cnt, score, mask, p };
  }
  if (!best || best.cnt < 2) return null;
  const use = lines.filter((_, k) => best.mask[k]);
  const f = fitLines(use, best.p);
  if (!f) return null;
  return { ...f, n: use.length, used: best.mask };
}

/**
 * doa tracks (rows with id, lat, lon, heading, extra JSON {f, s, c}, t) -> fix tracks. One fix per transmitter frequency, from the newest
 * line of each distinct station; stations are told apart by the id prefix before the last '-'.
 */
export function computeFixes(doaTracks, now) {
  const fresh = [];
  for (const t of doaTracks) {
    if (!t || now - t.t > DOA_TTL_MS) continue;
    let x = {};
    try { x = JSON.parse(t.extra || "{}"); } catch { continue; }
    if (!Number.isFinite(x.f) || !Number.isFinite(t.heading)) continue;
    const dash = String(t.id).lastIndexOf("-");
    fresh.push({ station: dash > 0 ? String(t.id).slice(0, dash) : t.id, lat: t.lat, lon: t.lon, bearing: t.heading, sigma: x.s || 5, f: x.f, t: t.t });
  }
  fresh.sort((a, b) => a.f - b.f);
  const out = [];
  let cluster = [];
  const flush = () => {
    if (cluster.length >= 2) {
      const newest = new Map();
      for (const l of cluster) { const p = newest.get(l.station); if (!p || l.t > p.t) newest.set(l.station, l); }
      const lines = [...newest.values()];
      if (lines.length >= 2) {
        const f = fixLines(lines);
        if (f && f.a <= MAX_FIX_MAJOR_M) {
          const freq = Math.round(lines.reduce((s, l) => s + l.f, 0) / lines.length);
          out.push({
            id: "fix:" + Math.round(freq / 1000), kind: "fix", label: (freq / 1e6).toFixed(3) + " MHz", sub: f.n + " stations",
            lat: f.lat, lon: f.lon, alt: null, heading: null, speed: null, vrate: null, squawk: null, cat: "", src: "df", age: 0, est: 0,
            extra: JSON.stringify({ a: Math.round(f.a), b: Math.round(f.b), o: Math.round(f.o), n: f.n, r: +f.rms.toFixed(1), f: freq }),
            t: Math.max(...lines.map((l) => l.t)),
          });
        }
      }
    }
    cluster = [];
  };
  for (const l of fresh) {
    if (cluster.length && l.f - cluster[cluster.length - 1].f > FREQ_CLUSTER_HZ) flush();
    cluster.push(l);
  }
  flush();
  return out;
}
