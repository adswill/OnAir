/* OnAir Web 3.9: the shared renderers of a mode's panel. One set of components draws every mode from its schema: figures (tiles), yes/no,
 * text, tables (sortable, with a map when rows carry a position), pictures, constellations, text logs, number lists (sparklines) and
 * nested blocks. The DOM is reused between updates: tables and tiles keep their place, only the values change. */
(function (root) {
  "use strict";
  const L = root.OnAirLib;
  const $el = (tag, cls, text) => { const e = document.createElement(tag); if (cls) e.className = cls; if (text !== undefined) e.textContent = text; return e; };

  class PanelView {
    constructor(container) {
      this.c = container;
      this.nodes = new Map();   // name -> {el, kind, ...}
      this.sort = {};           // table name -> {col, dir}
      this.mapOn = {};          // table name -> bool
    }
    clear() { this.c.textContent = ""; this.nodes.clear(); }
    render(widgets, note) {
      const seen = new Set();
      let order = 0;
      if (!widgets.length) {
        if (!this.nodes.has("__empty")) { this.clear(); const e = $el("div", "empty", note || "No data yet."); this.c.appendChild(e); this.nodes.set("__empty", { el: e, kind: "empty" }); }
        else this.nodes.get("__empty").el.textContent = note || "No data yet.";
        return;
      }
      if (this.nodes.has("__empty")) this.clear();
      for (const w of widgets) {
        seen.add(w.name);
        let n = this.nodes.get(w.name);
        if (!n || n.kind !== w.kind) {
          if (n) n.el.remove();
          n = this.make(w);
          this.nodes.set(w.name, n);
        }
        this.update(n, w);
        n.el.style.order = order++;
        if (n.el.parentElement !== this.c) this.c.appendChild(n.el);
      }
      for (const [name, n] of this.nodes) if (!seen.has(name)) { n.el.remove(); this.nodes.delete(name); }
    }
    make(w) {
      const n = { kind: w.kind, name: w.name };
      switch (w.kind) {
      case "num": case "bool": case "text": {
        n.el = $el("div", "tile " + w.kind);
        n.l = $el("div", "l", w.label); n.v = $el("div", "v");
        n.el.append(n.l, n.v);
        break; }
      case "table": {
        n.el = $el("div", "tile wide");
        const head = $el("div", "tbl-head");
        n.l = $el("span", "l", w.label); n.count = $el("span", "n");
        head.append(n.l, n.count);
        if (w.map) { n.mapBtn = $el("button", "", "Map"); n.mapBtn.onclick = () => { this.mapOn[w.name] = !this.mapOn[w.name]; n.mapBtn.classList.toggle("on", !!this.mapOn[w.name]); n.map.style.display = this.mapOn[w.name] ? "" : "none"; if (this.mapOn[w.name]) this.drawMap(n, n.lastRows, w.map); }; head.appendChild(n.mapBtn); }
        n.el.appendChild(head);
        if (w.map) { n.map = $el("canvas", "map"); n.map.width = 800; n.map.height = 400; n.map.style.display = "none"; n.el.appendChild(n.map); }
        n.wrap = $el("div", "tbl"); n.table = $el("table"); n.thead = $el("thead"); n.tbody = $el("tbody");
        n.table.append(n.thead, n.tbody); n.wrap.appendChild(n.table); n.el.appendChild(n.wrap);
        n.cols = null;
        break; }
      case "lines": {
        n.el = $el("div", "tile wide"); n.l = $el("div", "l", w.label); n.v = $el("div", "lines"); n.el.append(n.l, n.v); break; }
      case "list": {
        n.el = $el("div", "tile"); n.l = $el("div", "l", w.label); n.v = $el("canvas", "spark"); n.v.width = 300; n.v.height = 60; n.el.append(n.l, n.v); break; }
      case "const": {
        n.el = $el("div", "tile"); n.l = $el("div", "l", w.label); n.v = $el("canvas", "const"); n.v.width = 300; n.v.height = 300; n.el.append(n.l, n.v); break; }
      case "image": {
        n.el = $el("div", "tile wide img"); n.l = $el("div", "l", w.label); n.v = $el("img"); n.v.alt = w.label; n.el.append(n.l, n.v); break; }
      case "obj": {
        n.el = $el("div", "tile wide obj"); n.l = $el("div", "l", w.label); n.v = $el("div", "panel"); n.el.append(n.l, n.v); n.sub = new PanelView(n.v); break; }
      default:
        n.el = $el("div", "tile"); n.l = $el("div", "l", w.label); n.v = $el("div", "v"); n.el.append(n.l, n.v);
      }
      return n;
    }
    update(n, w) {
      const v = w.value;
      switch (w.kind) {
      case "num": {
        const s = L.formatValue(v, w.unit);
        if (n.last !== s) { n.last = s; n.v.textContent = s; if (w.unit) { const u = $el("span", "u", w.unit); n.v.appendChild(u); } }
        break; }
      case "bool": {
        n.el.classList.toggle("yes", !!v); n.el.classList.toggle("no", !v);
        const s = v ? "yes" : "no"; if (n.last !== s) { n.last = s; n.v.textContent = s; }
        break; }
      case "text": {
        const s = String(v); if (n.last !== s) { n.last = s; n.v.textContent = s; }
        break; }
      case "table": this.updateTable(n, w); break;
      case "lines": {
        const s = v.slice(-200).join("\n"); if (n.last !== s) { const atEnd = n.v.scrollTop + n.v.clientHeight >= n.v.scrollHeight - 4; n.last = s; n.v.textContent = s; if (atEnd) n.v.scrollTop = n.v.scrollHeight; }
        break; }
      case "list": this.drawSpark(n.v, v); break;
      case "const": this.drawConst(n.v, v); break;
      case "image": { if (n.last !== v) { n.last = v; n.v.src = v; } break; }
      case "obj": {
        if (w.f) { n.sub.render(L.buildPanel(w.f, v, null, true), "empty"); break; }
        const ws = [];
        for (const k in v) {
          const x = v[k];
          if (x === null || x === undefined) continue;
          const kind = typeof x === "number" ? "num" : typeof x === "boolean" ? "bool" : typeof x === "string" ? "text" : Array.isArray(x) ? (x.length && typeof x[0] === "object" ? "table" : typeof x[0] === "string" ? "lines" : "list") : "obj";
          if (kind === "list" && !x.length) continue;
          if (kind === "table" && !x.length) continue;
          const ww = { kind, name: k, label: L.humanize(k), unit: "", value: x, tier: "s" };
          if (kind === "table") ww.cols = L.tableColumns(x);
          ws.push(ww);
        }
        n.sub.render(ws, "empty");
        break; }
      default: n.v.textContent = typeof v === "object" ? JSON.stringify(v).slice(0, 200) : String(v);
      }
    }
    updateTable(n, w) {
      const rows = w.value;
      n.lastRows = rows;
      n.count.textContent = rows.length + (rows.length === 200 ? "+" : "") + " rows";
      const cols = w.cols;
      const sig = cols.map(c => c.name).join("|");
      if (n.colSig !== sig) {
        n.colSig = sig; n.cols = cols;
        n.thead.textContent = "";
        const tr = $el("tr");
        for (const c of cols) { const th = $el("th", c.num ? "num" : "", c.label); th.onclick = () => { const s = this.sort[w.name] || {}; this.sort[w.name] = { col: c.name, dir: s.col === c.name && s.dir === 1 ? -1 : 1 }; this.updateTable(n, w); }; tr.appendChild(th); }
        n.thead.appendChild(tr);
        n.tbody.textContent = ""; n.trs = [];
      }
      let list = rows;
      const s = this.sort[w.name];
      if (s) list = rows.slice().sort((a, b) => { const x = a[s.col], y = b[s.col]; if (x === y) return 0; if (x === undefined || x === null) return 1; if (y === undefined || y === null) return -1; return (x < y ? -1 : 1) * s.dir; });
      const max = Math.min(list.length, 200);
      while (n.trs.length > max) n.trs.pop().remove();
      for (let i = 0; i < max; i++) {
        let tr = n.trs[i];
        if (!tr) { tr = $el("tr"); for (const c of cols) tr.appendChild($el("td", c.num ? "num" : "")); n.tbody.appendChild(tr); n.trs.push(tr); }
        const r = list[i];
        for (let j = 0; j < cols.length; j++) {
          const td = tr.children[j], val = r[cols[j].name];
          const txt = val === undefined || val === null ? "" : typeof val === "object" ? "" : L.formatValue(val, "");
          if (td.textContent !== txt) td.textContent = txt;
        }
      }
      if (this.mapOn[w.name] && n.map) this.drawMap(n, rows, w.map);
    }
    drawSpark(cv, v) {
      const c = cv.getContext("2d"), w = cv.width, h = cv.height, n = v.length;
      c.fillStyle = "#000"; c.fillRect(0, 0, w, h);
      if (!n) return;
      let lo = Infinity, hi = -Infinity;
      for (const x of v) if (typeof x === "number" && isFinite(x)) { if (x < lo) lo = x; if (x > hi) hi = x; }
      if (!isFinite(lo)) return;
      if (hi === lo) hi = lo + 1;
      c.strokeStyle = "#8ec5ff"; c.beginPath();
      for (let i = 0; i < n; i++) { const x = i * w / Math.max(1, n - 1), y = h - 2 - (v[i] - lo) / (hi - lo) * (h - 4); if (i) c.lineTo(x, y); else c.moveTo(x, y); }
      c.stroke();
      c.fillStyle = "#98a2b0"; c.font = "10px system-ui"; c.fillText(L.formatValue(hi, ""), 2, 10); c.fillText(L.formatValue(lo, ""), 2, h - 2);
    }
    drawConst(cv, v) {
      const c = cv.getContext("2d"), w = cv.width, h = cv.height;
      c.fillStyle = "#000"; c.fillRect(0, 0, w, h);
      c.strokeStyle = "rgba(255,255,255,.15)"; c.beginPath(); c.moveTo(w / 2, 0); c.lineTo(w / 2, h); c.moveTo(0, h / 2); c.lineTo(w, h / 2); c.stroke();
      let m = 0;
      for (let i = 0; i + 1 < v.length; i += 2) { const a = Math.abs(v[i]), b = Math.abs(v[i + 1]); if (a > m) m = a; if (b > m) m = b; }
      if (!m) return;
      const s = (w / 2 - 6) / m;
      c.fillStyle = "#56d364";
      for (let i = 0; i + 1 < v.length; i += 2) c.fillRect(w / 2 + v[i] * s - 1.5, h / 2 - v[i + 1] * s - 1.5, 3, 3);
    }
    // a simple map: an equirectangular area around the rows' positions (or the whole world), a grid, a dot and label per row.
    // No map tiles: the page loads nothing from the internet (CSP: self only).
    drawMap(n, rows, m) {
      const cv = n.map, c = cv.getContext("2d"), w = cv.width, h = cv.height;
      c.fillStyle = "#0b1a2a"; c.fillRect(0, 0, w, h);
      const pts = [];
      for (const r of rows || []) { const la = Number(r[m.lat]), lo = Number(r[m.lon]); if (isFinite(la) && isFinite(lo) && (la !== 0 || lo !== 0) && Math.abs(la) <= 90 && Math.abs(lo) <= 180) pts.push({ la, lo, label: r[m.label] }); }
      let la0 = -90, la1 = 90, lo0 = -180, lo1 = 180;
      if (pts.length) {
        la0 = Math.min(...pts.map(p => p.la)); la1 = Math.max(...pts.map(p => p.la)); lo0 = Math.min(...pts.map(p => p.lo)); lo1 = Math.max(...pts.map(p => p.lo));
        const padLa = Math.max(0.5, (la1 - la0) * 0.2), padLo = Math.max(0.5, (lo1 - lo0) * 0.2);
        la0 -= padLa; la1 += padLa; lo0 -= padLo; lo1 += padLo;
        // keep the aspect 2:1 in degrees
        const dla = la1 - la0, dlo = lo1 - lo0;
        if (dlo < 2 * dla) { const cx = (lo0 + lo1) / 2; lo0 = cx - dla; lo1 = cx + dla; } else { const cy = (la0 + la1) / 2; la0 = cy - dlo / 4; la1 = cy + dlo / 4; }
      }
      const X = lo => (lo - lo0) / (lo1 - lo0) * w, Y = la => h - (la - la0) / (la1 - la0) * h;
      c.strokeStyle = "rgba(255,255,255,.12)"; c.beginPath();
      const gs = (lo1 - lo0) > 60 ? 30 : (lo1 - lo0) > 10 ? 5 : 1;
      for (let lo = Math.ceil(lo0 / gs) * gs; lo <= lo1; lo += gs) { c.moveTo(X(lo), 0); c.lineTo(X(lo), h); }
      for (let la = Math.ceil(la0 / gs) * gs; la <= la1; la += gs) { c.moveTo(0, Y(la)); c.lineTo(w, Y(la)); }
      c.stroke();
      c.fillStyle = "#98a2b0"; c.font = "10px system-ui";
      c.fillText(lo0.toFixed(1) + "°", 2, h - 3); c.fillText(lo1.toFixed(1) + "°", w - 40, h - 3); c.fillText(la1.toFixed(1) + "°", 2, 10);
      for (const p of pts) {
        const x = X(p.lo), y = Y(p.la);
        c.fillStyle = "#ffd166"; c.beginPath(); c.arc(x, y, 3, 0, Math.PI * 2); c.fill();
        if (p.label !== undefined && pts.length <= 150) { c.fillStyle = "#e6e9ee"; c.fillText(String(p.label), x + 5, y - 4); }
      }
      c.fillStyle = "#98a2b0"; c.fillText(pts.length + " with a position", w - 110, 12);
    }
  }
  root.PanelView = PanelView;
})(typeof globalThis !== "undefined" ? globalThis : this);
