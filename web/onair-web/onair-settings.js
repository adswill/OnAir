// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

/* OnAir Web client settings: schema, defaults, validation, persistence, auto-range. Pure code (no DOM): tested by tests/web/run.js.
 * Every value is checked against its schema on load and on import, so a damaged or hand-edited localStorage can never break the page. */
(function (root, factory) {
  const lib = factory();
  if (typeof module === "object" && module.exports) module.exports = lib;
  else root.OnAirSettings = lib;
})(typeof globalThis !== "undefined" ? globalThis : this, function () {
  "use strict";
  const KEY = "onair.settings.v1";
  const clamp = (v, lo, hi) => (v < lo ? lo : v > hi ? hi : v);

  // type: "enum" (values), "int" / "num" (min, max), "bool". group is the tab in the Settings panel.
  const SCHEMA = {
    // ---- receiver (this listener's virtual receiver) ----
    agc:        { group: "receiver", type: "enum", values: ["off", "slow", "fast"], def: "slow" },
    gainDb:     { group: "receiver", type: "int", min: -20, max: 60, def: 30 },
    deemphUs:   { group: "receiver", type: "enum", values: [0, 50, 75], def: 50 },
    audioCodec: { group: "receiver", type: "enum", values: ["adpcm", "pcm16"], def: "adpcm" },
    audioRate:  { group: "receiver", type: "enum", values: [0, 8000, 12000, 16000, 24000, 48000], def: 0 },   // 0 = automatic per mode
    step:       { group: "receiver", type: "int", min: 1, max: 1000000, def: 5000 },
    autoMode:   { group: "receiver", type: "bool", def: true },   // choosing a band or a bookmark also selects its mode
    // ---- display ----
    palette:    { group: "display", type: "enum", values: ["navy", "amber", "classic", "viridis", "grey", "turbo", "magma", "ice"], def: "navy" },
    rangeMode:  { group: "display", type: "enum", values: ["server", "adaptive", "manual"], def: "server" },
    range:      { group: "display", type: "int", min: 20, max: 120, def: 70 },        // dB span (server and adaptive mode)
    floorAdj:   { group: "display", type: "int", min: -30, max: 30, def: 0 },
    minDb:      { group: "display", type: "int", min: -160, max: -10, def: -110 },    // manual mode
    maxDb:      { group: "display", type: "int", min: -120, max: 0, def: -30 },
    wfFps:      { group: "display", type: "int", min: 1, max: 30, def: 12 },          // spectrum rows a second
    fftBins:    { group: "display", type: "enum", values: [0, 256, 512, 1024, 2048, 4096], def: 0 },   // 0 = fit the screen
    smooth:     { group: "display", type: "int", min: 0, max: 90, def: 0 },           // trace averaging, percent
    peakHold:   { group: "display", type: "bool", def: false },
    showBands:  { group: "display", type: "bool", def: true },
    showMarks:  { group: "display", type: "bool", def: true },
    showGrid:   { group: "display", type: "bool", def: true },
    // ---- layout and look ----
    theme:      { group: "layout", type: "enum", values: ["auto", "dark", "light"], def: "auto" },
    lang:       { group: "layout", type: "enum", values: ["auto", "en", "lt", "de"], def: "auto" },
    layout:     { group: "layout", type: "enum", values: ["auto", "wide", "compact"], def: "auto" },
    panelLeft:  { group: "layout", type: "bool", def: true },
    panelRight: { group: "layout", type: "bool", def: true },
    scale:      { group: "layout", type: "int", min: 85, max: 130, def: 100 },       // page text size, percent
    keys:       { group: "layout", type: "bool", def: true },                         // keyboard shortcuts
    reduceMotion: { group: "layout", type: "bool", def: false },
  };
  const KEYS = Object.keys(SCHEMA);

  function defaults() { const o = {}; for (const k of KEYS) o[k] = SCHEMA[k].def; return o; }

  // returns the value made valid for key k (the default when it cannot be)
  function coerce(k, v) {
    const s = SCHEMA[k]; if (!s) return undefined;
    switch (s.type) {
      case "bool": return typeof v === "boolean" ? v : s.def;
      case "enum": return s.values.includes(v) ? v : s.def;
      case "int": case "num": {
        if (typeof v !== "number" || !isFinite(v)) return s.def;
        const c = clamp(v, s.min, s.max); return s.type === "int" ? Math.round(c) : c;
      }
      default: return s.def;
    }
  }
  // a whole object from the outside (storage, import): unknown keys dropped, bad values defaulted, min < max kept
  function sanitize(obj) {
    const out = defaults();
    if (obj && typeof obj === "object" && !Array.isArray(obj)) for (const k of KEYS) if (Object.prototype.hasOwnProperty.call(obj, k)) out[k] = coerce(k, obj[k]);
    if (out.maxDb - out.minDb < 10) out.maxDb = Math.min(0, out.minDb + 10);
    return out;
  }
  // storage is any {getItem,setItem}; every access is guarded (private windows throw)
  function load(storage) {
    try { const raw = storage.getItem(KEY); if (raw) return sanitize(JSON.parse(raw)); } catch (e) { /* fall through */ }
    return defaults();
  }
  function save(storage, s) { try { storage.setItem(KEY, JSON.stringify(sanitize(s))); return true; } catch (e) { return false; } }
  function exportJson(s) { return JSON.stringify({ onair_web_settings: 1, settings: sanitize(s) }, null, 2); }
  // returns the settings, or null when the text is not an export of ours
  function importJson(text) {
    try { const o = JSON.parse(text); if (o && o.onair_web_settings === 1 && o.settings) return sanitize(o.settings); } catch (e) { /* not ours */ }
    return null;
  }

  // ---- spectrum request from the settings ----
  // bins: a setting of 0 means "fit the canvas" (next power of two of its pixel width, 256 .. 2048); never above the server's max_bins
  function binsFor(s, canvasPx, maxBins) {
    const fit = Math.max(256, Math.min(2048, 1 << Math.ceil(Math.log2(Math.max(256, canvasPx || 1024)))));
    return Math.max(64, Math.min(maxBins || 2048, s.fftBins || fit));
  }
  function fpsFor(s, maxFps) { return Math.max(1, Math.min(maxFps || 15, s.wfFps)); }

  // ---- auto-range ----
  // From a spectrum row (levels 0..255 between floorDb and topDb): a floor just under the noise and a top just over the strongest signal.
  function rowDb(row, i) { return row.floorDb + (row.levels[i] / 255) * (row.topDb - row.floorDb); }
  function autoRange(row) {
    const n = row.bins; if (!n) return null;
    const a = new Float32Array(n); for (let i = 0; i < n; i++) a[i] = rowDb(row, i);
    const sorted = Array.from(a).sort((x, y) => x - y);
    const q = (p) => sorted[Math.min(n - 1, Math.max(0, Math.floor(p * (n - 1))))];
    const lo = q(0.05) - 4, hi = Math.max(q(0.995) + 6, lo + 20);
    return { min: lo, max: hi };
  }
  // smooth the range over time: a new estimate moves a fraction of the way (down fast for the floor, up fast for the top)
  function followRange(cur, est, a) {
    if (!cur) return { min: est.min, max: est.max };
    a = a == null ? 0.05 : a;
    return {
      min: cur.min + (est.min - cur.min) * (est.min < cur.min ? Math.min(1, a * 3) : a),
      max: cur.max + (est.max - cur.max) * (est.max > cur.max ? Math.min(1, a * 3) : a),
    };
  }
  // display floor and span for a row under the settings and the adaptive state (cur: {min,max} or null)
  function levels(s, row, cur) {
    if (s.rangeMode === "manual") return { lo: s.minDb, span: Math.max(10, s.maxDb - s.minDb) };
    if (s.rangeMode === "adaptive" && cur) return { lo: cur.min, span: Math.max(10, cur.max - cur.min) };
    return { lo: row ? row.floorDb + s.floorAdj : -110, span: s.range };
  }
  // exponential trace averaging in the linear dB domain; prev is the Float32Array of the last drawn values (or null)
  function smoothTrace(prev, cur, pct) {
    if (!pct || !prev || prev.length !== cur.length) return cur.slice();
    const k = pct / 100, out = new Float32Array(cur.length);
    for (let i = 0; i < cur.length; i++) out[i] = isNaN(cur[i]) ? NaN : isNaN(prev[i]) ? cur[i] : prev[i] * k + cur[i] * (1 - k);
    return out;
  }

  // rx message for a settings object (the `rx` fields the protocol accepts)
  function rxFields(s, mode) {
    const m = { agc: s.agc, gain_db: clamp(s.gainDb, -20, 60) };
    if (mode === "wfm") m.deemph_us = s.deemphUs;
    return m;
  }
  // audio message parameters: rate 0 = per mode (wide FM needs 48 kHz, voice modes 12 kHz)
  function audioParams(s, mode) {
    const rate = s.audioRate || (mode === "wfm" ? 48000 : 12000);
    return { codec: s.audioCodec, rate };
  }

  return { KEY, SCHEMA, KEYS, defaults, coerce, sanitize, load, save, exportJson, importJson, binsFor, fpsFor, autoRange, followRange, levels, smoothTrace, rxFields, audioParams };
});
