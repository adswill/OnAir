// iconsvg.js - the icon set as ONE inline SVG sprite (<symbol> per shape, generated from shapes.js) for everything outside the map canvas:
// layer list, track list, search results, legend, ham contacts. The map itself draws the same shapes through icons.js (canvas -> sprite atlas).
// One source (shapes.js), one style: flat fill in the track colour, a hairline outline that does not scale with the icon, thin detail strokes.
// Vector, so it is sharp at any size and on any pixel ratio; colours and states come from CSS (currentColor, --ico-line), so both themes work.
// Pure string building (no DOM), covered by test/iconsvg.test.js.

import { SHAPES } from "./shapes.js";

/** The legend: groups of [shape, label]. Every shape of shapes.js must be listed exactly once (a test enforces it). */
export const LEGEND = [
  ["Aircraft", [
    ["narrow", "Narrow-body jet"], ["wide", "Wide-body twin"], ["quad", "Four-engine"], ["regional", "Regional jet"], ["bizjet", "Business jet"],
    ["turboprop", "Turboprop"], ["light", "Light aircraft"], ["lighttwin", "Light twin"], ["heli", "Helicopter"], ["fighter", "Fighter"],
    ["delta", "Delta-wing fighter"], ["transport", "Military transport"], ["glider", "Glider"], ["balloon", "Balloon"], ["airship", "Airship"],
    ["plane", "Unknown aircraft"], ["obstacle", "Obstacle"],
  ]],
  ["Drones", [["drone", "Drone"], ["drone_rid", "Drone, Remote ID"], ["uav", "Fixed-wing UAV"]]],
  ["Ships (dashed line = course)", [
    ["ship_cargo", "Cargo"], ["ship_tanker", "Tanker"], ["ship_passenger", "Passenger / ferry"], ["ship_hsc", "High-speed craft"], ["ship_tug", "Tug"],
    ["ship_fishing", "Fishing"], ["ship_sailing", "Sailing"], ["ship_pleasure", "Pleasure craft"], ["ship_military", "Military"], ["ship_sar", "Search and rescue"],
    ["ship_pilot", "Pilot"], ["ship_dredger", "Dredger"], ["ship_other", "Other / unknown"], ["buoy", "Buoy"], ["dsc", "DSC coast station"],
  ]],
  ["Land", [
    ["vehicle", "Ground vehicle"], ["train", "Train"], ["aprs_car", "APRS vehicle"], ["aprs_truck", "APRS truck"], ["aprs_house", "APRS home"],
    ["aprs_wx", "APRS weather"], ["aprs_digi", "APRS digipeater"], ["aprs_balloon", "APRS balloon"], ["aprs_ship", "APRS boat"], ["aprs", "APRS other"],
  ]],
  ["Radio", [
    ["ham", "Amateur station"], ["ham_qso", "Amateur contact"], ["station", "OnAir station"], ["cell", "Base station"], ["tower", "Tower"],
    ["pager", "Pager"], ["wx", "Weather"],
  ]],
  ["Sky and space", [["sonde", "Radiosonde"], ["sat", "Satellite"], ["sat_gnss", "GNSS satellite"]]],
];

const LINE = "var(--ico-line,#0b0c0d)";
const DETAIL = "var(--ico-detail,rgba(8,10,12,.55))";
const ne = 'vector-effect="non-scaling-stroke"';

/** The inner markup of one symbol. Colour = currentColor. */
export function symbolInner(sh) {
  const out = [];
  if (sh.leader) out.push(`<path d="${sh.leader}" fill="none" stroke="currentColor" stroke-width="1.6" stroke-dasharray="2.6 2.4"/>`);
  for (const d of sh.disc || []) out.push(`<path d="${d}" fill="currentColor" fill-opacity=".2" stroke="currentColor" stroke-opacity=".85" stroke-width="1.2" ${ne}/>`);
  for (const d of sh.ring || []) out.push(`<path d="${d}" fill="none" stroke="currentColor" stroke-opacity=".9" stroke-width="1.7"/>`);
  for (const d of sh.lines || []) out.push(`<path d="${d}" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round"/>`);
  out.push(`<g fill="currentColor" stroke="${LINE}" stroke-width="1.15" stroke-linejoin="round" ${ne}>${sh.body.map((d) => `<path d="${d}"/>`).join("")}</g>`);
  for (const d of sh.detail || []) out.push(`<path d="${d}" fill="none" stroke="${DETAIL}" stroke-width="1" stroke-linecap="round" ${ne}/>`);
  return out.join("");
}

/** `<svg>` with a <symbol id="i-NAME"> for every shape. Put it once in the page. */
export function spriteSvg() {
  const syms = Object.keys(SHAPES).map((n) => `<symbol id="i-${n}" viewBox="-32 -32 64 64">${symbolInner(SHAPES[n])}</symbol>`).join("");
  return `<svg xmlns="http://www.w3.org/2000/svg" width="0" height="0" style="position:absolute" aria-hidden="true" focusable="false"><defs>${syms}</defs></svg>`;
}

/** An icon from the sprite. `rotate` turns a heading-aware shape (degrees); `state`: "" | "stale" | "sel". */
export function iconSvg(shape, { color = "currentColor", size = 18, rotate = 0, state = "", cls = "" } = {}) {
  const name = SHAPES[shape] ? shape : "plane";
  const rot = SHAPES[name].rotates && rotate ? `transform:rotate(${Math.round(rotate)}deg);` : "";
  const c = String(color).replace(/[^#a-zA-Z0-9(),.% -]/g, "");
  return `<svg class="ico${state ? " ico-" + state : ""}${cls ? " " + cls : ""}" width="${size}" height="${size}" viewBox="0 0 64 64" style="${rot}color:${c}" aria-hidden="true"><use href="#i-${name}"/></svg>`;
}
