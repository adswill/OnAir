// app.js - OnAir live map: MapLibre map, layers, live feed (WebSocket + /live), search, stats, selection.
// Modules: geo.js (maths), types.js (type -> icon shape), shapes.js + icons.js (map icons), airports.js,
// card.js (info card), demo.js (?demo=1 preview data). Everything here is browser-only glue.

import { iconId, installLazyIcons, paintMini, altitudeColor, altitudeGradientCss, ALT_STOPS } from "./icons.js";
import { SHAPES } from "./shapes.js";
import { iconShape, vesselScale } from "./types.js";
import { createCard } from "./card.js";
import { dfFeatures } from "./df.js";
import { createSheet } from "./sheet.js";
import { generateDemo, stepDemo } from "./demo.js";
import { watchMapTheme, themeKey, onThemeChange } from "./theme.js";
import { mergeTrails, appendLive, toLine } from "./trail.js";
import { cellsToGeo, LEVEL_COLOURS } from "./jamlayer.js";
import { clusterPoints, shouldCluster } from "./cluster.js";
import { searchTracks as rankTracks, searchUsers, byMatches } from "./search.js";
import { initConsent, photoAllowed } from "./consent-ui.js";
import { initHam } from "./ham-ui.js";

const DEMO = new URLSearchParams(location.search).get("demo") === "1";
// ?view=lat,lon,zoom opens the map there (used for screenshots and shared links)
const VIEW = (new URLSearchParams(location.search).get("view") || "").split(",").map(Number);
const HAS_VIEW = VIEW.length === 3 && VIEW.every(Number.isFinite);
const REDUCED = window.matchMedia("(prefers-reduced-motion: reduce)").matches;
const $ = (id) => document.getElementById(id);

const KINDS = [
  { id: "aircraft", name: "Aircraft", color: "#e6e8ea", shape: "narrow" },
  { id: "vessel", name: "Ships", color: "#8cb8eb", shape: "ship_cargo" },
  { id: "sonde", name: "Sondes", color: "#cc94c7", shape: "sonde" },
  { id: "aprs", name: "APRS", color: "#8ccc9e", shape: "aprs_car" },
  { id: "ham", name: "Amateur", color: "#8ccc9e", shape: "ham" },
  { id: "drone", name: "Drones", color: "#e0ad6b", shape: "drone_rid" },
  { id: "flarm", name: "FLARM", color: "#6ccc7a", shape: "glider" },
  { id: "sat", name: "Satellites", color: "#c0c2c4", shape: "sat" },
  { id: "buoy", name: "Buoys", color: "#7aa8d4", shape: "buoy" },
  { id: "dsc", name: "DSC", color: "#e0ad6b", shape: "dsc" },
  { id: "station", name: "Stations", color: "#e0d48f", shape: "station" },
  { id: "gnss", name: "GNSS satellites", color: "#9ad0f0", shape: "sat_gnss" },
  { id: "doa", name: "DF bearings", color: "#ff9a4d", shape: "station" },
  { id: "fix", name: "DF fixes", color: "#ff5a5a", shape: "station" },
];

const on = Object.fromEntries(KINDS.map((k) => [k.id, true]));
const kindOf = Object.fromEntries(KINDS.map((k) => [k.id, k]));
let tracks = new Map();
let byFilter = "";   // lower-case user name while the map shows only that user's icons ("" = everyone)
let selected = null;
let liveOk = false;
let everLive = false;
let stations = 0;
let serverLoad = null;
let query = "";
let followId = null;      // id being followed (map kept centred)
let showTrack = true;     // draw the selected track's trail (remembered across reloads)
try { if (localStorage.getItem("onair.map.showTrack") === "0") showTrack = false; } catch { /* storage blocked */ }
let demoMeta = {};
let altMode = false;      // colour aircraft by altitude (remembered)
try { altMode = localStorage.getItem("onair.map.altColor") === "1"; } catch { /* storage blocked */ }

const map = new maplibregl.Map({
  container: "map",
  style: {
    version: 8,
    glyphs: "https://demotiles.maplibre.org/font/{fontstack}/{range}.pbf",
    sources: {
      osm: {
        type: "raster",
        tiles: [DEMO && !/on-air\.dev$/.test(location.hostname) ? "https://tile.openstreetmap.org/{z}/{x}/{y}.png" : location.origin + "/tiles/{z}/{x}/{y}.png"],
        tileSize: 256,
        attribution: "© OpenStreetMap",
        maxzoom: 19,
      },
      grid: { type: "geojson", data: graticule() },
    },
    layers: [
      { id: "osm", type: "raster", source: "osm", paint: {
        "raster-saturation": -0.38,
        "raster-contrast": 0.08,
        "raster-brightness-min": 0.08,
        "raster-brightness-max": 0.88,
      } },
      { id: "grid", type: "line", source: "grid", paint: {
        "line-color": "#6a6c70",
        "line-opacity": 0.22,
        "line-width": 0.6,
      } },
    ],
  },
  center: HAS_VIEW ? [VIEW[1], VIEW[0]] : [12, 28],
  zoom: HAS_VIEW ? VIEW[2] : 2.15,
  minZoom: 1.2,
  fadeDuration: 0,
  attributionControl: false,
});
map.addControl(new maplibregl.NavigationControl({ showCompass: false, visualizePitch: false }), "bottom-right");
installLazyIcons(map);
if (DEMO) window.__onairMap = map;   // handle for screenshots / debugging of the demo

function graticule() {
  const features = [];
  for (let lon = -180; lon <= 180; lon += 15) {
    features.push({ type: "Feature", geometry: { type: "LineString", coordinates: [[lon, -85], [lon, 85]] } });
  }
  for (let lat = -75; lat <= 75; lat += 15) {
    const line = [];
    for (let lon = -180; lon <= 180; lon += 5) line.push([lon, lat]);
    features.push({ type: "Feature", geometry: { type: "LineString", coordinates: line } });
  }
  return { type: "FeatureCollection", features };
}

const SRC = {
  adsb: { color: "#e6e8ea", stale: 30 },
  ais: { color: "#40b0e0", stale: 90 },
  sat: { color: "#5096dc", stale: 60 },
  acars: { color: "#dc4848", stale: 30 },
  hfdl: { color: "#e6c640", stale: 30 },
  flarm: { color: "#6ccc7a", stale: 30 },
  aprs: { color: "#c87848", stale: 120 },
  sonde: { color: "#a878dc", stale: 900 },
  dsc: { color: "#e87840", stale: 600 },
};
const SRC_RANK = { adsb: 40, ais: 38, flarm: 35, acars: 30, hfdl: 28, aprs: 25, ham: 25, sat: 22, sonde: 20, dsc: 18, buoy: 12 };
const STALE = "#969a9e";

function srcOf(t) {
  const s = (t.src || "").toLowerCase();
  if (s && SRC[s]) return s;
  const sub = (t.sub || "").toLowerCase();
  if (t.kind === "flarm" || sub.includes("flarm")) return "flarm";
  if (t.kind === "vessel" || sub.includes("ais")) return "ais";
  if (sub.includes("hfdl")) return "hfdl";
  if (sub.includes("vdl") || sub.includes("acars")) return "acars";
  if (sub.includes("aprs")) return "aprs";
  if (t.kind === "sonde" || sub.includes("sonde")) return "sonde";
  if (t.kind === "dsc" || sub.includes("dsc")) return "dsc";
  if (sub.includes("aero") || sub.includes("inmarsat") || sub.includes("ads-c")) return "sat";
  if (t.kind === "aircraft") return "adsb";
  return "";
}

function ageSec(t) {
  if (Number.isFinite(t.age)) return t.age;
  return (Date.now() - t.t) / 1000;
}

function positionTimeMs(t) {
  const a = Number(t.age);
  if (Number.isFinite(a) && a > 0) return t.t - a * 1000;
  return t.t;
}

// GNSS constellations, by the "sub" the app sends
const GNSS_COLORS = { gps: "#5aa0f0", galileo: "#f0b840", beidou: "#e86060", glonass: "#d070e0", qzss: "#50d0a0", sbas: "#a8b0b8" };

