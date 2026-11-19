// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

/* OnAir Web 3.9: the spectrum and waterfall with drag-to-tune (mouse, pen and touch). The capture's span is shown, or a zoomed part of it
 * (the + and - buttons, or Ctrl / Cmd with the wheel about the pointer; the server cuts the part from its FFT, so a zoomed row has its full
 * resolution); the tuned frequency is the red line. Drag anywhere: the line follows the pointer and the receiver is tuned on release (or
 * while dragging, throttled, when live is on). The keyboard arrows step the frequency. */
(function (root) {
  "use strict";
  const L = root.OnAirLib;

  class SpectrumView {
    constructor(specCanvas, wfCanvas, lineEl, onTune) {
      this.spec = specCanvas; this.wf = wfCanvas; this.line = lineEl; this.onTune = onTune;
      this.sc = specCanvas.getContext("2d"); this.wc = wfCanvas.getContext("2d");
      this.frame = null; this.tuneHz = 0; this.dragHz = NaN; this.peak = null; this.peakHold = true;
      this.minDb = -110; this.maxDb = -20; this.auto = true;
      this.pal = SpectrumView.palette();
      this.rows = 0;
      this.ring = new L.RowRing(SpectrumView.HISTORY);   // every received row, newest first (see paintWaterfall)
      this.view = null; this.full = true; this.pix = null;   // the span the waterfall was last drawn for, and whether it must be drawn again
      this.zoom = 1; this.zoomHz = 0; this.capCentre = 0; this.capRate = 0; this.onZoom = null;
      const wrap = specCanvas.parentElement;
      let dragging = false, moved = false, lastSend = 0, id = null;
      const hzAt = (ev) => {
        const b = wrap.getBoundingClientRect();
        const x = Math.max(0, Math.min(1, (ev.clientX - b.left) / b.width));
        return this.frame ? L.xToHz(x, this.frame.centreHz, this.frame.rateHz) : NaN;
      };
      wrap.addEventListener("pointerdown", (ev) => {
        if (ev.button !== undefined && ev.button !== 0) return;
        if (ev.target.closest && ev.target.closest(".spec-tools")) return;
        dragging = true; moved = false; id = ev.pointerId;
        try { wrap.setPointerCapture(id); } catch (e) {}
        this.dragHz = hzAt(ev);
        this.drawLine();
        ev.preventDefault();
      });
      wrap.addEventListener("pointermove", (ev) => {
        if (!dragging) return;
        moved = true;
        this.dragHz = hzAt(ev);
        this.drawLine();
        const now = performance.now();
        if (this.live && now - lastSend > 150 && isFinite(this.dragHz)) { lastSend = now; this.onTune(this.dragHz, true); }
      });
      const up = (ev) => {
        if (!dragging) return;
        dragging = false;
        const hz = isFinite(this.dragHz) ? this.dragHz : hzAt(ev);
        this.dragHz = NaN;
        if (isFinite(hz)) this.onTune(hz, false);
        this.drawLine();
      };
      wrap.addEventListener("pointerup", up);
      wrap.addEventListener("pointercancel", () => { dragging = false; this.dragHz = NaN; this.drawLine(); });
      wrap.addEventListener("wheel", (ev) => {
        if (!this.frame) return;
        ev.preventDefault();
        if (ev.ctrlKey || ev.metaKey) {   // zoom about the pointer
          const b = wrap.getBoundingClientRect();
          const x = Math.max(0, Math.min(1, (ev.clientX - b.left) / b.width));
          this.zoomBy(ev.deltaY < 0 ? 1 : -1, hzAt(ev), x);
          return;
        }
        const step = L.tuneStep(this.frame.rateHz) * (ev.shiftKey ? 10 : 1);
        this.onTune(this.tuneHz + (ev.deltaY < 0 ? step : -step), false);
      }, { passive: false });
    }
    static get HISTORY() { return 2048; }
    static palette() {
      const p = new Uint8ClampedArray(256 * 3);
      for (let i = 0; i < 256; i++) {
        const u = i / 255;
        let r, g, b;
        if (u < 0.25) { r = 0; g = 0; b = 40 + u * 4 * 200; }
        else if (u < 0.5) { r = 0; g = (u - 0.25) * 4 * 220; b = 240 - (u - 0.25) * 4 * 100; }
        else if (u < 0.75) { r = (u - 0.5) * 4 * 255; g = 220 + (u - 0.5) * 4 * 35; b = 140 - (u - 0.5) * 4 * 140; }
        else { r = 255; g = 255 - (u - 0.75) * 4 * 120; b = (u - 0.75) * 4 * 200; }
        p[3 * i] = r; p[3 * i + 1] = g; p[3 * i + 2] = b;
      }
      return p;
    }
    setTune(hz) { this.tuneHz = hz; this.drawLine(); }
    setCapture(centreHz, rateHz) { this.capCentre = centreHz; this.capRate = rateHz; }
    // dir +1 in, -1 out, 0 back to the whole capture; pointerHz / x: keep that frequency under the pointer
    zoomBy(dir, pointerHz, x) {
      const z = dir === 0 ? 1 : L.zoomStep(this.zoom, dir);
      const cc = this.capCentre || (this.frame ? this.frame.centreHz : 0), cr = this.capRate || (this.frame ? this.frame.rateHz * this.zoom : 0);
      let c;
      if (z <= 1) c = 0;
      else if (isFinite(pointerHz) && isFinite(x)) c = L.zoomAbout(pointerHz, x, cc, cr, z);
      else c = L.zoomCentre(cc, cr, z, this.tuneHz);
      this.setZoom(z, c);
    }
    setZoom(z, centreHz) {
      if (z === this.zoom && Math.abs(centreHz - this.zoomHz) < 1) return;
      this.zoom = z; this.zoomHz = centreHz;
      this.peak = null;
      this.full = true;   // the next paint maps every stored row into the new span by frequency
      if (this.onZoom) this.onZoom(z, centreHz);
    }
    // a new display range or palette, or a canvas of another size: draw the stored rows again
    invalidate() { this.full = true; }
    setRange(minDb, maxDb) { this.auto = false; this.minDb = minDb; this.maxDb = maxDb; this.invalidate(); }
    setPalette(pal) { this.pal = pal; this.invalidate(); }
    drawLine() {
      const hz = isFinite(this.dragHz) ? this.dragHz : this.tuneHz;
      if (!this.frame || !isFinite(hz)) { this.line.style.display = "none"; return; }
      const x = L.hzToX(hz, this.frame.centreHz, this.frame.rateHz);
      this.line.style.display = x >= 0 && x <= 1 ? "" : "none";
      this.line.style.left = (x * 100).toFixed(3) + "%";
      this.line.style.borderColor = isFinite(this.dragHz) ? "#ffd166" : "";
    }
    push(frame) {
      this.frame = frame;
      const db = frame.db, n = db.length;
      if (!n) return;
      this.ring.push({ u8: frame.u8, centreHz: frame.centreHz, rateHz: frame.rateHz, floorDb: frame.floorDb, topDb: frame.topDb });
      if (this.zoom > 1 && !isFinite(this.dragHz)) {   // the tuned frequency left the part shown: follow it
        const f = L.zoomFollow(frame.centreHz, frame.rateHz, this.tuneHz);
        if (f !== null) this.setZoom(this.zoom, L.zoomCentre(this.capCentre || frame.centreHz, this.capRate || frame.rateHz * this.zoom, this.zoom, f));
      }
      // automatic range: the noise floor (10th percentile) a little under, the peak a little over, moving slowly
      if (this.auto) {
        const s = Array.from(db).sort((a, b) => a - b);
        const lo = s[Math.floor(n * 0.1)] - 5, hi = s[n - 1] + 5;
        this.minDb += (lo - this.minDb) * 0.1; this.maxDb += (Math.max(hi, lo + 30) - this.maxDb) * 0.1;
      }
      this.drawSpec(frame);
      this.paintWaterfall(1);
      this.drawLine();
      this.rows++;
    }
    drawSpec(f) {
      const c = this.sc, w = this.spec.width, h = this.spec.height, db = f.db, n = db.length;
      c.fillStyle = "#000"; c.fillRect(0, 0, w, h);
      const y = (v) => h - (v - this.minDb) / (this.maxDb - this.minDb) * (h - 14) - 2;
      // grid
      c.strokeStyle = "rgba(255,255,255,.08)"; c.lineWidth = 1; c.beginPath();
      for (let d = Math.ceil(this.minDb / 10) * 10; d < this.maxDb; d += 10) { c.moveTo(0, y(d)); c.lineTo(w, y(d)); }
      for (let i = 1; i < 10; i++) { c.moveTo(i * w / 10, 0); c.lineTo(i * w / 10, h); }
      c.stroke();
      // fill + line
      c.beginPath(); c.moveTo(0, h);
      for (let i = 0; i < n; i++) c.lineTo(i * w / (n - 1), y(db[i]));
      c.lineTo(w, h); c.closePath();
      c.fillStyle = "rgba(88,166,255,.25)"; c.fill();
      c.strokeStyle = "#8ec5ff"; c.beginPath();
      for (let i = 0; i < n; i++) { const px = i * w / (n - 1), py = y(db[i]); if (i) c.lineTo(px, py); else c.moveTo(px, py); }
      c.stroke();
      // peak hold
      if (this.peakHold) {
        if (!this.peak || this.peak.length !== n) this.peak = Float32Array.from(db);
        else for (let i = 0; i < n; i++) this.peak[i] = Math.max(db[i], this.peak[i] - 0.3);
        c.strokeStyle = "rgba(255,209,102,.6)"; c.beginPath();
        for (let i = 0; i < n; i++) { const px = i * w / (n - 1), py = y(this.peak[i]); if (i) c.lineTo(px, py); else c.moveTo(px, py); }
        c.stroke();
      }
      // labels
      c.fillStyle = "#98a2b0"; c.font = "11px system-ui, sans-serif";
      const lo = (f.centreHz - f.rateHz / 2) / 1e6, hi = (f.centreHz + f.rateHz / 2) / 1e6;
      c.fillText(lo.toFixed(3) + " MHz", 3, h - 3);
      const t = hi.toFixed(3) + " MHz"; c.fillText(t, w - c.measureText(t).width - 3, h - 3);
      const m = (f.centreHz / 1e6).toFixed(3); c.fillText(m, w / 2 - c.measureText(m).width / 2, h - 3);
      c.fillText(Math.round(this.maxDb) + " dB", 3, 11); c.fillText(Math.round(this.minDb) + " dB", 3, h - 16);
    }
    // Rows are drawn newest first (row of age a at y = a). Normally the picture scrolls down by `fresh` rows and the new ones are painted on top;
    // after a zoom, a retune, a range or palette change or a resize every stored row is painted again, each mapped by frequency into the span
    // of the newest frame (a row that does not reach a pixel stays black).
    paintWaterfall(fresh) {
      const c = this.wc, w = this.wf.width, h = this.wf.height, f = this.frame;
      if (!f || !w || !h) return;
      const v = this.view;
      const same = v && Math.abs(v.centreHz - f.centreHz) < 1 && Math.abs(v.rateHz - f.rateHz) < 1;
      this.view = { centreHz: f.centreHz, rateHz: f.rateHz };
      if (this.full || !same || fresh >= h) {
        this.full = false;
        c.fillStyle = "#000"; c.fillRect(0, 0, w, h);
        const k = Math.min(this.ring.count, h);
        for (let a = 0; a < k; a++) this.paintRow(this.ring.get(a), a);
        return;
      }
      const k = Math.min(fresh, this.ring.count);
      if (k < 1) return;
      c.drawImage(this.wf, 0, 0, w, h - k, 0, k, w, h - k);
      for (let a = 0; a < k; a++) this.paintRow(this.ring.get(a), a);
    }
    paintRow(row, y) {
      const c = this.wc, w = this.wf.width, im = c.createImageData(w, 1), d = im.data, p = this.pal;
      const span = this.maxDb - this.minDb, v = this.view;
      if (!this.pix || this.pix.length !== w) this.pix = new Int16Array(w);
      const px = L.rowToPixels(row, v.centreHz, v.rateHz, w, this.pix);
      const lo = row.floorDb, step = (row.topDb - row.floorDb) / 255;
      for (let x = 0; x < w; x++) {
        d[4 * x + 3] = 255;
        if (px[x] < 0) continue;   // not covered by this row: black
        const u = Math.max(0, Math.min(255, Math.round((lo + px[x] * step - this.minDb) / span * 255)));
        d[4 * x] = p[3 * u]; d[4 * x + 1] = p[3 * u + 1]; d[4 * x + 2] = p[3 * u + 2];
      }
      c.putImageData(im, 0, y);
    }
    clear() { this.sc.fillStyle = "#000"; this.sc.fillRect(0, 0, this.spec.width, this.spec.height); this.wc.fillStyle = "#000"; this.wc.fillRect(0, 0, this.wf.width, this.wf.height); this.peak = null; this.ring.clear(); this.full = true; }
  }
  root.SpectrumView = SpectrumView;
})(typeof globalThis !== "undefined" ? globalThis : this);
