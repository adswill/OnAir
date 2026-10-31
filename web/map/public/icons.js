// icons.js - turns the vector shapes (shapes.js) into MapLibre sprite images, lazily and cached, plus the colour logic
// (altitude gradient, theme variants) and the image ids.
//
// One sprite atlas: MapLibre packs every image that addImage() receives into a single GPU atlas and draws all tracks
// in ONE symbol layer, so there are no per-marker DOM nodes, markers outside the view are culled by the renderer, and an
// icon is rasterised once per (shape, colour, theme) - the `styleimagemissing` event asks for it the first time it is on screen.
// Image ids look like "narrow|e6e8ea|d": shape | colour hex without # | theme (d = dark map, l = light map).
// Pure helpers (ids, altitude colour) have no DOM use at import time, so node --test can load this file.

import { shapeOf } from "./shapes.js";

export const ICON_PX = 96;       // canvas size; the box is -32..32 units
export const PIXEL_RATIO = 3;    // 96 px at pixelRatio 3 = 32 CSS px at icon-size 1, crisp on 2x/3x screens
const UNIT = 0.5;                // CSS px per drawing unit at icon-size 1 (32 px / 64 units)
const OUTLINE_PX = 1.15;          // outline width in CSS px

/** Outline / detail colours per map theme. */
const THEME = {
  d: { outline: "rgba(8,10,12,0.92)", detail: "rgba(8,10,12,0.5)", sheen: "rgba(255,255,255,0.22)", shade: "rgba(0,0,0,0.12)" },
  l: { outline: "rgba(14,18,22,0.95)", detail: "rgba(14,18,22,0.5)", sheen: "rgba(255,255,255,0.3)", shade: "rgba(0,0,0,0.14)" },
};

// ---------- altitude colouring ----------

/** Low -> high: amber, yellow, green, teal, blue, violet, magenta. Original palette, readable on dark and light maps. */
export const ALT_STOPS = [
  [0, "#f2994a"], [3000, "#e9c442"], [8000, "#86cf62"], [15000, "#38bdb0"], [25000, "#4a95e8"], [35000, "#7f72f2"], [45000, "#c866e2"],
];
export const ALT_BAND_FT = 2000;   // quantised, so the atlas holds at most 23 colours per shape

const hex2 = (n) => Math.round(Math.max(0, Math.min(255, n))).toString(16).padStart(2, "0");
const parseHex = (h) => [1, 3, 5].map((i) => parseInt(h.slice(i, i + 2), 16));

/** Colour for an altitude in feet (barometric, as sent). null / NaN -> null (caller keeps the source colour). */
export function altitudeColor(altFt) {
  const a = Number(altFt);
  if (altFt == null || !Number.isFinite(a)) return null;
  const q = Math.max(0, Math.min(ALT_STOPS[ALT_STOPS.length - 1][0], Math.round(a / ALT_BAND_FT) * ALT_BAND_FT));
  for (let i = 1; i < ALT_STOPS.length; i++) {
    const [a1, c1] = ALT_STOPS[i];
    if (q <= a1) {
      const [a0, c0] = ALT_STOPS[i - 1];
      const f = (q - a0) / (a1 - a0);
      const p = parseHex(c0), r = parseHex(c1);
      return "#" + p.map((v, k) => hex2(v + (r[k] - v) * f)).join("");
    }
  }
  return ALT_STOPS[ALT_STOPS.length - 1][1];
}

/** CSS linear-gradient for the legend, 0 to 45,000 ft. */
export function altitudeGradientCss() {
  const max = ALT_STOPS[ALT_STOPS.length - 1][0];
  return "linear-gradient(90deg," + ALT_STOPS.map(([a, c]) => `${c} ${Math.round((a / max) * 100)}%`).join(",") + ")";
}

// ---------- image ids ----------

/** Compose an image id. `color` may be "#rrggbb"; theme is "d" or "l". */
export function iconId(shape, color, theme = "d", state = "") {
  return shape + "|" + String(color).replace("#", "").toLowerCase() + "|" + (theme === "l" ? "l" : "d") + (state ? "|" + state : "");
}

/** Icon states: "h" = hovered (light halo behind the shape), "x" = stale (faded fill). Normal is "". */
export const STATES = ["", "h", "x"];
/** Like parseIconId, plus the state as a 4th element ("" when none or unknown). */
export function parseIconIdFull(id) {
  const [shape, hex, theme, st] = String(id).split("|");
  return [shape, "#" + (hex || "c8cacc"), theme === "l" ? "l" : "d", STATES.includes(st) ? st : ""];
}

/** Parse an image id into [shape, "#rrggbb", theme]. Two-part ids (older code) mean the dark theme. */
export function parseIconId(id) {
  const [shape, hex, theme] = String(id).split("|");
  return [shape, "#" + (hex || "c8cacc"), theme === "l" ? "l" : "d"];
}