function colorOf(kind, t) {
  if (kind === "gnss") {
    if (t && ageSec(t) > 60) return STALE;
    return GNSS_COLORS[String((t && t.sub) || "").toLowerCase()] || (kindOf.gnss || {}).color;
  }
  if (t) {
    const src = srcOf(t);
    if (src) {
      const spec = SRC[src];
      if (ageSec(t) > spec.stale) return STALE;
      if (altMode && kind === "aircraft") {
        if (t.alt != null && t.alt <= 100 && (t.speed == null || t.speed < 40)) return "#9aa0a6";   // on the ground
        const c = altitudeColor(t.alt);
        if (c) return c;
      }
      return spec.color;
    }
  }
  return (kindOf[kind] || {}).color || "#c8cacc";
}

/** ACARS / VDL2 (and same ingest source) stay a dot until we can point the icon (reported or trail heading). */
function datalinkDotUntilHeading(t, hdg) {
  if (t.kind !== "aircraft") return false;
  const src = srcOf(t);
  return src === "acars" && hdg == null;
}

function pathDistanceM(lon1, lat1, lon2, lat2) {
  const p = Math.PI / 180;
  const dLat = (lat2 - lat1) * p;
  const dLon = (lon2 - lon1) * p;
  const a = Math.sin(dLat / 2) ** 2
    + Math.cos(lat1 * p) * Math.cos(lat2 * p) * Math.sin(dLon / 2) ** 2;
  return 6371000 * 2 * Math.asin(Math.min(1, Math.sqrt(a)));
}

/** Bearing degrees true north from (lon1,lat1) to (lon2,lat2). */
function bearingDeg(lon1, lat1, lon2, lat2) {
  const p = Math.PI / 180;
  const dLon = (lon2 - lon1) * p;
  const y = Math.sin(dLon) * Math.cos(lat2 * p);
  const x = Math.cos(lat1 * p) * Math.sin(lat2 * p) - Math.sin(lat1 * p) * Math.cos(lat2 * p) * Math.cos(dLon);
  return (Math.atan2(y, x) / p + 360) % 360;
}

// Ignore sub-metre jitter; any real trail step (ACARS / VDL2) sets the icon direction.
const MOVE_BEARING_MIN_M = 15;

/** Reported heading when present; else the direction the aircraft is flying (from the trail). */
function effectiveHeading(t) {
  if (t.heading != null && Number.isFinite(t.heading)) return t.heading;
  if (!Number.isFinite(t.lon) || !Number.isFinite(t.lat)) return null;
  const path = Array.isArray(t.path) ? t.path.slice() : [];
  const lon2 = t.lon, lat2 = t.lat;
  if (!path.length || path[path.length - 1][0] !== lon2 || path[path.length - 1][1] !== lat2) path.push([lon2, lat2]);
  if (path.length < 2) return null;
  for (let i = path.length - 2; i >= 0; i--) {
    const [lon1, lat1] = path[i];
    const d = pathDistanceM(lon1, lat1, lon2, lat2);
    if (d >= MOVE_BEARING_MIN_M) return bearingDeg(lon1, lat1, lon2, lat2);
  }
  const [lon0, lat0] = path[0];
  const dTot = pathDistanceM(lon0, lat0, lon2, lat2);
  if (dTot >= MOVE_BEARING_MIN_M) return bearingDeg(lon0, lat0, lon2, lat2);
  return null;
}

/** Great-circle point `nm` nautical miles from (lon, lat) along true heading. */
function aheadPoint(lon, lat, headingDeg, nm) {
  const rad = Math.PI / 180, r = nm / 3440.065, h = headingDeg * rad, p1 = lat * rad;
  const lat2 = Math.asin(Math.sin(p1) * Math.cos(r) + Math.cos(p1) * Math.sin(r) * Math.cos(h)) / rad;
  const lon2 = lon + Math.atan2(Math.sin(h) * Math.sin(r) * Math.cos(p1), Math.cos(r) - Math.sin(p1) * Math.sin(lat2 * rad)) / rad;
  return [lon2, lat2];
}

const EXTRAP_MAX_SEC = {
  adsb: 40, acars: 120, hfdl: 120, sat: 90, flarm: 45, ais: 90, default: 60,
};

function extrapMaxSec(t) {
  const s = srcOf(t);
  return EXTRAP_MAX_SEC[s] || EXTRAP_MAX_SEC.default;
}

function effectiveSpeedKts(t) {
  if (Number.isFinite(t.speed) && t.speed >= 8) return t.speed;
  if (t._prevPos && Number.isFinite(t._prevPosMs)) {
    const dt = (positionTimeMs(t) - t._prevPosMs) / 1000;
    if (dt >= 3 && dt < 900) {
      const [lon1, lat1] = t._prevPos;
      const dM = pathDistanceM(lon1, lat1, t.lon, t.lat);
      const kts = (dM / 1852) / (dt / 3600);
      if (kts >= 8 && kts < 900) return kts;
    }
  }
  return null;
}

/** Where the icon should be drawn now (dead reckoning between fixes). */
function displayLonLat(t, nowMs = Date.now()) {
  const hdg = effectiveHeading(t);
  const spd = effectiveSpeedKts(t);
  if (hdg == null || spd == null) return [t.lon, t.lat];
  const posMs = positionTimeMs(t);
  let dt = (nowMs - posMs) / 1000;
  if (!Number.isFinite(dt) || dt < 0) dt = 0;
  dt = Math.min(dt, extrapMaxSec(t));
  const nm = spd * (dt / 3600);
  if (nm < 0.0005) return [t.lon, t.lat];
  return aheadPoint(t.lon, t.lat, hdg, nm);
}

function destPoint(t, fromLonLat) {
  const hdg = effectiveHeading(t);
  const spd = effectiveSpeedKts(t);
  if (hdg == null || spd == null || spd < 20) return null;
  const [lon, lat] = fromLonLat || displayLonLat(t);
  const nm = spd * (2 / 60);
  return aheadPoint(lon, lat, hdg, nm);
}


/** Shape name (and colour) for a track; the image id "shape|colour|theme" is created lazily by installLazyIcons. */
function shapeFor(t) {
  const ity = (t.meta && t.meta.icaoType) || "";
  // aircraft with neither a type nor a category are guessed from speed and altitude: re-evaluate when those change a lot
  const guess = t.kind === "aircraft" && !ity && !t.cat ? Math.round((t.alt || 0) / 6000) + "/" + Math.round((t.speed || 0) / 80) : "";
  const key = t.kind + "|" + (t.cat || "") + "|" + ity + "|" + (t.extra || "") + "|" + (t.sub || "") + "|" + guess;
  if (t._shapeKey !== key) {
    t._shapeKey = key;
    t._shape = iconShape({ kind: t.kind, cat: t.cat, extra: t.extra, sub: t.sub, label: t.label, src: t.src, icaoType: ity, hex: icaoOf(t),
      wake: t.meta && t.meta.wake, speed: t.speed, alt: t.alt });
    t._scale = t.kind === "vessel" ? vesselScale({ shape: t._shape, cat: t.cat, extra: t.extra, sub: t.sub }) : 1;
  }
  return t._shape;
}
function shapeRotates(shape) { return !!(SHAPES[shape] && SHAPES[shape].rotates); }

/** Course for drawing: eases toward the reported one by the shortest arc, so icons turn smoothly instead of jumping. */
function easedHeading(t, hdg) {
  if (hdg == null) return 0;
  if (REDUCED || t._dh == null) { t._dh = hdg; return hdg; }
  const d = ((hdg - t._dh + 540) % 360) - 180;
  if (Math.abs(d) < 0.5) t._dh = hdg; else t._dh = (t._dh + d * 0.4 + 360) % 360;
  return t._dh;
}

/** The only way tracks reach the renderer. A setData while the previous one is still being tiled cancels those tile
 * loads (they end up errored and their icons vanish until the next quiet moment), so rebuilds are coalesced:
 * at most one in flight, the latest state is pushed as soon as it lands. */
