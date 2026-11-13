// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

/* OnAir Web browser client. Vanilla JS, no build step, no network use except this server. Receive only: no transmit code exists here.
 * Protocol: docs/ONAIR-WEB-PROTOCOL.md. Pure helpers live in onair-lib.js (tested by tests/web/run.js). */
(function () {
  "use strict";
  const L = window.OnAirLib;
  const D = window.OnAirDecoders;
  const Cfg = window.OnAirSettings, I18N = window.OnAirI18n, MAP = window.OnAirMap;
  const t = (k, v) => I18N.t(k, v);
  const $ = (id) => document.getElementById(id);
  const el = (tag, cls, text) => { const e = document.createElement(tag); if (cls) e.className = cls; if (text != null) e.textContent = text; return e; };

  // ---------- storage (every access guarded: private windows throw) ----------
  const store = {
    get(k, d) { try { const v = localStorage.getItem("onair." + k); return v == null ? d : JSON.parse(v); } catch (e) { return d; } },
    set(k, v) { try { localStorage.setItem("onair." + k, JSON.stringify(v)); } catch (e) { /* ignore */ } },
  };

  // ---------- settings (docs/ONAIR-WEB.md, "Settings"): one validated object kept in localStorage ----------
  let ST;
  (function loadSettings() {
    let raw = null; try { raw = localStorage.getItem(Cfg.KEY); } catch (e) { /* private window */ }
    ST = Cfg.load(localStorage);
    if (!raw) {   // first run of this version: carry over what older versions of the page stored
      const o = {}; const pick = (k, name) => { const v = store.get(name, undefined); if (v !== undefined && v !== null) o[k] = v; };
      pick("step", "step"); pick("palette", "palette2"); pick("range", "range"); pick("floorAdj", "floorAdj"); pick("theme", "theme");
      ST = Cfg.sanitize(Object.assign(Cfg.defaults(), o));
    }
    I18N.setLang(I18N.pickLang(ST.lang, navigator.languages || [navigator.language]));
  })();
  const saveSettings = () => Cfg.save(localStorage, ST);

  // ---------- state ----------
  const S = {
    ws: null, info: null, welcome: null, ranges: null, cap: null, bounds: null, view: null, full: null,
    freq: 0, mode: "nfm", low: -6250, high: 6250, sq: -150, agc: ST.agc,
    audioOn: false, muted: false, volume: store.get("vol", 70), level: -150, peak: -150, peakAt: 0,
    spectrumOn: false, subs: [], step: ST.step, palette: ST.palette,
    range: ST.range, floorAdj: ST.floorAdj, adapt: null, trace: null, peakDb: null, mapMarks: [], mapSel: "",
    attempt: 0, retryTimer: 0, retryAt: 0, stopRetry: false, pendingTune: 0, lastTuneSent: 0, tuneTimer: 0,
    rows: [], rowSeq: -1, audioSeq: -1, lost: 0, decoders: {}, views: {}, decDirty: false, name: store.get("name", ""), invite: "", password: "",
    bw: 0, theme: ST.theme === "auto" ? null : ST.theme, peakHold: null, hover: null, rec: null, outLvl: 0, sel: "",
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

  // ---------- theme ----------
  function setTheme(tt, keep) {
    const th = tt || (window.matchMedia && matchMedia("(prefers-color-scheme: light)").matches ? "light" : "dark");
    document.documentElement.dataset.theme = th; { const tb = $("themeBtn"); if (tb) tb.textContent = t("menu.theme", { v: t(th === "light" ? "theme.light" : "theme.dark") }); }
    const m = document.querySelector('meta[name="theme-color"]'); if (m) m.content = th === "light" ? "#dadada" : "#252526";
    if (tt && !keep) { ST.theme = tt; saveSettings(); }
    readColors(); if (S.view) { drawSpectrum(); redrawWaterfall(); } if (mapView) mapView.refreshColours();
  }

  // ---------- small UI helpers ----------
  function setConn(text, cls) { const c = $("conn"); S.connKey = text; $("connText").textContent = I18N.CAT.en["conn." + text.replace(/ /g, "")] ? t("conn." + text.replace(/ /g, "")) : text; c.className = "lamp " + (cls || ""); renderStatus(); }
  function banner(text) { const b = $("banner"); b.hidden = !text; b.textContent = text || ""; if (text && !S.welcome) emptyState(text, true); }
  function emptyState(text, err, title) { const e = $("empty"); e.hidden = false; e.classList.toggle("err", !!err); $("emptyTitle").textContent = title || (err ? t("load.notconn") : t("load.title")); $("emptyText").textContent = err ? "" : (text || ""); if (err) { $("emptyText").textContent = t("load.why"); } }
  function send(m) { if (S.ws && S.ws.readyState === 1) { S.ws.send(JSON.stringify(m)); return true; } return false; }
  const fmtMHz = (hz) => (hz / 1e6).toFixed(hz % 1e6 === 0 ? 1 : 3);
  function schedule(fn, ms) { let t = 0; return () => { clearTimeout(t); t = setTimeout(fn, ms); }; }

  // ---------- frequency display ----------
  const digitEls = [];
  function buildFreq() {
    const f = $("freq"); f.textContent = "";
    for (let i = 12; i >= 0; i--) {
      const d = el("span", "d", "0"); d.dataset.place = String(i); digitEls[i] = d; f.appendChild(d);
      if (i === 12 || i === 9 || i === 6 || i === 3) f.appendChild(el("span", "p", "."));
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
  function renderFreq() {
    const s = String(Math.max(0, Math.round(S.freq))).padStart(13, "0");
    let lead = true;
    for (let i = 12; i >= 0; i--) {
      const ch = s[12 - i]; if (ch !== "0" || i < 3) lead = false;
      digitEls[i].textContent = ch; digitEls[i].className = "d" + (lead ? " z" : "");
    }
    const f = $("freq"); f.setAttribute("aria-valuenow", String(S.freq)); f.setAttribute("aria-valuetext", L.formatFreq(S.freq).replace(/\./g, " ") + " hertz");
    const s0 = $("viewText");
    if (S.view) { s0.textContent = fmtMHz(S.view.start) + " - " + fmtMHz(S.view.stop) + " MHz"; syncZoom(); }
    renderStatus();
  }
  function typeFreq() {
    if (!S.welcome) return;
    $("typeIn").value = (S.freq / 1e6).toFixed(6).replace(/0+$/, "").replace(/\.$/, ""); $("typeMsg").textContent = "";
    $("typeDlg").showModal(); $("typeIn").select();
  }

  // ---------- header info, status bar, zoom slider ----------
  function renderStatus() {
    const mhz = S.freq ? (S.freq / 1e6).toFixed(4) + " MHz" : "-", fa = $("infoA"); if (!fa) return;
    fa.textContent = S.mode.toUpperCase() + " " + mhz;
    const bw = S.high - S.low, lv = S.level > -149 ? ", " + S.level.toFixed(1) + " dBFS" : "";
    $("infoB").textContent = S.mode.toUpperCase() + " " + (bw >= 1000 ? (bw / 1000).toFixed(bw % 1000 ? 1 : 0) + " kHz" : bw + " Hz") + lv + (S.sq > -150 ? ", squelch " + S.sq + " dB" : ", squelch open");
    $("sbFreq").textContent = S.freq ? (S.freq / 1e6).toFixed(3) + " MHz" : "-"; $("sbMode").textContent = S.mode.toUpperCase();
    $("sbUsers").textContent = S.info ? S.info.users + " / " + S.info.max_users : "-";
    const on = !!S.welcome, led = $("sbLed");
    $("sbState").textContent = !on ? (S.connKey === "connecting" ? t("state.connecting") : t("state.offline")) : S.audioOn ? (S.muted ? t("state.muted") : t("state.receiving")) : t("state.idle");
    led.className = "led " + (!on ? "bad" : S.audioOn ? "ok" : "warn");
    { const mc = $("muteChk"); if (mc) mc.checked = S.muted; const ct = $("centreTxt"); if (ct && S.cap) ct.textContent = "centre " + (S.cap.centre_hz / 1e6).toFixed(3) + " MHz"; }
  }
  function syncZoom() {
    const z = $("zoomSlider"); if (!z || !S.view || !S.full || z.matches(":active")) return;
    const full = S.full.stop - S.full.start, sp = S.view.stop - S.view.start, mn = L.MIN_SPAN || 1000;
    z.value = String(Math.round(L.clamp(Math.log(full / sp) / Math.log(full / mn), 0, 1) * 100));
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
    renderFreq(); drawSpectrum(); persist(); sendTune(); markBands();
  }
  function changeFreq(d) { setFreq(S.freq + d, true); }
  function sendTune(extra) {
    S.pendingTune = performance.now();
    const m = Object.assign({ freq_hz: S.freq }, extra || {});
    if (S.control && !S.control.can_tune) { renderControl(); return; }   // the host's tune permission: the server would refuse it anyway
    const go = () => { S.lastTuneSent = performance.now(); send(L.msg.tune(m)); };
    clearTimeout(S.tuneTimer);
    const wait = Math.max(0, 70 - (performance.now() - S.lastTuneSent));
    if (extra || wait === 0) go(); else S.tuneTimer = setTimeout(go, wait);
  }
  function setMode(mode) {
    if (!L.MODES.includes(mode)) return;
    S.mode = mode;
    const d = L.MODE_DEFAULT[mode]; S.low = d[0]; S.high = d[1];
    if (L.rangeActive(S.ranges)) S.freq = Math.round(clampFreq(S.freq));   // the new passband must fit too
    renderMode(); renderFreq(); drawSpectrum(); sendTune({ mode });
    if (mode === "wfm") send(L.msg.rx({ deemph_us: ST.deemphUs }));
    if (S.audioOn) sendAudio();
    persist();
  }
  function setBandwidth(bw) {
    const pb = L.passband(S.mode, bw); S.low = pb.low_hz; S.high = pb.high_hz;
    if (L.rangeActive(S.ranges)) S.freq = Math.round(clampFreq(S.freq));
    renderMode(); renderFreq(); sendTune({ low_hz: S.low, high_hz: S.high }); drawSpectrum();
  }
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
    { const dr = $("deRow"); if (dr) dr.hidden = S.mode !== "wfm"; const gr = $("gainRow"); if (gr) gr.classList.toggle("off", ST.agc !== "off" && S.mode !== "nfm" && S.mode !== "wfm"); }
  }

  // ---------- persistence of the last tuning ----------
  const persist = schedule(() => store.set("last", { f: S.freq, m: S.mode, lo: S.low, hi: S.high, sq: S.sq }), 400);

  // ---------- view (zoom / pan) and the spectrum request ----------
  const requestSpan = schedule(() => {
    if (!S.welcome || !S.view) return;
    const canvas = $("spec"), want = Cfg.binsFor(ST, canvas.clientWidth * (window.devicePixelRatio || 1), S.welcome.limits.max_bins);
    const full = L.viewIsFull(S.view, S.full);
    send(L.msg.spectrum(true, want, Cfg.fpsFor(ST, S.welcome.limits.max_fps), full ? null : S.view.start, full ? null : S.view.stop));
    S.spectrumOn = true;
  }, 120);
  function setView(v) {
    if (!S.bounds) return;
    S.view = L.normView(v, S.full); S.peakDb = null; S.trace = null; renderFreq(); drawSpectrum(); redrawWaterfall(); requestSpan();
  }
  function zoom(f, anchor) { setView(L.zoomView(S.view, S.full, f, anchor == null ? S.freq : anchor)); }

  // ---------- drawing ----------
  const spec = $("spec"), wf = $("wf");
  const sctx = spec.getContext("2d"), wctx = wf.getContext("2d");
  let pal = L.makePalette(S.palette), pix = null, C = {};
  function readColors() {
    const cs = getComputedStyle(document.documentElement), g = (n) => cs.getPropertyValue(n).trim();
    C = { bg: g("--plotbg"), grid: g("--grid"), text: g("--axis"), trace: g("--trace"), accent: g("--accent"), edge: g("--edge"), bad: g("--bad"), good: g("--good"), panel: g("--rail"), bright: g("--bright"), tab: g("--tab"), light: document.documentElement.dataset.theme === "light" };
  }
  function fit(c) {
    const dpr = window.devicePixelRatio || 1, w = Math.max(50, Math.round(c.clientWidth * dpr)), h = Math.max(50, Math.round(c.clientHeight * dpr));
    if (c.width !== w || c.height !== h) { c.width = w; c.height = h; return true; }
    return false;
  }
  function lv(row) { return Cfg.levels(ST, row, S.adapt); }   // {lo, span} for the colours and the trace
  function rowLo(row) { return lv(row).lo; }
  function palBg() { return "rgb(" + pal[0] + "," + pal[1] + "," + pal[2] + ")"; }
  function fmtAxis(f, g) { return g >= 1e6 ? String(+(f / 1e6).toFixed(2)) : g >= 1e5 ? (f / 1e6).toFixed(1) : (f / 1e6).toFixed(3); }
  // layout of the spectrum canvas (device pixels): band strip on top, frequency labels below the plot
  function specGeom() { const dpr = window.devicePixelRatio || 1; return { dpr, top: Math.round(17 * dpr), bot: Math.round(20 * dpr), left: 0 }; }
  function edgeX(W, which) { // x of a passband edge in canvas pixels, or null
    if (!S.view) return null; return L.hzToX(S.view, S.freq + (which ? S.high : S.low), W);
  }
  function drawSpectrum() {
    fit(spec); const W = spec.width, H = spec.height, G = specGeom(), dpr = G.dpr;
    sctx.fillStyle = C.bg; sctx.fillRect(0, 0, W, H);
    if (!S.view) { updateWfMarks(); return; }
    const FONT = '"JetBrains Mono", Menlo, ui-monospace, monospace';
    const v = S.view, span = v.stop - v.start, ph = H - G.top - G.bot;
    const row = S.rows[S.rows.length - 1];
    const LV = row ? lv(row) : { lo: -110, span: ST.range }, lo = LV.lo, RG = LV.span, yOf = (db) => G.top + ph * (1 - L.clamp((db - lo) / RG, 0, 1));
    // dB grid and labels (small, dim, on the left, as in the app)
    sctx.font = `${11 * dpr}px ${FONT}`; sctx.textBaseline = "middle"; sctx.textAlign = "left";
    const dstep = RG > 80 ? 20 : 10;
    sctx.fillStyle = C.text; sctx.globalAlpha = 0.8; sctx.fillText("power (dBFS/Hz)", 5 * dpr, G.top + 8 * dpr); sctx.globalAlpha = 1;
    for (let db = Math.ceil(lo / dstep) * dstep; db <= lo + RG; db += dstep) {
      const y = Math.round(yOf(db)) + 0.5;
      if (ST.showGrid) { sctx.strokeStyle = C.grid; sctx.lineWidth = 1; sctx.beginPath(); sctx.moveTo(0, y); sctx.lineTo(W, y); sctx.stroke(); }
      if (y > G.top + 20 * dpr && y < H - G.bot - 6 * dpr) { sctx.fillStyle = C.text; sctx.fillText(String(db), 5 * dpr, y); }
    }
    // frequency grid; the labels sit in a strip below the trace in a larger size
    const cand = [1e3, 2e3, 5e3, 1e4, 2e4, 5e4, 1e5, 2e5, 5e5, 1e6, 2e6, 5e6, 1e7, 2e7, 5e7, 1e8];
    const g = cand.find((c) => span / c <= 9) || 1e8;
    sctx.font = `${14 * dpr}px ${FONT}`; sctx.textBaseline = "middle"; sctx.textAlign = "center";
    for (let f = Math.ceil(v.start / g) * g; f < v.stop; f += g) {
      const x = Math.round(L.hzToX(v, f, W)) + 0.5;
      if (ST.showGrid) { sctx.strokeStyle = C.grid; sctx.beginPath(); sctx.moveTo(x, G.top); sctx.lineTo(x, H - G.bot); sctx.stroke(); }
      if (x > 24 * dpr && x < W - 24 * dpr) { sctx.fillStyle = C.bright; sctx.fillText(fmtAxis(f, g), x, H - G.bot / 2); }
    }
    // band plan labels along the top: a filled bar with the band name, like the app's labels row
    sctx.font = `${11 * dpr}px ${FONT}`; sctx.textAlign = "left";
    for (const b of (ST.showBands ? bands : [])) {
      if (b.e < v.start || b.s > v.stop) continue;
      const x0 = Math.max(0, L.hzToX(v, Math.max(b.s, v.start), W)), x1 = Math.min(W, L.hzToX(v, Math.min(b.e, v.stop), W));
      sctx.fillStyle = /^(160|80|40|20|10|6|2) m$|^70 cm$/.test(b.n) ? (C.light ? "rgba(52,92,150,.6)" : "rgba(52,92,150,.95)") : (C.light ? "rgba(166,122,46,.55)" : "rgba(160,118,50,.85)"); sctx.fillRect(x0, 0, Math.max(1, x1 - x0), G.top - 2 * dpr);
      if (x1 - x0 > 40 * dpr) { sctx.save(); sctx.beginPath(); sctx.rect(x0, 0, x1 - x0, G.top); sctx.clip(); sctx.fillStyle = "#f4eddc"; sctx.fillText(b.n, x0 + 4 * dpr, G.top / 2 - 1 * dpr); sctx.restore(); }
    }
    // memories: small flags under the band strip, with the name when there is room; a tap on one tunes to it
    S.flags = [];
    if (ST.showMarks) {
      sctx.font = `${10 * dpr}px ${FONT}`; sctx.textAlign = "left"; sctx.textBaseline = "middle"; let lastEnd = -1e9;
      for (const m of marks().slice().sort((a, b) => a.f - b.f)) {
        if (m.f < v.start || m.f > v.stop) continue;
        const x = L.hzToX(v, m.f, W); S.flags.push({ x: x / dpr, m });
        sctx.fillStyle = C.accent; sctx.beginPath(); sctx.moveTo(x - 4 * dpr, G.top); sctx.lineTo(x + 4 * dpr, G.top); sctx.lineTo(x, G.top + 8 * dpr); sctx.closePath(); sctx.fill();
        sctx.fillRect(Math.round(x), G.top, Math.max(1, dpr), 14 * dpr);
        if (x > lastEnd + 4 * dpr) { const tw = sctx.measureText(m.name).width; if (x + tw + 10 * dpr < W) { sctx.fillStyle = C.bright; sctx.globalAlpha = 0.85; sctx.fillText(m.name, x + 5 * dpr, G.top + 20 * dpr); sctx.globalAlpha = 1; lastEnd = x + tw + 10 * dpr; } }
      }
    }
    // the host's ranges: what is not visible is darkened, the allowed bands get a strip on top, blocked ones a red tint
    if (L.rangeActive(S.ranges)) {
      sctx.fillStyle = "rgba(0,0,0,0.55)";
      for (const h of L.hiddenRanges(S.ranges, v.start, v.stop)) { const a = L.hzToX(v, h.lo, W), b = L.hzToX(v, h.hi, W); sctx.fillRect(a, G.top, Math.max(1, b - a), ph); }
      if (S.ranges.restricted) {
        sctx.fillStyle = C.good;
        for (const a of L.mergeRanges(S.ranges.allow)) { if (a.hi < v.start || a.lo > v.stop) continue; const x = L.hzToX(v, Math.max(a.lo, v.start), W), y = L.hzToX(v, Math.min(a.hi, v.stop), W); sctx.fillRect(x, G.top, Math.max(2, y - x), Math.max(3, 3 * dpr)); }
      }
      sctx.fillStyle = "rgba(220,70,60,0.22)";
      for (const b of S.ranges.block) { if (b.hi < v.start || b.lo > v.stop) continue; const x = L.hzToX(v, Math.max(b.lo, v.start), W), y = L.hzToX(v, Math.min(b.hi, v.stop), W); sctx.fillRect(x, G.top, Math.max(2, y - x), ph); }
    }
    // passband: a translucent red-brown column with a blue tag above it, as in the app
    const x0 = L.hzToX(v, S.freq + S.low, W), x1 = L.hzToX(v, S.freq + S.high, W), xc = L.hzToX(v, S.freq, W);
    sctx.fillStyle = "rgba(205,95,65,0.34)"; sctx.fillRect(x0, G.top, Math.max(2, x1 - x0), ph);
    // trace: soft white, 1.2 px, filled faintly underneath
    if (row) {
      pix = L.rowToPixels(row, v, W, pix);
      let dbs = new Float32Array(W);
      for (let x = 0; x < W; x++) dbs[x] = pix[x] ? L.levelToDb(row, pix[x]) : NaN;
      if (row !== S.traceRow) { S.trace = Cfg.smoothTrace(S.trace, dbs, ST.smooth); S.traceRow = row; if (ST.peakHold) { if (!S.peakDb || S.peakDb.length !== W) S.peakDb = dbs.slice(); else for (let x = 0; x < W; x++) S.peakDb[x] = isNaN(dbs[x]) ? NaN : Math.max(dbs[x], isNaN(S.peakDb[x]) ? -999 : S.peakDb[x] - 0.15); } }
      if (S.trace && S.trace.length === W) dbs = S.trace;
      const ys = new Float32Array(W);
      for (let x = 0; x < W; x++) ys[x] = isNaN(dbs[x]) ? NaN : yOf(dbs[x]);
      if (ST.peakHold && S.peakDb && S.peakDb.length === W) { sctx.strokeStyle = "rgba(235,180,90,.7)"; sctx.lineWidth = Math.max(1, dpr); sctx.beginPath(); let pen = false; for (let x = 0; x < W; x++) { if (isNaN(S.peakDb[x])) { pen = false; continue; } const y = yOf(S.peakDb[x]); if (pen) sctx.lineTo(x, y); else { sctx.moveTo(x, y); pen = true; } } sctx.stroke(); }
      const segs = []; let sx = -1;
      for (let x = 0; x <= W; x++) { const ok = x < W && !isNaN(ys[x]); if (ok && sx < 0) sx = x; else if (!ok && sx >= 0) { segs.push([sx, x - 1]); sx = -1; } }
      sctx.lineJoin = "round"; sctx.lineWidth = Math.max(1, dpr * 1.2);
      for (const [a0, a1] of segs) {
        sctx.beginPath(); sctx.moveTo(a0, ys[a0]); for (let x = a0 + 1; x <= a1; x++) sctx.lineTo(x, ys[x]);
        sctx.strokeStyle = C.trace; sctx.stroke();
        sctx.lineTo(a1, H - G.bot); sctx.lineTo(a0, H - G.bot); sctx.closePath(); sctx.fillStyle = C.light ? "rgba(38,40,43,0.05)" : "rgba(238,239,242,0.05)"; sctx.fill();
      }
    }
    sctx.strokeStyle = "rgba(235,120,90,0.9)"; sctx.lineWidth = Math.max(1, dpr);
    sctx.beginPath(); sctx.moveTo(Math.round(xc) + 0.5, G.top); sctx.lineTo(Math.round(xc) + 0.5, H - G.bot); sctx.stroke();
    // passband edge handles (drag to change the bandwidth)
    sctx.fillStyle = "rgba(235,120,90,0.95)";
    for (const x of [x0, x1]) { sctx.fillRect(Math.round(x) - 1, G.top, Math.max(2, dpr * 1.5), ph); sctx.fillRect(Math.round(x) - 3 * dpr, G.top + ph / 2 - 9 * dpr, 6 * dpr, 18 * dpr); }
    // the blue tag
    const tag = modeTag(); sctx.font = `${11 * dpr}px ${FONT}`; sctx.textAlign = "center"; sctx.textBaseline = "middle";
    const tw = sctx.measureText(tag).width + 12 * dpr, tx = Math.min(Math.max(xc - tw / 2, 2), W - tw - 2);
    sctx.fillStyle = C.tab; sctx.fillRect(tx, G.top + 1, tw, 15 * dpr); sctx.fillStyle = "#ffffff"; sctx.fillText(tag, tx + tw / 2, G.top + 1 + 7.5 * dpr);
    if (S.hover != null) { const hx = Math.round(S.hover * W) + 0.5; sctx.strokeStyle = C.bright; sctx.globalAlpha = 0.35; sctx.lineWidth = 1; sctx.setLineDash([3 * dpr, 3 * dpr]); sctx.beginPath(); sctx.moveTo(hx, G.top); sctx.lineTo(hx, H - G.bot); sctx.stroke(); sctx.setLineDash([]); sctx.globalAlpha = 1; }
    updateWfMarks(); drawBar();
  }
  function drawBar() {
    const c = $("cbar"); if (!c) return; const g = c.getContext("2d"), row = S.rows[S.rows.length - 1]; if (!row) return;
    for (let y = 0; y < 96; y++) { const i = Math.round((1 - y / 95) * 255) * 3; g.fillStyle = "rgb(" + pal[i] + "," + pal[i + 1] + "," + pal[i + 2] + ")"; g.fillRect(0, y, 10, 1); }
    const q = lv(row); $("cbTop").textContent = Math.round(q.lo + q.span); $("cbBot").textContent = Math.round(q.lo);
  }
  function modeTag() { const b = bands.find((x) => S.freq >= x.s && S.freq <= x.e); return (b ? b.n + " " : "") + S.mode.toUpperCase(); }
  // the waterfall is scrolled pixels; the tuned marker and passband sit in an overlay so they stay crisp
  function updateWfMarks() {
    let pb = $("wfPb"), cf = $("wfCf"); const wrap = wf.parentNode;
    if (!pb) { pb = el("div", "wfpb"); pb.id = "wfPb"; cf = el("div", "wfcf"); cf.id = "wfCf"; wrap.append(pb, cf); }
    if (!S.view) { pb.hidden = cf.hidden = true; return; }
    const v = S.view, sp = v.stop - v.start, p = (hz) => ((hz - v.start) / sp) * 100;
    pb.hidden = cf.hidden = false;
    const a = p(S.freq + S.low), b = p(S.freq + S.high);
    pb.style.left = a + "%"; pb.style.width = Math.max(0.15, b - a) + "%"; cf.style.left = p(S.freq) + "%";
  }
  function paintWfRow(img, y, row) {
    const W = img.width, px = img.data; pix = L.rowToPixels(row, S.view, W, pix);
    const Q = lv(row), lo = Q.lo, span = Q.span;
    for (let x = 0; x < W; x++) {
      const o = (y * W + x) * 4;
      if (!pix[x]) { px[o] = pal[0]; px[o + 1] = pal[1]; px[o + 2] = pal[2]; px[o + 3] = 255; continue; }
      const db = L.levelToDb(row, pix[x]), i = Math.round(L.clamp((db - lo) / span, 0, 1) * 255) * 3;
      px[o] = pal[i]; px[o + 1] = pal[i + 1]; px[o + 2] = pal[i + 2]; px[o + 3] = 255;
    }
  }
  function redrawWaterfall() {
    fit(wf); const W = wf.width, H = wf.height;
    wctx.fillStyle = palBg(); wctx.fillRect(0, 0, W, H);
    if (!S.view || !S.rows.length) return;
    const img = wctx.createImageData(W, H);
    for (let y = 0; y < H; y++) {
      const row = S.rows[S.rows.length - 1 - y];
      if (!row) { for (let x = 0; x < W; x++) { const o = (y * W + x) * 4; img.data[o] = pal[0]; img.data[o + 1] = pal[1]; img.data[o + 2] = pal[2]; img.data[o + 3] = 255; } continue; }
      paintWfRow(img, y, row);
    }
    wctx.putImageData(img, 0, 0);
  }
  function onRow(row) {
    if (!S.audioOn && S.view) {   // no audio frames yet: estimate the level in the passband from the spectrum
      const sp = row.stopHz - row.startHz, b0 = Math.max(0, Math.floor((S.freq + S.low - row.startHz) / sp * row.bins)), b1 = Math.min(row.bins - 1, Math.ceil((S.freq + S.high - row.startHz) / sp * row.bins));
      let m = -1; for (let b = b0; b <= b1; b++) if (row.levels[b] > m) m = row.levels[b];
      if (m >= 0) S.level = L.levelToDb(row, m);
    }
    if (ST.rangeMode === "adaptive") { const e = Cfg.autoRange(row); if (e) S.adapt = Cfg.followRange(S.adapt, e, 0.04); }
    S.rows.push(row); if (S.rows.length > 1200) S.rows.splice(0, S.rows.length - 1000);
    if (!S.view) return;
    if (fit(wf)) { redrawWaterfall(); } else {
      const W = wf.width;
      wctx.drawImage(wf, 0, 1);
      const img = wctx.createImageData(W, 1); paintWfRow(img, 0, row); wctx.putImageData(img, 0, 0);
    }
    drawSpectrum();
  }

  // ---------- pointer interaction on the scope (tap = tune, drag = pan, drag a passband edge = bandwidth, wheel / pinch = zoom) ----------
  function attachScope(c, isSpec) {
    const ptrs = new Map(); let startView = null, moved = false, startX = 0, pinch0 = 0, pinchView = null, edge = null;
    const wrap = c.parentNode, tip = $("tip");
    const wpos = (e) => { const r = c.getBoundingClientRect(); return (e.clientX - r.left) / r.width; };
    const hitEdge = (e) => {
      if (!isSpec || !S.view) return null;
      const r = c.getBoundingClientRect(), W = r.width, px = e.clientX - r.left;
      const a = L.hzToX(S.view, S.freq + S.low, W), b = L.hzToX(S.view, S.freq + S.high, W);
      const near = (x) => Math.abs(px - x) <= (e.pointerType === "touch" ? 16 : 7);
      const canLo = S.mode !== "usb", canHi = S.mode !== "lsb";   // USB / LSB keep their carrier-side edge fixed
      if (canHi && near(b)) return "hi"; if (canLo && near(a)) return "lo"; return null;
    };
    const setEdge = (e) => {
      const hz = L.xToHz(S.view, wpos(e), 1), off = hz - S.freq;
      let bw;
      if (S.mode === "usb") bw = off - 100; else if (S.mode === "lsb") bw = -100 - off; else bw = 2 * Math.abs(off);
      setBandwidth(bw);
    };
    const hover = (e) => {
      if (!S.view) return;
      const f = L.xToHz(S.view, wpos(e), 1);
      $("cursor").textContent = L.formatFreq(Math.round(f)).replace(/^0+(?=\d)/, "") + " Hz";
      if (isSpec) {
        S.hover = wpos(e);
        tip.hidden = false; tip.textContent = (f / 1e6).toFixed(4) + " MHz"; tip.style.left = Math.min(Math.max(wpos(e) * 100, 6), 94) + "%";
        wrap.classList.toggle("resize", !!hitEdge(e)); drawSpectrum();
      }
    };
    c.addEventListener("pointerleave", () => { S.hover = null; if (isSpec) { tip.hidden = true; drawSpectrum(); } $("cursor").textContent = ""; });
    c.addEventListener("pointerdown", (e) => {
      c.setPointerCapture(e.pointerId); ptrs.set(e.pointerId, e);
      if (ptrs.size === 1) { startView = S.view; moved = false; startX = e.clientX; edge = hitEdge(e); if (edge) moved = true; }
      else if (ptrs.size === 2) { const [a, b] = [...ptrs.values()]; pinch0 = Math.abs(a.clientX - b.clientX) || 1; pinchView = S.view; moved = true; edge = null; }
    });
    c.addEventListener("pointermove", (e) => {
      if (!ptrs.has(e.pointerId)) { if (e.pointerType !== "touch") hover(e); return; }
      if (!S.view) return;
      ptrs.set(e.pointerId, e);
      if (ptrs.size === 2 && pinchView) {
        const [a, b] = [...ptrs.values()], d = Math.abs(a.clientX - b.clientX) || 1;
        const r = c.getBoundingClientRect(), mid = ((a.clientX + b.clientX) / 2 - r.left) / r.width;
        setView(L.zoomView(pinchView, S.full, pinch0 / d, pinchView.start + mid * (pinchView.stop - pinchView.start)));
      } else if (ptrs.size === 1 && edge) {
        setEdge(e);
      } else if (ptrs.size === 1 && startView) {
        if (!moved && Math.abs(e.clientX - startX) > 6) moved = true;
        if (moved) {
          const w = c.getBoundingClientRect().width, dHz = -((e.clientX - startX) / w) * (startView.stop - startView.start);
          setView(L.panView(startView, S.full, dHz));
        }
      }
    });
    const up = (e) => {
      if (!ptrs.has(e.pointerId)) return;
      const wasMoved = moved; ptrs.delete(e.pointerId); edge = null;
      if (ptrs.size === 0 && !wasMoved && S.view && e.type === "pointerup") {
        const fl = isSpec && S.flags && (e.clientY - c.getBoundingClientRect().top) < 40 ? S.flags.find((q) => Math.abs(q.x - (e.clientX - c.getBoundingClientRect().left)) <= (e.pointerType === "touch" ? 14 : 7)) : null;
        if (fl) gotoMark(fl.m); else setFreq(L.snap(L.xToHz(S.view, wpos(e), 1), S.step));
      }
      if (ptrs.size < 2) pinchView = null;
      if (ptrs.size === 1) { const r = [...ptrs.values()][0]; startView = S.view; startX = r.clientX; moved = true; }
    };
    c.addEventListener("pointerup", up); c.addEventListener("pointercancel", up);
    c.addEventListener("wheel", (e) => {
      if (!S.view) return; e.preventDefault();
      if (Math.abs(e.deltaX) > Math.abs(e.deltaY)) { setView(L.panView(S.view, S.full, (e.deltaX / c.clientWidth) * (S.view.stop - S.view.start))); return; }
      zoom(e.deltaY < 0 ? 0.8 : 1.25, L.xToHz(S.view, wpos(e), 1));
    }, { passive: false });
  }

  // ---------- S-meter, output level, buffer ----------
  let anBuf = null;
  function drawMeter() {
    const now = performance.now();
    if (now - S.peakAt > 1200) S.peak = Math.max(S.level, S.peak - 0.6);
    const m = L.sMeter(S.level), pk = L.sMeter(S.peak);
    $("sfill").style.width = (m.frac * 100).toFixed(1) + "%"; $("speak").style.left = (pk.frac * 100).toFixed(1) + "%";
    $("stext").textContent = m.text; $("sdb").textContent = isFinite(S.level) && S.level > -149 ? S.level.toFixed(0) + " dBFS" : "-- dBFS";
    const sb = $("sbar"); sb.setAttribute("aria-valuenow", String(Math.round(m.frac * 100))); sb.setAttribute("aria-valuetext", m.text);
    const sq = $("sqmark"); sq.hidden = S.sq <= -150; if (!sq.hidden) sq.style.left = (L.sMeter(S.sq).frac * 100).toFixed(1) + "%";
    // output level from an analyser on the audio path
    let lv = 0;
    if (A.an && S.audioOn) {
      if (!anBuf) anBuf = new Float32Array(A.an.fftSize);
      A.an.getFloatTimeDomainData(anBuf); let sum = 0; for (let i = 0; i < anBuf.length; i++) sum += anBuf[i] * anBuf[i];
      lv = L.clamp((20 * Math.log10(Math.sqrt(sum / anBuf.length) + 1e-6) + 54) / 54, 0, 1);
    }
    S.outLvl = Math.max(lv, S.outLvl * 0.9);
    const of = $("outFill"); of.style.width = (S.outLvl * 100).toFixed(0) + "%"; of.className = S.outLvl > 0.92 ? "hot" : "";
    $("outLevel").setAttribute("aria-valuenow", String(Math.round(S.outLvl * 100)));
    const bt = $("bufText");
    if (S.audioOn && S.audioBuf && typeof S.audioBuf.buffered === "number") bt.textContent = "buf " + Math.round(S.audioBuf.buffered * 1000) + " ms" + (S.audioBuf.underruns ? " xr" + S.audioBuf.underruns : "");
    else bt.textContent = "buf --";
    $("sbBuf").textContent = bt.textContent.replace("buf ", "");
    if (now - (S.stAt || 0) > 250) { S.stAt = now; renderStatus(); }
    requestAnimationFrame(drawMeter);
  }

  // ---------- audio ----------
  const A = { ctx: null, node: null, gain: null, player: null, sp: null };
  async function ensureAudio() {
    if (A.ctx) { if (A.ctx.state === "suspended") await A.ctx.resume(); return true; }
    const AC = window.AudioContext || window.webkitAudioContext;
    if (!AC) { banner(t("audio.unsupported")); return false; }
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
    try {
      A.an = A.ctx.createAnalyser(); A.an.fftSize = 1024; A.an.smoothingTimeConstant = 0;
      A.dest = A.ctx.createMediaStreamDestination();
      const src = A.node || A.sp; src.connect(A.an); src.connect(A.dest);
    } catch (e) { A.an = null; A.dest = null; }
    if (A.ctx.state === "suspended") await A.ctx.resume();
    return true;
  }
  // ---- record a clip of what is heard (before volume and mute), saved in this browser only ----
  function recStop() {
    const r = S.rec; if (!r) return; S.rec = null;
    clearInterval(r.timer); try { r.mr.stop(); } catch (e) { /* ignore */ }
    renderRec();
  }
  async function recToggle() {
    if (S.rec) { recStop(); return; }
    if (!window.MediaRecorder) { banner(t("rec.unsupported")); return; }
    if (!S.audioOn) { if (!S.welcome) return; await toggleAudio(); }
    if (!A.dest) { banner(t("rec.na")); return; }
    const types = ["audio/webm;codecs=opus", "audio/mp4", "audio/ogg;codecs=opus", "audio/webm"];
    const mime = types.find((t) => MediaRecorder.isTypeSupported(t)) || "";
    const mr = new MediaRecorder(A.dest.stream, mime ? { mimeType: mime } : undefined), chunks = [];
    const label = L.formatFreq(S.freq).replace(/^0+(?=\d)/, "").replace(/\./g, "") + "Hz-" + S.mode;
    mr.ondataavailable = (e) => { if (e.data && e.data.size) chunks.push(e.data); };
    mr.onstop = () => {
      if (!chunks.length) return;
      const blob = new Blob(chunks, { type: mr.mimeType || mime || "audio/webm" }), ext = /mp4/.test(blob.type) ? "m4a" : /ogg/.test(blob.type) ? "ogg" : "webm";
      const a = document.createElement("a"); a.href = URL.createObjectURL(blob);
      a.download = "onair-" + label + "-" + new Date().toISOString().replace(/[-:]/g, "").slice(0, 15) + "Z." + ext;
      document.body.appendChild(a); a.click(); setTimeout(() => { URL.revokeObjectURL(a.href); a.remove(); }, 2000);
    };
    mr.start(1000);
    S.rec = { mr, t0: Date.now(), timer: setInterval(renderRec, 500) };
    renderRec();
  }
  function renderRec() {
    const b = $("recBtn"), on = !!S.rec; b.setAttribute("aria-pressed", String(on));
    const s = on ? Math.floor((Date.now() - S.rec.t0) / 1000) : 0;
    $("recLbl").textContent = on ? Math.floor(s / 60) + ":" + String(s % 60).padStart(2, "0") : t("rec");
    b.title = on ? "Stop and save the clip (R)" : "Record a clip of what you hear (R)";
  }
  function applyVolume() {
    if (!A.gain) return;
    const v = S.muted ? 0 : Math.pow(S.volume / 100, 2) * 1.5;
    A.gain.gain.setTargetAtTime(v, A.ctx.currentTime, 0.02);
  }
  function sendAudio() { const a = Cfg.audioParams(ST, S.mode); send(L.msg.audio(true, a.codec, a.rate)); }
  function playFrame(f) {
    const f32 = new Float32Array(f.samples.length);
    for (let i = 0; i < f32.length; i++) f32[i] = f.samples[i] / 32768;
    if (A.node) A.node.port.postMessage({ pcm: f32, rate: f.rate }, [f32.buffer]);
    else if (A.player) A.player.push(f32, f.rate);
  }
  function flushAudio() { if (A.node) A.node.port.postMessage({ flush: true }); else if (A.player) A.player.flush(); }
  async function toggleAudio() {
    if (S.audioOn) { S.audioOn = false; send(L.msg.audio(false)); flushAudio(); if (S.rec) recStop(); }
    else { if (!(await ensureAudio())) return; S.audioOn = true; S.audioSeq = -1; sendAudio(); }
    renderAudio();
  }
  function renderAudio() {
    const b = $("audioBtn"); b.setAttribute("aria-pressed", String(S.audioOn)); $("audioLbl").textContent = S.audioOn ? t("audio.stop") : t("audio.listen");
    $("audioIcon").setAttribute("d", S.audioOn ? "M7 6h10v12H7z" : "M7 5l12 7-12 7z");
    const m = $("muteBtn"); m.setAttribute("aria-pressed", String(S.muted)); m.title = S.muted ? "Unmute (M)" : "Mute (M)"; m.setAttribute("aria-label", S.muted ? t("audio.unmute") : t("audio.mute")); { const w = $("spkWave"); if (w) w.style.display = S.muted ? "none" : ""; } renderStatus();
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
  // Who may tune: the host chooses everyone, members, a time-limited control slot or nobody (welcome.control, {"type":"control"}); older servers send nothing
  function renderControl() {
    const b = $("ctrlBtn"); if (!b) return;
    const c = S.control;
    if (!c || c.mode === "everyone") { b.hidden = true; return; }
    b.hidden = false;
    if (c.mode === "slot") {
      b.disabled = !!c.busy;
      b.textContent = c.holder ? "Give back control (" + Math.max(0, c.left_s) + " s left)" : c.busy ? "Someone else is tuning (" + Math.max(0, c.left_s) + " s)" : "Take control (" + c.slot_min + " min)";
      b.onclick = () => send({ type: "control", take: !c.holder });
    } else { b.disabled = true; b.textContent = c.mode === "members" && !c.can_tune ? "Tuning needs the password or an invite" : c.mode === "nobody" ? "Listen only" : "You can tune"; b.onclick = null; }
  }
  function renderInfo() {
    const i = S.info; if (!i) return;
    $("srvName").textContent = i.name || "OnAir Web"; $("srvWhere").textContent = [i.location, i.antenna].filter(Boolean).join(", ");
    document.title = (i.name || "OnAir Web");
    $("usersText").textContent = t("head.listeners", { n: i.users, max: i.max_users });
    $("users").className = "box" + (i.users >= i.max_users ? " full" : ""); renderStatus();
    const n = i.notice || i.legal_notice;
    showNotice(typeof n === "string" && n ? n : null);
    $("footInfo").textContent = i.capture ? "Capture " + fmtMHz(i.capture.centre_hz) + " MHz, " + (i.capture.rate_hz / 1e6).toFixed(2) + " Msps. Level scale is relative to full scale, not calibrated." : "";
  }
  const DEFAULT_NOTICE = "This receiver is run by its host, who is responsible for it. Listening is monitored only by the host. Radio laws differ by country: do not pass on or act on messages that are not meant for the general public, and do not record or share what you hear unless the law where you are allows it.";
  function showNotice(text) {
    if (window.__onairDemo && /[?&]quiet=1\b/.test(location.search)) { $("notice").hidden = true; return; }
    const t = text || DEFAULT_NOTICE; $("noticeText").textContent = t;
    const key = "n" + t.length + ":" + t.slice(0, 24);
    $("notice").hidden = store.get("noticeAck") === key;
    $("noticeOk").onclick = () => { store.set("noticeAck", key); $("notice").hidden = true; };
  }
  function authNeeded() {
    const a = S.info && S.info.auth;
    return (a === "password" && !S.password && !S.invite) || (a === "invite" && !S.invite);
  }
  function showLogin(show) {
    $("login").hidden = !show; const a = S.info && S.info.auth;
    $("loginPwRow").hidden = a !== "password"; $("loginInvRow").hidden = a === "open";
    $("loginName").value = S.name;
    if (show) { $("empty").hidden = true; $("loginGo").focus(); }
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
    if (code === 4001) { S.password = ""; showLogin(true); $("app").hidden = true; $("empty").hidden = true; banner(adv.text); return; }
    if (adv.retry && !S.stopRetry) { S.attempt++; scheduleRetry(code, adv); }
    else banner(S.kickText || adv.text);
  }
  function scheduleRetry(code, adv) {
    adv = adv || L.closeAdvice(code, S.attempt);
    S.retryAt = performance.now() + adv.delayMs;
    const tick = () => {
      const s = Math.max(0, Math.ceil((S.retryAt - performance.now()) / 1000));
      banner(adv.text + (s ? " " + t("retry.in", { s }) : ""));
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
      case "control": S.control = m; renderControl(); return;
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
    S.rows = []; renderFreq(); buildBands(); drawSpectrum(); redrawWaterfall(); requestSpan();
  }
  function onWelcome(m) {
    S.welcome = m; S.attempt = 0; S.kickText = ""; banner("");
    S.ranges = L.readRanges(m.ranges);   // missing: the host set no ranges (older servers never send it)
    S.cap = m.capture; S.view = null; setCapture();
    $("app").hidden = false; $("login").hidden = true; $("empty").hidden = true;
    setConn(m.class, "ok"); if (S.info) renderInfo();
    fetchInfo(); clearInterval(S.infoTimer); S.infoTimer = setInterval(() => { if (S.welcome) fetchInfo(); }, 15000);   // the listener count of the header
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
    S.control = m.control || null; renderControl();
    if (!S.control || S.control.can_tune) send(L.msg.tune({ freq_hz: S.freq, mode: S.mode, low_hz: S.low, high_hz: S.high }));
    send(L.msg.rx(Object.assign({ squelch_db: S.sq }, Cfg.rxFields(ST, S.mode))));
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
    renderMode(); renderFreq(); drawSpectrum();
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
    drawSpectrum();
  }

  // ---------- squelch / volume ----------
  function renderSquelch() { $("sq").value = Math.max(-150, S.sq); $("sqOut").textContent = S.sq <= -150 ? t("open") : S.sq + " dB"; }
  function renderVolume() { $("vol").value = S.volume; $("volOut").textContent = S.volume + " %"; applyVolume(); }

  // ---------- bands and bookmarks ----------
  function buildBands() {
    const box = $("bandList"); box.textContent = ""; box.className = "bandlist";
    for (const b of bands) {
      const slice = S.full ? L.bandSlice({ start: b.s, stop: b.e }, S.bounds) : null;
      const btn = el("button", null, b.n); btn.type = "button"; btn.disabled = !slice || !L.rangeOverlaps(S.ranges, b.s, b.e, 1000);
      btn.dataset.s = b.s; btn.dataset.e = b.e; btn.appendChild(el("span", "f", fmtMHz(b.s) + " - " + fmtMHz(b.e) + " MHz"));
      btn.addEventListener("click", () => {
        const f = L.clamp(b.f, slice.start, slice.stop); setView(slice);
        if (ST.autoMode && S.welcome && S.welcome.demods.includes(b.m)) { S.mode = b.m; const d = L.MODE_DEFAULT[b.m]; S.low = d[0]; S.high = d[1]; renderMode(); sendTune({ mode: b.m }); }
        setFreq(f, true);
      });
      box.appendChild(btn);
    }
    markBands();
  }
  function markBands() { for (const b of $("bandList").children) b.classList.toggle("cur", S.freq >= +b.dataset.s && S.freq <= +b.dataset.e); }
  function marks() { return store.get("marks", []); }
  function renderMarks() {
    const ul = $("markList"); ul.textContent = "";
    const list = marks();
    if (!list.length) ul.appendChild(el("li", "dim", t("marks.none")));
    list.forEach((m, i) => {
      const li = el("li"); const go = el("button", "go", m.name); go.type = "button";
      go.appendChild(el("span", null, L.formatFreq(m.f).replace(/^0+(?=\d)/, "") + " " + m.m.toUpperCase()));
      go.addEventListener("click", () => gotoMark(m));
      const del = el("button", "del", "\u00d7"); del.type = "button"; del.setAttribute("aria-label", t("marks.delete", { n: m.name }));
      del.addEventListener("click", () => { const l = marks(); l.splice(i, 1); store.set("marks", l); renderMarks(); drawSpectrum(); });
      li.append(go, del); ul.appendChild(li);
    });
  }
  function gotoMark(m) {
    if (S.bounds && (m.f < S.bounds.start || m.f > S.bounds.stop)) { banner(t("marks.outside")); return; }
    banner(""); if (m.m !== S.mode && ST.autoMode && S.welcome && S.welcome.demods.includes(m.m)) { S.mode = m.m; S.low = m.lo; S.high = m.hi; renderMode(); sendTune({ mode: m.m, low_hz: m.lo, high_hz: m.hi }); }
    setFreq(m.f, true);
  }
  function exportMarks() {
    const blob = new Blob([JSON.stringify({ onair_web_memories: 1, marks: marks() }, null, 2)], { type: "application/json" }), a = document.createElement("a");
    a.href = URL.createObjectURL(blob); a.download = "onair-memories.json"; document.body.appendChild(a); a.click(); setTimeout(() => { URL.revokeObjectURL(a.href); a.remove(); }, 2000);
  }
  function importMarks(text) {
    let o = null; try { o = JSON.parse(text); } catch (e) { /* bad file */ }
    const list = o && o.onair_web_memories === 1 && Array.isArray(o.marks) ? o.marks : null;
    if (!list) { banner(t("marks.bad")); return; }
    const ok = list.filter((m) => m && typeof m === "object" && isFinite(m.f) && m.f >= 0 && m.f < 1e11 && L.MODES.includes(m.m) && isFinite(m.lo) && isFinite(m.hi))
      .map((m) => ({ name: L.sanitizeText(m.name, 40) || L.formatFreq(m.f).replace(/^0+(?=\d)/, ""), f: Math.round(m.f), m: m.m, lo: Math.round(m.lo), hi: Math.round(m.hi) }));
    const cur = marks(); for (const m of ok) if (!cur.some((c) => c.f === m.f && c.m === m.m && c.name === m.name)) cur.push(m);
    store.set("marks", cur.slice(-200)); renderMarks(); banner("");
  }
  function addMark() {
    const name = L.sanitizeText($("markName").value, 40) || L.formatFreq(S.freq).replace(/^0+(?=\d)/, "");
    const l = marks(); l.push({ name, f: S.freq, m: S.mode, lo: S.low, hi: S.high }); store.set("marks", l.slice(-200));
    $("markName").value = ""; renderMarks(); drawSpectrum();
  }

  // ---------- decoders ----------
  function buildDecoders(list) {
    const box = $("decList"); box.textContent = "";
    if (!list || !list.length) box.appendChild(el("span", "dim", t("dec.none")));
    for (const d of list || []) {
      const b = el("button", null, d.name || d.kind); b.type = "button"; b.dataset.kind = d.kind; b.dataset.needs = d.needs_freq ? "1" : "";
      b.title = d.needs_freq ? t("dec.at") : "";
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
  function dropViews(kind) { mapDirty = true; for (const k of Object.keys(S.views)) if (k === kind || k.startsWith(kind + "@")) { const v = S.views[k]; if (v.node.parentNode) v.node.parentNode.removeChild(v.node); delete S.views[k]; } for (const k of Object.keys(S.decoders)) if (k === kind || k.startsWith(kind + "@")) delete S.decoders[k]; }
  function onEvent(m) {
    if (typeof m.decoder !== "string") return;
    const key = m.decoder.slice(0, 64);
    if (D.validEvent(m.data)) {   // the v1 decoder events: every one goes to its view (pictures need all strips)
      const sub = S.subs.find((x) => key === x.decoder || key.startsWith(x.decoder + "@"));
      if (!sub) return;
      const v = S.views[key] || (S.views[key] = new D.DecoderView(document, key, m.data.kind));
      v.ingest(m.data); S.decDirty = true; mapDirty = true; return;
    }
    S.decoders[key] = m.data; S.decDirty = true;
  }
  function renderDecoders() {
    if (!S.decDirty || $("paneDec").hidden) return; S.decDirty = false;
    const out = $("decOut"); out.textContent = "";
    const subbed = (key) => S.subs.some((s) => key === s.decoder || key.startsWith(s.decoder + "@"));
    const keys = Object.keys(S.views).concat(Object.keys(S.decoders).filter((k) => !S.views[k] && subbed(k)));
    if (!keys.includes(S.sel)) S.sel = keys[0] || "";
    const tabs = $("decTabs"); tabs.textContent = ""; tabs.hidden = keys.length < 2;
    for (const k of keys) {
      const b = el("button", "tab", k.replace(/@(\d+)k$/, "")); b.type = "button"; b.setAttribute("role", "tab"); b.setAttribute("aria-selected", String(k === S.sel));
      b.addEventListener("click", () => { S.sel = k; S.decDirty = true; renderDecoders(); });
      tabs.appendChild(b);
    }
    const key = S.sel;
    if (key && S.views[key]) { const v = S.views[key]; v.refresh(); out.appendChild(v.node); }
    else if (key && S.decoders[key]) {
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
    if (!out.children.length) out.appendChild(el("p", "dim", S.subs.length ? t("dec.wait") : t("dec.pick")));
  }

  // ---------- layout and look from the settings ----------
  function applyLook() {
    const h = document.documentElement, w = window.innerWidth;
    h.classList.toggle("compact", ST.layout === "compact" || (ST.layout === "auto" && w <= 820));
    h.classList.toggle("reduce-motion", !!ST.reduceMotion);
    h.style.setProperty("--ui-scale", String(ST.scale / 100));
    const app = $("app"); app.classList.toggle("no-left", !ST.panelLeft); app.classList.toggle("no-right", !ST.panelRight);
  }

  // ---------- the Settings panel: built from the schema in onair-settings.js ----------
  const LANG_NAMES = { en: "English", lt: "Lietuvių", de: "Deutsch" };
  const SET_ORDER = {
    receiver: ["agc", "gainDb", "deemphUs", "audioCodec", "audioRate", "step", "autoMode"],
    display: ["palette", "rangeMode", "range", "floorAdj", "minDb", "maxDb", "wfFps", "fftBins", "smooth", "peakHold", "showBands", "showMarks", "showGrid"],
    layout: ["theme", "lang", "layout", "panelLeft", "panelRight", "scale", "keys", "reduceMotion"],
  };
  let setTab = "receiver";
  function optLabel(k, v) {
    if (k === "step") return L.stepLabel(v);
    const key = "set." + k + "." + v;
    if (I18N.CAT.en[key]) return t(key);
    if (k === "palette") return t("pal." + v);
    if (k === "theme") return t("theme." + v);
    if (k === "agc") return t("agc." + v);
    if (k === "lang") return v === "auto" ? t("set.lang.auto") : LANG_NAMES[v] || v;
    if (k === "audioRate") return v / 1000 + " kHz";
    if (k === "deemphUs") return v ? v + " µs" : t("deemph.off");
    return String(v);
  }
  function setVisible(k) {
    if (k === "range") return ST.rangeMode !== "manual";
    if (k === "floorAdj") return ST.rangeMode === "server";
    if (k === "minDb" || k === "maxDb") return ST.rangeMode === "manual";
    if (k === "gainDb") return ST.agc === "off";
    return true;
  }
  function renderSettings() {
    const tabs = $("setTabs"); tabs.textContent = "";
    for (const g of Object.keys(SET_ORDER)) {
      const b = el("button", "tab", t("set.tab." + g)); b.type = "button"; b.setAttribute("role", "tab"); b.setAttribute("aria-selected", String(g === setTab));
      b.addEventListener("click", () => { setTab = g; renderSettings(); }); tabs.appendChild(b);
    }
    const body = $("setBody"); body.textContent = "";
    for (const k of SET_ORDER[setTab]) {
      const sc = Cfg.SCHEMA[k]; if (!setVisible(k)) continue;
      const row = el("div", "setrow"), id = "set_" + k, lab = el("label", null, t("set." + k)); lab.htmlFor = id; let inp;
      if (sc.type === "bool") {
        inp = el("input"); inp.type = "checkbox"; inp.checked = !!ST[k]; inp.id = id;
        inp.addEventListener("change", () => setSetting(k, inp.checked));
        row.classList.add("chk"); row.append(inp, lab);
      } else if (sc.type === "enum" || k === "step") {
        inp = el("select"); inp.id = id;
        const vals = k === "step" ? (L.STEPS.includes(ST.step) ? L.STEPS : L.STEPS.concat([ST.step]).sort((a, b) => a - b)) : sc.values;
        for (const v of vals) { const o = el("option", null, optLabel(k, v)); o.value = String(v); inp.appendChild(o); }
        inp.value = String(ST[k]); inp.addEventListener("change", () => setSetting(k, k === "step" || typeof sc.values[0] === "number" ? +inp.value : inp.value));
        row.append(lab, inp);
      } else {
        inp = el("input"); inp.type = "range"; inp.id = id; inp.min = sc.min; inp.max = sc.max; inp.step = 1; inp.value = ST[k];
        const out = el("output", "mono", String(ST[k])); inp.addEventListener("input", () => { out.textContent = inp.value; setSetting(k, +inp.value, true); });
        row.append(lab, out, inp);
      }
      body.appendChild(row);
    }
  }
  // one place that changes a setting: validate, store, and apply it
  function setSetting(k, v, noRebuild) {
    const nv = Cfg.coerce(k, v); ST[k] = nv; saveSettings(); applySetting(k);
    if (!noRebuild && (k === "rangeMode" || k === "agc" || k === "lang")) renderSettings();
  }
  function applySetting(k) {
    switch (k) {
      case "agc": S.agc = ST.agc; send(L.msg.rx({ agc: ST.agc, gain_db: ST.gainDb })); syncControls(); break;
      case "gainDb": send(L.msg.rx({ gain_db: ST.gainDb })); syncControls(); break;
      case "deemphUs": send(L.msg.rx({ deemph_us: ST.deemphUs })); syncControls(); break;
      case "audioCodec": case "audioRate": if (S.audioOn) sendAudio(); break;
      case "step": S.step = ST.step; syncControls(); break;
      case "palette": S.palette = ST.palette; pal = L.makePalette(S.palette); syncControls(); redrawWaterfall(); drawSpectrum(); break;
      case "range": S.range = ST.range; syncControls(); redrawWaterfall(); drawSpectrum(); break;
      case "floorAdj": S.floorAdj = ST.floorAdj; syncControls(); redrawWaterfall(); drawSpectrum(); break;
      case "rangeMode": S.adapt = null; redrawWaterfall(); drawSpectrum(); break;
      case "minDb": case "maxDb": if (ST.maxDb - ST.minDb < 10) { if (k === "minDb") ST.minDb = ST.maxDb - 10; else ST.maxDb = ST.minDb + 10; } redrawWaterfall(); drawSpectrum(); break;
      case "wfFps": case "fftBins": requestSpan(); break;
      case "smooth": case "peakHold": S.trace = null; S.peakDb = null; S.traceRow = null; drawSpectrum(); break;
      case "showBands": case "showMarks": case "showGrid": drawSpectrum(); break;
      case "theme": setTheme(ST.theme === "auto" ? null : ST.theme, true); break;
      case "lang": I18N.setLang(I18N.pickLang(ST.lang, navigator.languages || [navigator.language])); relabel(); break;
      case "layout": case "panelLeft": case "panelRight": case "scale": case "reduceMotion": applyLook(); setTimeout(() => { redrawWaterfall(); drawSpectrum(); }, 50); break;
      default: break;
    }
  }
  // controls in the side panels that mirror a setting
  function syncControls() {
    const set = (id, v) => { const e = $(id); if (e && e.value !== String(v)) e.value = String(v); };
    set("agc", ST.agc); set("gain", ST.gainDb); set("deemph", ST.deemphUs); set("step", ST.step); set("palette", ST.palette); set("range", ST.range); set("floorAdj", ST.floorAdj);
    $("gainOut").textContent = ST.gainDb + " dB";
    const gr = $("gainRow"); if (gr) { gr.classList.toggle("off", ST.agc !== "off"); $("gain").disabled = ST.agc !== "off"; }
  }
  // the language changed: static text from the data-i18n attributes, then the pieces built by code
  function relabel() {
    document.documentElement.lang = I18N.lang(); I18N.apply(document);
    setTheme(ST.theme === "auto" ? null : ST.theme, true); setConn(S.connKey || "offline", $("conn").className.replace("lamp ", "")); renderAudio(); renderRec(); renderMarks(); renderSquelch(); renderStatus();
    if ($("settingsDlg").open) renderSettings();
    if (S.info) renderInfo(); buildDecoders(S.welcome && S.welcome.decoders); renderMap();
  }
  function openSettings() { renderSettings(); const d = $("settingsDlg"); if (!d.open) d.showModal(); }

  // ---------- the map tab (ADS-B aircraft, AIS vessels, APRS stations from the decoders you started) ----------
  let mapView = null, mapDirty = false;
  function collectMarks() {
    const out = [];
    for (const k of Object.keys(S.views)) {
      const v = S.views[k]; if (!v || !v.data) continue;
      try {
        const tb = v.kind === "adsb" ? D.aircraftTable(v.data) : v.kind === "ais" ? D.vesselTable(v.data) : v.kind === "aprs" ? D.aprsTables(v.data) : null;
        if (tb) for (const m of tb.marks) out.push(m);
      } catch (e) { /* a bad event never breaks the page */ }
    }
    return out;
  }
  function renderMap() {
    const pane = $("paneMap"); if (pane.hidden) return;
    if (!mapView) mapView = MAP.create($("mapCanvas"), { onSelect: (m) => { S.mapSel = m ? m.id : ""; renderMapInfo(); } });
    S.mapMarks = collectMarks(); mapView.setMarks(S.mapMarks);
    const n = S.mapMarks.length; $("mapNone").hidden = n > 0; $("mapCount").textContent = n ? t("map.count", { n }) : "";
    renderMapInfo();
  }
  function renderMapInfo() {
    const m = S.mapMarks.find((x) => x.id === S.mapSel), box = $("mapInfo");
    box.textContent = m ? m.label + (m.info ? ", " + m.info : "") + "  " + m.lat.toFixed(4) + ", " + m.lon.toFixed(4) : "";
  }

  // ---------- wiring ----------
  function init() {
    setTheme(S.theme, true);
    buildFreq();
    const stepSel = $("step");
    for (const s of L.STEPS) { const o = el("option", null, L.stepLabel(s)); o.value = String(s); stepSel.appendChild(o); }
    if (!L.STEPS.includes(S.step)) S.step = 5000; stepSel.value = String(S.step);
    stepSel.addEventListener("change", () => { setSetting("step", +stepSel.value, true); });
    const pSel = $("palette");
    for (const n of L.PALETTE_NAMES) { const o = el("option", null, L.PALETTE_LABELS[n] || n); o.value = n; pSel.appendChild(o); }
    pSel.value = S.palette; pSel.addEventListener("change", () => setSetting("palette", pSel.value, true));
    $("range").value = S.range; $("floorAdj").value = S.floorAdj;
    $("range").addEventListener("input", (e) => setSetting("range", +e.target.value, true));
    $("floorAdj").addEventListener("input", (e) => setSetting("floorAdj", +e.target.value, true));
    $("agc").addEventListener("change", (e) => setSetting("agc", e.target.value, true));
    $("gain").addEventListener("input", (e) => setSetting("gainDb", +e.target.value, true));
    $("deemph").addEventListener("change", (e) => setSetting("deemphUs", +e.target.value, true));
    for (const b of $("filterBtns").children) b.addEventListener("click", () => { const r = L.MODE_BW_RANGE[S.mode], d = L.MODE_DEFAULT[S.mode]; setBandwidth(L.clamp(Math.round((d[1] - d[0]) * +b.dataset.w), r[0], r[1])); });
    $("autoRange").addEventListener("click", () => { const row = S.rows[S.rows.length - 1]; if (!row) return; const e = Cfg.autoRange(row); if (!e) return; S.adapt = { min: e.min, max: e.max }; if (ST.rangeMode === "server") ST.rangeMode = "adaptive"; saveSettings(); redrawWaterfall(); drawSpectrum(); });
    $("mSettings").addEventListener("click", openSettings); $("gearBtn").addEventListener("click", openSettings);
    $("setReset").addEventListener("click", () => { ST = Cfg.defaults(); saveSettings(); for (const k of Cfg.KEYS) applySetting(k); renderSettings(); $("setMsg").textContent = ""; });
    $("setExport").addEventListener("click", () => { const blob = new Blob([Cfg.exportJson(ST)], { type: "application/json" }), a = document.createElement("a"); a.href = URL.createObjectURL(blob); a.download = "onair-web-settings.json"; document.body.appendChild(a); a.click(); setTimeout(() => { URL.revokeObjectURL(a.href); a.remove(); }, 2000); });
    $("setImport").addEventListener("click", () => $("setFile").click());
    $("setFile").addEventListener("change", (e) => { const f = e.target.files[0]; if (!f || f.size > 20000) return; f.text().then((tx) => { const o = Cfg.importJson(tx); if (!o) { $("setMsg").textContent = t("set.badfile"); return; } ST = o; saveSettings(); for (const k of Cfg.KEYS) applySetting(k); renderSettings(); $("setMsg").textContent = t("set.imported"); }); e.target.value = ""; });
    $("markExport").addEventListener("click", exportMarks); $("markImport").addEventListener("click", () => $("markFile").click());
    $("markFile").addEventListener("change", (e) => { const f = e.target.files[0]; if (!f || f.size > 200000) return; f.text().then(importMarks); e.target.value = ""; });
    $("mapWide").addEventListener("click", () => { const on = !$("app").classList.contains("mapwide"); $("app").classList.toggle("mapwide", on); $("mapWide").setAttribute("aria-pressed", String(on)); setTimeout(() => { if (mapView) mapView.draw(); redrawWaterfall(); drawSpectrum(); }, 30); });
    $("mapFit").addEventListener("click", () => { if (mapView) mapView.fit(); }); $("mapIn").addEventListener("click", () => { if (mapView) mapView.zoom(1); }); $("mapOut").addEventListener("click", () => { if (mapView) mapView.zoom(-1); });
    I18N.apply(document); applyLook(); syncControls();
    pal = L.makePalette(S.palette);
    $("freqType").addEventListener("click", typeFreq);
    $("bw").addEventListener("input", (e) => setBandwidth(+e.target.value));
    $("sq").addEventListener("input", (e) => { S.sq = +e.target.value; renderSquelch(); persist(); send(L.msg.rx({ squelch_db: S.sq })); });
    $("vol").addEventListener("input", (e) => { S.volume = +e.target.value; store.set("vol", S.volume); renderVolume(); });
    $("audioBtn").addEventListener("click", toggleAudio);
    $("muteBtn").addEventListener("click", () => { S.muted = !S.muted; renderAudio(); applyVolume(); });
    $("zoomIn").addEventListener("click", () => zoom(0.5));
    $("zoomOut").addEventListener("click", () => zoom(2));
    $("zoomFull").addEventListener("click", () => setView({ start: S.full.start, stop: S.full.stop }));
    $("zoomSig").addEventListener("click", () => setView({ start: S.freq - 25000, stop: S.freq + 25000 }));
    $("markAdd").addEventListener("click", addMark);
    $("markName").addEventListener("keydown", (e) => { if (e.key === "Enter") addMark(); });
    for (const t of document.querySelectorAll(".tabs button")) t.addEventListener("click", () => {
      for (const o of document.querySelectorAll(".tabs button")) { const on = o === t; o.setAttribute("aria-selected", String(on)); o.classList.toggle("sel", on); $("pane" + o.dataset.tab[0].toUpperCase() + o.dataset.tab.slice(1)).hidden = !on; }
      S.decDirty = true; renderDecoders(); renderMap();
    });
    attachScope(spec, true); attachScope(wf, false);
    $("spectrum").addEventListener("keydown", (e) => {
      if (e.key === "ArrowLeft") { changeFreq(-S.step * (e.shiftKey ? 10 : 1)); e.preventDefault(); }
      else if (e.key === "ArrowRight") { changeFreq(S.step * (e.shiftKey ? 10 : 1)); e.preventDefault(); }
    });
    document.addEventListener("keydown", (e) => {
      const t = e.target; if (e.ctrlKey || e.metaKey || e.altKey) return;
      if (t && (t.tagName === "INPUT" || t.tagName === "SELECT" || t.tagName === "TEXTAREA")) return;
      if (t && t.tagName === "BUTTON" && (e.key === " " || e.key === "Enter")) return;
      if (document.querySelector("dialog[open]")) return;
      const k = e.key;
      if (k === "?") { $("help").showModal(); e.preventDefault(); return; }
      if (!ST.keys) return;
      if (k === "s" || k === "S") { openSettings(); e.preventDefault(); return; }
      if (k === "a" || k === "A") { $("autoRange").click(); return; }
      if (k === "t" || k === "T") { setTheme((document.documentElement.dataset.theme === "light") ? "dark" : "light"); return; }
      if (!S.welcome) return;
      const own = t && (t.id === "freq" || t.id === "spectrum");
      if (k === " ") { toggleAudio(); e.preventDefault(); }
      else if (k === "m" || k === "M") { S.muted = !S.muted; renderAudio(); applyVolume(); }
      else if (/^[1-6]$/.test(k)) { const m = $("modes").children[+k - 1]; if (m && !m.disabled) setMode(m.dataset.mode); }
      else if (k === "+" || k === "=") zoom(0.5);
      else if (k === "-") zoom(2);
      else if (k === "0") setView({ start: S.full.start, stop: S.full.stop });
      else if (k === "f" || k === "F") { typeFreq(); e.preventDefault(); }
      else if (k === "b" || k === "B") { addMark(); const tab = $("tabMarks"); tab.click(); }
      else if (k === "r" || k === "R") recToggle();
      else if (k === "[" || k === "]") { const i = L.STEPS.indexOf(S.step) + (k === "]" ? 1 : -1); if (i >= 0 && i < L.STEPS.length) { S.step = L.STEPS[i]; $("step").value = String(S.step); store.set("step", S.step); } }
      else if (k === "ArrowLeft" && !own) changeFreq(-S.step * (e.shiftKey ? 10 : 1));
      else if (k === "ArrowRight" && !own) changeFreq(S.step * (e.shiftKey ? 10 : 1));
      else if (k === "PageUp" && !own) changeFreq(S.step * 10);
      else if (k === "PageDown" && !own) changeFreq(-S.step * 10);
    });
    $("recBtn").addEventListener("click", recToggle);
    $("muteChk").addEventListener("change", (e) => { S.muted = e.target.checked; renderAudio(); applyVolume(); });
        $("themeBtn").addEventListener("click", () => setTheme(document.documentElement.dataset.theme === "light" ? "dark" : "light"));
    $("helpBtn").addEventListener("click", () => $("help").showModal());
    // menus (View, Help): small pop-ups like the app's menu bar
    const menus = [["mView", "popView"], ["mHelp", "popHelp"]];
    const closeMenus = () => { for (const [b, p] of menus) { $(p).hidden = true; $(b).setAttribute("aria-expanded", "false"); } };
    for (const [b, p] of menus) $(b).addEventListener("click", (e) => { const open = $(p).hidden; closeMenus(); $(p).hidden = !open; $(b).setAttribute("aria-expanded", String(open)); e.stopPropagation(); });
    document.addEventListener("click", closeMenus);
    document.addEventListener("keydown", (e) => { if (e.key === "Escape") closeMenus(); });
    $("mFull").addEventListener("click", () => { if (S.full) setView({ start: S.full.start, stop: S.full.stop }); });
    $("zoomSlider").addEventListener("input", (e) => {
      if (!S.view || !S.full) return; const full = S.full.stop - S.full.start, mn = L.MIN_SPAN || 1000, sp = full * Math.pow(mn / full, +e.target.value / 100), c = (S.view.start + S.view.stop) / 2;
      setView({ start: c - sp / 2, stop: c + sp / 2 });
    });
    $("typeForm").addEventListener("submit", (e) => {
      const hz = L.parseFreq($("typeIn").value);
      if (isNaN(hz)) { e.preventDefault(); $("typeMsg").textContent = t("type.bad"); return; }
      banner(""); setFreq(hz, true);
    });
    $("typeCancel").addEventListener("click", () => $("typeDlg").close());
    $("login").addEventListener("submit", (e) => {
      e.preventDefault(); S.name = L.sanitizeText($("loginName").value, 32); store.set("name", S.name);
      S.password = $("loginPw").value; if ($("loginInv").value.trim()) S.invite = $("loginInv").value.trim();
      $("loginPw").value = ""; $("login").hidden = true; banner(""); connect();
    });
    window.addEventListener("resize", schedule(() => { applyLook(); redrawWaterfall(); drawSpectrum(); requestSpan(); if (mapView) mapView.draw(); }, 150));
    document.addEventListener("visibilitychange", () => { if (!document.hidden && S.ws && S.ws.readyState > 1 && !S.stopRetry) { clearTimeout(S.retryTimer); fetchInfo().then(connect); } });
    renderVolume(); renderAudio(); renderMarks(); renderSquelch(); renderFreq(); drawMeter();
    setInterval(renderDecoders, 500); setInterval(() => { if (mapDirty) { mapDirty = false; renderMap(); } }, 1000);
    setInterval(() => { if (S.info || S.welcome === null) fetchInfo(); }, 10000);

    // an invite arrives as #invite=TOKEN (kept out of the server's logs); remove it from the address bar
    const h = new URLSearchParams(location.hash.replace(/^#/, ""));
    if (h.get("invite")) { S.invite = h.get("invite").slice(0, 128); try { history.replaceState(null, "", location.pathname + location.search); } catch (e) { /* ignore */ } }
    fetchInfo().then((info) => {
      if (S.notServer) { setConn("no receiver", "bad"); emptyState("", true, "No OnAir receiver at this address"); banner("There is no OnAir receiver behind this address. OnAir Web is served by a receiver you run yourself: start onair-server with --web --web-lan (see docs/ONAIR-WEB.md), then open http://<that computer>:8073 from a browser on your network. Nothing is received on this page."); return; }
      if (info && authNeeded()) { showLogin(true); setConn("sign in", "warn"); } else connect();
    });
  }
  // ?demo=1 runs the page against a built-in simulated receiver (synthetic spectrum, audio and decoder lines); it never opens a network connection
  if (/[?&]demo=1\b/.test(location.search)) {
    const sc = document.createElement("script"); sc.src = "onair-demo.js"; sc.onload = init; sc.onerror = init; document.head.appendChild(sc);
  } else init();
})();
