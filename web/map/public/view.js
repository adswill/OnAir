// view.js - pure helpers of the page chrome (no DOM): layer groups, age fade and age filter, shortcut table, sparkline geometry.
// Covered by test/view.test.js.

/** Layers in groups for the panel. Every layer id of app.js KINDS must be in exactly one group (a test enforces it). */
export const LAYER_GROUPS = [
  ["Air", ["aircraft", "flarm", "drone", "sonde"]],
  ["Sea", ["vessel", "buoy", "dsc"]],
  ["Ground and radio", ["aprs", "ham", "station"]],
  ["Space", ["sat", "gnss"]],
  ["Direction finding", ["doa", "fix"]],
];

/** Age filter choices: [value in minutes ("" = any), label]. */
export const AGE_OPTIONS = [["", "Any age"], ["2", "2 min"], ["10", "10 min"], ["60", "1 hour"], ["360", "6 hours"]];

/** How long after the last report each kind counts as stale (s). Moving things go stale in seconds, fixed stations in hours. */
export const STALE_SEC = { aircraft: 60, flarm: 45, drone: 60, vessel: 180, sonde: 900, aprs: 1800, ham: 2700, sat: 90, gnss: 90, buoy: 7200, dsc: 900, station: 21600, doa: 600, fix: 600 };
export function staleSecOf(kind, srcStale) {
  if (Number.isFinite(srcStale) && srcStale > 0) return srcStale;
  return STALE_SEC[kind] || 300;
}

/** Icon opacity by age: full until the stale limit, then fading to 0.35 over five more limits. */
export function ageFade(ageSec, staleSec) {
  const a = Number(ageSec), s = Number(staleSec) > 0 ? Number(staleSec) : 60;
  if (!Number.isFinite(a) || a <= s) return 1;
  return Math.round((1 - 0.65 * Math.min(1, (a - s) / (s * 5))) * 100) / 100;
}

/** True when a report `ageSec` old passes the age filter (`maxMin` = minutes, 0 / null = any). */
export function passesAge(ageSec, maxMin) {
  const m = Number(maxMin);
  if (!Number.isFinite(m) || m <= 0) return true;
  return Number(ageSec) <= m * 60;
}

/** Keyboard shortcuts: [keys, what]. One table feeds the help dialog and the handler. */
export const SHORTCUTS = [
  ["/", "Search"], ["Esc", "Close card, search or panel"], ["?", "This list"], ["L", "Show or hide the layer panel"],
  ["T", "Theme: auto, dark, light"], ["F", "Follow the selected track"], ["+ / -", "Zoom in or out"], ["0", "Reset the view"],
  ["1 - 5", "Show only one layer group (Air, Sea, Ground, Space, DF)"], ["A", "Show all layers"],
];

/**
 * Sparkline geometry for [[x, y], ...] (x ascending). Returns {line, area, min, max, last, n} with paths in a w x h box, or null when
 * there are fewer than 2 usable points. A flat series is drawn in the middle.
 */
export function sparkline(points, w = 280, h = 56, pad = 3) {
  const p = (points || []).filter((q) => q && Number.isFinite(q[0]) && Number.isFinite(q[1]));
  if (p.length < 2) return null;
  let x0 = Infinity, x1 = -Infinity, y0 = Infinity, y1 = -Infinity;
  for (const [x, y] of p) { x0 = Math.min(x0, x); x1 = Math.max(x1, x); y0 = Math.min(y0, y); y1 = Math.max(y1, y); }
  if (x1 === x0) return null;
  const sx = (x) => pad + ((x - x0) / (x1 - x0)) * (w - 2 * pad);
  const sy = (y) => (y1 === y0 ? h / 2 : h - pad - ((y - y0) / (y1 - y0)) * (h - 2 * pad));
  const pts = p.map(([x, y]) => [sx(x).toFixed(1), sy(y).toFixed(1)]);
  const line = "M" + pts.map((q) => q.join(",")).join("L");
  const area = line + `L${pts[pts.length - 1][0]},${h - pad}L${pts[0][0]},${h - pad}Z`;
  return { line, area, min: y0, max: y1, last: p[p.length - 1][1], n: p.length, x0, x1 };
}

/** The series the detail panel plots from a track's history points [t_ms, lat, lon, alt, heading]: altitude when it has some, else distance from the first point. */
export function seriesOfTrail(tpts) {
  const pts = (tpts || []).filter((q) => Array.isArray(q) && Number.isFinite(q[0]));
  const alt = pts.filter((q) => Number.isFinite(q[3])).map((q) => [q[0], q[3]]);
  if (alt.length >= 3 && Math.max(...alt.map((q) => q[1])) !== Math.min(...alt.map((q) => q[1]))) return { name: "Altitude", unit: "ft", points: alt };
  if (pts.length >= 3) {
    const [, la0, lo0] = pts[0], k = Math.PI / 180;
    const d = pts.map((q) => {
      const a = Math.sin(((q[1] - la0) * k) / 2) ** 2 + Math.cos(la0 * k) * Math.cos(q[1] * k) * Math.sin(((q[2] - lo0) * k) / 2) ** 2;
      return [q[0], 6371 * 2 * Math.asin(Math.min(1, Math.sqrt(a)))];
    });
    if (d.some((q) => q[1] > 0.05)) return { name: "Distance from first fix", unit: "km", points: d };
  }
  return null;
}