let liveBusy = false, liveDirty = false, liveBusyTimer = 0;
function pushLive() {
  const src = map.getSource("live");
  if (!src) return;
  if (liveBusy) { liveDirty = true; return; }
  liveBusy = true;
  clearTimeout(liveBusyTimer);
  liveBusyTimer = setTimeout(liveIdle, 1500);   // safety: never stay blocked
  src.setData(geojson());
}
function liveIdle() {
  clearTimeout(liveBusyTimer);
  liveBusy = false;
  if (liveDirty) { liveDirty = false; pushLive(); }
}
map.on("sourcedata", (e) => { if (e.sourceId === "live" && liveBusy && map.isSourceLoaded("live")) liveIdle(); });

function geojson() {
  const features = [];
  const theme = themeKey();
  for (const t of tracks.values()) {
    if (!on[t.kind] || t.kind === "doa" || t.kind === "fix") continue;   // drawn by the DF layers
    if (byFilter && !byMatches(t, byFilter)) continue;
    const hdg = effectiveHeading(t);
    const dot = datalinkDotUntilHeading(t, hdg);
    const shape = shapeFor(t);
    const color = colorOf(t.kind, t);
    features.push({
      type: "Feature",
      id: t.id,
      properties: {
        id: t.id,
        kind: t.kind,
        icon: iconId(shape, color, theme),
        label: t.label || "",
        color,
        heading: dot ? 0 : easedHeading(t, hdg),
        scale: t._scale || 1,
        sym: dot ? 0 : 1,
        dot: dot ? 1 : 0,
        // a heading derived from position reports (dv) at walking pace is noise: do not turn the icon on it
        iconRot: shapeRotates(shape) && hdg != null && !dot && !(t.dv && !(t.speed >= 8)) ? 1 : 0,
        sel: t.id === selected ? 1 : 0,
        est: t.est ? 1 : 0,   // an estimated (dead-reckoned) position: drawn as a ghost
      },
      geometry: { type: "Point", coordinates: displayLonLat(t) },
    });
  }
  return { type: "FeatureCollection", features: clusterFeatures(features) };
}

/** Zoomed out with many tracks: dense cells become one counted bubble, the rest stay single icons.
 * Bubbles live in the SAME source as the icons (property cl=1), so one setData swaps both atomically: a separate
 * clusters source was reloaded on every rebuild and its tiles kept ending up errored, so bubbles and icons flickered. */
function clusterFeatures(features) {
  const zoom = map.getZoom();
  if (!shouldCluster(features.length, zoom)) return features;
  const pts = features.map((f) => ({ id: f.properties.id, kind: f.properties.kind, lon: f.geometry.coordinates[0], lat: f.geometry.coordinates[1] }));
  const { clusters, singles } = clusterPoints(pts, zoom, { keep: (id) => id === selected });
  const keepIds = new Set(singles.map((p) => p.id));
  const bubbles = clusters.map((c) => ({
    type: "Feature", properties: { cl: 1, n: c.n, label: c.n >= 1000 ? Math.round(c.n / 100) / 10 + "k" : String(c.n) },
    geometry: { type: "Point", coordinates: [c.lon, c.lat] },
  }));
  return features.filter((f) => keepIds.has(f.properties.id)).concat(bubbles);
}

function trailGeo() {
  const g = trailGeoSelected();
  g.features.push(...dfFeatures(tracks, on, Date.now()));
  return g;
}

function trailGeoSelected() {
  const features = [];
  if (!selected) return { type: "FeatureCollection", features };
  const t = tracks.get(selected);
  if (!t || !on[t.kind] || t.kind === "gnss") return { type: "FeatureCollection", features };
  const [dlon, dlat] = displayLonLat(t);
  // t.tpts = server history merged with the fixes this page saw (see fetchTrail); t.path is the short fallback
  const path = t.tpts && t.tpts.length >= 2 ? toLine(t.tpts) : Array.isArray(t.path) ? t.path.slice() : [];
  if (path.length && (path[path.length - 1][0] !== dlon || path[path.length - 1][1] !== dlat)) path.push([dlon, dlat]);
  else if (!path.length) path.push([dlon, dlat]);
  if (showTrack && path.length >= 2) {
    features.push({
      type: "Feature",
      properties: { id: t.id, color: colorOf(t.kind, t), kind: "trail" },
      geometry: { type: "LineString", coordinates: path },
    });
  }
  const d = destPoint(t, [dlon, dlat]);
  if (d) {
    features.push({
      type: "Feature",
      properties: { id: t.id, color: colorOf(t.kind, t), kind: "pred" },
      geometry: { type: "LineString", coordinates: [[dlon, dlat], d] },
    });
  }
  return { type: "FeatureCollection", features };
}

function mergePathClient(prev, t) {
  if (prev && positionTimeMs(t) + 500 < positionTimeMs(prev)) return prev;
  const cur = Array.isArray(t.path) ? t.path.slice() : [];
  const old = prev && Array.isArray(prev.path) ? prev.path.slice() : [];
  let path = cur.length >= 2 ? cur : (old.length ? old : cur);
  if (old.length > path.length) path = old;
  const last = path[path.length - 1];
  if (!last || last[0] !== t.lon || last[1] !== t.lat) path.push([t.lon, t.lat]);
  t.path = path.slice(-80);
  const hdg = effectiveHeading({ ...t, path, heading: t.heading });
  if (hdg != null) t.heading = hdg;
  if (t.heading == null && prev && prev.heading != null) t.heading = prev.heading;
  if (t.speed == null && prev && prev.speed != null) { t.speed = prev.speed; if (prev.dv) t.dv = 1; }
  if (prev && prev.meta && !t.meta) t.meta = prev.meta;
  // first sighting (this page) feeds the "first seen" and departure estimate on the card
  t.first = (prev && prev.first) || t.first || { ms: positionTimeMs(t), altFt: t.kind === "aircraft" ? t.alt : null, lat: t.lat, lon: t.lon };
  if (prev) {
    const prevR = SRC_RANK[srcOf(prev)] || 10;
    const incR = SRC_RANK[srcOf(t)] || 10;
    if (incR < prevR) {
      if (prev.label && !t.label) t.label = prev.label;
      t.sub = prev.sub || t.sub;
      t.src = prev.src || t.src;
    }
    if (prev.squawk && !t.squawk) t.squawk = prev.squawk;
    if (prev.extra && !t.extra) t.extra = prev.extra;
    if (prev.cat && !t.cat) t.cat = prev.cat;
  }
  // carry the timestamped trail forward and add this fix (dedupes against what the server already holds)
  t.tpts = prev && prev.tpts ? prev.tpts : [];
  t.tpts = appendLive(t.tpts, positionTimeMs(t), t.lat, t.lon, t.alt, t.heading);
  const moved = !prev || prev.lat !== t.lat || prev.lon !== t.lon;
  if (moved && prev) {
    t._prevPos = [prev.lon, prev.lat];
    t._prevPosMs = positionTimeMs(prev);
  } else if (prev) {
    t._prevPos = prev._prevPos;
    t._prevPosMs = prev._prevPosMs;
  }
  return t;
}

let upsertPending = null;
function upsert(list) {
  if (!upsertPending) upsertPending = [];
  upsertPending.push(...list);
  if (upsertPending._scheduled) return;
  upsertPending._scheduled = true;
  requestAnimationFrame(() => {
    const batch = upsertPending;
    upsertPending = null;
    for (const t of batch) tracks.set(t.id, mergePathClient(tracks.get(t.id), t));
    apply();
  });
}

function icaoOf(t) {
  const m = String(t.id || "").match(/:([0-9A-Fa-f]{6})$/);
  if (m) return m[1].toUpperCase();
  const sub = String(t.sub || "");
  if (/^[0-9A-Fa-f]{6}$/.test(sub)) return sub.toUpperCase();
  return "";
}

function callsignOf(t) {
  return String(t.label || "").toUpperCase().replace(/[^A-Z0-9]/g, "").slice(0, 8);
}

function cap(s) { return s ? s.charAt(0).toUpperCase() + s.slice(1) : s; }

function fmtAlt(t) {
  if (t.kind === "sat" || t.kind === "gnss") return (t.alt > 50 ? t.alt.toFixed(0) : t.alt.toFixed(1)) + " km";
  if (t.kind === "aircraft") return Math.round(t.alt) + " ft";
  return Math.round(t.alt) + " m";
}

