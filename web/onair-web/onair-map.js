// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

/* Offline interactive map for the decoder results (ADS-B aircraft, AIS vessels, APRS stations). Web Mercator on a canvas: drag to pan, wheel / pinch to zoom,
 * click a marker to select it. No tiles and no network: the land is the coarse outline of onair-world.js. Pure maths (project / unproject / fit / zoom) is tested
 * by tests/web/run.js; the canvas class below needs a DOM. */
(function (root, factory) {
  const lib = factory(root);
  if (typeof module === "object" && module.exports) module.exports = lib;
  else root.OnAirMap = lib;
})(typeof globalThis !== "undefined" ? globalThis : this, function (root) {
  "use strict";
  const clamp = (v, lo, hi) => (v < lo ? lo : v > hi ? hi : v);
  const RAD = Math.PI / 180, MAXLAT = 85.0511;
  const KIND_COLOUR = { adsb: "#dba64d", ais: "#5fb4d8", aprs: "#8fc79a" };

  // view: {lat, lon, z}: the centre and the zoom (world width = 256 * 2^z pixels, as in common web maps)
  function worldSize(z) { return 256 * Math.pow(2, z); }
  function mercY(lat) { const s = Math.sin(clamp(lat, -MAXLAT, MAXLAT) * RAD); return 0.5 - Math.log((1 + s) / (1 - s)) / (4 * Math.PI); }   // 0 .. 1, north first
  function mercLat(y) { const n = Math.PI * (1 - 2 * y); return Math.atan(Math.sinh(n)) / RAD; }
  function project(view, w, h, lat, lon) {
    const S = worldSize(view.z);
    let dx = (lon - view.lon) / 360; dx -= Math.round(dx);   // the shortest way round: the map wraps in longitude
    return { x: w / 2 + dx * S, y: h / 2 + (mercY(lat) - mercY(view.lat)) * S };
  }
  function unproject(view, w, h, x, y) {
    const S = worldSize(view.z);
    let lon = view.lon + ((x - w / 2) / S) * 360; lon = ((lon + 540) % 360) - 180;
    return { lat: mercLat(clamp(mercY(view.lat) + (y - h / 2) / S, 0, 1)), lon };
  }
  function clampView(v, w, h) {
    const zmin = Math.max(0, Math.log2(Math.max(w, h * 1.0, 64) / 256) - 0.5);
    const z = clamp(v.z, zmin, 18), S = worldSize(z), halfY = (h / 2) / S;
    const y = clamp(mercY(v.lat), halfY, 1 - halfY);
    return { lat: halfY >= 0.5 ? 0 : mercLat(y), lon: ((v.lon + 540) % 360) - 180, z };
  }
  // zoom by dz levels keeping the point under (x, y) fixed
  function zoomAt(view, w, h, x, y, dz) {
    const before = unproject(view, w, h, x, y), z = clamp(view.z + dz, 0, 18);
    const S = worldSize(z);
    const lon = before.lon - ((x - w / 2) / S) * 360, my = mercY(before.lat) - (y - h / 2) / S;
    return clampView({ lat: mercLat(clamp(my, 0, 1)), lon, z }, w, h);
  }
  function panBy(view, w, h, dx, dy) {
    const S = worldSize(view.z);
    return clampView({ lat: mercLat(clamp(mercY(view.lat) - dy / S, 0, 1)), lon: view.lon - (dx / S) * 360, z: view.z }, w, h);
  }
  // the view that shows all points (padding in pixels); no points: the whole world; one point: a city-sized view
  function fit(points, w, h, pad) {
    pad = pad == null ? 30 : pad;
    if (!points.length) return clampView({ lat: 25, lon: 10, z: 0 }, w, h);
    let la0 = 90, la1 = -90, lo0 = 360, lo1 = -360, sx = 0, sy = 0;
    for (const p of points) { la0 = Math.min(la0, p.lat); la1 = Math.max(la1, p.lat); lo0 = Math.min(lo0, p.lon); lo1 = Math.max(lo1, p.lon); sx += Math.cos(p.lon * RAD); sy += Math.sin(p.lon * RAD); }
    // when the points straddle the antimeridian the plain min / max would cover the whole world: use the circular mean for the centre
    const cLon = Math.atan2(sy, sx) / RAD;
    let spanLon = lo1 - lo0; if (spanLon > 180) { const wrapped = points.map((p) => ((p.lon - cLon + 540) % 360) - 180); spanLon = Math.max(...wrapped) - Math.min(...wrapped); }
    const cLat = (la0 + la1) / 2, spanY = Math.max(0.0001, mercY(la0) - mercY(la1)), spanX = Math.max(0.0001, spanLon / 360);
    const aw = Math.max(40, w - 2 * pad), ah = Math.max(40, h - 2 * pad);
    const z = Math.min(Math.log2(aw / (256 * spanX)), Math.log2(ah / (256 * spanY)), 12);
    const midY = (mercY(la0) + mercY(la1)) / 2;
    const lon = spanLon === lo1 - lo0 ? (lo0 + lo1) / 2 : cLon;
    return clampView({ lat: mercLat(midY) || cLat, lon, z }, w, h);
  }
  // nearest mark to (x, y) within r pixels, or null
  function hit(view, w, h, marks, x, y, r) {
    let best = null, bd = r * r;
    for (const m of marks) { const p = project(view, w, h, m.lat, m.lon), d = (p.x - x) * (p.x - x) + (p.y - y) * (p.y - y); if (d <= bd) { bd = d; best = m; } }
    return best;
  }
  // a round scale bar length: metres at the view's centre latitude for about 100 px
  function scaleBar(view, px) {
    const mPerPx = (40075016.686 * Math.cos(view.lat * RAD)) / worldSize(view.z), m = mPerPx * (px || 100);
    const p = Math.pow(10, Math.floor(Math.log10(m))), n = m / p, nice = n >= 5 ? 5 : n >= 2 ? 2 : 1;
    const len = nice * p; return { metres: len, px: len / mPerPx, label: len >= 1000 ? len / 1000 + " km" : len + " m" };
  }
  function graticuleStep(view, w) {
    const degPerPx = 360 / worldSize(view.z), span = degPerPx * w;
    return [0.01, 0.02, 0.05, 0.1, 0.2, 0.5, 1, 2, 5, 10, 20, 30].find((s) => span / s <= 8) || 30;
  }

  // ---------------------------------------------------------------- the canvas map
  function create(canvas, opts) {
    opts = opts || {};
    const g = canvas.getContext("2d"), st = { view: { lat: 25, lon: 10, z: 0 }, marks: [], sel: "", userMoved: false, colours: null, onSelect: opts.onSelect || null, drag: null, pinch: null, home: null };
    const size = () => { const dpr = root.devicePixelRatio || 1, w = Math.max(60, Math.round(canvas.clientWidth * dpr)), h = Math.max(60, Math.round(canvas.clientHeight * dpr)); if (canvas.width !== w || canvas.height !== h) { canvas.width = w; canvas.height = h; } return { w, h, dpr }; };
    function cols() {
      const cs = root.getComputedStyle ? getComputedStyle(canvas) : null, v = (n, d) => (cs && cs.getPropertyValue(n).trim()) || d;
      return { bg: v("--plotbg", "#050505"), land: v("--mapland", "#1c2430"), sea: v("--mapsea", "#0b0f16"), grid: v("--grid", "rgba(255,255,255,.1)"), text: v("--axis", "#9a9a9a"), bright: v("--bright", "#eee"), accent: v("--accent", "#3b82c4"), bad: v("--bad", "#e0615a") };
    }
    function draw() {
      const { w, h, dpr } = size(), C = st.colours || (st.colours = cols()), v = st.view = clampView(st.view, w, h);
      g.fillStyle = C.sea; g.fillRect(0, 0, w, h);
      const world = root.OnAirWorld || [];
      g.fillStyle = C.land; g.strokeStyle = C.grid; g.lineWidth = Math.max(1, dpr);
      const S = worldSize(v.z);
      for (const poly of world) {   // draw each polygon at the wraps that touch the screen
        for (let k = -1; k <= 1; k++) {
          g.beginPath();
          poly.forEach((q, i) => { const p = project(v, w, h, q[1], q[0]); const x = p.x + k * S; if (i) g.lineTo(x, p.y); else g.moveTo(x, p.y); });
          g.closePath(); g.fill(); g.stroke();
        }
      }
      // graticule with labels
      const gs = graticuleStep(v, w); g.strokeStyle = C.grid; g.fillStyle = C.text; g.font = `${10 * dpr}px ui-monospace, Menlo, monospace`; g.textBaseline = "top";
      const tl = unproject(v, w, h, 0, 0), br = unproject(v, w, h, w, h);
      g.beginPath();
      for (let la = Math.ceil(br.lat / gs) * gs; la <= tl.lat; la += gs) { const y = Math.round(project(v, w, h, la, v.lon).y) + 0.5; g.moveTo(0, y); g.lineTo(w, y); if (y > 14 * dpr) g.fillText(la.toFixed(gs < 1 ? 2 : 0), 3 * dpr, y + 2 * dpr); }
      for (let i = 0; i * gs < 360; i++) { const lo = Math.ceil((v.lon - (w / 2 / S) * 360) / gs) * gs + i * gs; if (lo > v.lon + (w / 2 / S) * 360) break; const x = Math.round(project(v, w, h, v.lat, lo).x) + 0.5; g.moveTo(x, 0); g.lineTo(x, h); g.fillText(lo.toFixed(gs < 1 ? 2 : 0), x + 3 * dpr, h - 14 * dpr); }
      g.stroke();
      // trails, then markers
      for (const m of st.marks) {
        if (m.trail && m.trail.length > 1) { g.strokeStyle = (KIND_COLOUR[m.kind] || "#dba64d") + "99"; g.lineWidth = Math.max(1, dpr); g.beginPath(); m.trail.forEach((q, i) => { const p = project(v, w, h, q[0], q[1]); if (i) g.lineTo(p.x, p.y); else g.moveTo(p.x, p.y); }); const e = project(v, w, h, m.lat, m.lon); g.lineTo(e.x, e.y); g.stroke(); }
      }
      const labels = st.marks.length <= 80 || v.z >= 6;
      g.font = `${11 * dpr}px ui-monospace, Menlo, monospace`; g.textBaseline = "middle";
      for (const m of st.marks) {
        const p = project(v, w, h, m.lat, m.lon); if (p.x < -20 || p.y < -20 || p.x > w + 20 || p.y > h + 20) continue;
        const col = m.alert ? C.bad : (KIND_COLOUR[m.kind] || "#dba64d"), sel = m.id === st.sel, r = (sel ? 6 : 4) * dpr;
        g.fillStyle = col; g.strokeStyle = sel ? C.bright : "rgba(0,0,0,.6)"; g.lineWidth = (sel ? 2 : 1) * dpr;
        g.beginPath();
        if (m.kind === "adsb" && m.heading != null) {   // a small arrow head pointing along the track
          const a = m.heading * RAD, c = Math.cos(a), s = Math.sin(a), pt = (fx, fy) => [p.x + (fx * c + fy * s), p.y + (fy * c - fx * s)];
          const q0 = pt(0, -r * 1.6), q1 = pt(r, r), q2 = pt(0, r * 0.4), q3 = pt(-r, r);
          g.moveTo(q0[0], q0[1]); g.lineTo(q1[0], q1[1]); g.lineTo(q2[0], q2[1]); g.lineTo(q3[0], q3[1]); g.closePath();
        } else if (m.kind === "ais") { g.moveTo(p.x, p.y - r * 1.4); g.lineTo(p.x + r, p.y + r); g.lineTo(p.x - r, p.y + r); g.closePath(); }
        else g.arc(p.x, p.y, r, 0, 2 * Math.PI);
        g.fill(); g.stroke();
        if (labels || sel) { g.fillStyle = C.bright; g.textAlign = "left"; g.fillText(m.label, p.x + r + 4 * dpr, p.y); }
      }
      // the receiver, when known
      if (st.home) { const p = project(v, w, h, st.home.lat, st.home.lon); g.strokeStyle = C.accent; g.lineWidth = 2 * dpr; g.beginPath(); g.arc(p.x, p.y, 7 * dpr, 0, 2 * Math.PI); g.moveTo(p.x - 11 * dpr, p.y); g.lineTo(p.x + 11 * dpr, p.y); g.moveTo(p.x, p.y - 11 * dpr); g.lineTo(p.x, p.y + 11 * dpr); g.stroke(); }
      // scale bar
      const sb = scaleBar(v, 100 * dpr); g.strokeStyle = C.bright; g.fillStyle = C.bright; g.lineWidth = 2 * dpr; g.textAlign = "left"; g.textBaseline = "alphabetic";
      g.beginPath(); g.moveTo(10 * dpr, h - 26 * dpr); g.lineTo(10 * dpr + sb.px, h - 26 * dpr); g.stroke(); g.fillText(sb.label, 10 * dpr, h - 30 * dpr);
    }
    // ---- interaction
    const pos = (e) => { const r = canvas.getBoundingClientRect(), dpr = root.devicePixelRatio || 1; return { x: (e.clientX - r.left) * dpr, y: (e.clientY - r.top) * dpr }; };
    const pts = new Map();
    canvas.addEventListener("pointerdown", (e) => { canvas.setPointerCapture(e.pointerId); pts.set(e.pointerId, pos(e)); st.drag = { moved: false, x: pos(e).x, y: pos(e).y }; if (pts.size === 2) { const [a, b] = [...pts.values()]; st.pinch = { d: Math.hypot(a.x - b.x, a.y - b.y) || 1, z: st.view.z }; st.drag.moved = true; } });
    canvas.addEventListener("pointermove", (e) => {
      if (!pts.has(e.pointerId)) return; const p = pos(e), prev = pts.get(e.pointerId); pts.set(e.pointerId, p); const { w, h } = size();
      if (pts.size === 2 && st.pinch) { const [a, b] = [...pts.values()], d = Math.hypot(a.x - b.x, a.y - b.y) || 1; st.view = zoomAt(st.view, w, h, (a.x + b.x) / 2, (a.y + b.y) / 2, Math.log2(d / st.pinch.d) + st.pinch.z - st.view.z); st.userMoved = true; draw(); return; }
      if (st.drag && (st.drag.moved || Math.hypot(p.x - st.drag.x, p.y - st.drag.y) > 6)) { st.drag.moved = true; st.view = panBy(st.view, w, h, p.x - prev.x, p.y - prev.y); st.userMoved = true; draw(); }
    });
    const up = (e) => {
      if (!pts.has(e.pointerId)) return; pts.delete(e.pointerId); if (pts.size < 2) st.pinch = null;
      if (st.drag && !st.drag.moved && pts.size === 0 && e.type === "pointerup") { const p = pos(e), { w, h } = size(), m = hit(st.view, w, h, st.marks, p.x, p.y, 16 * (root.devicePixelRatio || 1)); select(m ? m.id : "", true); }
      if (pts.size === 0) st.drag = null;
    };
    canvas.addEventListener("pointerup", up); canvas.addEventListener("pointercancel", up);
    canvas.addEventListener("wheel", (e) => { e.preventDefault(); const p = pos(e), { w, h } = size(); st.view = zoomAt(st.view, w, h, p.x, p.y, e.deltaY < 0 ? 0.5 : -0.5); st.userMoved = true; draw(); }, { passive: false });
    canvas.addEventListener("dblclick", (e) => { const p = pos(e), { w, h } = size(); st.view = zoomAt(st.view, w, h, p.x, p.y, 1); st.userMoved = true; draw(); });
    canvas.addEventListener("keydown", (e) => {
      const { w, h } = size(); const k = e.key, d = 60;
      if (k === "ArrowLeft") st.view = panBy(st.view, w, h, d, 0); else if (k === "ArrowRight") st.view = panBy(st.view, w, h, -d, 0);
      else if (k === "ArrowUp") st.view = panBy(st.view, w, h, 0, d); else if (k === "ArrowDown") st.view = panBy(st.view, w, h, 0, -d);
      else if (k === "+" || k === "=") st.view = zoomAt(st.view, w, h, w / 2, h / 2, 0.5); else if (k === "-") st.view = zoomAt(st.view, w, h, w / 2, h / 2, -0.5);
      else return; e.preventDefault(); st.userMoved = true; draw();
    });
    function select(id, fromMap) { st.sel = id || ""; draw(); if (st.onSelect) st.onSelect(id ? st.marks.find((m) => m.id === id) || null : null, !!fromMap); }
    return {
      state: st, draw,
      setMarks(marks) { st.marks = marks; if (!st.userMoved) this.fit(); else draw(); },
      fit() { const { w, h } = size(); st.userMoved = false; st.view = fit(st.marks.concat(st.home ? [st.home] : []), w, h); draw(); },
      zoom(dz) { const { w, h } = size(); st.view = zoomAt(st.view, w, h, w / 2, h / 2, dz); st.userMoved = true; draw(); },
      centreOn(id) { const m = st.marks.find((x) => x.id === id); if (!m) return; const { w, h } = size(); st.view = clampView({ lat: m.lat, lon: m.lon, z: Math.max(st.view.z, 7) }, w, h); st.userMoved = true; select(id, false); },
      select(id) { select(id, false); },
      setHome(h) { st.home = h; draw(); },
      refreshColours() { st.colours = null; draw(); },
    };
  }

  return { KIND_COLOUR, worldSize, mercY, mercLat, project, unproject, clampView, zoomAt, panBy, fit, hit, scaleBar, graticuleStep, create };
});