// ---------- drawing ----------

const pathCache = new Map();
/** Path2D for an SVG path string (cached). */
function path2d(d) {
  let p = pathCache.get(d);
  if (!p) { p = new Path2D(d); pathCache.set(d, p); }
  return p;
}

function withAlpha(hex, a) {
  const [r, g, b] = parseHex(hex.length === 7 ? hex : "#c8cacc");
  return `rgba(${r},${g},${b},${a})`;
}

/** Draw a shape into a fresh canvas: translucent discs, body fill with a faint sheen, 1.2 px outline, detail, strokes. */
export function drawShape(shapeName, color, px = ICON_PX, theme = "d", state = "") {
  const sh = shapeOf(shapeName);
  const th = THEME[theme] || THEME.d;
  const c = document.createElement("canvas");
  c.width = c.height = px;
  const g = c.getContext("2d");
  const k = px / 64;                       // canvas px per unit
  g.translate(px / 2, px / 2);
  g.scale(k, k);
  g.lineJoin = "round";
  g.lineCap = "round";
  const outlineW = OUTLINE_PX / UNIT;      // 2.4 units = 1.2 CSS px
  const thin = 1.6;                        // thin colour strokes, units (0.8 CSS px)

  // course vector first (under everything): dashed, with a dark under-stroke for contrast
  if (sh.leader) {
    g.setLineDash([2.6, 2.4]);
    g.lineCap = "butt";
    g.strokeStyle = th.outline; g.lineWidth = 3.2; g.stroke(path2d(sh.leader));
    g.strokeStyle = color; g.lineWidth = 1.6; g.stroke(path2d(sh.leader));
    g.setLineDash([]);
    g.lineCap = "round";
  }
  for (const d of sh.disc || []) {          // rotor discs
    const p = path2d(d);
    g.fillStyle = withAlpha(color, 0.2); g.fill(p);
    g.strokeStyle = withAlpha(color, 0.85); g.lineWidth = 1.2; g.stroke(p);
  }
  for (const d of sh.ring || []) {          // status ring
    const p = path2d(d);
    g.strokeStyle = th.outline; g.lineWidth = 3.4; g.stroke(p);
    g.strokeStyle = withAlpha(color, 0.9); g.lineWidth = 1.7; g.stroke(p);
  }
  // thin strokes (propellers, antennas, blades): dark under-stroke, then colour
  for (const d of sh.lines || []) {
    const p = path2d(d);
    g.strokeStyle = th.outline; g.lineWidth = thin + 1.8; g.stroke(p);
  }
  for (const d of sh.lines || []) {
    g.strokeStyle = color; g.lineWidth = thin; g.stroke(path2d(d));
  }
  if (state === "h") {   // hovered: a light halo under the whole shape
    g.strokeStyle = theme === "l" ? "rgba(255,255,255,0.95)" : "rgba(255,255,255,0.85)"; g.lineWidth = outlineW + 5;
    for (const d of sh.body) g.stroke(path2d(d));
  }
  // body: flat fill (the same look as the SVG sprite in iconsvg.js), hairline outline. A stale icon is faded.
  if (state === "x") g.globalAlpha = 0.55;
  for (const d of sh.body) { g.fillStyle = color; g.fill(path2d(d)); }
  g.strokeStyle = th.outline;
  g.lineWidth = outlineW;
  for (const d of sh.body) g.stroke(path2d(d));
  g.strokeStyle = th.detail;
  g.lineWidth = 1.0;
  for (const d of sh.detail || []) g.stroke(path2d(d));
  g.globalAlpha = 1;
  return c;
}

/** Canvas to the {width,height,data} form MapLibre addImage accepts. */
export function asImage(c) {
  const img = c.getContext("2d").getImageData(0, 0, c.width, c.height);
  return { width: img.width, height: img.height, data: new Uint8Array(img.data) };
}

/** Register the lazy loader: MapLibre calls it for any image id it does not have yet. */
export function installLazyIcons(map) {
  map.on("styleimagemissing", (e) => {
    if (!String(e.id).includes("|") || map.hasImage(e.id)) return;
    const [shape, color, theme, state] = parseIconIdFull(e.id);
    try { map.addImage(e.id, asImage(drawShape(shape, color, ICON_PX, theme, state)), { pixelRatio: PIXEL_RATIO }); } catch { /* map gone */ }
  });
}

/** Paint a shape into a small DOM canvas (layer list, key) at device resolution. */
export function paintMini(canvas, shapeName, color, cssPx = 18, theme = "d") {
  const dpr = Math.min(3, window.devicePixelRatio || 1);
  const px = Math.round(cssPx * dpr);
  canvas.width = canvas.height = px;
  canvas.style.width = canvas.style.height = cssPx + "px";
  canvas.getContext("2d").drawImage(drawShape(shapeName, color, ICON_PX, theme), 0, 0, px, px);
}
