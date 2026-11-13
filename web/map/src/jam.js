// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// GPS interference heatmap: aggregate-only. A station posts, per 0.5 degree grid cell, how many aircraft it heard in the last hour and
// how many of them reported a degraded GNSS position (NACp / NIC low, or a position jump). No aircraft ids, no tracks, no positions finer
// than the cell. The hub keeps the newest post of each station (a post replaces the station's earlier one), sums the stations' cells and
// serves the sum. Nothing is stored per aircraft, so nothing about an aircraft can be read back.

export const CELL_DEG = 0.5;
export const JAM_TTL_MS = 2 * 3600e3;       // a station's cells count for this long after its last post
export const JAM_MIN_GAP_MS = 30e3;         // one post per station per 30 s
export const MAX_CELLS_PER_POST = 600;      // 16 x 16 degrees at 0.5 degree cells is 1024 cells, far more than 1090 MHz reaches
export const MAX_AIRCRAFT_PER_CELL = 300;   // per station and cell
export const MAX_STATIONS = 2000;
export const MIN_AIRCRAFT_PUBLISHED = 3;    // a cell below this many aircraft (summed over stations) is not shown: too few to say anything
const LI_MIN = -180, LI_MAX = 179, LO_MIN = -360, LO_MAX = 359;   // floor(lat/0.5), floor(lon/0.5)

const isInt = (v) => Number.isInteger(v);

/** Validate a posted body: { station, cell: 0.5, cells: [[latIdx, lonIdx, aircraft, degraded], ...] } -> clean cells or null. */
export function sanitizeJam(body) {
  if (!body || typeof body !== "object") return null;
  const station = String(body.station || "");
  if (!/^[a-zA-Z0-9_-]{8,64}$/.test(station)) return null;
  if (body.cell !== CELL_DEG) return null;
  if (!Array.isArray(body.cells) || body.cells.length > MAX_CELLS_PER_POST) return null;
  const seen = new Set();
  const cells = [];
  for (const c of body.cells) {
    if (!Array.isArray(c) || c.length !== 4) return null;   // anything else (an id, a position, extra fields) rejects the whole post
    const [li, lo, n, bad] = c;
    if (![li, lo, n, bad].every(isInt)) return null;
    if (li < LI_MIN || li > LI_MAX || lo < LO_MIN || lo > LO_MAX) return null;
    if (n < 1 || n > MAX_AIRCRAFT_PER_CELL || bad < 0 || bad > n) return null;
    const key = li * 1000 + lo;
    if (seen.has(key)) return null;
    seen.add(key);
    cells.push([li, lo, n, bad]);
  }
  return { station, cells };
}

export class JamBook {
  constructor() {
    this.stations = new Map();   // station -> { t, cells: [[li, lo, n, bad]] }
    this.cache = null;
    this.cacheAt = 0;
  }

  /** Returns "ok", "slow" (too soon) or "full" (station table full). A post replaces the station's earlier cells. */
  accept(station, cells, now) {
    const old = this.stations.get(station);
    if (old && now - old.t < JAM_MIN_GAP_MS) return "slow";
    if (!old) {
      if (this.stations.size >= MAX_STATIONS) this.expire(now);
      if (this.stations.size >= MAX_STATIONS) return "full";
    }
    this.stations.set(station, { t: now, cells });
    this.cache = null;
    return "ok";
  }

  expire(now) {
    for (const [id, s] of this.stations) if (now - s.t > JAM_TTL_MS) this.stations.delete(id);
  }

  /** The sum over stations: [[li, lo, aircraft, degraded]], cells with fewer than MIN_AIRCRAFT_PUBLISHED aircraft left out. */
  cells(now) {
    if (this.cache && now - this.cacheAt < 10e3) return this.cache;
    this.expire(now);
    const sum = new Map();
    for (const s of this.stations.values()) {
      for (const [li, lo, n, bad] of s.cells) {
        const k = li * 1000 + lo;
        const e = sum.get(k);
        if (e) { e[2] += n; e[3] += bad; } else sum.set(k, [li, lo, n, bad]);
      }
    }
    this.cache = [...sum.values()].filter((c) => c[2] >= MIN_AIRCRAFT_PUBLISHED).sort((a, b) => a[0] - b[0] || a[1] - b[1]);
    this.cacheAt = now;
    return this.cache;
  }

  /** Persistable form (the hub keeps it in its meta table so a restart does not empty the layer). */
  toJSON() {
    return [...this.stations].map(([id, s]) => [id, s.t, s.cells]);
  }

  load(rows, now) {
    if (!Array.isArray(rows)) return;
    for (const r of rows) {
      if (!Array.isArray(r) || r.length !== 3 || now - r[1] > JAM_TTL_MS) continue;
      const clean = sanitizeJam({ station: r[0], cell: CELL_DEG, cells: r[2] });
      if (clean) this.stations.set(clean.station, { t: r[1], cells: clean.cells });
    }
    this.cache = null;
  }
}
