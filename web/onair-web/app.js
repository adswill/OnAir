/* OnAir Web browser client. Vanilla JS, no build step, no network use except this server. Receive only: no transmit code exists here.
 * Protocol: docs/ONAIR-WEB-PROTOCOL.md. Pure helpers live in onair-lib.js (tested by tests/web/run.js). */
(function () {
  "use strict";
  const L = window.OnAirLib;
  const D = window.OnAirDecoders;
  const $ = (id) => document.getElementById(id);
  const el = (tag, cls, text) => { const e = document.createElement(tag); if (cls) e.className = cls; if (text != null) e.textContent = text; return e; };

  // ---------- storage (every access guarded: private windows throw) ----------
  const store = {
    get(k, d) { try { const v = localStorage.getItem("onair." + k); return v == null ? d : JSON.parse(v); } catch (e) { return d; } },
    set(k, v) { try { localStorage.setItem("onair." + k, JSON.stringify(v)); } catch (e) { /* ignore */ } },
  };

  // ---------- state ----------
  const S = {
    ws: null, info: null, welcome: null, ranges: null, cap: null, bounds: null, view: null, full: null,
    freq: 0, mode: "nfm", low: -6250, high: 6250, sq: -150, agc: "slow",
    audioOn: false, muted: false, volume: store.get("vol", 70), level: -150, peak: -150, peakAt: 0,
    spectrumOn: false, subs: [], step: store.get("step", 5000), palette: store.get("palette", "turbo"),
    range: store.get("range", 70), floorAdj: store.get("floorAdj", 0),
    attempt: 0, retryTimer: 0, retryAt: 0, stopRetry: false, pendingTune: 0, lastTuneSent: 0, tuneTimer: 0,
    rows: [], rowSeq: -1, audioSeq: -1, lost: 0, decoders: {}, views: {}, decDirty: false, name: store.get("name", ""), invite: "", password: "",
    bw: 0, tuneExtra: {}, connState: "", retryNote: "", drag: null, notServer: false, sheetOpen: store.get("sheet", true), tab: "tune",
  };
  const bands = [
    { n: "LW broadcast", s: 148500, e: 283500, f: 189000, m: "am" }, { n: "MW broadcast", s: 526500, e: 1606500, f: 1000000, m: "am" },
    { n: "160 m", s: 1810000, e: 2000000, f: 1840000, m: "lsb" }, { n: "80 m", s: 3500000, e: 3800000, f: 3700000, m: "lsb" },
    { n: "49 m SW", s: 5900000, e: 6200000, f: 6000000, m: "am" }, { n: "40 m", s: 7000000, e: 7200000, f: 7100000, m: "lsb" },
    { n: "31 m SW", s: 9400000, e: 9900000, f: 9600000, m: "am" }, { n: "20 m", s: 14000000, e: 14350000, f: 14200000, m: "usb" },
    { n: "CB 27 MHz", s: 26965000, e: 27405000, f: 27185000, m: "am" }, { n: "10 m", s: 28000000, e: 29700000, f: 28400000, m: "usb" },
    { n: "6 m", s: 50000000, e: 52000000, f: 50200000, m: "usb" }, { n: "FM broadcast", s: 87500000, e: 108000000, f: 100000000, m: "wfm" },
    { n: "Airband", s: 118000000, e: 137000000, f: 121500000, m: "am" }, { n: "2 m", s: 144000000, e: 146000000, f: 145500000, m: "nfm" },
    { n: "Marine VHF", s: 156000000, e: 162025000, f: 156800000, m: "nfm" }, { n: "70 cm", s: 430000000, e: 440000000, f: 433500000, m: "nfm" },
    { n: "PMR446", s: 446000000, e: 446200000, f: 446100000, m: "nfm" }, { n: "ADS-B", s: 1089000000, e: 1091000000, f: 1090000000, m: "am" },
  ];

  // ---------- small UI helpers ----------
  function setConn(text, cls) { const c = $("conn"); c.textContent = text; c.className = "pill " + (cls || ""); S.connState = text; renderScopeMsg(); }
  function banner(text) { const b = $("banner"); b.hidden = !text; b.textContent = text || ""; }
  function send(m) { if (S.ws && S.ws.readyState === 1) { S.ws.send(JSON.stringify(m)); return true; } return false; }
  const fmtMHz = (hz) => { const t = (hz / 1e6).toFixed(6).replace(/0+$/, "").replace(/\.$/, ""); return t.includes(".") ? t : t + ".0"; };
  function schedule(fn, ms) { let t = 0; return () => { clearTimeout(t); t = setTimeout(fn, ms); }; }

  // ---------- frequency display ----------
  const digitEls = [];
  function buildFreq() {
    const f = $("freq"); f.textContent = "";
    for (let i = 9; i >= 0; i--) {
      const d = el("span", "d", "0"); d.dataset.place = String(i); digitEls[i] = d; f.appendChild(d);
      if (i === 9 || i === 6 || i === 3) f.appendChild(el("span", "p", "."));
    }
    f.addEventListener("pointerdown", (e) => {
      const d = e.target.closest(".d"); if (!d) return;
      const r = d.getBoundingClientRect(), up = e.clientY < r.top + r.height / 2;
      changeFreq((up ? 1 : -1) * Math.pow(10, +d.dataset.place)); e.preventDefault(); f.focus();
    });
    f.addEventListener("wheel", (e) => {
      e.preventDefault();
      const d = e.target.closest(".d");
      changeFreq((e.deltaY < 0 ? 1 : -1) * (d ? Math.pow(10, +d.dataset.place) : S.step));
    }, { passive: false });
    f.addEventListener("keydown", (e) => {
      if (e.key === "ArrowUp" || e.key === "ArrowRight") { changeFreq(S.step * (e.shiftKey ? 10 : 1)); e.preventDefault(); }
      else if (e.key === "ArrowDown" || e.key === "ArrowLeft") { changeFreq(-S.step * (e.shiftKey ? 10 : 1)); e.preventDefault(); }
      else if (e.key === "PageUp") { changeFreq(S.step * 10); e.preventDefault(); }
      else if (e.key === "PageDown") { changeFreq(-S.step * 10); e.preventDefault(); }
      else if (e.key === "Enter") { typeFreq(); e.preventDefault(); }
    });
  }
  const mhzText = (hz) => (hz / 1e6).toFixed(6).replace(/0+$/, "").replace(/\.$/, "");
  function renderFreq() {
    const s = String(Math.max(0, Math.round(S.freq))).padStart(10, "0");
    let lead = true;
    for (let i = 9; i >= 0; i--) {
      const ch = s[9 - i]; if (ch !== "0" || i < 6) lead = false;
      digitEls[i].textContent = ch; digitEls[i].className = "d" + (lead ? " z" : "");
    }
    const f = $("freq"); f.setAttribute("aria-valuenow", String(S.freq)); f.setAttribute("aria-valuetext", mhzText(S.freq) + " megahertz");
    const s0 = $("viewText");
    if (S.view) s0.textContent = L.formatMHz(S.view.start, 3) + " to " + L.formatMHz(S.view.stop, 3) + " MHz";
    invalidate({ ov: 1 });
  }
  // typing a frequency replaces the readout in place (no dialog, no layout shift)
  function typeFreq() {
    const form = $("freqForm"), inp = $("freqInput");
    form.hidden = false; $("freq").hidden = true; $("freqType").hidden = true;
    inp.value = mhzText(S.freq); inp.focus(); inp.select();
  }
  function closeType() { $("freqForm").hidden = true; $("freq").hidden = false; $("freqType").hidden = false; }
  function wireType() {
    $("freqForm").addEventListener("submit", (e) => {
      e.preventDefault();
      const hz = L.parseFreq($("freqInput").value);
      if (isNaN(hz)) { banner("That is not a frequency. Type it in MHz, for example 98.3, or add kHz or Hz."); return; }
      banner(""); closeType(); setFreq(hz, true); $("freq").focus();
    });
    $("freqInput").addEventListener("keydown", (e) => { if (e.key === "Escape") { closeType(); $("freq").focus(); } });
    $("freqInput").addEventListener("blur", () => setTimeout(() => { if (document.activeElement !== $("freqInput") && !$("freqForm").hidden) closeType(); }, 100));
  }

  // ---------- tuning ----------
  function clampFreq(hz) {
    const f = S.bounds ? L.clamp(hz, S.bounds.start, S.bounds.stop) : Math.max(0, hz);
    if (!L.rangeActive(S.ranges)) return f;
    // the host limits the ranges: stay inside them (the server checks again and answers forbidden_range)
    const s = L.rangeSnap(S.ranges, S.mode, f, S.low, S.high, S.cap);
    return s === null ? S.freq : s;
  }
  function setFreq(hz, followView) {
    const want = Math.round(L.clamp(hz, S.bounds ? S.bounds.start : 0, S.bounds ? S.bounds.stop : 1e12));
    S.freq = Math.round(clampFreq(hz));
    if (L.rangeActive(S.ranges) && S.freq !== want) banner("The host only allows listening in certain ranges: moved to the nearest allowed frequency.");
    if (followView && S.view && (S.freq < S.view.start || S.freq > S.view.stop)) {
      const sp = S.view.stop - S.view.start; setView({ start: S.freq - sp / 2, stop: S.freq + sp / 2 });
    }
    renderFreq(); renderBandsActive(); persist(); sendTune();
  }
  function changeFreq(d) { setFreq(S.freq + d, true); }
  // tune messages are sent at most every 60 ms; fields asked for in between are merged into the next one
  function sendTune(extra) {
    S.pendingTune = performance.now();
    Object.assign(S.tuneExtra, extra || {});
    const go = () => { S.lastTuneSent = performance.now(); const m = Object.assign({ freq_hz: S.freq }, S.tuneExtra); S.tuneExtra = {}; send(L.msg.tune(m)); };
    clearTimeout(S.tuneTimer);
    const wait = Math.max(0, 60 - (performance.now() - S.lastTuneSent));
    if (wait === 0) go(); else S.tuneTimer = setTimeout(go, wait);
  }
  function setMode(mode) {
    if (!L.MODES.includes(mode)) return;
    S.mode = mode;
    const d = L.MODE_DEFAULT[mode]; S.low = d[0]; S.high = d[1];
    if (L.rangeActive(S.ranges)) S.freq = Math.round(clampFreq(S.freq));   // the new passband must fit too
    renderMode(); renderFreq(); sendTune({ mode });
    if (S.audioOn) sendAudio();
    persist();
  }
  function applyPassband(pb) {
    S.low = pb.low_hz; S.high = pb.high_hz;
    if (L.rangeActive(S.ranges)) S.freq = Math.round(clampFreq(S.freq));
    renderMode(); renderFreq(); sendTune({ low_hz: S.low, high_hz: S.high }); persist();
  }
  function setBandwidth(bw) { applyPassband(L.passband(S.mode, bw)); }
  function buildModes(allowed) {
    const box = $("modes"); box.textContent = "";
    for (const m of L.MODES) {
      if (allowed && !allowed.includes(m)) continue;
      const rm = L.rangeModes(S.ranges);   // the host's ranges: a mode that is allowed nowhere is greyed out
      const b = el("button", null, m.toUpperCase()); if (rm && !rm.includes(m)) { b.disabled = true; b.title = "The host does not allow this mode in any range"; } b.type = "button"; b.dataset.mode = m; b.setAttribute("role", "radio");
      b.addEventListener("click", () => setMode(m)); box.appendChild(b);
    }
  }
  function renderMode() {
    for (const b of $("modes").children) b.setAttribute("aria-checked", String(b.dataset.mode === S.mode)), b.setAttribute("aria-pressed", String(b.dataset.mode === S.mode));
    const r = L.MODE_BW_RANGE[S.mode], bw = L.bandwidthOf(S.low, S.high), s = $("bw");
    s.min = r[0]; s.max = r[1]; s.step = r[1] > 30000 ? 1000 : 50; s.value = bw;
    $("bwOut").textContent = bw >= 1000 ? (bw / 1000).toFixed(bw % 1000 ? 2 : 0) + " kHz" : bw + " Hz";
    S.bw = bw; $("freq").setAttribute("data-mode", S.mode);
    invalidate({ ov: 1 });
  }

  // ---------- persistence of the last tuning ----------
  const persist = schedule(() => store.set("last", { f: S.freq, m: S.mode, lo: S.low, hi: S.high, sq: S.sq }), 400);

  // ---------- view (zoom / pan) and the spectrum request ----------
  const requestSpan = schedule(() => {
    if (!S.welcome || !S.view) return;
    const canvas = $("spec"), want = Math.max(256, Math.min(S.welcome.limits.max_bins, 2048, 1 << Math.ceil(Math.log2(Math.max(256, canvas.clientWidth * (window.devicePixelRatio || 1))))));
    const full = L.viewIsFull(S.view, S.full);
    send(L.msg.spectrum(true, want, Math.min(S.welcome.limits.max_fps, 12), full ? null : S.view.start, full ? null : S.view.stop));
    S.spectrumOn = true;
  }, 120);
  function setView(v) {
    if (!S.bounds) return;
    S.view = L.normView(v, S.full); renderFreq(); invalidate({ spec: 1, wf: 1 }); requestSpan();
  }
  function zoom(f, anchor) { setView(L.zoomView(S.view, S.full, f, anchor == null ? S.freq : anchor)); }

  // ---------- drawing: everything is painted from requestAnimationFrame, at most once per frame ----------
  const spec = $("spec"), wf = $("wf"), ax = $("ax"), ov = $("ov");
  const sctx = spec.getContext("2d"), wctx = wf.getContext("2d"), actx = ax.getContext("2d"), octx = ov.getContext("2d");
  let pal = L.makePalette(S.palette), pix = null;
  const C = { bg: "#040405", grid: "rgba(150,155,160,0.16)", gridMajor: "#545557", text: "#999c9e", accent: "#e6e8eb", axisBg: "#222324" };
  const FONT = (px) => `${px}px ui-monospace, "SF Mono", Menlo, Consolas, monospace`;
  const dirty = { spec: 1, wf: 1, ov: 1 };
  let raf = 0, hover = null;   // hover: { x (css px), hz } while the pointer is over the scope
  function invalidate(w) { for (const k in w) dirty[k] = 1; if (!raf) raf = requestAnimationFrame(frame); }
  function frame() {
    raf = 0;
    const dpr = window.devicePixelRatio || 1;
    const resized = [spec, ax, wf, ov].map(fit).some(Boolean);
    if (resized) { dirty.spec = dirty.wf = dirty.ov = 1; }
    if (dirty.wf) { dirty.wf = 0; redrawWaterfall(); }
    if (dirty.spec) { dirty.spec = 0; drawSpectrum(dpr); drawAxis(dpr); }
    if (dirty.ov) { dirty.ov = 0; drawOverlay(dpr); }
    if (resized) requestSpan();
  }
  function fit(c) {
    const dpr = window.devicePixelRatio || 1, w = Math.max(50, Math.round(c.clientWidth * dpr)), h = Math.max(10, Math.round(c.clientHeight * dpr));
    if (c.width !== w || c.height !== h) { c.width = w; c.height = h; return true; }
    return false;
  }
  function rowLo(row) { return row.floorDb + S.floorAdj; }
  function drawAxis(dpr) {
    const W = ax.width, H = ax.height; actx.fillStyle = C.axisBg; actx.fillRect(0, 0, W, H);
    if (!S.view) return;
    const t = L.axisTicks(S.view, W / dpr, 84);
    actx.font = FONT(11 * dpr); actx.textBaseline = "middle"; actx.textAlign = "center";
    actx.strokeStyle = "#4a4338"; actx.lineWidth = 1; actx.beginPath();
    for (const hz of t.minor) { const x = Math.round(L.hzToX(S.view, hz, W)) + 0.5; actx.moveTo(x, H); actx.lineTo(x, H - 4 * dpr); }
    for (const m of t.major) { const x = Math.round(L.hzToX(S.view, m.hz, W)) + 0.5; actx.moveTo(x, H); actx.lineTo(x, H - 7 * dpr); }
    actx.stroke(); actx.fillStyle = C.text;
    for (const m of t.major) {
      const x = L.hzToX(S.view, m.hz, W), w = actx.measureText(m.label).width / 2 + 3 * dpr;
      actx.fillText(m.label, L.clamp(x, w, W - w), H / 2 - 3 * dpr);
    }
    actx.strokeStyle = C.gridMajor; actx.beginPath(); actx.moveTo(0, H - 0.5); actx.lineTo(W, H - 0.5); actx.stroke();
  }
  function drawSpectrum(dpr) {
    const W = spec.width, H = spec.height;
    sctx.fillStyle = C.bg; sctx.fillRect(0, 0, W, H);
    if (!S.view) return;
    const v = S.view, row = S.rows[S.rows.length - 1];
    // vertical grid on the major ticks, horizontal grid every 10 dB with the level at the left edge
    const t = L.axisTicks(v, W / dpr, 84);
    sctx.strokeStyle = C.grid; sctx.lineWidth = 1; sctx.beginPath();
    for (const m of t.major) { const x = Math.round(L.hzToX(v, m.hz, W)) + 0.5; sctx.moveTo(x, 0); sctx.lineTo(x, H); }
    if (row) {
      const lo = rowLo(row), yOf = (db) => H - L.clamp((db - lo) / S.range, 0, 1) * (H - 4) - 2;
      sctx.font = FONT(10 * dpr); sctx.textBaseline = "bottom"; sctx.textAlign = "left";
      const dbStep = (H / dpr) * 10 / S.range < 26 ? 20 : 10;
      for (let db = Math.ceil(lo / dbStep) * dbStep; db <= lo + S.range; db += dbStep) { const y = Math.round(yOf(db)) + 0.5; sctx.moveTo(0, y); sctx.lineTo(W, y); }
      sctx.stroke(); sctx.fillStyle = C.text;
      for (let db = Math.ceil(lo / dbStep) * dbStep; db <= lo + S.range; db += dbStep) { const y = yOf(db); if (y > 12 * dpr && y < H - 2) sctx.fillText(String(db), 4 * dpr, y - 1); }
    } else sctx.stroke();
    // the host's ranges: what is not visible is darkened, the allowed bands get a strip on top, blocked ones a red tint
    if (L.rangeActive(S.ranges)) {
      sctx.fillStyle = "rgba(0,0,0,0.62)";
      for (const h of L.hiddenRanges(S.ranges, v.start, v.stop)) { const a = L.hzToX(v, h.lo, W), b = L.hzToX(v, h.hi, W); sctx.fillRect(a, 0, Math.max(1, b - a), H); }
      if (S.ranges.restricted) {
        sctx.fillStyle = "rgba(96,190,140,0.85)";
        for (const a of L.mergeRanges(S.ranges.allow)) { if (a.hi < v.start || a.lo > v.stop) continue; const x = L.hzToX(v, Math.max(a.lo, v.start), W), y = L.hzToX(v, Math.min(a.hi, v.stop), W); sctx.fillRect(x, 0, Math.max(2, y - x), Math.max(3, 3 * dpr)); }
      }
      sctx.fillStyle = "rgba(190,60,50,0.22)";
      for (const b of S.ranges.block) { if (b.hi < v.start || b.lo > v.stop) continue; const x = L.hzToX(v, Math.max(b.lo, v.start), W), y = L.hzToX(v, Math.min(b.hi, v.stop), W); sctx.fillRect(x, 0, Math.max(2, y - x), H); }
    }
    if (!row) return;
    // trace: a line over a faint fill
    pix = L.rowToPixels(row, v, W, pix);
    const lo = rowLo(row), yOf = (db) => H - L.clamp((db - lo) / S.range, 0, 1) * (H - 4) - 2;
    sctx.beginPath(); let started = false, lastX = 0;
    for (let x = 0; x < W; x++) {
      if (!pix[x]) { started = false; continue; }
      const y = yOf(L.levelToDb(row, pix[x]));
      if (!started) { sctx.moveTo(x, y); started = true; } else sctx.lineTo(x, y);
      lastX = x;
    }
    sctx.strokeStyle = C.accent; sctx.lineWidth = Math.max(1, dpr); sctx.stroke();
    if (started) { sctx.lineTo(lastX, H); sctx.lineTo(0, H); sctx.closePath(); sctx.fillStyle = "rgba(230,232,235,0.08)"; sctx.fill(); }
  }
  // the passband, the tuned frequency and the pointer: one transparent canvas over spectrum, axis and waterfall
  function passEdges(W) {
    if (!S.view) return null;
    return { x0: L.hzToX(S.view, S.freq + S.low, W), x1: L.hzToX(S.view, S.freq + S.high, W), xc: L.hzToX(S.view, S.freq, W) };
  }
  function drawOverlay(dpr) {
    const W = ov.width, H = ov.height; octx.clearRect(0, 0, W, H);
    const e = passEdges(W); if (!e) return;
    const axTop = spec.clientHeight * dpr, axH = ax.clientHeight * dpr;
    octx.fillStyle = "rgba(230,232,235,0.09)"; octx.fillRect(e.x0, 0, Math.max(2, e.x1 - e.x0), H);
    octx.fillStyle = "rgba(230,232,235,0.55)";
    octx.fillRect(Math.round(e.x0), 0, Math.max(1, Math.round(dpr)), H); octx.fillRect(Math.round(e.x1), 0, Math.max(1, Math.round(dpr)), H);
    // grips on the axis strip show that the edges can be dragged
    const gh = 10 * dpr, gw = 3 * dpr, gy = axTop + (axH - gh) / 2;
    octx.fillStyle = C.accent; octx.fillRect(Math.round(e.x0 - gw / 2), gy, gw, gh); octx.fillRect(Math.round(e.x1 - gw / 2), gy, gw, gh);
    octx.fillRect(Math.round(e.xc), 0, Math.max(1, Math.round(dpr)), H);
    octx.beginPath(); octx.moveTo(e.xc - 5 * dpr, axTop - 7 * dpr); octx.lineTo(e.xc + 5 * dpr, axTop - 7 * dpr); octx.lineTo(e.xc, axTop - 1 * dpr); octx.closePath(); octx.fill();
    if (hover && !S.drag) {
      const x = hover.x * dpr;
      octx.fillStyle = "rgba(220,212,198,0.35)"; octx.fillRect(Math.round(x), 0, Math.max(1, Math.round(dpr)), H);
      const txt = L.formatMHz(hover.hz, 4) + " MHz" + (hover.db != null ? "  " + hover.db.toFixed(0) + " dB" : "");
      octx.font = FONT(11 * dpr); octx.textBaseline = "middle"; octx.textAlign = "left";
      const tw = octx.measureText(txt).width + 10 * dpr, bx = L.clamp(x + 8 * dpr, 2, W - tw - 2), by = 4 * dpr;
      octx.fillStyle = "rgba(34,35,36,0.95)"; octx.fillRect(bx, by, tw, 18 * dpr);
      octx.strokeStyle = "#545557"; octx.lineWidth = 1; octx.strokeRect(bx + 0.5, by + 0.5, tw, 18 * dpr);
      octx.fillStyle = C.text; octx.fillText(txt, bx + 5 * dpr, by + 9.5 * dpr);
    }
  }
  const ROWH = () => Math.max(1, Math.round(2 * (window.devicePixelRatio || 1)));
  function paintWfRow(img, y, row) {
    const W = img.width, px = img.data; pix = L.rowToPixels(row, S.view, W, pix);
    const lo = rowLo(row);
    for (let x = 0; x < W; x++) {
      const o = (y * W + x) * 4;
      if (!pix[x]) { px[o] = 10; px[o + 1] = 9; px[o + 2] = 8; px[o + 3] = 255; continue; }
      const db = L.levelToDb(row, pix[x]), i = Math.round(L.clamp((db - lo) / S.range, 0, 1) * 255) * 3;
      px[o] = pal[i]; px[o + 1] = pal[i + 1]; px[o + 2] = pal[i + 2]; px[o + 3] = 255;
    }
  }
  function redrawWaterfall() {
    const W = wf.width, H = wf.height, rh = ROWH();
    wctx.fillStyle = C.bg; wctx.fillRect(0, 0, W, H);
    if (!S.view || !S.rows.length) return;
    const img = wctx.createImageData(W, H), line = W * 4;
    for (let y = 0; y < H; y++) {
      const k = (y / rh) | 0, o = y * line;
      if (y % rh) { img.data.copyWithin(o, o - line, o); continue; }
      const row = S.rows[S.rows.length - 1 - k];
      if (!row) { for (let x = 0; x < W; x++) { const q = o + x * 4; img.data[q] = 10; img.data[q + 1] = 9; img.data[q + 2] = 8; img.data[q + 3] = 255; } continue; }
      paintWfRow({ width: W, data: img.data }, y, row);
    }
    wctx.putImageData(img, 0, 0);
  }
  function onRow(row) {
    const first = !S.rows.length;
    S.rows.push(row); if (S.rows.length > 900) S.rows.splice(0, S.rows.length - 800);
    if (first) renderScopeMsg();
    if (!S.audioOn) { const db = L.passbandLevel(row, S.freq, S.low, S.high); if (db != null) { S.level = db; if (db > S.peak) { S.peak = db; S.peakAt = performance.now(); } } }
    if (!S.view) return;
    if (fit(wf)) { invalidate({ wf: 1 }); } else {
      const W = wf.width, rh = ROWH();
      wctx.drawImage(wf, 0, rh);
      const img = wctx.createImageData(W, 1); paintWfRow(img, 0, row); wctx.putImageData(img, 0, 0);
      if (rh > 1) wctx.drawImage(wf, 0, 0, W, 1, 0, 1, W, rh - 1);
    }
    invalidate({ spec: 1 });
  }
  // the scope says in words why it is empty
  function renderScopeMsg() {
    let t = "";
    if (S.notServer) t = "No receiver here.";
    else if (!S.welcome) t = S.connState === "offline" ? "Offline. " + (S.retryNote || "") : S.connState === "sign in" ? "Sign in to listen." : "Connecting to the receiver.";
    else if (!S.rows.length) t = "Connected. Waiting for the first spectrum.";
    $("scopeMsg").textContent = t.trim();
  }

  // ---------- pointer interaction on the scope ----------
  // click = tune. Drag a passband edge = bandwidth. Drag inside the passband = fine tune. Drag elsewhere = pan. Wheel / pinch = zoom.
  function attachScope(c) {
    const ptrs = new Map(); let startView = null, moved = false, startX = 0, pinch0 = 0, pinchView = null, mode = "pan", edge = "high", grab = 0;
    const rect = () => c.getBoundingClientRect();
    const wpos = (e) => { const r = rect(); return (e.clientX - r.left) / r.width; };
    const hitTest = (e) => {
      const r = rect(), W = r.width, x = e.clientX - r.left, p = passEdges(W); if (!p) return "pan";
      const tol = e.pointerType === "touch" ? 16 : 7;
      const d0 = Math.abs(x - p.x0), d1 = Math.abs(x - p.x1);
      if (Math.min(d0, d1) <= tol && p.x1 - p.x0 > 3) { edge = d0 < d1 ? "low" : "high"; return "edge"; }
      if (x > p.x0 && x < p.x1) return "inside";
      return "pan";
    };
    const hoverInfo = (e) => {
      if (!S.view) return null;
      const f = wpos(e), hz = L.xToHz(S.view, f, 1); let db = null;
      const row = S.rows[S.rows.length - 1];
      if (row && pix && pix.length) { const px = pix[Math.min(pix.length - 1, Math.max(0, Math.floor(f * pix.length)))]; if (px) db = L.levelToDb(row, px); }
      return { x: e.clientX - rect().left, hz, db };
    };
    c.addEventListener("pointerdown", (e) => {
      try { c.setPointerCapture(e.pointerId); } catch (err) { /* synthetic or already released */ } ptrs.set(e.pointerId, e);
      if (ptrs.size === 1) { startView = S.view; moved = false; startX = e.clientX; mode = hitTest(e); S.drag = mode; if (mode === "inside" && S.view) grab = L.xToHz(S.view, wpos(e), 1) - S.freq; }
      else if (ptrs.size === 2) { const [a, b] = [...ptrs.values()]; pinch0 = Math.abs(a.clientX - b.clientX) || 1; pinchView = S.view; moved = true; mode = "pan"; S.drag = "pan"; }
      c.focus({ preventScroll: true });
    });
    c.addEventListener("pointermove", (e) => {
      if (!ptrs.has(e.pointerId)) {   // no button: show the pointer, and which gesture a press here would start
        hover = hoverInfo(e); const h = e.pointerType === "touch" ? "pan" : hitTest(e);
        c.style.cursor = h === "edge" ? "ew-resize" : h === "inside" ? "grab" : "crosshair"; invalidate({ ov: 1 }); return;
      }
      if (!S.view) return;
      ptrs.set(e.pointerId, e);
      if (ptrs.size === 2 && pinchView) {
        const [a, b] = [...ptrs.values()], d = Math.abs(a.clientX - b.clientX) || 1;
        const r = rect(), mid = ((a.clientX + b.clientX) / 2 - r.left) / r.width;
        setView(L.zoomView(pinchView, S.full, pinch0 / d, pinchView.start + mid * (pinchView.stop - pinchView.start)));
      } else if (ptrs.size === 1 && startView) {
        if (!moved && Math.abs(e.clientX - startX) > 6) moved = true;
        if (!moved) return;
        if (mode === "edge") {
          const hz = L.xToHz(S.view, wpos(e), 1) - S.freq;
          applyPassband(L.dragEdge(S.mode, S.low, S.high, edge, hz));
        } else if (mode === "inside") {
          setFreq(L.snap(L.xToHz(S.view, wpos(e), 1) - grab, S.step), true);
        } else {
          const w = rect().width, dHz = -((e.clientX - startX) / w) * (startView.stop - startView.start);
          setView(L.panView(startView, S.full, dHz));
        }
      }
    });
    const up = (e) => {
      if (!ptrs.has(e.pointerId)) return;
      const wasMoved = moved; ptrs.delete(e.pointerId);
      if (ptrs.size === 0 && !wasMoved && S.view && e.type === "pointerup") setFreq(L.snap(L.xToHz(S.view, wpos(e), 1), S.step));
      if (ptrs.size < 2) pinchView = null;
      if (ptrs.size === 1) { const r = [...ptrs.values()][0]; startView = S.view; startX = r.clientX; moved = true; mode = "pan"; }
      if (ptrs.size === 0) { S.drag = null; if (e.pointerType === "touch") hover = null; invalidate({ ov: 1 }); }
    };
    c.addEventListener("pointerup", up); c.addEventListener("pointercancel", up);
    c.addEventListener("pointerleave", (e) => { if (!ptrs.has(e.pointerId)) { hover = null; invalidate({ ov: 1 }); } });
    c.addEventListener("wheel", (e) => {
      if (!S.view) return; e.preventDefault();
      if (Math.abs(e.deltaX) > Math.abs(e.deltaY)) { setView(L.panView(S.view, S.full, (e.deltaX / c.clientWidth) * (S.view.stop - S.view.start))); return; }
      zoom(e.deltaY < 0 ? 0.8 : 1.25, L.xToHz(S.view, wpos(e), 1));
    }, { passive: false });
  }

  // ---------- S-meter ----------
  function buildSticks() {
    const box = $("sticks"); box.textContent = "";
    for (const t of L.sMeterTicks()) { const s = el("span", t.label[0] === "+" ? "hot" : "", t.label); s.style.left = (t.frac * 100).toFixed(2) + "%"; box.appendChild(s); }
  }
  let lastMeter = "";
  function drawMeter() {
    const now = performance.now();
    if (now - S.peakAt > 1200) S.peak = Math.max(S.level, S.peak - 0.6);
    const m = L.sMeter(S.level), pk = L.sMeter(S.peak);
    const key = m.frac.toFixed(3) + pk.frac.toFixed(3) + m.text + Math.round(S.level);
    if (key !== lastMeter) {
      lastMeter = key;
      $("sfill").style.transform = "scaleX(" + m.frac.toFixed(3) + ")"; $("speak").style.left = (pk.frac * 100).toFixed(1) + "%";
      $("stext").textContent = m.text; $("sdb").textContent = isFinite(S.level) && S.level > -190 ? S.level.toFixed(0) + (S.audioOn ? " dBFS" : " dB spectrum") : "-- dB";
      const sb = $("sbar"); sb.setAttribute("aria-valuenow", String(Math.round(m.frac * 100))); sb.setAttribute("aria-valuetext", m.text);
    }
    requestAnimationFrame(drawMeter);
  }

  // ---------- audio ----------
  const A = { ctx: null, node: null, gain: null, player: null, sp: null };
  async function ensureAudio() {
    if (A.ctx) { if (A.ctx.state === "suspended") await A.ctx.resume(); return true; }
    const AC = window.AudioContext || window.webkitAudioContext;
    if (!AC) { banner("This browser cannot play audio."); return false; }
    A.ctx = new AC({ latencyHint: "interactive" });
    A.gain = A.ctx.createGain(); A.gain.connect(A.ctx.destination); applyVolume();
    if (A.ctx.audioWorklet && window.AudioWorkletNode) {
      try {
        await A.ctx.audioWorklet.addModule("onair-worklet.js");
        A.node = new AudioWorkletNode(A.ctx, "onair-player", { numberOfInputs: 0, outputChannelCount: [1] });
        A.node.connect(A.gain);
        A.node.port.onmessage = (e) => { if (e.data.buffered !== undefined) S.audioBuf = e.data; };
      } catch (e) { A.node = null; }
    }
    if (!A.node) {
      A.player = new window.OnAirPlayer.RingPlayer(A.ctx.sampleRate);
      A.sp = A.ctx.createScriptProcessor(2048, 0, 1);
      A.sp.onaudioprocess = (e) => A.player.pull(e.outputBuffer.getChannelData(0));
      A.sp.connect(A.gain);
    }
    if (A.ctx.state === "suspended") await A.ctx.resume();
    return true;
  }
  function applyVolume() {
    if (!A.gain) return;
    const v = S.muted ? 0 : Math.pow(S.volume / 100, 2) * 1.5;
    A.gain.gain.setTargetAtTime(v, A.ctx.currentTime, 0.02);
  }
  function audioRate() { return S.mode === "wfm" ? 48000 : 12000; }
  function sendAudio() { send(L.msg.audio(true, "adpcm", audioRate())); }
  function playFrame(f) {
    const f32 = new Float32Array(f.samples.length);
    for (let i = 0; i < f32.length; i++) f32[i] = f.samples[i] / 32768;
    if (A.node) A.node.port.postMessage({ pcm: f32, rate: f.rate }, [f32.buffer]);
    else if (A.player) A.player.push(f32, f.rate);
  }
  function flushAudio() { if (A.node) A.node.port.postMessage({ flush: true }); else if (A.player) A.player.flush(); }
  async function toggleAudio() {
    if (S.audioOn) { S.audioOn = false; send(L.msg.audio(false)); flushAudio(); }
    else { if (!(await ensureAudio())) return; S.audioOn = true; S.audioSeq = -1; sendAudio(); }
    renderAudio();
  }
  function renderAudio() {
    for (const id of ["audioBtn", "audioQuick"]) { const b = $(id); b.setAttribute("aria-pressed", String(S.audioOn)); b.textContent = S.audioOn ? "Stop" : "Listen"; }
    $("muteBtn").checked = S.muted;
  }
  function onAudio(f) {
    if (!S.audioOn) return;
    if (S.audioSeq >= 0) {
      const gap = (f.seq - S.audioSeq) >>> 0;
      if (gap > 1 && gap < 0x80000000) { S.lost += gap - 1; if (gap > 25) flushAudio(); }   // ADPCM frames carry their own state; a long gap restarts the buffer
      else if (gap === 0 || gap >= 0x80000000) flushAudio();
    }
    S.audioSeq = f.seq;
    S.level = f.squelched ? Math.min(f.levelDb, S.level) : f.levelDb; if (S.level > S.peak) { S.peak = S.level; S.peakAt = performance.now(); }
    playFrame(f);
  }

  // ---------- connection ----------
  async function fetchInfo() {
    try {
      const r = await fetch("/web/v1/info", { cache: "no-store" });
      if (r.status === 404) S.notServer = true;   // a plain web host (no OnAir receiver behind this address)
      if (!r.ok) throw new Error(String(r.status));
      S.info = await r.json(); renderInfo(); return S.info;
    } catch (e) { return null; }
  }
  function renderInfo() {
    const i = S.info; if (!i) return;
    $("srvName").textContent = i.name || "OnAir Web"; $("srvWhere").textContent = [i.location, i.antenna].filter(Boolean).join(", ");
    document.title = (i.name || "OnAir Web");
    $("users").textContent = i.users + " / " + i.max_users + " listeners";
    $("users").className = "pill " + (i.users >= i.max_users ? "warn" : "dim");
    const n = i.notice || i.legal_notice;
    showNotice(typeof n === "string" && n ? n : null);
    $("footInfo").textContent = i.capture ? "Capture " + fmtMHz(i.capture.centre_hz) + " MHz, " + (i.capture.rate_hz / 1e6).toFixed(2) + " Msps. Level scale is relative to full scale, not calibrated." : "";
  }
  const DEFAULT_NOTICE = "This receiver is run by its host, who is responsible for it. Listening is monitored only by the host. Radio laws differ by country: do not pass on or act on messages that are not meant for the general public, and do not record or share what you hear unless the law where you are allows it.";
  function showNotice(text) {
    const t = text || DEFAULT_NOTICE; $("noticeText").textContent = t;
    const key = "n" + t.length + ":" + t.slice(0, 24);
    $("notice").hidden = store.get("noticeAck") === key;
    $("noticeOk").onclick = () => { store.set("noticeAck", key); $("notice").hidden = true; };
    $("noticeMore").onclick = () => { const p = $("noticeText"), open = p.classList.toggle("clamp") === false; $("noticeMore").textContent = open ? "Less" : "More"; $("noticeMore").setAttribute("aria-expanded", String(open)); };
  }
  function authNeeded() {
    const a = S.info && S.info.auth;
    return (a === "password" && !S.password && !S.invite) || (a === "invite" && !S.invite);
  }
  function showLogin(show) {
    $("login").hidden = !show; const a = S.info && S.info.auth;
    $("loginPwRow").hidden = a !== "password"; $("loginInvRow").hidden = a === "open";
    $("loginName").value = S.name;
    if (show) $("loginGo").focus();
  }
  function connect() {
    clearTimeout(S.retryTimer); S.stopRetry = false;
    if (S.ws) { try { S.ws.onclose = null; S.ws.close(); } catch (e) { /* ignore */ } }
    setConn("connecting", "warn");
    const proto = location.protocol === "https:" ? "wss:" : "ws:";
    let ws;
    try { ws = new WebSocket(proto + "//" + location.host + "/web/v1/ws", L.SUBPROTOCOL); } catch (e) { setConn("error", "bad"); scheduleRetry(1006); return; }
    ws.binaryType = "arraybuffer"; S.ws = ws; S.welcome = null;
    ws.onopen = () => send(L.msg.hello(S.name, S.password, S.invite));
    ws.onmessage = (e) => {
      if (typeof e.data === "string") return onText(L.parseServerText(e.data));
      const b = new Uint8Array(e.data, 0, 1)[0];
      if (b === 0x10) { const r = L.parseSpectrum(e.data); if (r) onRow(r); }
      else if (b === 0x20) { const f = L.parseAudio(e.data); if (f) onAudio(f); }
    };
    ws.onclose = (e) => onClose(e.code);
    ws.onerror = () => { /* onclose follows */ };
  }
  function onClose(code) {
    clearInterval(S.pinger); S.welcome = null; S.spectrumOn = false; flushAudio();
    const adv = L.closeAdvice(code, S.attempt);
    setConn(code === 4029 ? "busy" : "offline", code === 4029 ? "warn" : "bad");
    if (code === 4001) { S.password = ""; showLogin(true); $("app").hidden = true; banner(adv.text); return; }
    if (adv.retry && !S.stopRetry) { S.attempt++; scheduleRetry(code, adv); }
    else banner(S.kickText || adv.text);
  }
  function scheduleRetry(code, adv) {
    adv = adv || L.closeAdvice(code, S.attempt);
    S.retryAt = performance.now() + adv.delayMs;
    const tick = () => {
      const s = Math.max(0, Math.ceil((S.retryAt - performance.now()) / 1000));
      banner(adv.text + (s ? " Retrying in " + s + " s." : "")); S.retryNote = s ? "Retrying in " + s + " s." : "Retrying now."; renderScopeMsg();
      if (s > 0) S.retryTimer = setTimeout(tick, 500); else { fetchInfo().then(connect); }
    };
    tick();
  }
  function onText(m) {
    if (!m) return;
    switch (m.type) {
      case "welcome": return onWelcome(m);
      case "state": return onState(m);
      case "ranges": S.ranges = L.readRanges(m.ranges); applyRanges(true); return;
      case "capture": S.cap = { centre_hz: m.centre_hz, rate_hz: m.rate_hz }; setCapture(); return;
      case "event": return onEvent(m);
      case "error": if (m.code !== "busy") banner(m.message || m.code); return;
      case "kicked": S.stopRetry = !/^(server_busy)$/.test(m.reason); S.kickText = "Disconnected: " + String(m.reason).slice(0, 120); return;
      default: return;   // unknown server messages are ignored (protocol rule)
    }
  }
  function setCapture() {
    S.bounds = L.captureBounds(S.cap); S.full = L.fullBounds(S.cap);
    S.full = { start: Math.max(S.full.start, 0), stop: S.full.stop };
    S.view = S.view ? L.normView(S.view, S.full) : { start: S.full.start, stop: S.full.stop };
    S.rows = []; renderFreq(); buildBands(); invalidate({ spec: 1, wf: 1 }); renderScopeMsg(); requestSpan();
  }
  function onWelcome(m) {
    S.welcome = m; S.attempt = 0; S.kickText = ""; banner("");
    S.ranges = L.readRanges(m.ranges);   // missing: the host set no ranges (older servers never send it)
    S.cap = m.capture; S.view = null; setCapture();
    $("app").hidden = false; $("login").hidden = true;
    setConn(m.class, "ok"); if (S.info) renderInfo();
    if (m.server && m.server.name) { $("srvName").textContent = m.server.name; }
    buildModes(m.demods); buildDecoders(m.decoders);
    const last = store.get("last", null);
    if (last && last.f >= S.bounds.start && last.f <= S.bounds.stop && m.demods.includes(last.m)) {
      S.freq = last.f; S.mode = last.m; S.low = last.lo; S.high = last.hi; S.sq = last.sq != null ? last.sq : -150;
    } else { S.freq = Math.round(m.capture.centre_hz); S.mode = m.demods.includes("nfm") ? "nfm" : m.demods[0]; const d = L.MODE_DEFAULT[S.mode]; S.low = d[0]; S.high = d[1]; }
    if (L.rangeActive(S.ranges)) { const sn = L.rangeSnap(S.ranges, S.mode, S.freq, S.low, S.high, S.cap); if (sn !== null) S.freq = Math.round(sn); }
    if (last && last.f && S.view && (S.freq < S.view.start || S.freq > S.view.stop)) S.view = null;
    renderRangeInfo(); buildBands();
    renderMode(); renderFreq(); renderSquelch();
    send(L.msg.tune({ freq_hz: S.freq, mode: S.mode, low_hz: S.low, high_hz: S.high }));
    send(L.msg.rx({ squelch_db: S.sq }));
    requestSpan();
    if (S.audioOn) { S.audioSeq = -1; sendAudio(); }
    S.pinger = setInterval(() => send(L.msg.ping(Date.now())), 20000);
    for (const s of S.subs) send(L.msg.subscribe(s.decoder, s.freq));
  }
  // ---------- the host's frequency ranges ----------
  function renderRangeInfo() {
    const box = $("rangeInfo"); if (!box) return;
    box.textContent = "";
    if (!L.rangeActive(S.ranges)) { box.hidden = true; return; }
    const d = L.describeRanges(S.ranges, (hz) => String(Math.round(hz / 100) / 10000) + " MHz");
    const add = (cls, head, items) => { if (!items.length) return; const p = el("p", cls); p.appendChild(el("b", null, head + " ")); p.appendChild(document.createTextNode(items.join("; "))); box.appendChild(p); };
    add("ok", "This server lets you listen in:", d.allow);
    add("bad", "Blocked:", d.block);
    add("dim", "Host notes:", d.notes);
    box.hidden = !box.childElementCount;
  }
  // the ranges changed (welcome, or the host changed them while you listen): re-fit the tuning and redraw
  function applyRanges(changed) {
    renderRangeInfo();
    buildModes(S.welcome && S.welcome.demods);
    buildBands();
    if (L.rangeActive(S.ranges)) {
      const sn = L.rangeSnap(S.ranges, S.mode, S.freq, S.low, S.high, S.cap);
      if (sn !== null && Math.round(sn) !== S.freq) { S.freq = Math.round(sn); if (changed) banner("The host changed the allowed ranges: your frequency was moved."); sendTune(); }
    } else if (changed) banner("");
    renderMode(); renderFreq(); invalidate({ spec: 1 });
  }
  function onState(m) {
    if (performance.now() - S.pendingTune > 600) {
      if (typeof m.freq_hz === "number") S.freq = m.freq_hz;
      if (L.MODES.includes(m.mode)) S.mode = m.mode;
      if (typeof m.low_hz === "number" && typeof m.high_hz === "number") { S.low = m.low_hz; S.high = m.high_hz; }
      renderMode(); renderFreq();
    }
    if (typeof m.squelch_db === "number" && m.squelch_db !== S.sq) { S.sq = m.squelch_db; renderSquelch(); }
    if (Array.isArray(m.subscriptions)) { S.subKeys = m.subscriptions; renderDecoderButtons(); }
    const d = typeof m.degrade === "number" ? m.degrade : 0;
    if (d > 0) $("footInfo").textContent = "The server is sending fewer spectrum rows to keep your connection within its limits (level " + d + ").";
    invalidate({ spec: 1 });
  }

  // ---------- squelch / volume ----------
  function renderSquelch() { $("sq").value = Math.max(-150, S.sq); $("sqOut").textContent = S.sq <= -150 ? "open" : S.sq + " dB"; }
  function renderVolume() { $("vol").value = S.volume; $("volOut").textContent = String(S.volume); applyVolume(); }

  // ---------- bands and bookmarks ----------
  function buildBands() {
    const box = $("bandList"); box.textContent = ""; let hidden = 0, shown = 0;
    for (const b of bands) {
      const slice = S.full ? L.bandSlice({ start: b.s, stop: b.e }, S.bounds) : null;
      if (!slice || !L.rangeOverlaps(S.ranges, b.s, b.e, 1000)) { hidden++; continue; }
      shown++;
      const btn = el("button", null, b.n); btn.type = "button"; btn.dataset.s = String(b.s); btn.dataset.e = String(b.e);
      btn.appendChild(el("span", "f", fmtMHz(b.s) + " - " + fmtMHz(b.e) + " MHz"));
      btn.addEventListener("click", () => {
        const f = L.clamp(b.f, slice.start, slice.stop); setView(slice);
        if (S.welcome && S.welcome.demods.includes(b.m)) { S.mode = b.m; const d = L.MODE_DEFAULT[b.m]; S.low = d[0]; S.high = d[1]; renderMode(); sendTune({ mode: b.m }); }
        setFreq(f, true);
      });
      box.appendChild(btn);
    }
    $("bandNote").textContent = !S.full ? "" : !shown ? "No preset band falls inside this receiver's capture. Use the spectrum or type a frequency." : hidden ? hidden + " preset bands are outside this receiver's capture and are not listed." : "";
    renderBandsActive();
  }
  function renderBandsActive() { for (const b of $("bandList").children) b.setAttribute("aria-current", String(S.freq >= +b.dataset.s && S.freq <= +b.dataset.e)); }
  function marks() { return store.get("marks", []); }
  function renderMarks() {
    const ul = $("markList"); ul.textContent = "";
    const list = marks();
    if (!list.length) ul.appendChild(el("li", "dim", "No bookmarks yet."));
    list.forEach((m, i) => {
      const li = el("li"); const go = el("button", "go", m.name); go.type = "button";
      go.appendChild(el("span", null, fmtMHz(m.f) + " MHz " + m.m.toUpperCase()));
      go.addEventListener("click", () => {
        if (S.bounds && (m.f < S.bounds.start || m.f > S.bounds.stop)) { banner("That bookmark is outside this server's capture."); return; }
        banner(""); if (m.m !== S.mode) { S.mode = m.m; S.low = m.lo; S.high = m.hi; renderMode(); sendTune({ mode: m.m, low_hz: m.lo, high_hz: m.hi }); }
        setFreq(m.f, true);
      });
      const del = el("button", "del", "×"); del.type = "button"; del.setAttribute("aria-label", "Delete bookmark " + m.name);
      del.addEventListener("click", () => { const l = marks(); l.splice(i, 1); store.set("marks", l); renderMarks(); });
      li.append(go, del); ul.appendChild(li);
    });
  }
  function addMark() {
    const name = L.sanitizeText($("markName").value, 40) || fmtMHz(S.freq) + " MHz";
    const l = marks(); l.push({ name, f: S.freq, m: S.mode, lo: S.low, hi: S.high }); store.set("marks", l.slice(-200));
    $("markName").value = ""; renderMarks();
  }

  // ---------- decoders ----------
  function buildDecoders(list) {
    const box = $("decList"); box.textContent = "";
    if (!list || !list.length) box.appendChild(el("span", "dim", "This server offers no decoders."));
    for (const d of list || []) {
      const b = el("button", null, d.name || d.kind); b.type = "button"; b.dataset.kind = d.kind; b.dataset.needs = d.needs_freq ? "1" : "";
      b.title = d.needs_freq ? "Decodes at the tuned frequency" : "";
      b.setAttribute("aria-pressed", "false");
      b.addEventListener("click", () => toggleDecoder(d));
      box.appendChild(b);
    }
  }
  function toggleDecoder(d) {
    const idx = S.subs.findIndex((s) => s.decoder === d.kind);
    if (idx >= 0) { const s = S.subs.splice(idx, 1)[0]; send(L.msg.unsubscribe(s.decoder, s.freq)); dropViews(s.decoder); }
    else { const s = { decoder: d.kind, freq: d.needs_freq ? Math.round(D.defaultFreq(d.kind, S.freq, S.cap)) : null }; S.subs.push(s); send(L.msg.subscribe(s.decoder, s.freq)); }
    renderDecoderButtons();
  }
  function renderDecoderButtons() {
    for (const b of $("decList").querySelectorAll("button")) b.setAttribute("aria-pressed", String(S.subs.some((s) => s.decoder === b.dataset.kind)));
  }
  function dropViews(kind) { for (const k of Object.keys(S.views)) if (k === kind || k.startsWith(kind + "@")) { const v = S.views[k]; if (v.node.parentNode) v.node.parentNode.removeChild(v.node); delete S.views[k]; } for (const k of Object.keys(S.decoders)) if (k === kind || k.startsWith(kind + "@")) delete S.decoders[k]; }
  function onEvent(m) {
    if (typeof m.decoder !== "string") return;
    const key = m.decoder.slice(0, 64);
    if (D.validEvent(m.data)) {   // the v1 decoder events: every one goes to its view (pictures need all strips)
      const sub = S.subs.find((x) => key === x.decoder || key.startsWith(x.decoder + "@"));
      if (!sub) return;
      const v = S.views[key] || (S.views[key] = new D.DecoderView(document, key, m.data.kind));
      v.ingest(m.data); S.decDirty = true; return;
    }
    S.decoders[key] = m.data; S.decDirty = true;
  }
  function renderDecoders() {
    if (!S.decDirty || $("paneDec").hidden) return; S.decDirty = false;
    const out = $("decOut"); out.textContent = "";
    for (const key of Object.keys(S.views)) { const v = S.views[key]; v.refresh(); out.appendChild(v.node); }
    for (const key of Object.keys(S.decoders)) {
      if (!S.subs.some((s) => key === s.decoder || key.startsWith(s.decoder + "@"))) continue;
      const sec = el("section"); sec.appendChild(el("h3", null, key));
      for (const t of L.findTables(S.decoders[key])) {
        if (t.name) sec.appendChild(el("h3", "dim", t.name));
        const tb = t.table;
        if (tb.kind === "text") { sec.appendChild(el("pre", "mono", tb.text)); continue; }
        const table = el("table");
        if (tb.kind === "table") { const tr = el("tr"); for (const c of tb.columns) tr.appendChild(el("th", null, c)); table.appendChild(tr); for (const r of tb.rows) { const row = el("tr"); for (const v of r) row.appendChild(el("td", null, v)); table.appendChild(row); } }
        else for (const r of tb.rows) { const row = el("tr"); row.appendChild(el("th", null, r[0])); row.appendChild(el("td", null, r[1])); table.appendChild(row); }
        sec.appendChild(table);
      }
      out.appendChild(sec);
    }
    if (!out.children.length) out.appendChild(el("p", "dim", S.subs.length ? "Waiting for results." : "Pick a decoder above."));
  }

  // ---------- controls sheet (bottom sheet on a phone, side column on a wide screen) ----------
  const isWide = () => matchMedia("(min-width: 960px)").matches;
  function showTab(name) {
    if (isWide() && name === "tune") name = "bands";   // on a wide screen the tuning controls sit above the tabs and are always shown
    S.tab = name;
    for (const o of document.querySelectorAll(".tabs button")) o.setAttribute("aria-selected", String(o.dataset.tab === name));
    $("paneTune").hidden = !(name === "tune" || isWide());
    for (const [k, id] of [["bands", "paneBands"], ["marks", "paneMarks"], ["dec", "paneDec"], ["help", "paneHelp"]]) $(id).hidden = name !== k;
    S.decDirty = true; renderDecoders();
  }
  function setSheet(open) {
    S.sheetOpen = open; store.set("sheet", open); $("sheet").classList.toggle("collapsed", !open);
    $("grab").setAttribute("aria-expanded", String(open)); $("grab").setAttribute("aria-label", open ? "Collapse controls" : "Expand controls");
    invalidate({ spec: 1, wf: 1, ov: 1 });
  }

  // ---------- wiring ----------
  function init() {
    buildFreq();
    const stepSel = $("step");
    for (const s of L.STEPS) { const o = el("option", null, L.stepLabel(s)); o.value = String(s); stepSel.appendChild(o); }
    if (!L.STEPS.includes(S.step)) S.step = 5000; stepSel.value = String(S.step);
    stepSel.addEventListener("change", () => { S.step = +stepSel.value; store.set("step", S.step); });
    const pSel = $("palette");
    for (const n of L.PALETTE_NAMES) { const o = el("option", null, n); o.value = n; pSel.appendChild(o); }
    pSel.value = S.palette; pSel.addEventListener("change", () => { S.palette = pSel.value; pal = L.makePalette(S.palette); store.set("palette", S.palette); invalidate({ wf: 1 }); });
    $("range").value = S.range; $("floorAdj").value = S.floorAdj;
    $("range").addEventListener("input", (e) => { S.range = +e.target.value; store.set("range", S.range); $("rangeOut").textContent = S.range + " dB"; invalidate({ spec: 1, wf: 1 }); });
    $("floorAdj").addEventListener("input", (e) => { S.floorAdj = +e.target.value; store.set("floorAdj", S.floorAdj); $("floorOut").textContent = (S.floorAdj > 0 ? "+" : "") + S.floorAdj + " dB"; invalidate({ spec: 1, wf: 1 }); });
    pal = L.makePalette(S.palette);
    $("freqType").addEventListener("click", typeFreq);
    $("bw").addEventListener("input", (e) => setBandwidth(+e.target.value));
    $("sq").addEventListener("input", (e) => { S.sq = +e.target.value; renderSquelch(); persist(); send(L.msg.rx({ squelch_db: S.sq })); });
    $("vol").addEventListener("input", (e) => { S.volume = +e.target.value; store.set("vol", S.volume); renderVolume(); });
    $("audioBtn").addEventListener("click", toggleAudio);
    $("muteBtn").addEventListener("change", (e) => { S.muted = e.target.checked; renderAudio(); applyVolume(); });
    $("zoomIn").addEventListener("click", () => zoom(0.5));
    $("zoomOut").addEventListener("click", () => zoom(2));
    $("zoomFull").addEventListener("click", () => setView({ start: S.full.start, stop: S.full.stop }));
    $("zoomSig").addEventListener("click", () => { const half = L.clamp(S.bw * 3, 10000, 500000); setView({ start: S.freq - half, stop: S.freq + half }); });
    $("markAdd").addEventListener("click", addMark);
    $("markName").addEventListener("keydown", (e) => { if (e.key === "Enter") addMark(); });
    for (const t of document.querySelectorAll(".tabs button")) t.addEventListener("click", () => { showTab(t.dataset.tab); if (!isWide() && !S.sheetOpen) setSheet(true); });
    $("grab").addEventListener("click", () => setSheet(!S.sheetOpen));
    $("audioQuick").addEventListener("click", toggleAudio);
    $("noticeText").classList.add("clamp");
    matchMedia("(min-width: 960px)").addEventListener("change", () => showTab(S.tab));
    $("rangeOut").textContent = S.range + " dB"; $("floorOut").textContent = (S.floorAdj > 0 ? "+" : "") + S.floorAdj + " dB";
    buildSticks(); wireType(); setSheet(S.sheetOpen); showTab("tune");
    attachScope(ov);
    $("stage").addEventListener("keydown", (e) => {
      if (e.key === "ArrowLeft") { changeFreq(-S.step * (e.shiftKey ? 10 : 1)); e.preventDefault(); }
      else if (e.key === "ArrowRight") { changeFreq(S.step * (e.shiftKey ? 10 : 1)); e.preventDefault(); }
    });
    document.addEventListener("keydown", (e) => {
      const t = e.target; if (e.ctrlKey || e.metaKey || e.altKey) return;
      if (t && (t.tagName === "INPUT" || t.tagName === "SELECT" || t.tagName === "TEXTAREA")) return;
      if (t && (t.id === "freq" || t.id === "stage")) { if (!/^[0-9mMlL+=\-\[\]]$/.test(e.key)) return; }
      else if (t && t.tagName === "BUTTON" && /^[ ]$|Enter/.test(e.key)) return;
      if (!S.welcome) return;
      if (e.key === "m" || e.key === "M") { S.muted = !S.muted; renderAudio(); applyVolume(); }
      else if (/^[1-6]$/.test(e.key)) { const m = $("modes").children[+e.key - 1]; if (m) setMode(m.dataset.mode); }
      else if (e.key === "l" || e.key === "L") toggleAudio();
      else if (e.key === "0") setView({ start: S.full.start, stop: S.full.stop });
      else if (e.key === "[" || e.key === "]") { const i = L.STEPS.indexOf(S.step) + (e.key === "]" ? 1 : -1); if (i >= 0 && i < L.STEPS.length) { S.step = L.STEPS[i]; $("step").value = String(S.step); store.set("step", S.step); } }
      else if (e.key === "+" || e.key === "=") zoom(0.5);
      else if (e.key === "-") zoom(2);
      else if (e.key === "ArrowLeft" && !(t && t.id === "freq")) { changeFreq(-S.step * (e.shiftKey ? 10 : 1)); }
      else if (e.key === "ArrowRight" && !(t && t.id === "freq")) { changeFreq(S.step * (e.shiftKey ? 10 : 1)); }
    });
    $("login").addEventListener("submit", (e) => {
      e.preventDefault(); S.name = L.sanitizeText($("loginName").value, 32); store.set("name", S.name);
      S.password = $("loginPw").value; if ($("loginInv").value.trim()) S.invite = $("loginInv").value.trim();
      $("loginPw").value = ""; $("login").hidden = true; banner(""); connect();
    });
    if (window.ResizeObserver) new ResizeObserver(() => invalidate({ spec: 1, wf: 1, ov: 1 })).observe($("stage")); else window.addEventListener("resize", () => invalidate({ spec: 1, wf: 1, ov: 1 }));
    document.addEventListener("visibilitychange", () => { if (!document.hidden && S.ws && S.ws.readyState > 1 && !S.stopRetry) { clearTimeout(S.retryTimer); fetchInfo().then(connect); } });
    renderVolume(); renderAudio(); renderMarks(); renderSquelch(); renderFreq(); drawMeter();
    setInterval(renderDecoders, 500);
    setInterval(() => { if (S.info || S.welcome === null) fetchInfo(); }, 10000);

    // an invite arrives as #invite=TOKEN (kept out of the server's logs); remove it from the address bar
    const h = new URLSearchParams(location.hash.replace(/^#/, ""));
    if (h.get("invite")) { S.invite = h.get("invite").slice(0, 128); try { history.replaceState(null, "", location.pathname + location.search); } catch (e) { /* ignore */ } }
    fetchInfo().then((info) => {
      if (S.notServer) { setConn("no receiver", "dim"); banner("There is no OnAir receiver behind this address. OnAir Web is served by a receiver you run yourself: start onair-server with --web --web-lan (see docs/ONAIR-WEB.md), then open http://<that computer>:8073 from a browser on your network. Nothing is received on this page."); return; }
      if (info && authNeeded()) { showLogin(true); setConn("sign in", "warn"); } else connect();
    });
  }
  init();
})();
