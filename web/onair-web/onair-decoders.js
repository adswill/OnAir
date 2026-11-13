// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

/* OnAir Web: the Decoders tab. Pure helpers first (tested in tests/web/run.js), then the DOM views (browser only).
 * Events are the v1 schema of docs/ONAIR-WEB-PROTOCOL.md ("Decoder events"): {v:1, kind, type, ...}. Everything from the server is data: text goes in with
 * textContent, numbers are checked, pictures are drawn from PNG data URLs after the checks below. Nothing is stored on the server; "Save PNG" saves in this browser. */
(function (root, factory) {
  const lib = factory();
  if (typeof module === "object" && module.exports) module.exports = lib;
  else root.OnAirDecoders = lib;
})(typeof globalThis !== "undefined" ? globalThis : this, function () {
  "use strict";

  const PRESET_HZ = { adsb: 1090e6, ais: 162.0e6, aprs: 144.8e6, meteor: 137.9e6, wefax: 7.88e6, ft8: 14.074e6, ft4: 14.080e6, wspr: 14.0956e6, sstv: 14.230e6, cw: 14.030e6, rtty: 14.083e6 };
  const TITLES = { adsb: "ADS-B aircraft", ais: "AIS vessels", aprs: "APRS / packet", ft8: "FT8", ft4: "FT4", wspr: "WSPR", sstv: "SSTV", wefax: "Weather fax", meteor: "Meteor-M LRPT",
                   rds: "RDS", cw: "Morse", rtty: "RTTY" };

  const isNum = (x) => typeof x === "number" && isFinite(x);
  const n = (x, d) => (isNum(x) ? x : d === undefined ? 0 : d);
  const s = (x, max) => (typeof x === "string" ? x.slice(0, max || 200) : "");
  const arr = (x, max) => (Array.isArray(x) ? x.slice(0, max || 1000) : []);

  function validEvent(d) { return !!d && typeof d === "object" && d.v === 1 && typeof d.kind === "string" && typeof d.type === "string"; }

  // the frequency to subscribe at: the decoder's usual frequency when it lies in the capture, else the tuned one
  function defaultFreq(kind, tuned, capture) {
    const p = PRESET_HZ[kind];
    if (p && capture && isNum(capture.centre_hz) && isNum(capture.rate_hz) && Math.abs(p - capture.centre_hz) <= capture.rate_hz * 0.48) return p;
    return tuned;
  }

  function fmtFreq(hz) { return isNum(hz) ? (hz >= 1e6 ? (hz / 1e6).toFixed(hz % 1000 ? 4 : 3) + " MHz" : (hz / 1e3).toFixed(1) + " kHz") : ""; }
  function fmtAge(sec) { sec = n(sec); return sec < 90 ? Math.round(sec) + " s" : Math.round(sec / 60) + " min"; }
  function fmtUtc(sec) { if (!isNum(sec) || sec <= 0) return ""; const d = new Date(sec * 1000); const p = (v) => String(v).padStart(2, "0"); return p(d.getUTCHours()) + ":" + p(d.getUTCMinutes()) + ":" + p(d.getUTCSeconds()); }
  const pos = (la, lo) => isNum(la) && isNum(lo) && Math.abs(la) <= 90 && Math.abs(lo) <= 180;

  // ---------------------------------------------------------------- tables
  function aircraftTable(d) {
    const list = arr(d.aircraft, 300).filter((a) => a && typeof a === "object");
    list.sort((a, b) => n(a.age_s) - n(b.age_s));
    const rows = list.map((a) => [s(a.icao, 8), s(a.callsign, 16), isNum(a.alt_ft) ? String(a.alt_ft) : a.ground ? "ground" : "", isNum(a.speed_kt) ? String(a.speed_kt) : "",
      isNum(a.track_deg) ? String(a.track_deg) : "", isNum(a.vrate_fpm) ? String(a.vrate_fpm) : "", pos(a.lat, a.lon) ? n(a.lat).toFixed(3) + ", " + n(a.lon).toFixed(3) : "",
      s(a.squawk, 4), fmtAge(a.age_s)]);
    const marks = list.filter((a) => pos(a.lat, a.lon)).map((a) => ({ kind: "adsb", info: (isNum(a.alt_ft) ? a.alt_ft + " ft " : a.ground ? "ground " : "") + (isNum(a.speed_kt) ? a.speed_kt + " kt" : ""), id: s(a.icao, 8), label: s(a.callsign, 16) || s(a.icao, 8), lat: n(a.lat), lon: n(a.lon), heading: isNum(a.track_deg) ? a.track_deg : null,
      alert: n(a.emergency) > 0 || a.squawk === "7700" || a.squawk === "7600" || a.squawk === "7500",
      trail: arr(a.trail, 12).filter((p) => Array.isArray(p) && pos(p[0], p[1])).map((p) => [p[0], p[1]]) }));
    return { columns: ["ICAO", "Call", "Alt ft", "kt", "Trk", "ft/min", "Position", "Sq", "Age"], rows, marks,
      summary: n(d.aircraft_count) + " aircraft, " + n(d.with_position) + " with position, " + n(d.msgs_per_s).toFixed(0) + " msg/s" };
  }
  function vesselTable(d) {
    const list = arr(d.vessels, 400).filter((v) => v && typeof v === "object");
    list.sort((a, b) => n(a.age_s) - n(b.age_s));
    const rows = list.map((v) => [String(n(v.mmsi)), s(v.name, 40), s(v.callsign, 16), s(v.ship_type_text, 40), s(v.nav_status_text, 40), isNum(v.sog_kt) ? v.sog_kt.toFixed(1) : "",
      isNum(v.cog_deg) ? String(v.cog_deg) : "", pos(v.lat, v.lon) ? n(v.lat).toFixed(3) + ", " + n(v.lon).toFixed(3) : "", s(v.destination, 40), fmtAge(v.age_s)]);
    const marks = list.filter((v) => pos(v.lat, v.lon)).map((v) => ({ kind: "ais", info: s(v.ship_type_text, 40) + (isNum(v.sog_kt) ? " " + v.sog_kt.toFixed(1) + " kt" : "") + (v.destination ? " to " + s(v.destination, 40) : ""), id: String(n(v.mmsi)), label: s(v.name, 40) || String(n(v.mmsi)), lat: n(v.lat), lon: n(v.lon),
      heading: isNum(v.heading_deg) ? v.heading_deg : isNum(v.cog_deg) ? v.cog_deg : null, alert: false, trail: [] }));
    return { columns: ["MMSI", "Name", "Call", "Type", "Status", "kt", "Cog", "Position", "Destination", "Age"], rows, marks,
      summary: n(d.vessel_count) + " vessels, " + n(d.messages_ok) + " messages" };
  }
  function aprsTables(d) {
    const st = arr(d.stations, 300).filter((x) => x && typeof x === "object");
    const rows = st.map((x) => [s(x.call, 16), s(x.symbol, 4), pos(x.lat, x.lon) ? n(x.lat).toFixed(3) + ", " + n(x.lon).toFixed(3) : "", isNum(x.alt_m) ? String(x.alt_m) : "", isNum(x.speed_kt) ? String(x.speed_kt) : "",
      s(x.comment, 100), String(n(x.count))]);
    const frames = arr(d.frames, 60).filter((x) => x && typeof x === "object").map((f) => [s(f.from, 16) + ">" + s(f.to, 16) + (f.path ? "," + s(f.path, 80) : ""), s(f.type, 32), s(f.summary, 200)]);
    const marks = st.filter((x) => pos(x.lat, x.lon)).map((x) => ({ kind: "aprs", info: s(x.comment, 100), id: s(x.call, 16), label: s(x.call, 16), lat: n(x.lat), lon: n(x.lon), heading: isNum(x.course_deg) ? x.course_deg : null, alert: false, trail: [] }));
    return { columns: ["Station", "Sym", "Position", "Alt m", "kt", "Comment", "Frames"], rows, frames, frameColumns: ["Frame", "Type", "Says"], marks,
      summary: n(d.frames_ok) + " frames (" + n(d.frames_bad) + " failed the check), " + st.length + " stations" };
  }
  function ftxRows(list) {
    return arr(list, 400).filter((x) => x && typeof x === "object").map((x) => [fmtUtc(x.slot_utc), isNum(x.snr_db) ? String(x.snr_db) : "", isNum(x.dt_s) ? n(x.dt_s).toFixed(1) : "", isNum(x.rf_hz) ? fmtFreq(x.rf_hz) : String(n(x.audio_hz)), s(x.msg, 120)]);
  }
  // history of decodes: newest first, no duplicates (a replay for a late listener repeats what is known), at most max
  function mergeDecodes(list, d, max) {
    const seen = new Set(list.map((x) => x.slot_utc + "|" + x.audio_hz + "|" + x.msg));
    const fresh = arr(d.decodes, 400).filter((x) => x && typeof x === "object" && !seen.has(x.slot_utc + "|" + x.audio_hz + "|" + x.msg));
    return fresh.slice().reverse().concat(list).slice(0, max || 300);
  }

  // ---------------------------------------------------------------- the simple map (equirectangular, fitted to the points)
  function mapFit(points) {
    if (!points.length) return { lat0: -1, lat1: 1, lon0: -1, lon1: 1 };
    let la0 = 90, la1 = -90, lo0 = 180, lo1 = -180;
    for (const p of points) { la0 = Math.min(la0, p.lat); la1 = Math.max(la1, p.lat); lo0 = Math.min(lo0, p.lon); lo1 = Math.max(lo1, p.lon); }
    const padLa = Math.max(0.05, (la1 - la0) * 0.1), padLo = Math.max(0.05, (lo1 - lo0) * 0.1);
    return { lat0: Math.max(-90, la0 - padLa), lat1: Math.min(90, la1 + padLa), lon0: Math.max(-180, lo0 - padLo), lon1: Math.min(180, lo1 + padLo) };
  }
  // pixel position of a point; the longitude is scaled by cos(latitude) so a square on the ground is square on the screen
  function mapProject(lat, lon, b, w, h) {
    const k = Math.max(0.1, Math.cos(((b.lat0 + b.lat1) / 2) * Math.PI / 180));
    const spanX = (b.lon1 - b.lon0) * k, spanY = b.lat1 - b.lat0;
    const sc = Math.min(w / spanX, h / spanY);
    const ox = (w - spanX * sc) / 2, oy = (h - spanY * sc) / 2;
    return { x: ox + (lon - b.lon0) * k * sc, y: oy + (b.lat1 - lat) * sc };
  }

  // ---------------------------------------------------------------- pictures: strips of rows (events type "image" / "image_end")
  const B64 = /^[A-Za-z0-9+/]+={0,2}$/;
  // state: {id, w, h, ch, label, rows, done} or null. Returns {state, draw, reset}: draw = {url, dx, dy, dw, dh} to paint, reset = a new picture started
  function planStrip(state, ev) {
    if (!validEvent(ev)) return { state, draw: null, reset: false };
    if (ev.type === "image_end") {
      if (state && state.id === ev.id) { state = Object.assign({}, state, { done: true, rows: Math.max(state.rows, n(ev.rows)) }); }
      return { state, draw: null, reset: false };
    }
    if (ev.type !== "image") return { state, draw: null, reset: false };
    const w = n(ev.w), h = n(ev.h), y = n(ev.y, -1), rows = n(ev.rows), pw = n(ev.pw), ph = n(ev.ph);
    if (!Number.isInteger(w) || !Number.isInteger(h) || w < 1 || h < 1 || w > 8192 || h > 20000 || w * h > 40e6) return { state, draw: null, reset: false };
    if (!Number.isInteger(y) || !Number.isInteger(rows) || y < 0 || rows < 1 || y + rows > h || pw < 1 || ph < 1 || pw > w || ph > rows) return { state, draw: null, reset: false };
    if (typeof ev.png !== "string" || ev.png.length > 120000 || !B64.test(ev.png)) return { state, draw: null, reset: false };
    let reset = false;
    if (!state || state.id !== ev.id) { state = { id: ev.id, w, h, ch: ev.ch === 3 ? 3 : 1, label: s(ev.label, 64), rows: 0, done: false }; reset = true; }
    if (state.w !== w) return { state, draw: null, reset: false };
    state = Object.assign({}, state, { rows: Math.max(state.rows, y + rows), done: state.done || ev.final === true });
    return { state, draw: { url: "data:image/png;base64," + ev.png, dx: 0, dy: y, dw: w, dh: rows }, reset };
  }

  // ---------------------------------------------------------------- DOM views (browser)
  function el(doc, tag, cls, text) { const e = doc.createElement(tag); if (cls) e.className = cls; if (text != null) e.textContent = text; return e; }
  function tableEl(doc, columns, rows, max) {
    const t = el(doc, "table"), hr = el(doc, "tr");
    for (const c of columns) hr.appendChild(el(doc, "th", null, c));
    t.appendChild(hr);
    for (const r of rows.slice(0, max || 200)) { const tr = el(doc, "tr"); for (const v of r) tr.appendChild(el(doc, "td", null, v)); t.appendChild(tr); }
    return t;
  }
  function drawMap(canvas, marks) {
    const g = canvas.getContext("2d");
    const w = canvas.width, h = canvas.height;
    g.fillStyle = "#0a0908"; g.fillRect(0, 0, w, h);
    const all = [];
    for (const m of marks) { all.push(m); for (const p of m.trail) all.push({ lat: p[0], lon: p[1] }); }
    const b = mapFit(all);
    g.strokeStyle = "#2c2822"; g.lineWidth = 1; g.beginPath();   // a light graticule every 1, 2, 5 .. degrees
    const step = [0.1, 0.2, 0.5, 1, 2, 5, 10, 20].find((v) => (b.lat1 - b.lat0) / v <= 8) || 30;
    for (let la = Math.ceil(b.lat0 / step) * step; la <= b.lat1; la += step) { const p = mapProject(la, b.lon0, b, w, h), q = mapProject(la, b.lon1, b, w, h); g.moveTo(p.x, p.y); g.lineTo(q.x, q.y); }
    for (let lo = Math.ceil(b.lon0 / step) * step; lo <= b.lon1; lo += step) { const p = mapProject(b.lat0, lo, b, w, h), q = mapProject(b.lat1, lo, b, w, h); g.moveTo(p.x, p.y); g.lineTo(q.x, q.y); }
    g.stroke();
    g.font = "10px ui-monospace, monospace";
    for (const m of marks) {
      if (m.trail.length > 1) { g.strokeStyle = "#8a6a2f"; g.beginPath(); m.trail.forEach((p, i) => { const q = mapProject(p[0], p[1], b, w, h); if (i) g.lineTo(q.x, q.y); else g.moveTo(q.x, q.y); }); g.stroke(); }
      const p = mapProject(m.lat, m.lon, b, w, h);
      g.fillStyle = m.alert ? "#d9674f" : "#dba64d";
      g.beginPath(); g.arc(p.x, p.y, 3, 0, 2 * Math.PI); g.fill();
      if (m.heading != null) { const a = m.heading * Math.PI / 180; g.strokeStyle = g.fillStyle; g.beginPath(); g.moveTo(p.x, p.y); g.lineTo(p.x + Math.sin(a) * 9, p.y - Math.cos(a) * 9); g.stroke(); }
      if (marks.length <= 60) { g.fillStyle = "#d9d1c3"; g.fillText(m.label, p.x + 5, p.y - 4); }
    }
    g.fillStyle = "#8d8473";
    g.fillText(b.lat1.toFixed(2) + "N " + b.lon0.toFixed(2) + "E", 4, 11);
    g.fillText(b.lat0.toFixed(2) + "N " + b.lon1.toFixed(2) + "E", w - 120, h - 4);
  }

  // One view per subscription key. ingest() takes every event (pictures need all strips); refresh() redraws what changed.
  class DecoderView {
    constructor(doc, key, kind) {
      this.doc = doc; this.key = key; this.kind = kind;
      this.node = el(doc, "section", "decview");
      this.node.appendChild(el(doc, "h3", null, (TITLES[kind] || kind) + " (" + key + ")"));
      this.status = el(doc, "p", "dim mono", "Waiting for results.");
      this.body = el(doc, "div");
      this.node.appendChild(this.status); this.node.appendChild(this.body);
      this.data = null; this.decodes = []; this.pic = null; this.canvas = null; this.dirty = false;
      this.imgs = new Map();
    }
    ingest(d) {
      if (!validEvent(d)) return;
      if (d.type === "status") { this.statusEv = d; this.dirty = true; return; }
      if (d.type === "image" || d.type === "image_end") { this.ingestImage(d); return; }
      if (d.type === "decodes") this.decodes = mergeDecodes(this.decodes, d, 300);
      this.data = d; this.dirty = true;
    }
    ingestImage(d) {
      const r = planStrip(this.pic, d);
      this.pic = r.state; this.dirty = true;
      if (r.reset || !this.canvas) {
        if (!this.pic) return;
        this.canvas = el(this.doc, "canvas", "decimg");
        this.canvas.width = this.pic.w; this.canvas.height = this.pic.h;
        const g = this.canvas.getContext("2d"); g.fillStyle = "#0a0908"; g.fillRect(0, 0, this.canvas.width, this.canvas.height);
      }
      if (!r.draw || !this.canvas) return;
      const cv = this.canvas, im = new Image(), dr = r.draw;
      im.onload = () => { const g = cv.getContext("2d"); g.imageSmoothingEnabled = dr.dw !== im.width; g.drawImage(im, 0, 0, im.width, im.height, dr.dx, dr.dy, dr.dw, dr.dh); };
      im.src = dr.url;
    }
    refresh() {
      if (!this.dirty) return; this.dirty = false;
      const doc = this.doc, d = this.data, k = this.kind;
      if (this.statusEv) {
        const e = this.statusEv;
        this.status.textContent = e.state === "no_data" ? s(e.why, 200) : Object.keys(e).filter((x) => !["v", "kind", "type"].includes(x)).map((x) => x + " " + (typeof e[x] === "string" ? s(e[x], 40) : isNum(e[x]) ? e[x] : "")).join("  ");
      }
      if (k === "adsb" && d) { const t = aircraftTable(d); this.status.textContent = t.summary; this.tableMap(t); }
      else if (k === "ais" && d) { const t = vesselTable(d); this.status.textContent = t.summary; this.tableMap(t); }
      else if (k === "aprs" && d) {
        const t = aprsTables(d); this.status.textContent = t.summary; this.tableMap(t);
        this.body.appendChild(el(doc, "h3", "dim", "Latest frames")); this.body.appendChild(tableEl(doc, t.frameColumns, t.frames.slice().reverse(), 30));
      } else if ((k === "ft8" || k === "ft4" || k === "wspr") && d) {
        this.status.textContent = this.decodes.length + " decodes, clock error " + n(d.clock_err_s).toFixed(1) + " s";
        this.body.textContent = ""; this.body.appendChild(tableEl(doc, ["UTC", "SNR", "DT", "Frequency", "Message"], ftxRows(this.decodes), 300));
      } else if (k === "rds" && d) {
        this.status.textContent = d.sync ? "RDS in sync, " + n(d.groups) + " groups" : "No RDS yet";
        this.body.textContent = "";
        const rows = [["Station", s(d.ps, 16)], ["Radio text", s(d.radiotext, 80)], ["Programme type", s(d.pty, 32)], ["PI", s(d.pi, 8)], ["Call sign", s(d.callsign, 8)],
          ["Traffic", (d.tp ? "TP " : "") + (d.ta ? "TA" : "")], ["Content", d.music ? "music" : "speech"], ["Stereo", d.stereo ? "yes" : "no"]].filter((r) => r[1]);
        this.body.appendChild(tableEl(doc, ["", ""], rows));
      } else if (k === "cw" && d) {
        const sg = arr(d.signals, 24).filter((x) => x && typeof x === "object");
        this.status.textContent = sg.length + " signals, " + n(d.chars) + " characters";
        this.body.textContent = "";
        for (const x of sg) { this.body.appendChild(el(doc, "h3", "dim", "" + (n(x.offset_hz) >= 0 ? "+" : "") + n(x.offset_hz) + " Hz, " + n(x.wpm) + " wpm, " + n(x.snr_db) + " dB")); this.body.appendChild(el(doc, "pre", "mono", s(x.text, 700))); }
      } else if (k === "rtty" && d) {
        this.status.textContent = "RTTY " + n(d.baud) + " baud, shift " + n(d.shift_hz) + " Hz, " + n(d.chars) + " characters";
        this.body.textContent = ""; this.body.appendChild(el(doc, "pre", "mono", s(d.text, 2600)));
      } else if ((k === "sstv" || k === "wefax" || k === "meteor") && (this.pic || this.statusEv)) {
        if (this.pic) {
          const p = this.pic;
          this.status.textContent = (p.label ? p.label + ", " : "") + p.rows + " of " + p.h + " rows" + (p.done ? ", complete" : "");
          if (this.canvas && this.canvas.parentNode !== this.body) {
            this.body.textContent = ""; this.body.appendChild(this.canvas);
            const save = el(doc, "button", null, "Save PNG"); save.type = "button";
            save.addEventListener("click", () => { const cv = this.canvas; if (!cv) return; cv.toBlob((b) => { if (!b) return; const a = doc.createElement("a"); a.href = URL.createObjectURL(b); a.download = k + "-" + p.id + ".png"; a.click(); setTimeout(() => URL.revokeObjectURL(a.href), 5000); }); });
            this.body.appendChild(save);
          }
        }
      }
    }
    tableMap(t) {
      const doc = this.doc;
      if (!this.map) { this.map = el(doc, "canvas", "decmap"); this.map.width = 480; this.map.height = 300; this.tbl = el(doc, "div", "dectable"); }
      if (this.map.parentNode !== this.body) { this.body.textContent = ""; this.body.appendChild(this.map); this.body.appendChild(this.tbl); }
      drawMap(this.map, t.marks);
      this.tbl.textContent = ""; this.tbl.appendChild(tableEl(doc, t.columns, t.rows, 200));
    }
  }

  return { PRESET_HZ, TITLES, validEvent, defaultFreq, fmtFreq, fmtAge, fmtUtc, aircraftTable, vesselTable, aprsTables, ftxRows, mergeDecodes, mapFit, mapProject, planStrip, DecoderView };
});
