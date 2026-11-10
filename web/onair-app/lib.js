/* OnAir Web 3.9: the page's pure code (no DOM), so it runs in node for the tests (tests/web/app.test.js).
 * Frequencies, the spectrum frame, the audio frame, mode search, the panel model built from a schema and a panel object, settings. */
(function (root, factory) {
  const lib = factory();
  if (typeof module === "object" && module.exports) module.exports = lib;
  else root.OnAirLib = lib;
})(typeof globalThis !== "undefined" ? globalThis : this, function () {
  "use strict";

  // ---- frequencies
  function formatMhz(hz) {
    if (!isFinite(hz)) return "---.------";
    const mhz = hz / 1e6;
    let s = mhz.toFixed(6);
    if (mhz >= 1000) s = mhz.toFixed(4);
    return s;
  }
  // "100.5", "100,5", "100.5 MHz", "145800 kHz", "7074000", "7.074M", "1090m" -> Hz, or NaN
  function parseFreq(text) {
    if (typeof text !== "string") return NaN;
    let t = text.trim().toLowerCase().replace(/,/g, ".").replace(/\s+/g, "");
    if (!t) return NaN;
    let mul = 1e6;
    const m = /^([0-9]*\.?[0-9]+)(ghz|g|mhz|m|khz|k|hz)?$/.exec(t);
    if (!m) return NaN;
    const u = m[2] || "";
    if (u === "ghz" || u === "g") mul = 1e9;
    else if (u === "khz" || u === "k") mul = 1e3;
    else if (u === "hz") mul = 1;
    else if (u === "") {
      const v = parseFloat(m[1]);
      mul = v >= 1e5 ? 1 : v >= 1e4 ? 1e3 : 1e6;   // a bare number: Hz when huge, kHz when 5 digits, else MHz
    }
    const hz = Math.round(parseFloat(m[1]) * mul);
    return hz >= 0 && hz <= 1e12 ? hz : NaN;
  }
  // a tuning step that suits the span (one keyboard arrow)
  function tuneStep(spanHz) {
    if (spanHz >= 5e6) return 25e3;
    if (spanHz >= 1e6) return 5e3;
    if (spanHz >= 2e5) return 1e3;
    return 100;
  }

  // ---- the binary frames of the WebSocket
  // spectrum: [1][seq u32][centre f32][rate f32][floor f32][top f32][u8 bins...]
  function decodeSpectrum(buf) {
    const d = new DataView(buf);
    if (buf.byteLength < 21 || d.getUint8(0) !== 1) return null;
    const seq = d.getUint32(1, true), centre = d.getFloat32(5, true), rate = d.getFloat32(9, true), lo = d.getFloat32(13, true), hi = d.getFloat32(17, true);
    const n = buf.byteLength - 21;
    const db = new Float32Array(n);
    const u8 = new Uint8Array(buf, 21, n);
    for (let i = 0; i < n; i++) db[i] = lo + u8[i] / 255 * (hi - lo);
    return { seq, centreHz: centre, rateHz: rate, floorDb: lo, topDb: hi, db };
  }
  // audio: [2][rate u32][pcm16le mono...]
  function decodeAudio(buf) {
    const d = new DataView(buf);
    if (buf.byteLength < 5 || d.getUint8(0) !== 2) return null;
    const rate = d.getUint32(1, true);
    const n = (buf.byteLength - 5) >> 1;
    const f = new Float32Array(n);
    for (let i = 0; i < n; i++) f[i] = d.getInt16(5 + 2 * i, true) / 32768;
    return { rate, samples: f };
  }
  // x (0..1 across the canvas) <-> Hz for a frame
  function xToHz(x, centreHz, rateHz) { return centreHz + (x - 0.5) * rateHz; }
  function hzToX(hz, centreHz, rateHz) { return rateHz > 0 ? (hz - centreHz) / rateHz + 0.5 : 0.5; }

  // ---- modes: ranked search over name, blurb, sub id and group
  function modeSearch(modes, groups, query) {
    const q = (query || "").trim().toLowerCase();
    const words = q.split(/\s+/).filter(Boolean);
    const out = [];
    for (const m of modes) {
      const name = (m.name || "").toLowerCase(), blurb = (m.blurb || "").toLowerCase(), sub = (m.sub || "").toLowerCase();
      const grp = (groups[m.group] || "").toLowerCase();
      let score = 0;
      if (!words.length) score = 1;
      for (const w of words) {
        let s = 0;
        if (name === w || sub === w) s = 100;
        else if (name.startsWith(w)) s = 60;
        else if (name.includes(w)) s = 40;
        else if (sub.includes(w)) s = 30;
        else if (grp.includes(w)) s = 15;
        else if (blurb.includes(w)) s = 10;
        if (!s) { score = 0; break; }
        score += s;
      }
      if (score > 0) out.push({ mode: m, score });
    }
    out.sort((a, b) => b.score - a.score || a.mode.name.localeCompare(b.mode.name));
    return out.map(x => x.mode);
  }

  // ---- the panel model: the schema (an array of field descriptors) + the panel's "fields" object -> widgets in display order
  // A widget: {kind, name, label, unit, value, tier ("s" simple / "a" advanced), cols?, map?}
  const SIMPLE_COMMON = ["snrDb", "locked", "state", "cfoHz", "blocksOk", "blocksBad"];
  function buildPanel(schema, fields, common, advanced) {
    const widgets = [];
    const f = fields || {};
    const c = common || {};
    // the common block first: signal, lock, counts
    for (const n of SIMPLE_COMMON) if (c[n] !== undefined && c[n] !== null) widgets.push({ kind: typeof c[n] === "boolean" ? "bool" : typeof c[n] === "number" ? "num" : "text", name: n, label: commonLabel(n), unit: n === "snrDb" ? "dB" : n === "cfoHz" ? "Hz" : "", value: c[n], tier: "s" });
    if (c.quality && typeof c.quality === "object" && c.quality.label) widgets.push({ kind: "text", name: "quality", label: "Signal quality", value: c.quality.label + (isFinite(c.quality.percent) ? " (" + Math.round(c.quality.percent) + " %)" : ""), tier: "s" });
    let own = 0;
    const arr = Array.isArray(schema) ? schema : [];
    for (const d of arr) {
      if (!d || !d.n) continue;
      const v = f[d.n];
      if (v === undefined) continue;
      let kind = d.k || "text";
      if (kind === "num" || kind === "bool" || kind === "text") {
        if (SIMPLE_COMMON.indexOf(d.n) >= 0 || d.n === "seq" || d.n === "dataValid") continue;   // shown from the common block
        if (v === null) continue;
        if (kind === "num" && typeof v !== "number") kind = typeof v === "boolean" ? "bool" : "text";
      }
      const tier = kind === "num" || kind === "bool" || kind === "text" ? (own++ < 10 ? "s" : "a") : "s";
      if (tier === "a" && !advanced) continue;
      const w = { kind, name: d.n, label: d.l || d.n, unit: d.u || "", value: v, tier };
      if (kind === "table") {
        if (!Array.isArray(v) || !v.length) { if (advanced) widgets.push({ kind: "text", name: d.n, label: d.l || d.n, value: "none yet", tier: "a" }); continue; }
        w.cols = tableColumns(v);
        if (d.map) w.map = d.map;
      }
      if (kind === "list" && (!Array.isArray(v) || !v.length)) continue;
      if (kind === "lines" && (!Array.isArray(v) || !v.length)) continue;
      if (kind === "const" && (!Array.isArray(v) || v.length < 4)) continue;
      if (kind === "image" && !v) continue;
      if (kind === "obj" && (!v || typeof v !== "object" || !Object.keys(v).length)) continue;
      widgets.push(w);
    }
    return widgets;
  }
  function commonLabel(n) {
    return { snrDb: "Signal-to-noise", locked: "Locked", state: "State", cfoHz: "Carrier offset", blocksOk: "Blocks OK", blocksBad: "Blocks bad" }[n] || n;
  }
  // the columns of a table: the primitive fields of its rows (first row's keys, plus any later row adds), at most 24
  function tableColumns(rows) {
    const cols = [];
    const seen = {};
    for (const r of rows.slice(0, 20)) {
      if (!r || typeof r !== "object") continue;
      for (const k in r) {
        const v = r[k];
        if (seen[k] || v === null || typeof v === "object") continue;
        seen[k] = true;
        cols.push({ name: k, label: humanize(k), num: typeof v === "number" });
        if (cols.length >= 24) return cols;
      }
    }
    return cols;
  }
  function humanize(name) {
    let s = String(name).replace(/([a-z0-9])([A-Z])/g, "$1 $2").replace(/_/g, " ");
    s = s.charAt(0).toUpperCase() + s.slice(1);
    return s.replace(/\bDb\b/, "dB").replace(/\bHz\b/, "Hz").replace(/\bSnr\b/, "SNR").replace(/\bId\b/, "ID").replace(/\bPct\b/, "%");
  }
  function formatValue(v, unit) {
    if (v === null || v === undefined) return "";
    if (typeof v === "boolean") return v ? "yes" : "no";
    if (typeof v === "number") {
      if (!isFinite(v)) return "-";
      let s;
      if (Number.isInteger(v)) s = Math.abs(v) >= 1e6 ? v.toLocaleString("en-US") : String(v);
      else if (Math.abs(v) >= 1000) s = v.toFixed(1);
      else if (Math.abs(v) >= 10) s = v.toFixed(2);
      else s = v.toFixed(3);
      if (unit === "Hz" && Math.abs(v) >= 1e6) s = (v / 1e6).toFixed(4) + " M";
      else if (unit === "Hz" && Math.abs(v) >= 1e4) s = (v / 1e3).toFixed(2) + " k";
      return s;
    }
    return String(v);
  }

  // ---- settings: group the server's items
  function groupSettings(items, groups) {
    const out = [];
    for (const g of groups) {
      const its = items.filter(i => i.group === g);
      if (its.length) out.push({ group: g, items: its });
    }
    return out;
  }
  // the value a settings input gives back, typed for the API
  function settingValue(kind, raw) {
    if (kind === "bool") return !!raw;
    if (kind === "int") { const n = Number(raw); return Number.isInteger(n) ? n : NaN; }
    return String(raw);
  }

  // ---- the viewer's state of a session: what the page may do (from hello / session)
  function permissions(hello) {
    const h = hello || {};
    return { control: !!h.control, owner: !!h.owner, role: h.role || "none" };
  }

  return { formatMhz, parseFreq, tuneStep, decodeSpectrum, decodeAudio, xToHz, hzToX, modeSearch, buildPanel, tableColumns, humanize, formatValue, groupSettings, settingValue, permissions };
});
