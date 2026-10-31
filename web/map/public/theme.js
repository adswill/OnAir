// theme.js - colour scheme: follows prefers-color-scheme unless the visitor chose dark or light (the T key / the theme button, remembered).
// It tints the raster basemap to match, sets data-theme on <html> for the CSS, and tells the app when the theme flips so icons
// (outline colour) and label colours can follow.
const mq = window.matchMedia("(prefers-color-scheme: dark)");
const listeners = [];
let pref = "auto";   // "auto" | "dark" | "light"
try { const v = localStorage.getItem("onair.map.theme"); if (v === "dark" || v === "light") pref = v; } catch { /* storage blocked */ }

const isDark = () => (pref === "auto" ? mq.matches : pref === "dark");
function paintRoot() {
  const r = document.documentElement;
  if (pref === "auto") r.removeAttribute("data-theme"); else r.setAttribute("data-theme", pref);
  r.style.colorScheme = pref === "auto" ? "dark light" : pref;
}
paintRoot();

/** "d" on a dark map, "l" on a light one (the suffix of the sprite ids). */
export function themeKey() { return isDark() ? "d" : "l"; }
export function themePref() { return pref; }

export function applyMapTheme(map) {
  const dark = isDark();
  const set = (p, v) => { try { map.setPaintProperty("osm", p, v); } catch { /* style not ready */ } };
  set("raster-saturation", dark ? -0.38 : -0.55);
  set("raster-contrast", dark ? 0.08 : 0);
  set("raster-brightness-min", dark ? 0.08 : 0);
  set("raster-brightness-max", dark ? 0.88 : 1);
  try { map.setPaintProperty("grid", "line-color", dark ? "#6a6c70" : "#8a8f96"); } catch { /* ignore */ }
}

/** Call fn(themeKey) whenever the colour scheme changes. */
export function onThemeChange(fn) { listeners.push(fn); }

let theMap = null;
const flip = () => { if (theMap) applyMapTheme(theMap); for (const fn of listeners) fn(themeKey()); };

/** Choose "auto", "dark" or "light" (remembered). */
export function setThemePref(p) {
  pref = p === "dark" || p === "light" ? p : "auto";
  try { localStorage.setItem("onair.map.theme", pref); } catch { /* storage blocked */ }
  paintRoot();
  flip();
}
/** auto -> dark -> light -> auto. Returns the new preference. */
export function cycleTheme() { setThemePref(pref === "auto" ? "dark" : pref === "dark" ? "light" : "auto"); return pref; }

export function watchMapTheme(map) {
  theMap = map;
  if (map.isStyleLoaded()) applyMapTheme(map); else map.once("load", () => applyMapTheme(map));
  mq.addEventListener?.("change", () => { if (pref === "auto") flip(); });
}
