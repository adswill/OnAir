// badges/art.js - the badge icons: original vector art in the style of the map icons (shapes.js): flat fills, a hairline outline and thin
// detail strokes in a -32..32 box. Each badge is a notched medal in its tier's colour with a glyph on a dark disc. The aircraft, ship,
// sonde, APRS, satellite and contact glyphs ARE the map's own shapes (shapes.js), so a badge looks like the thing it is for.
// Pure string building (no DOM), covered by test/badges-ui.test.js.

import { SHAPES } from "../shapes.js";

export const TIER_COLOURS = { 0: "#59606a", 1: "#c47a3d", 2: "#b9c2cc", 3: "#e6b422", 4: "#8fd8e0" };

const circ = (cx, cy, r) => `M${cx - r},${cy}a${r},${r} 0 1,0 ${2 * r},0a${r},${r} 0 1,0 ${-2 * r},0Z`;
const star = (n, ro, ri, rot = -90) => {
  let d = "";
  for (let i = 0; i < n * 2; i++) {
    const r = i % 2 ? ri : ro, a = ((rot + (i * 180) / n) * Math.PI) / 180;
    d += (i ? "L" : "M") + (r * Math.cos(a)).toFixed(2) + "," + (r * Math.sin(a)).toFixed(2);
  }
  return d + "Z";
};

/** Glyphs drawn for the badges (body / detail / lines like shapes.js). */
export const GLYPHS = {
  spark: { body: [star(4, 20, 5, -90)], lines: ["M13,-13L16,-16M-13,13L-16,16M13,13L16,16M-13,-13L-16,-16"] },
  clock: { body: [circ(0, 0, 18)], detail: ["M0,0V-12M0,0L9,5", "M0,-15V-13M15,0H13M0,15V13M-15,0H-13"] },
  calendar: { body: ["M-17,-13H17V17H-17Z"], detail: ["M-17,-5H17", "M-9,2H-5M-1,2H3M7,2H11M-9,9H-5M-1,9H3"], lines: ["M-9,-18V-10M9,-18V-10"] },
  flame: { body: ["M0,-21C8,-11 15,-4 13,6C11,15 5,19 0,19C-5,19 -11,15 -13,6C-14,-2 -7,-6 -5,-15C-2,-9 2,-8 0,-21Z"], detail: ["M0,17C-4,17 -6,13 -5,9C-4,5 -1,4 0,0C3,5 5,8 4,12C3,15 2,17 0,17"] },
  range: { body: [circ(-12, 12, 4)], lines: ["M-12,12L14,-14", "M-12,-4A16,16 0 0,1 4,12", "M-12,-14A26,26 0 0,1 14,12"] },
  globe: { body: [circ(0, 0, 19)], detail: ["M-19,0H19M0,-19V19", "M0,-19C-10,-12 -10,12 0,19M0,-19C10,-12 10,12 0,19", "M-16,-9H16M-16,9H16"] },
  grid: { body: ["M-18,-18H18V18H-18Z"], detail: ["M-6,-18V18M6,-18V18M-18,-6H18M-18,6H18"], lines: [] },
  moon: { body: ["M5,-19A19,19 0 1,0 19,5A14,14 0 1,1 5,-19Z"], lines: ["M12,-14L13,-11L16,-10L13,-9L12,-6L11,-9L8,-10L11,-11Z"] },
  sun: { body: [circ(0, 4, 9)], lines: ["M0,-11V-17M10,-6L14,-10M-10,-6L-14,-10M14,4H20M-14,4H-20", "M-20,15H20"] },
  star: { body: [star(5, 20, 8.5)], detail: ["M0,-9V-2"] },
  modes: { body: [circ(-9, -9, 7), "M9,-17L17,-3H1Z", "M-16,2H-2V16H-16Z", "M9,1L17,9L9,17L1,9Z"] },
  acars: { body: ["M-18,-14H18V8H-2L-10,16V8H-18Z"], detail: ["M-12,-7H12M-12,-1H12M-12,5H4"] },
  df: { body: [circ(0, 0, 4)], lines: ["M-19,16L0,0L19,14", "M0,0L4,-20", "M-14,-16A21,21 0 0,1 14,-16"], detail: [] },
};
const FROM_MAP = { plane: "narrow", ship: "ship_cargo", sonde: "sonde", aprs: "aprs_digi", sat: "sat", ham: "ham_qso" };

function glyphOf(icon) { return GLYPHS[icon] || (FROM_MAP[icon] && SHAPES[FROM_MAP[icon]]) || GLYPHS.star; }

/** The notched medal outline (16 notches) in the -32..32 box. */
export const MEDAL = star(16, 30, 26.5, -90);
const ne = 'vector-effect="non-scaling-stroke"';

function glyphMarkup(sh, fill) {
  const out = [];
  for (const d of sh.disc || []) out.push(`<path d="${d}" fill="${fill}" fill-opacity=".25" stroke="${fill}" stroke-width="1.1" ${ne}/>`);
  for (const d of sh.ring || []) out.push(`<path d="${d}" fill="none" stroke="${fill}" stroke-width="1.5"/>`);
  for (const d of sh.lines || []) out.push(`<path d="${d}" fill="none" stroke="${fill}" stroke-width="2.6" stroke-linecap="round" stroke-linejoin="round"/>`);
  if (sh.body && sh.body.length) out.push(`<g fill="${fill}" stroke="#0b0c0d" stroke-width="1" stroke-linejoin="round" ${ne}>${sh.body.map((d) => `<path d="${d}"/>`).join("")}</g>`);
  for (const d of sh.detail || []) out.push(`<path d="${d}" fill="none" stroke="rgba(8,10,12,.6)" stroke-width="1.6" stroke-linecap="round"/>`);
  return out.join("");
}

/** One badge as a standalone inline SVG. tier 0 draws the locked (grey) medal. `title` becomes the accessible name. */
export function badgeSvg(icon, tier = 1, { size = 48, title = "" } = {}) {
  const t = Math.max(0, Math.min(4, tier | 0));
  const c = TIER_COLOURS[t];
  const glyph = glyphMarkup(glyphOf(icon), t ? "#f4f6f8" : "#9aa1a9");
  const esc = (s) => String(s).replace(/[&<>"]/g, (x) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" })[x]);
  const lbl = title ? `<title>${esc(title)}</title>` : "";
  const pips = t > 1 ? Array.from({ length: t }, (_, i) => `<circle cx="${(i - (t - 1) / 2) * 6}" cy="25.5" r="2.1" fill="#f4f6f8" stroke="#0b0c0d" stroke-width=".8"/>`).join("") : "";
  return `<svg class="badge-ico${t ? "" : " badge-locked"}" width="${size}" height="${size}" viewBox="-32 -32 64 64" role="img"${title ? "" : ' aria-hidden="true"'}>${lbl}` +
    `<path d="${MEDAL}" fill="${c}" stroke="#0b0c0d" stroke-width="1.2" stroke-linejoin="round" ${ne}/>` +
    `<circle r="22" fill="#141a22" stroke="rgba(255,255,255,.18)" stroke-width="1" ${ne}/>` +
    `<g transform="scale(.82)">${glyph}</g>${pips}</svg>`;
}
