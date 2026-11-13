// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// GPS interference layer: grid cells from GET /jam -> GeoJSON squares coloured by the share of aircraft with a degraded GNSS position.
// The share is an indicator, not proof of jamming: see docs/gps-jamming-map.md.

export const CELL_DEG = 0.5;

/** Share of degraded reports -> 0 normal, 1 some, 2 many, 3 most. Needs at least 3 aircraft. */
export function level(n, bad) {
  if (!(n >= 3)) return -1;
  const r = bad / n;
  return r < 0.1 ? 0 : r < 0.3 ? 1 : r < 0.6 ? 2 : 3;
}

export const LEVEL_COLOURS = ["#3fae5a", "#e6c229", "#f08a24", "#e0443a"];

export function cellsToGeo(cells, cellDeg = CELL_DEG) {
  const features = [];
  for (const c of cells || []) {
    if (!Array.isArray(c) || c.length < 4) continue;
    const [li, lo, n, bad] = c;
    const lv = level(n, bad);
    if (lv < 0) continue;
    const s = li * cellDeg, w = lo * cellDeg;
    features.push({
      type: "Feature",
      geometry: { type: "Polygon", coordinates: [[[w, s], [w + cellDeg, s], [w + cellDeg, s + cellDeg], [w, s + cellDeg], [w, s]]] },
      properties: { n, bad, pct: Math.round((100 * bad) / n), lv, color: LEVEL_COLOURS[lv] },
    });
  }
  return { type: "FeatureCollection", features };
}
