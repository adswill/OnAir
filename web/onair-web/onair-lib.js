/* OnAir Web: pure helpers (no DOM, no network). Loaded by the page as a plain script and by the node tests (tests/web/).
 * ADPCM decode, frame parsing, message building, frequency and bin mapping, view zoom and pan, palettes, passband rules.
 * Protocol: docs/ONAIR-WEB-PROTOCOL.md (version 1). */
(function (root, factory) {
  const lib = factory();
  if (typeof module === "object" && module.exports) module.exports = lib;
  else root.OnAirLib = lib;
})(typeof globalThis !== "undefined" ? globalThis : this, function () {
  "use strict";

  const PROTO = 1;
  const SUBPROTOCOL = "onair-web.v1";
  const MODES = ["am", "nfm", "wfm", "usb", "lsb", "cw"];

  // ---- IMA ADPCM (standard step table, same as WAV 0x11 / DVI) ----
  const STEP_TABLE = [
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143,
    157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552,
    1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487,
    12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767];
  const INDEX_TABLE = [-1, -1, -1, -1, 2, 4, 6, 8];

  function clamp(v, lo, hi) { return v < lo ? lo : v > hi ? hi : v; }

  /** Decode n samples of IMA ADPCM (low nibble first) starting from the given predictor and step index. Returns Int16Array. */
  function adpcmDecode(bytes, n, predictor, index) {
    const out = new Int16Array(n);
    let pred = clamp(predictor | 0, -32768, 32767);
    let idx = clamp(index | 0, 0, 88);
    for (let i = 0; i < n; i++) {
      const b = bytes[i >> 1];
      const code = (i & 1) ? (b >> 4) & 15 : b & 15;
      const step = STEP_TABLE[idx];
      let diff = step >> 3;
      if (code & 4) diff += step;
      if (code & 2) diff += step >> 1;
      if (code & 1) diff += step >> 2;
      pred = clamp(code & 8 ? pred - diff : pred + diff, -32768, 32767);
      idx = clamp(idx + INDEX_TABLE[code & 7], 0, 88);
      out[i] = pred;
    }
    return out;
  }

  // ---- binary frames ----
  /** Spectrum row (0x10). Returns null for anything malformed. */
  function parseSpectrum(buf) {
    if (!buf || buf.byteLength < 32) return null;
    const v = new DataView(buf);
    if (v.getUint8(0) !== 0x10 || v.getUint8(1) !== 1) return null;
    const n = v.getUint16(2, true);
    if (n < 1 || buf.byteLength < 32 + n) return null;
    const startHz = v.getFloat64(8, true), stopHz = v.getFloat64(16, true);
    const floorDb = v.getFloat32(24, true), topDb = v.getFloat32(28, true);
    if (!(stopHz > startHz) || !isFinite(floorDb) || !isFinite(topDb)) return null;
    return { seq: v.getUint32(4, true), bins: n, startHz, stopHz, floorDb, topDb, levels: new Uint8Array(buf, 32, n) };
  }

  /** Audio frame (0x20). Returns { seq, rate, levelDb, squelched, samples:Int16Array } or null. */
  function parseAudio(buf) {
    if (!buf || buf.byteLength < 24) return null;
    const v = new DataView(buf);
    if (v.getUint8(0) !== 0x20) return null;
    const codec = v.getUint8(1), n = v.getUint16(2, true), rate = v.getUint32(8, true);
    if (rate < 1000 || rate > 192000 || n > 48000) return null;
    const flags = v.getUint8(16);
    const f = { seq: v.getUint32(4, true), rate, levelDb: v.getFloat32(12, true), squelched: (flags & 1) !== 0, samples: null };
    if (f.squelched) { f.samples = new Int16Array(n); return f; }
    if (codec === 0) {
      if (buf.byteLength < 24 + n * 2) return null;
      f.samples = new Int16Array(n);
      for (let i = 0; i < n; i++) f.samples[i] = v.getInt16(24 + i * 2, true);
    } else if (codec === 1) {
      if (buf.byteLength < 24 + ((n + 1) >> 1)) return null;
      f.samples = adpcmDecode(new Uint8Array(buf, 24, (n + 1) >> 1), n, v.getInt16(18, true), v.getUint8(20));
    } else return null;
    return f;
  }

  /** Spectrum level byte to dBFS. */
  function levelToDb(row, v) { return row.floorDb + (v / 255) * (row.topDb - row.floorDb); }

  // ---- messages ----
  function sanitizeText(s, max) { return String(s == null ? "" : s).replace(/[<>&"'`\u0000-\u001f]/g, "").slice(0, max); }

  const msg = {
    hello(name, password, invite) {
      const m = { type: "hello", proto: PROTO };
      const n = sanitizeText(name, 32); if (n) m.name = n;
      if (password) m.password = String(password).slice(0, 128);
      if (invite) m.invite = String(invite).slice(0, 128);
      return m;
    },
    tune(o) {
      const m = { type: "tune" };
      if (o.freq_hz != null) m.freq_hz = Math.round(clamp(o.freq_hz, 0, 1e11));
      if (o.mode != null && MODES.includes(o.mode)) m.mode = o.mode;
      if (o.low_hz != null && o.high_hz != null) { m.low_hz = Math.round(o.low_hz); m.high_hz = Math.round(o.high_hz); }
      return m;
    },
    rx(o) {
      const m = { type: "rx" };
      if (o.squelch_db != null) m.squelch_db = Math.round(clamp(o.squelch_db, -200, 0));
      if (o.agc != null) m.agc = o.agc;
      if (o.gain_db != null) m.gain_db = Math.round(clamp(o.gain_db, -20, 60));
      if (o.deemph_us != null) m.deemph_us = o.deemph_us;
      return m;
    },
    audio(on, codec, rate) { const m = { type: "audio", on: !!on }; if (codec) m.codec = codec; if (rate) m.rate = rate; return m; },
    spectrum(on, bins, fps, startHz, stopHz) {
      const m = { type: "spectrum", on: !!on };
      if (bins) m.bins = Math.round(clamp(bins, 64, 4096));
      if (fps) m.fps = Math.round(clamp(fps, 1, 30));
      if (startHz != null && stopHz != null) { m.start_hz = Math.round(startHz); m.stop_hz = Math.round(stopHz); }
      return m;
    },
    subscribe(decoder, freq) { const m = { type: "subscribe", decoder }; if (freq != null) m.freq_hz = Math.round(freq); return m; },
    unsubscribe(decoder, freq) { const m = { type: "unsubscribe", decoder }; if (freq != null) m.freq_hz = Math.round(freq); return m; },
    ping(t) { return { type: "ping", t }; },
    bye() { return { type: "bye" }; },
  };

  /** Parse one server text frame. Returns the object or null (non-JSON, no type). Unknown types are returned; the caller ignores them. */
  function parseServerText(s) {
    let o;
    try { o = JSON.parse(s); } catch (e) { return null; }
    return o && typeof o === "object" && !Array.isArray(o) && typeof o.type === "string" ? o : null;
  }

  /** What to do after a WebSocket close. Returns { retry, delayMs, text }. */
  function closeAdvice(code, attempt) {
    const backoff = Math.min(30000, 1000 * Math.pow(2, Math.min(attempt, 5)));
    switch (code) {
      case 4029: return { retry: true, delayMs: Math.max(5000, backoff), text: "Server busy or full. Trying again automatically." };
      case 4001: return { retry: false, delayMs: 0, text: "Access refused: wrong or missing password or invite." };
      case 4040: return { retry: false, delayMs: 0, text: "You were removed by the server." };
      case 4041: return { retry: false, delayMs: 0, text: "Idle or session time limit reached." };
      case 4008: return { retry: true, delayMs: 10000, text: "Too many messages sent; reconnecting." };
      case 1000: return { retry: false, delayMs: 0, text: "Disconnected." };
      default: return { retry: true, delayMs: backoff, text: "Connection lost. Reconnecting." };
    }
  }

  // ---- frequency, passband, bins ----
  function formatFreq(hz) {
    hz = Math.max(0, Math.round(hz));
    const s = String(hz).padStart(10, "0");
    return s.slice(0, 4) + "." + s.slice(4, 7) + "." + s.slice(7);
  }
  /** "145.5" (MHz), "145500" (kHz when no dot and > 30000... ) or "145.500.000" to Hz. Returns NaN if not a number. */
  function parseFreq(text, unit) {
    let t = String(text).trim().toLowerCase().replace(/\s+/g, "");
    let mult = unit === "khz" ? 1e3 : unit === "hz" ? 1 : 1e6;
    const m = t.match(/(hz|khz|mhz|ghz)$/);
    if (m) { mult = { hz: 1, khz: 1e3, mhz: 1e6, ghz: 1e9 }[m[1]]; t = t.slice(0, -m[1].length); }
    if ((t.match(/\./g) || []).length > 1) { t = t.replace(/\./g, ""); if (!m) mult = 1; }   // 145.500.000 is written in Hz
    const v = Number(t.replace(",", "."));
    return isFinite(v) && t !== "" ? Math.round(v * mult) : NaN;
  }
  const STEPS = [10, 100, 500, 1000, 2500, 5000, 6250, 8330, 9000, 10000, 12500, 25000, 50000, 100000, 200000, 1000000];
  function stepLabel(hz) { return hz >= 1e6 ? hz / 1e6 + " MHz" : hz >= 1000 ? hz / 1000 + " kHz" : hz + " Hz"; }
  function snap(hz, step) { return Math.round(hz / step) * step; }

  const MODE_LIMITS = {
    am: [-10000, 10000], nfm: [-10000, 10000], wfm: [-110000, 110000], usb: [-1000, 10000], lsb: [-10000, 1000], cw: [-1500, 1500] };
  const MODE_DEFAULT = {
    am: [-4500, 4500], nfm: [-6250, 6250], wfm: [-100000, 100000], usb: [100, 2800], lsb: [-2800, -100], cw: [-250, 250] };
  const MODE_BW_RANGE = { am: [1000, 20000], nfm: [2500, 20000], wfm: [30000, 220000], usb: [500, 4000], lsb: [500, 4000], cw: [50, 1000] };

  /** Passband for a mode and a total bandwidth in Hz, within the mode's limits. */
  function passband(mode, bw) {
    const lim = MODE_LIMITS[mode] || MODE_LIMITS.nfm, r = MODE_BW_RANGE[mode] || MODE_BW_RANGE.nfm;
    bw = clamp(Math.round(bw), r[0], r[1]);
    let lo, hi;
    if (mode === "usb") { lo = 100; hi = lo + bw; }
    else if (mode === "lsb") { hi = -100; lo = hi - bw; }
    else { lo = -Math.round(bw / 2); hi = Math.round(bw / 2); }
    lo = clamp(lo, lim[0], lim[1]); hi = clamp(hi, lim[0], lim[1]);
    if (hi - lo < 50) hi = lo + 50;
    return { low_hz: lo, high_hz: hi };
  }
  function bandwidthOf(low, high) { return Math.abs(high - low); }

  /** The usable part of the capture (the server accepts centre +- 48 % of the rate). */
  function captureBounds(cap) { const h = cap.rate_hz * 0.48; return { start: cap.centre_hz - h, stop: cap.centre_hz + h }; }
  function fullBounds(cap) { return { start: cap.centre_hz - cap.rate_hz / 2, stop: cap.centre_hz + cap.rate_hz / 2 }; }

  // ---- view (the slice of spectrum on screen) ----
  const MIN_SPAN = 2000;
  function hzToX(view, hz, width) { return ((hz - view.start) / (view.stop - view.start)) * width; }
  function xToHz(view, x, width) { return view.start + (x / width) * (view.stop - view.start); }
  function normView(view, b) {
    let span = clamp(view.stop - view.start, Math.min(MIN_SPAN, b.stop - b.start), b.stop - b.start);
    let s = view.start;
    if (s < b.start) s = b.start;
    if (s + span > b.stop) s = b.stop - span;
    return { start: s, stop: s + span };
  }
  /** factor < 1 zooms in. anchorHz stays under the pointer. */
  function zoomView(view, b, factor, anchorHz) {
    const span = view.stop - view.start, ns = span * factor;
    const frac = (anchorHz - view.start) / span;
    const s = anchorHz - frac * ns;
    return normView({ start: s, stop: s + ns }, b);
  }
  function panView(view, b, dHz) { return normView({ start: view.start + dHz, stop: view.stop + dHz }, b); }
  function viewIsFull(view, b) { return view.start <= b.start + 1 && view.stop >= b.stop - 1; }
  /** Slice for a band (start/stop Hz) clipped to the capture; null when it does not overlap by at least 1 kHz. */
  function bandSlice(band, b) {
    const s = Math.max(band.start, b.start), e = Math.min(band.stop, b.stop);
    return e - s >= 1000 ? normView({ start: s, stop: e }, b) : null;
  }

  /** Bin index of a frequency in a spectrum row (-1 outside). */
  function freqToBin(row, hz) {
    if (hz < row.startHz || hz >= row.stopHz) return -1;
    return Math.min(row.bins - 1, Math.floor(((hz - row.startHz) / (row.stopHz - row.startHz)) * row.bins));
  }
  function binToFreq(row, i) { return row.startHz + ((i + 0.5) * (row.stopHz - row.startHz)) / row.bins; }

  /** Map a spectrum row onto `width` pixels of `view`: out[x] = level byte, 0 where the row does not reach. Peak of the bins that share a pixel. */
  function rowToPixels(row, view, width, out) {
    out = out && out.length === width ? out : new Uint8Array(width);
    out.fill(0);
    const vspan = view.stop - view.start, rspan = row.stopHz - row.startHz;
    const binHz = rspan / row.bins;
    for (let x = 0; x < width; x++) {
      const h0 = view.start + (x / width) * vspan, h1 = view.start + ((x + 1) / width) * vspan;
      if (h1 <= row.startHz || h0 >= row.stopHz) continue;
      let b0 = Math.floor((Math.max(h0, row.startHz) - row.startHz) / binHz);
      let b1 = Math.ceil((Math.min(h1, row.stopHz) - row.startHz) / binHz) - 1;
      b0 = clamp(b0, 0, row.bins - 1); b1 = clamp(b1, b0, row.bins - 1);
      let m = 0;
      for (let b = b0; b <= b1; b++) if (row.levels[b] > m) m = row.levels[b];
      out[x] = m;
    }
    return out;
  }

  // ---- palettes ----
  // the app's default map (SDR++ "Navy heat", colormap.h): navy floor, blue, white, yellow, orange, red
  const PALETTES = {
    navy: [[0, 0, 0, 32], [1 / 14, 0, 0, 48], [2 / 14, 0, 0, 80], [3 / 14, 0, 0, 145], [4 / 14, 30, 144, 255], [5 / 14, 255, 255, 255], [6 / 14, 255, 255, 0], [7 / 14, 254, 109, 22], [8 / 14, 254, 109, 22], [9 / 14, 255, 0, 0], [10 / 14, 255, 0, 0], [11 / 14, 198, 0, 0], [12 / 14, 159, 0, 0], [13 / 14, 117, 0, 0], [1, 74, 0, 0]],
    amber: [[0, 0x0a, 0x08, 0x06], [0.25, 0x3a, 0x22, 0x08], [0.55, 0xa0, 0x62, 0x14], [0.8, 0xdb, 0xa6, 0x4d], [1, 0xff, 0xf0, 0xc8]],
    classic: [[0, 0, 0, 0x20], [0.2, 0, 0x20, 0xa0], [0.4, 0, 0xc0, 0xe0], [0.6, 0x40, 0xe0, 0x40], [0.8, 0xf0, 0xe0, 0x20], [1, 0xff, 0x30, 0x20]],
    viridis: [[0, 0x44, 0x01, 0x54], [0.25, 0x3b, 0x52, 0x8b], [0.5, 0x21, 0x91, 0x8c], [0.75, 0x5d, 0xc8, 0x63], [1, 0xfd, 0xe7, 0x25]],
    grey: [[0, 0, 0, 0], [1, 255, 255, 255]],
    turbo: [[0, 0x30, 0x12, 0x3b], [0.15, 0x46, 0x6b, 0xe3], [0.35, 0x1b, 0xd0, 0xd5], [0.55, 0x62, 0xfc, 0x6b], [0.75, 0xf4, 0xc2, 0x1f], [0.9, 0xe6, 0x4b, 0x0b], [1, 0x7a, 0x04, 0x03]],
    magma: [[0, 0x00, 0x00, 0x04], [0.25, 0x3b, 0x0f, 0x70], [0.5, 0x8c, 0x29, 0x81], [0.75, 0xf1, 0x60, 0x5d], [1, 0xfc, 0xfd, 0xbf]],
    ice: [[0, 0x05, 0x0a, 0x14], [0.3, 0x0e, 0x3a, 0x6b], [0.6, 0x3c, 0x9a, 0xc9], [0.85, 0xbf, 0xe6, 0xf2], [1, 0xff, 0xff, 0xff]],
  };
  const PALETTE_NAMES = Object.keys(PALETTES);
  const PALETTE_LABELS = { navy: "Classic", amber: "Amber", classic: "Blue-green", viridis: "Viridis", grey: "Grey", turbo: "Turbo", magma: "Magma", ice: "Ice" };
  /** 256 x RGB bytes for the named palette (unknown name: amber). */
  function makePalette(name) {
    const stops = PALETTES[name] || PALETTES.amber, out = new Uint8Array(256 * 3);
    for (let i = 0; i < 256; i++) {
      const t = i / 255;
      let k = 1; while (k < stops.length - 1 && stops[k][0] < t) k++;
      const a = stops[k - 1], b = stops[k], u = clamp((t - a[0]) / (b[0] - a[0] || 1), 0, 1);
      for (let c = 0; c < 3; c++) out[i * 3 + c] = Math.round(a[c + 1] + (b[c + 1] - a[c + 1]) * u);
    }
    return out;
  }

  // ---- S-meter: relative to full scale, not calibrated. S0 = -120 dBFS, 6 dB per S unit, S9 = -66, then +10 dB steps ----
  function sMeter(levelDb) {
    if (!isFinite(levelDb)) return { s: 0, over: 0, text: "S0", frac: 0 };
    const s9 = -66;
    let s, over = 0;
    if (levelDb <= s9) s = clamp(Math.round(9 + (levelDb - s9) / 6), 0, 9);
    else { s = 9; over = Math.round((levelDb - s9) / 10) * 10; }
    const frac = clamp((levelDb + 120) / 90, 0, 1);
    return { s, over, text: over ? "S9+" + over : "S" + s, frac };
  }

  // ---- decoder results: turn any JSON value into rows for a table ----
  /** Returns { kind:"table", columns, rows } for an array of objects, { kind:"kv", rows:[[k,v]] } for an object, { kind:"text", text } otherwise. Depth and size limited. */
  function toTable(data) {
    const cell = (v) => v == null ? "" : typeof v === "object" ? JSON.stringify(v).slice(0, 200) : String(v).slice(0, 200);
    if (Array.isArray(data)) {
      if (data.length && data.every((r) => r && typeof r === "object" && !Array.isArray(r))) {
        const cols = [];
        for (const r of data.slice(0, 200)) for (const k of Object.keys(r)) if (!cols.includes(k) && cols.length < 24) cols.push(k);
        return { kind: "table", columns: cols, rows: data.slice(0, 200).map((r) => cols.map((c) => cell(r[c]))) };
      }
      return { kind: "text", text: data.slice(0, 200).map(cell).join("\n") };
    }
    if (data && typeof data === "object") {
      const rows = [];
      const walk = (o, prefix, depth) => {
        for (const k of Object.keys(o)) {
          if (rows.length >= 300) return;
          const v = o[k], key = prefix ? prefix + "." + k : k;
          if (v && typeof v === "object" && !Array.isArray(v) && depth < 3) walk(v, key, depth + 1);
          else rows.push([key, cell(v)]);
        }
      };
      walk(data, "", 0);
      return { kind: "kv", rows };
    }
    return { kind: "text", text: cell(data) };
  }

  // ---- the host's frequency ranges (welcome.ranges / "ranges" message; docs/ONAIR-WEB.md, "Frequency ranges") ----
  // An advisory copy of the server's rules, so the page can grey out, highlight and snap. The SERVER decides (error "forbidden_range").
  // r = { restricted, action, allow: [{lo, hi, modes?, note?}], block: [...], notes: [...] }; modes missing: all modes. Edges are inclusive.
  const RANGE_EPS = 0.001;
  function guardMargin(bw) { bw = isFinite(bw) && bw > 0 ? bw : 0; return Math.min(5000, Math.max(200, 0.05 * bw)); }
  function rangeActive(r) { return !!(r && (r.restricted || (r.block && r.block.length))); }
  function mergeRanges(list) {
    const v = (list || []).filter((x) => x && x.hi >= x.lo).map((x) => ({ lo: x.lo, hi: x.hi })).sort((a, b) => a.lo - b.lo), o = [];
    for (const x of v) { if (o.length && x.lo <= o[o.length - 1].hi + RANGE_EPS) o[o.length - 1].hi = Math.max(o[o.length - 1].hi, x.hi); else o.push(x); }
    return o;
  }
  function hasMode(x, mode) { return !x.modes || x.modes.includes(mode); }
  function modeRanges(r, mode) { return mergeRanges((r.allow || []).filter((a) => hasMode(a, mode))); }
  function containsSpan(merged, lo, hi) { return merged.some((e) => e.lo - RANGE_EPS <= lo && hi <= e.hi + RANGE_EPS); }
  /** "ok", "not_allowed", "mode", "blocked", "capture" (runs over the capture edge) or "invalid". cap = {centre_hz, rate_hz} or null. */
  function rangeCheck(r, mode, f, low, high, cap) {
    if (!isFinite(f) || !isFinite(low) || !isFinite(high) || !(low < high) || f < 0 || f > 1e12) return "invalid";
    if (!rangeActive(r)) return "ok";
    const g = guardMargin(high - low), lo = f + low - g, hi = f + high + g;
    if (cap && cap.rate_hz > 0) { const u = cap.rate_hz * 0.48; if (lo < cap.centre_hz - u || hi > cap.centre_hz + u) return "capture"; }
    for (const b of r.block || []) if (hasMode(b, mode) && b.lo <= hi && b.hi >= lo) return "blocked";
    if (!r.restricted) return "ok";
    if (containsSpan(modeRanges(r, mode), lo, hi)) return "ok";
    return containsSpan(mergeRanges(r.allow), lo, hi) ? "mode" : "not_allowed";
  }
  /** The nearest tuned frequency (Hz) for which rangeCheck is "ok", or null. */
  function rangeSnap(r, mode, f, low, high, cap) {
    if (!isFinite(f) || !(low < high)) return null;
    if (rangeCheck(r, mode, f, low, high, cap) === "ok") return f;
    const g = guardMargin(high - low);
    let areas = r.restricted ? modeRanges(r, mode) : [{ lo: 0, hi: 1e12 }];
    if (cap && cap.rate_hz > 0) { const u = cap.rate_hz * 0.48; areas = areas.map((a) => ({ lo: Math.max(a.lo, cap.centre_hz - u + 0.01), hi: Math.min(a.hi, cap.centre_hz + u - 0.01) })); }
    let best = null;
    const consider = (c) => { if (rangeCheck(r, mode, c, low, high, cap) === "ok" && (best === null || Math.abs(c - f) < Math.abs(best - f))) best = c; };
    for (const a of areas) {
      const fmin = a.lo - low + g, fmax = a.hi - high - g;
      if (!(a.hi > a.lo) || fmin > fmax) continue;
      consider(clamp(f, fmin, fmax));
      for (const b of r.block || []) { if (!hasMode(b, mode)) continue; consider(clamp(b.hi + 0.01 - low + g, fmin, fmax)); consider(clamp(b.lo - 0.01 - high - g, fmin, fmax)); }
      consider(fmin); consider(fmax);
    }
    return best;
  }
  /** The modes that are allowed somewhere (null: all of them). */
  function rangeModes(r) {
    if (!r || !r.restricted) return null;
    return MODES.filter((m) => (r.allow || []).some((a) => hasMode(a, m)));
  }
  /** What the listener can see: the allowed ranges minus the blocks that name no mode (the same rule the server uses to mask the spectrum). */
  function visibleRanges(r) {
    let vis = r && r.restricted ? mergeRanges(r.allow) : [{ lo: 0, hi: 1e12 }];
    const hide = mergeRanges((r && r.block || []).filter((b) => !b.modes));
    for (const h of hide) {
      const next = [];
      for (const v of vis) {
        if (h.hi < v.lo || h.lo > v.hi) { next.push(v); continue; }
        if (h.lo > v.lo) next.push({ lo: v.lo, hi: h.lo });
        if (h.hi < v.hi) next.push({ lo: h.hi, hi: v.hi });
      }
      vis = next;
    }
    return vis;
  }
  /** The parts of [start, stop] that are NOT visible (to be darkened on the spectrum). */
  function hiddenRanges(r, start, stop) {
    const out = [];
    let at = start;
    for (const v of visibleRanges(r)) {
      if (v.hi <= at) continue;
      if (v.lo > at) out.push({ lo: at, hi: Math.min(v.lo, stop) });
      at = Math.max(at, v.hi);
      if (at >= stop) break;
    }
    if (at < stop) out.push({ lo: at, hi: stop });
    return out.filter((x) => x.hi > x.lo);
  }
  /** Does the range [lo, hi] share at least minWidth Hz with the visible part? (for greying out band buttons) */
  function rangeOverlaps(r, lo, hi, minWidth) {
    if (!rangeActive(r)) return true;
    return visibleRanges(r).some((v) => Math.min(v.hi, hi) - Math.max(v.lo, lo) >= (minWidth || 1000));
  }
  /** Short lines for the page: the allowed ranges (with modes and notes), the blocked ones, the host's notes. */
  function describeRanges(r, fmt) {
    const f = fmt || ((hz) => (hz / 1e6).toFixed(hz % 1e6 ? 4 : 0).replace(/0+$/, "").replace(/\.$/, "") + " MHz");
    const line = (x) => f(x.lo) + " - " + f(x.hi) + (x.modes ? " (" + x.modes.join(", ").toUpperCase() + ")" : "") + (x.note ? ": " + sanitizeText(x.note, 100) : "");
    return {
      allow: r && r.restricted ? (r.allow || []).map(line) : [],
      block: (r && r.block || []).map(line),
      notes: (r && r.notes || []).map((n) => sanitizeText(n, 200)),
    };
  }
  /** Reads a ranges object from the server (welcome.ranges or the "ranges" message) defensively; null when there is nothing to enforce. */
  function readRanges(x) {
    if (!x || typeof x !== "object") return null;
    const list = (a) => (Array.isArray(a) ? a : []).filter((e) => e && isFinite(e.lo) && isFinite(e.hi) && e.hi >= e.lo)
      .map((e) => ({ lo: +e.lo, hi: +e.hi, modes: Array.isArray(e.modes) ? e.modes.filter((m) => MODES.includes(m)) : undefined, note: typeof e.note === "string" ? e.note : "" }));
    const r = { restricted: !!x.restricted, action: x.action === "clamp" ? "clamp" : "refuse", allow: list(x.allow), block: list(x.block), notes: Array.isArray(x.notes) ? x.notes.filter((n) => typeof n === "string") : [] };
    return rangeActive(r) ? r : null;
  }

  /** The record structure under a decoder's data that holds the tables: arrays of objects found at the top level or one level down. */
  function findTables(data) {
    const out = [];
    if (Array.isArray(data)) return [{ name: "", table: toTable(data) }];
    if (data && typeof data === "object") {
      const scalars = {};
      for (const k of Object.keys(data)) {
        const v = data[k];
        if (Array.isArray(v) && v.length && v.every((r) => r && typeof r === "object" && !Array.isArray(r))) out.push({ name: k, table: toTable(v) });
        else scalars[k] = v;
      }
      if (Object.keys(scalars).length) out.unshift({ name: "", table: toTable(scalars) });
    } else out.push({ name: "", table: toTable(data) });
    return out;
  }

  return {
    PROTO, SUBPROTOCOL, MODES, STEPS, STEP_TABLE, MODE_LIMITS, MODE_DEFAULT, MODE_BW_RANGE, PALETTE_NAMES, PALETTE_LABELS, MIN_SPAN,
    adpcmDecode, parseSpectrum, parseAudio, levelToDb, msg, parseServerText, closeAdvice, sanitizeText,
    formatFreq, parseFreq, stepLabel, snap, passband, bandwidthOf, captureBounds, fullBounds,
    hzToX, xToHz, normView, zoomView, panView, viewIsFull, bandSlice, freqToBin, binToFreq, rowToPixels,
    makePalette, sMeter, toTable, findTables, clamp,
    guardMargin, rangeActive, mergeRanges, rangeCheck, rangeSnap, rangeModes, visibleRanges, hiddenRanges, rangeOverlaps, describeRanges, readRanges,
  };
});