function fmtSpeed(t) {
  if (t.kind === "aircraft" || t.kind === "flarm") return (t.dv ? "~" : "") + Math.round(t.speed) + " kt";
  if (t.kind === "vessel") return Number(t.speed).toFixed(1) + " kn";
  return Number(t.speed).toFixed(1);
}

function fmtLat(lat) {
  const a = Math.abs(lat).toFixed(4);
  return (lat >= 0 ? a + " N" : a + " S");
}
function fmtLon(lon) {
  const a = Math.abs(lon).toFixed(4);
  return (lon >= 0 ? a + " E" : a + " W");
}

function age(ms) {
  if (!Number.isFinite(ms) || ms < 0) return "—";
  const s = Math.round(ms / 1000);
  if (s < 60) return s + "s";
  if (s < 3600) return Math.round(s / 60) + "m";
  return Math.round(s / 3600) + "h";
}

function esc(s) {
  return String(s).replace(/[&<>"']/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
}


// ---------- layers, list, stats ----------

const miniCache = new Map();
/** Data URL of a small icon (cached per shape+colour) so lists use <img> instead of redrawing canvases. */
function miniUrl(shape, color) {
  const k = shape + color + themeKey();
  let u = miniCache.get(k);
  if (!u) {
    const c = document.createElement("canvas");
    paintMini(c, shape, color, 18, themeKey());
    u = c.toDataURL();
    miniCache.set(k, u);
  }
  return u;
}

watchMapTheme(map);

let layersBuilt = false;
/** Build the layer toggles once; later calls only refresh the counts (keeps focus and hover state). */
function renderLayers() {
  const counts = {};
  for (const t of tracks.values()) counts[t.kind] = (counts[t.kind] || 0) + 1;
  const el = $("layers");
  if (!layersBuilt) {
    layersBuilt = true;
    el.innerHTML = KINDS.map((k) => `<label data-k="${k.id}">
      <input type="checkbox" data-k="${k.id}" checked aria-label="${k.name}">
      <img class="ico" alt="" src="${miniUrl(k.shape, k.color)}" width="18" height="18">
      <span>${k.name}</span>
      <span class="n" data-n="${k.id}">0</span>
    </label>`).join("");
    for (const box of el.querySelectorAll("input")) {
      box.onchange = () => { on[box.dataset.k] = box.checked; box.closest("label").classList.toggle("off", !box.checked); apply(); };
    }
  }
  for (const k of KINDS) {
    const n = el.querySelector(`[data-n="${k.id}"]`);
    const v = String(counts[k.id] || 0);
    if (n && n.textContent !== v) n.textContent = v;
  }
}

function matchText(t, q) {
  const m = t.meta || {};
  return [t.label, t.id, t.sub, m.registration, m.flightNumber, m.operator].filter(Boolean).join(" ").toLowerCase().includes(q);
}

function visibleTracks() {
  const q = query.trim().toLowerCase();
  const out = [];
  for (const t of tracks.values()) {
    if (!on[t.kind]) continue;
    if (byFilter && !byMatches(t, byFilter)) continue;
    if (q && !matchText(t, q)) continue;
    out.push(t);
  }
  out.sort((a, b) => (a.kind + a.label).localeCompare(b.kind + b.label));
  return out;
}

let listTimer = 0;
/** The rail list can hold hundreds of rows, so it re-renders at most once a second. */
function renderListSoon() {
  if (listTimer) return;
  listTimer = setTimeout(() => { listTimer = 0; renderList(); }, 1000);
}

function renderList() {
  const list = visibleTracks();
  $("trackn").textContent = String(list.length);
  const el = $("list");
  if (!list.length) {
    el.innerHTML = `<div class="empty">${tracks.size ? "No tracks match this filter." : (liveOk ? "Waiting for reports…" : "Connecting…")}</div>`;
    return;
  }
  const now = Date.now();
  el.innerHTML = list.slice(0, 200).map((t) => `
    <button type="button" data-id="${esc(t.id)}" class="${t.id === selected ? "sel" : ""}">
      <img class="sym" alt="" src="${miniUrl(shapeFor(t), colorOf(t.kind, t))}" width="18" height="18" loading="lazy">
      <span class="lab">${esc(t.label || t.id)}</span>
      <span class="age">${esc(age(now - t.t))}</span>
      <span class="sub">${esc(cap(t.kind) + (t.sub ? " · " + t.sub : ""))}</span>
    </button>`).join("");
  for (const b of el.querySelectorAll("button")) b.onclick = () => focusTrack(b.dataset.id);
}

function renderLoad() {
  const el = $("load");
  const fill = $("loadFill");
  const pctEl = $("loadPct");
  if (!el || !fill || !pctEl) return;
  if (!liveOk || !serverLoad || serverLoad.pct == null) { el.hidden = true; return; }
  el.hidden = false;
  const pct = Math.max(0, Math.min(100, Number(serverLoad.pct) || 0));
  fill.style.width = pct + "%";
  pctEl.textContent = pct + "%";
  el.classList.toggle("warn", pct >= 55 && pct < 82);
  el.classList.toggle("hot", pct >= 82);
  el.title = [
    "Estimated hub capacity (not CPU). 100% = very busy; ingests may be slowed.",
    "Load " + pct + "%",
    serverLoad.viewers != null ? serverLoad.viewers + " viewers" : "",
    serverLoad.stations != null ? serverLoad.stations + " stations (30m)" : "",
    serverLoad.tracks != null ? serverLoad.tracks + " tracks stored" : "",
    serverLoad.ingestPerMin != null ? serverLoad.ingestPerMin + " ingests/min" : "",
    serverLoad.throttlePerMin ? serverLoad.throttlePerMin + " rate-limits/min" : "",
  ].filter(Boolean).join(" · ");
}

/** Menu bar stats: link pill (stations, viewers), one chip per layer with tracks, load meter. */
function renderStats() {
  const pill = $("link");
  pill.classList.toggle("ok", liveOk);
  const viewers = serverLoad && serverLoad.viewers != null ? serverLoad.viewers : null;
  if (liveOk) everLive = true;
  const label = DEMO ? "demo" : liveOk ? (stations ? stations + " station" + (stations === 1 ? "" : "s") : "live") : everLive ? "reconnecting" : "connecting";
  pill.querySelector("span").textContent = label;
  pill.title = DEMO ? "Synthetic demo data" : [liveOk ? "Connected" : "Not connected", stations ? stations + " stations sending (30 min)" : "", viewers != null ? viewers + " viewers" : ""].filter(Boolean).join(" · ");
  const counts = {};
  for (const t of tracks.values()) counts[t.kind] = (counts[t.kind] || 0) + 1;
  const chips = KINDS.filter((k) => counts[k.id]).map((k) =>
    `<span class="chip" title="${esc(k.name)}"><img alt="" src="${miniUrl(k.shape, k.color)}" width="14" height="14">${counts[k.id].toLocaleString()}<span class="chip-n"> ${esc(k.name.toLowerCase())}</span></span>`);
  const html = chips.length ? chips.join("") : `<span class="chip-none">${liveOk ? "no reports" : "…"}</span>`;
  const box = $("counts");
  if (box._html !== html) { box.innerHTML = html; box._html = html; }
  const v = $("viewers");
  if (v) { v.hidden = viewers == null; v.textContent = viewers != null ? viewers + (viewers === 1 ? " viewer" : " viewers") : ""; }
  renderLoad();
}

// ---------- apply / upsert ----------

function apply() {
  pushLive();
  const tr = map.getSource("trails");
  if (tr) tr.setData(trailGeo());
  renderLayers();
  renderListSoon();
  renderStats();
  $("hint").classList.toggle("hide", tracks.size > 0);
  $("overlay").hidden = tracks.size > 0 || liveOk || DEMO;
  if (selected && !tracks.has(selected)) clearSelection();
  tryHash();
}


// ---------- aircraft metadata (route, type, registration) ----------

const metaCache = new Map();
const metaPending = new Set();

/** Ask the worker's /meta (adsbdb) once per aircraft/callsign; in demo mode use the built-in table. */
function loadMeta(t) {
  if (DEMO) { if (demoMeta[t.id]) t.meta = demoMeta[t.id]; return; }
  if (t.kind !== "aircraft" && t.kind !== "flarm") return;
  const icao = icaoOf(t);
  const cs = callsignOf(t);
  if (icao.length !== 6 && cs.length < 3) return;
  const k = (icao || "-") + ":" + (cs || "-");
  if (t.meta) return;
  if (metaCache.has(k)) { t.meta = metaCache.get(k); return; }
  if (metaPending.has(k)) return;
  metaPending.add(k);
  const q = new URLSearchParams();
  if (icao.length === 6) q.set("icao", icao);
  if (cs.length >= 3) q.set("cs", cs);
  fetch("/meta?" + q.toString()).then((r) => r.json()).then((m) => {
    metaCache.set(k, m);
    const cur = tracks.get(t.id);
    if (cur) cur.meta = m;
    if (selected === t.id) refreshCard();
  }).catch(() => {}).finally(() => metaPending.delete(k));
}

// ---------- selection and the info card ----------

const sheet = createSheet({
  host: $("card"), map, mq: window.matchMedia("(max-width: 860px)"), reduced: REDUCED,
  getLngLat: () => (selected && tracks.has(selected) ? displayLonLat(tracks.get(selected)) : null),
});

const card = createCard($("card"), {
  demo: DEMO,
  loadPhoto: () => photoAllowed(),
  onClose: () => clearSelection(),
  onFollow: (v) => { followId = v ? selected : null; },
  onTrack: (v) => {
    showTrack = v;
    try { localStorage.setItem("onair.map.showTrack", v ? "1" : "0"); } catch { /* storage blocked */ }
    if (v && selected) fetchTrail(selected);
    const tr = map.getSource("trails");
    if (tr) tr.setData(trailGeo());
  },
  isFollowing: () => followId != null && followId === selected,
  isTrackShown: () => showTrack,
  getDisplayPos: (t) => displayLonLat(t),
});

/** Copy the derived fields the card reads (colour, heading) onto the track. */
function prepForCard(t) {
  t._color = colorOf(t.kind, t);
  t._hdg = effectiveHeading(t);
  return t;
}

function refreshCard() {
  const t = selected && tracks.get(selected);
  if (t) card.update(prepForCard(t));
}

function setHash(t) {
  try {
    const hex = t && t.kind === "aircraft" ? String(t.id).split(":")[1] : "";
    const h = !t ? "" : /^[0-9A-Fa-f]{6}$/.test(hex || "") ? "#hex=" + hex : "#id=" + encodeURIComponent(t.id);
    history.replaceState(null, "", location.pathname + location.search + h);
  } catch { /* sandboxed frame */ }
}

/** Pull the hub's stored history for a track (one request) and merge it with the points seen live. */
const trailAsked = new Map(); // id -> ms of the last request, so reselecting does not hammer the hub
function fetchTrail(id) {
  if (DEMO || !showTrack) return;
  const now = Date.now();
  if (now - (trailAsked.get(id) || 0) < 30000) return;
  trailAsked.set(id, now);
  fetch("/trail?id=" + encodeURIComponent(id)).then((r) => (r.ok ? r.json() : null)).then((d) => {
    const pts = d && d.trails && d.trails[id];
    const t = tracks.get(id);
    if (!pts || !t) return;
    t.tpts = mergeTrails(pts, t.tpts);
    if (selected === id) { const tr = map.getSource("trails"); if (tr) tr.setData(trailGeo()); }
  }).catch(() => { trailAsked.delete(id); });
}

function selectTrack(t) {
  if (followId && followId !== t.id) followId = null;
  selected = t.id;
  loadMeta(t);
  fetchTrail(t.id);
  card.open(prepForCard(t));
  sheet.onOpen();
  setHash(t);
  applyListSel();
  pushLive();
  const tr = map.getSource("trails");
  if (tr) tr.setData(trailGeo());
  if (window.matchMedia("(max-width: 860px)").matches) setRailOpen(false);
}

function clearSelection() {
  selected = null;
  followId = null;
  card.close();
  sheet.onClose();
  setHash(null);
  applyListSel();
  pushLive();
  const tr = map.getSource("trails");
  if (tr) tr.setData(trailGeo());
}

function applyListSel() {
  for (const b of document.querySelectorAll("#list button")) b.classList.toggle("sel", b.dataset.id === selected);
}

/** Select a track and fly to it. */
function focusTrack(id) {
  const t = tracks.get(id);
  if (!t) return;
  selectTrack(t);
  const [lon, lat] = displayLonLat(t);
  // recentre keeps the marker in the free area above the phone sheet (plain centring on desktop)
  sheet.recentre(true, { center: [lon, lat], zoom: Math.max(map.getZoom(), 6.5), duration: REDUCED ? 0 : 500 });
}

// card refresh at 1 Hz, regardless of how often positions arrive
setInterval(refreshCard, 1000);

// ---------- deep links: #hex=ABC123 or #id=... ----------

let pendingHash = null;
let pendingSince = 0;
function readHash() {
  const m = location.hash.match(/^#(hex|id)=(.+)$/);
  if (!m) return null;
  return m[1] === "hex" ? { hex: m[2].toUpperCase() } : { id: decodeURIComponent(m[2]) };
}
pendingHash = readHash();
pendingSince = Date.now();
window.addEventListener("hashchange", () => { pendingHash = readHash(); pendingSince = Date.now(); tryHash(); });

/** Open the card for a pending deep link as soon as the track is on the map; tell the user if it never appears. */
function tryHash() {
  if (!pendingHash) return;
  let found = null;
  if (pendingHash.id) found = tracks.get(pendingHash.id);
  else for (const t of tracks.values()) if (t.kind === "aircraft" && String(t.id).toUpperCase().endsWith(":" + pendingHash.hex)) { found = t; break; }
  if (found) { pendingHash = null; focusTrack(found.id); }
  else if (tracks.size && Date.now() - pendingSince > 15000) { pendingHash = null; toast("That track is not on the map right now."); }
}

let toastTimer = 0;
function toast(msg) {
  const el = $("toast");
  el.textContent = msg;
  el.hidden = false;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => { el.hidden = true; }, 4500);
}

// ---------- map layers ----------

map.on("load", () => {
  map.addSource("trails", { type: "geojson", data: trailGeo() });
  map.addSource("live", { type: "geojson", data: geojson() });
  map.addLayer({
    id: "trail", type: "line", source: "trails",
    filter: ["==", ["get", "kind"], "trail"],
    layout: { "line-cap": "round", "line-join": "round" },
    paint: { "line-color": ["get", "color"], "line-width": 2.2, "line-opacity": 0.75 },
  });
  map.addLayer({
    id: "pred", type: "line", source: "trails",
    filter: ["==", ["get", "kind"], "pred"],
    paint: { "line-color": ["get", "color"], "line-width": 1.2, "line-opacity": 0.45, "line-dasharray": [2, 2] },
  });
  // direction finding: bearing wedges and lines from the stations, fix markers with their 95 % ellipse
  map.addLayer({ id: "dfwedge", type: "fill", source: "trails", filter: ["==", ["get", "kind"], "dfwedge"],
    paint: { "fill-color": "#ff9a4d", "fill-opacity": ["*", 0.12, ["get", "fade"]] } });
  map.addLayer({ id: "dfell", type: "fill", source: "trails", filter: ["==", ["get", "kind"], "dfell"],
    paint: { "fill-color": "#ff5a5a", "fill-opacity": 0.18 } });
  map.addLayer({ id: "dfellline", type: "line", source: "trails", filter: ["==", ["get", "kind"], "dfell"],
    paint: { "line-color": "#ff5a5a", "line-width": 1.5, "line-opacity": 0.9 } });
  map.addLayer({ id: "dfline", type: "line", source: "trails", filter: ["==", ["get", "kind"], "dfline"],
    paint: { "line-color": "#ff9a4d", "line-width": 2, "line-opacity": ["*", 0.9, ["get", "fade"]], "line-dasharray": [4, 2] } });
  map.addLayer({ id: "dfstation", type: "circle", source: "trails", filter: ["==", ["get", "kind"], "dfstation"],
    paint: { "circle-radius": 5, "circle-color": "#ff9a4d", "circle-stroke-width": 1.2, "circle-stroke-color": "#0b0c0d" } });
  map.addLayer({ id: "dffix", type: "circle", source: "trails", filter: ["==", ["get", "kind"], "dffix"],
    paint: { "circle-radius": 7, "circle-color": "#ff5a5a", "circle-stroke-width": 2, "circle-stroke-color": "#ffffff" } });
  map.addLayer({ id: "dffixlabel", type: "symbol", source: "trails", filter: ["==", ["get", "kind"], "dffix"],
    layout: { "text-field": ["concat", "TX ", ["get", "label"]], "text-size": 12, "text-offset": [0, 1.4], "text-anchor": "top", "text-font": ["Open Sans Regular", "Arial Unicode MS Regular"] },
    paint: { "text-color": "#ffd0d0", "text-halo-color": "#0b0c0d", "text-halo-width": 1.5 } });
  // selected state: soft glow plus a ring in the track's colour
  map.addLayer({
    id: "halo", type: "circle", source: "live",
    filter: [">", ["to-number", ["get", "sel"]], 0],
    paint: {
      "circle-radius": 20,
      "circle-color": ["get", "color"],
      "circle-opacity": 0.1,
      "circle-blur": 0,
      "circle-stroke-width": 1.6,
      "circle-stroke-color": ["get", "color"],
      "circle-stroke-opacity": 0.9,
    },
  });
  map.addLayer({
    id: "dot", type: "circle", source: "live",
    filter: ["==", ["to-number", ["get", "dot"]], 1],
    paint: {
      "circle-radius": ["interpolate", ["linear"], ["zoom"], 2, 3, 8, 5],
      "circle-color": ["get", "color"],
      "circle-stroke-width": 1.2,
      "circle-stroke-color": "#0b0c0d",
      "circle-opacity": ["case", ["==", ["to-number", ["get", "est"]], 1], 0.5, 1],
    },
  });
  map.addLayer({
    id: "sym", type: "symbol", source: "live",
    filter: ["==", ["to-number", ["get", "sym"]], 1],
    layout: {
      "icon-image": ["get", "icon"],
      "icon-rotate": ["case", ["==", ["to-number", ["get", "iconRot"]], 1], ["to-number", ["get", "heading"]], 0],
      "icon-rotation-alignment": "map",
      "icon-pitch-alignment": "map",
      "icon-allow-overlap": true,
      "icon-ignore-placement": true,
      "icon-anchor": "center",
      "icon-size": ["interpolate", ["linear"], ["zoom"], 1.5, ["*", 0.5, ["get", "scale"]], 4, ["*", 0.7, ["get", "scale"]], 8, ["*", 0.95, ["get", "scale"]], 12, ["*", 1.25, ["get", "scale"]]],
      "symbol-sort-key": ["to-number", ["get", "sel"]],
    },
    paint: { "icon-opacity": ["case", ["==", ["to-number", ["get", "est"]], 1], 0.5, 1] },
  });
  // callsigns: only for the selected track, and for everything once zoomed in (the renderer drops labels that would collide)
  const labelLayout = {
    "text-field": ["get", "label"],
    "text-font": ["Open Sans Regular", "Arial Unicode MS Regular"],
    "text-size": 11,
    "text-anchor": "top",
    "text-offset": [0, 1.5],
    "text-optional": true,
  };
  const dark = themeKey() === "d";
  map.addLayer({
    id: "lab", type: "symbol", source: "live", minzoom: 7.5,
    filter: ["all", ["!=", ["to-number", ["get", "cl"], 0], 1], ["!=", ["get", "label"], ""], ["==", ["to-number", ["get", "sel"]], 0], ["!=", ["get", "kind"], "gnss"]],
    layout: { ...labelLayout, "text-allow-overlap": false, "text-ignore-placement": false },
    paint: { "text-color": dark ? "#dfe2e5" : "#14181c", "text-halo-color": dark ? "#0b0c0d" : "#ffffff", "text-halo-width": 1.4, "text-opacity": 0.92 },
  });
  map.addLayer({
    id: "lab_sel", type: "symbol", source: "live",
    filter: [">", ["to-number", ["get", "sel"]], 0],
    layout: { ...labelLayout, "text-size": 12, "text-allow-overlap": true, "text-ignore-placement": true, "text-offset": [0, 1.9] },
    paint: { "text-color": dark ? "#ffffff" : "#000000", "text-halo-color": dark ? "#0b0c0d" : "#ffffff", "text-halo-width": 1.6 },
  });
  map.addLayer({
    id: "clusterDot", type: "circle", source: "live", filter: ["==", ["to-number", ["get", "cl"], 0], 1],
    paint: {
      "circle-radius": ["interpolate", ["linear"], ["get", "n"], 4, 13, 50, 20, 500, 30],
      "circle-color": themeKey() === "d" ? "#2a3a4c" : "#dce8f4",
      "circle-opacity": 0.92, "circle-stroke-width": 1.5, "circle-stroke-color": "#6fa8dc",
    },
  });
  map.addLayer({
    id: "clusterNum", type: "symbol", source: "live", filter: ["==", ["to-number", ["get", "cl"], 0], 1],
    layout: { "text-field": ["get", "label"], "text-size": 12, "text-font": ["Open Sans Regular", "Arial Unicode MS Regular"], "text-allow-overlap": true },
    paint: { "text-color": themeKey() === "d" ? "#e4e6e8" : "#14181c" },
  });
  map.on("click", "clusterDot", (e) => {
    map.easeTo({ center: e.lngLat, zoom: Math.min(map.getZoom() + 2, 8), duration: REDUCED ? 0 : 400 });
  });
  map.on("zoomend", () => { pushLive(); });
  map.on("mouseenter", "clusterDot", () => { map.getCanvas().style.cursor = "pointer"; });
  map.on("mouseleave", "clusterDot", () => { map.getCanvas().style.cursor = ""; });
  initHam({ map, tracks: () => tracks, selectById: (id) => { const t = tracks.get(id); if (t) focusTrack(id); }, esc, REDUCED });
  const hit = ["dot", "sym"];
  /** Nearest feature to the click within a 14 px box, so thin icons are easy to hit on touch screens too. */
  map.on("click", (e) => {
    const r = 14;
    const feats = map.queryRenderedFeatures([[e.point.x - r, e.point.y - r], [e.point.x + r, e.point.y + r]], { layers: hit });
    if (!feats.length) {
      if (selected) clearSelection();
      if (window.matchMedia("(max-width: 860px)").matches) setRailOpen(false);
      return;
    }
    let best = null, bd = Infinity;
    for (const f of feats) {
      const p = map.project(f.geometry.coordinates);
      const d = (p.x - e.point.x) ** 2 + (p.y - e.point.y) ** 2;
      if (d < bd) { bd = d; best = f; }
    }
    const t = tracks.get(best.properties.id);
    if (t) selectTrack(t);
  });
  for (const layer of hit) {
    map.on("mouseenter", layer, () => { map.getCanvas().style.cursor = "pointer"; });
    map.on("mouseleave", layer, () => { map.getCanvas().style.cursor = ""; });
  }
  apply();
  ensureMotionLoop();
});

// no viewport culling: the source always holds every track (the renderer culls itself), so panning/zooming never drops icons.
// Clusters are recomputed whenever the zoom crosses a whole level, not only when the gesture ends.
let lastZoomStep = null;
map.on("zoom", () => {
  const z = Math.floor(map.getZoom());
  if (lastZoomStep === null) { lastZoomStep = z; return; }
  if (z !== lastZoomStep) { lastZoomStep = z; pushLive(); }
});

// user panning cancels follow mode
for (const ev of ["dragstart", "wheel", "touchstart"]) {
  map.on(ev, (e) => { if (followId && e.originalEvent) { followId = null; card.syncToggles(); } });
}

map.on("mousemove", (e) => { $("cursor").textContent = fmtLat(e.lngLat.lat) + "   " + fmtLon(e.lngLat.lng); });
map.on("move", () => { $("zoom").textContent = "z " + map.getZoom().toFixed(1); });

function setRailOpen(open) {
  const rail = $("rail");
  const backdrop = $("railBackdrop");
  const btn = $("panelBtn");
  if (!rail) return;
  rail.classList.toggle("open", open);
  if (backdrop) { backdrop.classList.toggle("show", open); backdrop.hidden = !open; }
  if (btn) btn.setAttribute("aria-expanded", open ? "true" : "false");
}
$("panelBtn")?.addEventListener("click", () => setRailOpen(!$("rail")?.classList.contains("open")));
$("railBackdrop")?.addEventListener("click", () => setRailOpen(false));
$("railClose")?.addEventListener("click", () => setRailOpen(false));
for (const [id, v] of [["layersAll", true], ["layersNone", false]]) {
  $(id)?.addEventListener("click", () => {
    for (const box of $("layers").querySelectorAll("input")) { box.checked = v; on[box.dataset.k] = v; box.closest("label").classList.toggle("off", !v); }
    apply();
  });
}

initConsent();

// ---------- search: callsign, registration, ICAO hex, MMSI ----------

const qEl = $("q");
const resEl = $("results");
let resItems = [];
let resIdx = -1;

function searchTracks(q) { return rankTracks(tracks.values(), q, 8); }

function renderResults() {
  const q = qEl.value;
  const users = searchUsers(tracks.values(), q, 3);
  resItems = [...users.map((u) => ({ user: u })), ...searchTracks(q).map((t) => ({ t }))];
  resIdx = resItems.length ? 0 : -1;
  if (q.trim().length < 2) { resEl.hidden = true; qEl.setAttribute("aria-expanded", "false"); return; }
  resEl.hidden = false;
  qEl.setAttribute("aria-expanded", "true");
  let html = "";
  resItems.forEach((it, i) => {
    if (it.user && i === 0) html += `<li class="hd" role="presentation">Users</li>`;
    if (it.t && (i === 0 || resItems[i - 1].user)) html += users.length ? `<li class="hd" role="presentation">Tracks</li>` : "";
    if (it.user) html += `<li role="option" id="res${i}" data-user="${esc(it.user.name)}" aria-selected="${i === 0}"><span></span><b>${esc(it.user.name)}</b><span>${it.user.n} icon${it.user.n === 1 ? "" : "s"}</span></li>`;
    else { const t = it.t; html += `<li role="option" id="res${i}" data-id="${esc(t.id)}" aria-selected="${i === 0}"><img alt="" src="${miniUrl(shapeFor(t), colorOf(t.kind, t))}" width="18" height="18"><b>${esc(t.label || t.id)}</b><span>${esc(cap(t.kind) + (t.sub ? " · " + t.sub : ""))}</span></li>`; }
  });
  resEl.innerHTML = html || `<li class="none">No match on the map right now</li>`;
  for (const li of resEl.querySelectorAll("li[data-id]")) li.onmousedown = (e) => { e.preventDefault(); pickResult(li.dataset.id); };
  for (const li of resEl.querySelectorAll("li[data-user]")) li.onmousedown = (e) => { e.preventDefault(); pickUser(li.dataset.user); };
}

function pickResult(id) {
  resEl.hidden = true;
  qEl.setAttribute("aria-expanded", "false");
  qEl.blur();
  focusTrack(id);
}

const byChip = $("byChip");

/** Show only the icons last heard by this user and fit the map to them; the chip puts everyone back. */
function pickUser(name) {
  resEl.hidden = true;
  qEl.setAttribute("aria-expanded", "false");
  qEl.blur();
  setByFilter(String(name).toLowerCase(), name);
}

function setByFilter(key, shown) {
  byFilter = key;
  if (key) {
    byChip.textContent = "Showing only " + shown + " \u00b7 show everyone \u2715";
    byChip.hidden = false;
    const pts = [...tracks.values()].filter((t) => byMatches(t, key));
    if (pts.length) {
      let w = 180, s = 90, e = -180, n = -90;
      for (const t of pts) { const [lo, la] = displayLonLat(t); w = Math.min(w, lo); e = Math.max(e, lo); s = Math.min(s, la); n = Math.max(n, la); }
      map.fitBounds([[w, s], [e, n]], { padding: { top: 90, bottom: 140, left: 40, right: 40 }, maxZoom: 8, duration: REDUCED ? 0 : 500 });
    } else toast("No icons from that user are on the map right now.");
  } else byChip.hidden = true;
  apply();
}
byChip.onclick = () => setByFilter("", "");

function moveResult(d) {
  if (!resItems.length) return;
  resIdx = (resIdx + d + resItems.length) % resItems.length;
  [...resEl.querySelectorAll("li[role=option]")].forEach((li, i) => li.setAttribute("aria-selected", String(i === resIdx)));
  qEl.setAttribute("aria-activedescendant", "res" + resIdx);
}

let qTimer = 0;
qEl.addEventListener("input", () => {
  query = qEl.value;
  renderListSoon();
  clearTimeout(qTimer);
  qTimer = setTimeout(renderResults, 120);
});
qEl.addEventListener("keydown", (e) => {
  if (e.key === "ArrowDown") { e.preventDefault(); moveResult(1); }
  else if (e.key === "ArrowUp") { e.preventDefault(); moveResult(-1); }
  else if (e.key === "Enter") {
    if (!resItems.length) renderResults();
    const it = resItems[Math.max(0, resIdx)];
    if (it && it.user) pickUser(it.user.name);
    else if (it) pickResult(it.t.id);
    else toast("No match on the map right now.");
  } else if (e.key === "Escape") {
    if (qEl.value) { qEl.value = ""; query = ""; renderResults(); renderListSoon(); }
    else qEl.blur();
    e.stopPropagation();
  }
});
qEl.addEventListener("blur", () => { setTimeout(() => { resEl.hidden = true; }, 150); });

// keyboard shortcuts: / search, Esc close
document.addEventListener("keydown", (e) => {
  const inField = /^(INPUT|TEXTAREA|SELECT)$/.test((e.target && e.target.tagName) || "");
  if (e.key === "/" && !inField && !e.metaKey && !e.ctrlKey) { e.preventDefault(); qEl.focus(); qEl.select(); return; }
  if (e.key === "Escape" && !inField) {
    if (byFilter && !selected) { setByFilter("", ""); return; }
    if (selected) clearSelection();
    else if ($("rail").classList.contains("open")) setRailOpen(false);
  }
});

function tickClock() { $("clock").textContent = new Date().toISOString().slice(11, 19) + " UTC"; }
tickClock();
setInterval(tickClock, 1000);

function setLive(ok) {
  liveOk = ok;
  renderStats();
}

let liveVersion = 0;   // hub version of the data in `tracks` (for /live?since=)
let liveFullAt = 0;

/** A /live?since= answer: only the changed tracks and the ids that went away. */
function takeDelta(d) {
  for (const t of d.tracks || []) tracks.set(t.id, mergePathClient(tracks.get(t.id), t));
  for (const id of d.removed || []) tracks.delete(id);
  stations = d.stations || 0;
  if (d.load) serverLoad = d.load;
  liveVersion = d.version || liveVersion;
  apply();
  setLive(true);
}

/** Polling fallback (WebSocket down): small deltas, with a full snapshot at the start and every 5 minutes. */
function pollLive() {
  const full = !liveVersion || Date.now() - liveFullAt > 300000;
  fetch(full ? "/live" : "/live?since=" + liveVersion).then((r) => r.json()).then((d) => {
    if (d.delta) takeDelta(d); else takeSnapshot(d);
  }).catch(() => setLive(false));
}

function takeSnapshot(d) {
  liveVersion = d.version || 0;
  liveFullAt = Date.now();
  const next = new Map();
  for (const t of d.tracks || []) next.set(t.id, mergePathClient(tracks.get(t.id), t));
  tracks = next;
  stations = d.stations || 0;
  if (d.load) serverLoad = d.load;
  apply();
  setLive(true);
  if (tracks.size) ensureMotionLoop();
}

let wsOpen = false;

function connect() {
  const proto = location.protocol === "https:" ? "wss" : "ws";
  let ws;
  try { ws = new WebSocket(proto + "://" + location.host + "/ws"); } catch { return; }
  ws.onopen = () => { wsOpen = true; setLive(true); };
  ws.onmessage = (ev) => {
    const m = JSON.parse(ev.data);
    if (m.type === "hello") takeSnapshot(m);
    else if (m.type === "load" && m.load) { serverLoad = m.load; renderLoad(); }
    else if (m.type === "upsert") upsert(m.tracks || []);
  };
  ws.onclose = () => {
    wsOpen = false;
    setLive(false);
    setTimeout(connect, 5000);
  };
  setTimeout(() => {
    if (!wsOpen && tracks.size === 0) {
      pollLive();
    }
  }, 6000);
}



// ---------- motion loop: dead reckoning, selected trail, follow mode ----------

let motionTimer = 0;
let pulse = 0;
function refreshMotion() {
  if (document.hidden) return;
  pushLive();
  const tr = map.getSource("trails");
  if (tr && selected) tr.setData(trailGeo());
  if (selected && map.getLayer("halo") && !REDUCED) {
    pulse = (pulse + 0.25) % 4;
    map.setPaintProperty("halo", "circle-radius", 19 + 2 * Math.sin(pulse * Math.PI / 2));
  }
  if (followId && tracks.has(followId)) {
    const [lon, lat] = displayLonLat(tracks.get(followId));
    map.easeTo({ center: [lon, lat], duration: REDUCED ? 0 : 260, easing: (x) => x });
  }
}

function ensureMotionLoop() {
  if (motionTimer) return;
  motionTimer = window.setInterval(refreshMotion, 250);
}

/** Fill the collapsible icon guide (what each shape means). */
function renderIconGuide() {
  const g = (title, rows) => `<h4>${title}</h4>` + rows.map(([s, n]) => `<div><img alt="" src="${miniUrl(s, "#c8cacc")}" width="22" height="22">${n}</div>`).join("");
  $("iconkey").innerHTML =
    g("Aircraft", [
      ["narrow", "Narrow-body jet"], ["wide", "Wide-body twin"], ["quad", "Four-engine (A380 / 747)"], ["regional", "Regional jet"], ["bizjet", "Business jet"],
      ["turboprop", "Turboprop"], ["light", "Light aircraft"], ["lighttwin", "Light twin"], ["heli", "Helicopter"], ["fighter", "Fighter"],
      ["delta", "Delta-wing fighter"], ["transport", "Military transport"], ["glider", "Glider"], ["balloon", "Balloon"], ["airship", "Airship"],
      ["uav", "Fixed-wing UAV"], ["plane", "Unknown aircraft"], ["vehicle", "Ground vehicle"],
    ])
    + g("Ships (the dashed line is the course)", [
      ["ship_cargo", "Cargo"], ["ship_tanker", "Tanker"], ["ship_passenger", "Passenger / ferry"], ["ship_hsc", "High-speed craft"], ["ship_tug", "Tug"],
      ["ship_fishing", "Fishing"], ["ship_sailing", "Sailing"], ["ship_pleasure", "Pleasure craft"], ["ship_military", "Military"], ["ship_sar", "Search and rescue"],
      ["ship_pilot", "Pilot"], ["ship_dredger", "Dredger"], ["ship_other", "Other / unknown"],
    ])
    + g("Other", [
      ["drone", "Drone"], ["drone_rid", "Drone with Remote ID"], ["sonde", "Radiosonde"], ["sat", "Satellite"], ["sat_gnss", "GNSS satellite"],
      ["aprs_car", "APRS vehicle"], ["aprs_truck", "APRS truck"], ["aprs_house", "APRS home"], ["aprs_wx", "APRS weather station"],
      ["aprs_digi", "APRS digipeater"], ["aprs_balloon", "APRS balloon"], ["aprs_ship", "APRS boat"], ["ham", "Amateur station"], ["buoy", "Buoy"],
    ]);
}

// altitude colouring: option + legend in the Filters sheet
function syncAltUi() {
  const box = $("altColor");
  if (box) box.checked = altMode;
  const lg = $("altLegend");
  if (lg) lg.hidden = !altMode;
  const bar = $("altBar");
  if (bar) bar.style.background = altitudeGradientCss();
}
$("altColor")?.addEventListener("change", (e) => {
  altMode = e.target.checked;
  try { localStorage.setItem("onair.map.altColor", altMode ? "1" : "0"); } catch { /* storage blocked */ }
  syncAltUi();
  apply();
});
syncAltUi();

// the map theme flipped: icons get the other outline colour, labels and the small icons follow
onThemeChange((k) => {
  const dark = k === "d";
  for (const [id, light] of [["lab", "#dfe2e5"], ["lab_sel", "#ffffff"]]) {
    try {
      map.setPaintProperty(id, "text-color", dark ? light : (id === "lab" ? "#14181c" : "#000000"));
      map.setPaintProperty(id, "text-halo-color", dark ? "#0b0c0d" : "#ffffff");
    } catch { /* layers not added yet */ }
  }
  for (const k2 of KINDS) {
    const img = document.querySelector(`#layers label[data-k="${k2.id}"] img.ico`);
    if (img) img.src = miniUrl(k2.shape, k2.color);
  }
  renderIconGuide();
  renderListSoon();
  apply();
});

// ---------- start ----------

renderLayers();
renderIconGuide();
$("zoom").textContent = "z " + map.getZoom().toFixed(1);

if (DEMO) {
  document.title = "DEMO · " + document.title;
  $("demoBanner").hidden = false;
  const d = generateDemo();
  demoMeta = d.meta;
  for (const t of d.tracks) if (demoMeta[t.id]) t.meta = demoMeta[t.id];
  const list = d.tracks;
  // simple back-trail so selected demo tracks show a line straight away
  for (const t of list) {
    if (t.heading != null && t.speed) {
      const back = [];
      const probe = { ...t, heading: (t.heading + 180) % 360, path: [] };
      for (let i = 0; i < 6; i++) { stepDemo([probe], 90); back.unshift([probe.lon, probe.lat]); }
      t.path = back;
    }
  }
  takeSnapshot({ tracks: list, stations: 3, load: { pct: 12, viewers: 1, stations: 3, tracks: list.length } });
  stepTimer();
  function stepTimer() {
    setInterval(() => { stepDemo([...tracks.values()], 2); apply(); }, 2000);
  }
} else {
  connect();
  // Full snapshots are expensive on the hub; rely on WebSocket when connected.
  setInterval(() => {
    if (wsOpen) return;
    pollLive();
  }, 30000);
}

// ---------- GPS interference (community heatmap, from GET /jam) ----------
// Cells where a larger share of aircraft report a degraded GNSS position. An indicator, not proof of jamming (see the note under the toggle).
let jamOn = false;
try { jamOn = localStorage.getItem("onair.map.gpsjam") === "1"; } catch { /* storage blocked */ }
let jamTimer = 0;
function jamLoad() {
  if (!jamOn || !map.getSource("jam")) return;
  fetch("/jam").then((r) => (r.ok ? r.json() : null)).then((d) => {
    if (d && map.getSource("jam")) map.getSource("jam").setData(cellsToGeo(d.cells, d.cell));
  }).catch(() => {});
}
function jamApply() {
  const box = $("gpsJam");
  if (box) box.checked = jamOn;
  const lg = $("jamLegend");
  if (lg) lg.hidden = !jamOn;
  if (map.getLayer("jamfill")) map.setLayoutProperty("jamfill", "visibility", jamOn ? "visible" : "none");
  clearInterval(jamTimer);
  if (jamOn) { jamLoad(); jamTimer = setInterval(jamLoad, 300e3); }
}
function jamInit() {
  if (map.getSource("jam")) return;
  map.addSource("jam", { type: "geojson", data: cellsToGeo([]) });
  map.addLayer({
    id: "jamfill", type: "fill", source: "jam",
    layout: { visibility: jamOn ? "visible" : "none" },
    paint: { "fill-color": ["get", "color"], "fill-opacity": 0.38 },
  }, map.getLayer("trail") ? "trail" : undefined);
  jamApply();
}
const jamBar = $("jamBar");
if (jamBar) jamBar.innerHTML = LEVEL_COLOURS.map((c) => `<i style="background:${c}"></i>`).join("");
$("gpsJam")?.addEventListener("change", (e) => {
  jamOn = e.target.checked;
  try { localStorage.setItem("onair.map.gpsjam", jamOn ? "1" : "0"); } catch { /* storage blocked */ }
  jamApply();
});
if (map.loaded()) jamInit(); else map.on("load", () => setTimeout(jamInit, 0));
