// cluster.js - pure grid clustering for the zoomed-out map. Points {id, kind, lon, lat} -> clusters {lon, lat, n, kinds}.
// Cell size in degrees shrinks with zoom; a cell with fewer than `minSize` points is returned as singles.

export function cellDeg(zoom, px = 56) {
  // whole zoom levels only: cell boundaries stay put while zooming within a level, so members do not hop between clusters
  return px * 360 / (256 * Math.pow(2, Math.floor(zoom)));
}

/** Returns {clusters, singles}. keep(id) forces a point to stay single (e.g. the selected track). */
export function clusterPoints(points, zoom, { minSize = 4, keep = null, px = 56 } = {}) {
  const d = cellDeg(zoom, px);
  const cells = new Map();
  for (const p of points) {
    if (!Number.isFinite(p.lon) || !Number.isFinite(p.lat)) continue;
    const key = Math.floor((p.lon + 180) / d) + ":" + Math.floor((p.lat + 90) / d);
    let c = cells.get(key);
    if (!c) cells.set(key, c = []);
    c.push(p);
  }
  const clusters = [], singles = [];
  for (const arr of cells.values()) {
    const forced = keep ? arr.filter((p) => keep(p.id)) : [];
    const rest = forced.length ? arr.filter((p) => !keep(p.id)) : arr;
    singles.push(...forced);
    if (rest.length < minSize) { singles.push(...rest); continue; }
    let lo = 0, la = 0;
    const kinds = {};
    for (const p of rest) { lo += p.lon; la += p.lat; kinds[p.kind] = (kinds[p.kind] || 0) + 1; }
    clusters.push({ lon: lo / rest.length, lat: la / rest.length, n: rest.length, kinds });
  }
  return { clusters, singles };
}

/** Clustering only pays off zoomed out with many points. */
export function shouldCluster(count, zoom) { return zoom < 5 && count > 300; }
