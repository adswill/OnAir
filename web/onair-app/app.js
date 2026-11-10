/* OnAir Web 3.9: the page. State, the API (fetch with the CSRF header), one WebSocket (status, panel, spectrum, audio), the mode picker,
 * the settings page, login and first-time setup, keyboard shortcuts and the phone layout (tabs). */
(function () {
  "use strict";
  const L = window.OnAirLib;
  const $ = (id) => document.getElementById(id);
  const S = {
    info: null, session: null, csrf: "", perms: { control: false, owner: false, role: "none" },
    status: null, modes: null, schemas: null, ws: null, wsOk: false, retry: 1000,
    spec: null, panel: null, audio: null, listening: false, advanced: localStorage.getItem("onair.advanced") === "1",
    bins: 1024, fps: 10, panelMs: 500, arate: 16000, tuneHz: 0, lastPanelSeq: -1, lastRow: null, schemaStd: -1, recAudio: false,
  };

  // ---- helpers
  function toast(msg, bad) { const t = $("toast"); t.textContent = msg; t.className = "toast" + (bad ? " bad" : ""); t.hidden = false; clearTimeout(toast.t); toast.t = setTimeout(() => { t.hidden = true; }, bad ? 5000 : 2500); }
  async function api(path, body) {
    const o = { headers: {}, credentials: "same-origin" };
    if (body !== undefined) { o.method = "POST"; o.headers["Content-Type"] = "application/json"; o.headers["X-OnAir-CSRF"] = S.csrf; o.body = JSON.stringify(body); }
    const r = await fetch("/app/v1/" + path, o);
    let j = null;
    try { j = await r.json(); } catch (e) { j = {}; }
    if (!r.ok) { const e = new Error(j.error || ("HTTP " + r.status)); e.status = r.status; e.body = j; throw e; }
    return j;
  }
  function modal(title, bodyEl) { $("modalTitle").textContent = title; const b = $("modalBody"); b.textContent = ""; b.appendChild(bodyEl); $("modal").hidden = false; }
  function closeModal() { $("modal").hidden = true; }
  const el = (tag, cls, text) => { const e = document.createElement(tag); if (cls) e.className = cls; if (text !== undefined) e.textContent = text; return e; };

  // ---- session and login
  async function loadSession() {
    const s = await api("session");
    S.session = s; S.csrf = s.csrf || "";
    S.info = await api("info");
    S.perms = { role: s.role, owner: s.role === "owner", control: s.role === "owner" || (s.role === "viewer" && S.info.publicTune) };
    $("srvname").textContent = S.info.name || "OnAir";
    document.title = (S.info.name || "OnAir") + " - OnAir";
    $("loginbtn").textContent = s.loggedIn ? "Log out" : s.role === "owner" ? "Owner (LAN)" : "Log in";
    $("loginbtn").disabled = s.role === "owner" && !s.loggedIn && S.info.hasPassword;
    $("settingsbtn").hidden = !S.perms.owner;
    $("recbtn").hidden = !S.perms.owner;
    $("recCard").hidden = !S.perms.owner;
    const ro = !S.perms.control;
    for (const id of ["modebtn", "startbtn", "gain", "autogain", "demod", "rate", "bw", "dev", "autostart"]) $(id).disabled = ro;
    if (s.role === "none") showLogin(s.needs);
    else if (s.role === "owner" && !S.info.hasPassword && s.implicit && !sessionStorage.getItem("onair.setupLater")) showSetup();
    return s;
  }
  function showLogin(needs) {
    const box = el("div", "login");
    if (needs === "setup") {
      box.appendChild(el("p", "", "This server has no password yet. Its owner sets one from a computer on the same network (or starts the server with --app-password). Until then, nobody from the internet can log in."));
    } else {
      box.appendChild(el("p", "muted", "Log in as the owner to use this receiver."));
      const pw = el("input"); pw.type = "password"; pw.placeholder = "Password"; pw.autocomplete = "current-password";
      const btn = el("button", "p", "Log in");
      const err = el("div", "err");
      const go = async () => { try { btn.disabled = true; await api("login", { password: pw.value }); closeModal(); await loadSession(); connect(); toast("Logged in"); } catch (e) { err.textContent = e.message; } finally { btn.disabled = false; } };
      btn.onclick = go; pw.onkeydown = (ev) => { if (ev.key === "Enter") go(); };
      box.append(pw, btn, err);
      setTimeout(() => pw.focus(), 50);
    }
    if (S.info && S.info.public) { const w = el("button", "", "Just watch"); w.onclick = () => closeModal(); box.appendChild(w); }
    modal("Log in", box);
  }
  function showSetup() {
    const box = el("div", "login");
    box.appendChild(el("p", "", "You are the owner (same network). Set a password so you can use this receiver from anywhere, through the relay or a forwarded port. 8 characters or more."));
    const pw = el("input"); pw.type = "password"; pw.placeholder = "New password"; pw.autocomplete = "new-password";
    const pw2 = el("input"); pw2.type = "password"; pw2.placeholder = "Again"; pw2.autocomplete = "new-password";
    const btn = el("button", "p", "Set the password"), later = el("button", "", "Later"), err = el("div", "err");
    btn.onclick = async () => { if (pw.value !== pw2.value) { err.textContent = "the two passwords differ"; return; } try { await api("setup", { password: pw.value }); closeModal(); await loadSession(); toast("Password set"); } catch (e) { err.textContent = e.message; } };
    later.onclick = () => { sessionStorage.setItem("onair.setupLater", "1"); closeModal(); };
    box.append(pw, pw2, btn, later, err);
    modal("First start: set a password", box);
  }
  $("loginbtn").onclick = async () => {
    if (S.session && S.session.loggedIn) { await api("logout", {}); await loadSession(); connect(); toast("Logged out"); }
    else showLogin(S.session ? S.session.needs : "login");
  };

  // ---- the WebSocket
  function connect() {
    if (S.ws) { try { S.ws.onclose = null; S.ws.close(); } catch (e) {} S.ws = null; }
    const url = (location.protocol === "https:" ? "wss://" : "ws://") + location.host + "/app/v1/ws?csrf=" + encodeURIComponent(S.csrf);
    const ws = new WebSocket(url);
    ws.binaryType = "arraybuffer";
    S.ws = ws;
    ws.onopen = () => { S.wsOk = true; S.retry = 1000; sub(); setState("wait", "connected"); };
    ws.onclose = (ev) => {
      S.wsOk = false; setState("", "disconnected - retrying");
      if (ev.code === 4001) { setState("", "login needed"); return; }
      if (ev.code === 4029) { setState("", "server busy (too many visitors)"); toast("The server has as many visitors as it allows. Try again later.", true); }
      setTimeout(connect, S.retry); S.retry = Math.min(15000, S.retry * 1.5);
    };
    ws.onmessage = (m) => {
      if (typeof m.data === "string") { onText(JSON.parse(m.data)); return; }
      const d = new DataView(m.data);
      const t = d.getUint8(0);
      if (t === 1) { const f = L.decodeSpectrum(m.data); if (f) { S.spec.push(f); $("specinfo").textContent = (f.rateHz / 1e6).toFixed(3) + " Msps, " + f.db.length + " bins"; } }
      else if (t === 2 && S.audio) { const a = L.decodeAudio(m.data); if (a) S.audio.push(a.samples, a.rate); }
    };
  }
  function send(o) { if (S.ws && S.ws.readyState === 1) S.ws.send(JSON.stringify(o)); }
  function sub() { send({ type: "sub", panel: true, status: true, spectrum: $("specon").checked, bins: S.bins, fps: S.fps, panelMs: S.panelMs, audio: S.listening, rate: S.arate }); }
  function setState(cls, text) { $("dot").className = "dot " + cls; $("state").textContent = text; }
  function onText(j) {
    if (j.type === "hello") { S.perms = L.permissions(j); return; }
    if (j.type === "status") { onStatus(j.data); return; }
    if (j.type === "panel") { onPanel(j); return; }
    if (j.type === "error") { if (j.code === "busy" || j.code === "login") return; toast(j.message || j.code, true); return; }
  }

  // ---- status
  function onStatus(s) {
    S.status = s;
    const row = s.row;
    setState(s.running ? "ok" : s.wanted ? "wait" : "", s.running ? "receiving" + (s.device ? " - " + s.device : "") : s.wanted ? "waiting for the radio" : "stopped");
    $("startbtn").textContent = s.running || s.wanted ? "Stop" : "Start";
    $("startbtn").className = s.running || s.wanted ? "" : "p";
    if (isFinite(s.frequencyHz) && !S.editingFreq) { S.tuneHz = s.frequencyHz; $("freq").textContent = L.formatMhz(s.frequencyHz); S.spec.setTune(s.frequencyHz); }
    $("modename").textContent = row ? row.name + (row.sub ? "" : "") : (s.mode || "Mode");
    if (!S.focus("gain")) $("gain").value = Math.round(s.gainDb);
    if (!S.focus("rate")) $("rate").value = s.sampleRateHz ? +(s.sampleRateHz / 1e6).toFixed(3) : 0;
    if (s.server) { if (!S.focus("demod")) $("demod").value = s.server.demod || "auto"; $("autostart").checked = !!s.server.autostart; }
    if (s.error) $("ctlerr").textContent = s.error; else $("ctlerr").textContent = "";
    const t = [];
    if (s.running) t.push((s.sampleRateHz / 1e6).toFixed(3) + " Msps", "load " + s.loadPct + " %", "dropped " + s.droppedSamples);
    if (s.app) t.push(s.app.viewers + " visitor" + (s.app.viewers === 1 ? "" : "s"));
    $("about").textContent = t.join(" - ") || "stopped";
    const key = row ? row.family + "/" + row.sub : "";
    if (key !== S.lastRow) { S.lastRow = key; S.panel.clear(); S.lastPanelSeq = -1; $("panelTitle").textContent = row ? row.name : "Decoder"; renderQuick(row); }
  }
  S.focus = (id) => document.activeElement === $(id);

  // ---- the panel
  function onPanel(j) {
    if (j.private) { S.panel.render([], "This mode is private on this server (the owner can allow it for visitors)."); $("panelState").textContent = ""; return; }
    const d = j.data;
    if (!d || d === null) { S.panel.render([], S.status && S.status.running ? "Waiting for the first report..." : "The receiver is stopped. Press Start."); return; }
    const schema = S.schemas ? S.schemas[String(d.standard)] : null;
    const c = d.common || {};
    $("panelState").textContent = c.standardName ? c.standardName + (c.locked ? " - locked" : c.state === 1 ? " - converging" : " - searching") : "";
    const widgets = L.buildPanel(schema || [], d.fields, c, S.advanced);
    if (!schema && d.common && d.common.details) widgets.push({ kind: "obj", name: "details", label: "Details", value: d.common.details, tier: "s" });
    S.panel.render(widgets, "No fields reported yet.");
  }
  $("advanced").checked = S.advanced;
  $("advanced").onchange = () => { S.advanced = $("advanced").checked; localStorage.setItem("onair.advanced", S.advanced ? "1" : "0"); };

  // ---- quick controls under the spectrum: the mode's sub-modes (siblings in the same family)
  function renderQuick(row) {
    const q = $("quick"); q.textContent = "";
    if (!row || !S.modes) return;
    const sibs = S.modes.modes.filter(m => m.family === row.family && m.sub);
    if (sibs.length < 2) return;
    for (const m of sibs) { const b = el("button", m.sub === row.sub ? "on" : "", m.name); b.disabled = !S.perms.control; b.onclick = () => setMode(m); q.appendChild(b); }
  }

  // ---- tuning
  async function tune(hz, live) {
    if (!S.perms.control || !isFinite(hz)) return;
    hz = Math.round(hz);
    S.tuneHz = hz; $("freq").textContent = L.formatMhz(hz); S.spec.setTune(hz);
    if (live) { send({ type: "tune", hz }); return; }
    try { await api("frequency", { hz }); } catch (e) { toast(e.message, true); }
  }
  $("freqbox").onclick = () => {
    if (!S.perms.control) return;
    const inp = el("input"); inp.value = L.formatMhz(S.tuneHz); inp.style.width = "160px"; inp.inputMode = "decimal";
    const box = $("freqbox"); const old = $("freq");
    S.editingFreq = true;
    box.replaceChild(inp, old); inp.focus(); inp.select();
    const done = (ok) => { S.editingFreq = false; box.replaceChild(old, inp); if (ok) { const hz = L.parseFreq(inp.value); if (isFinite(hz)) tune(hz, false); else toast("Type a frequency like 100.5, 145800k or 7074000", true); } };
    inp.onkeydown = (ev) => { if (ev.key === "Enter") done(true); else if (ev.key === "Escape") done(false); };
    inp.onblur = () => done(false);
  };
  document.addEventListener("keydown", (ev) => {
    if (ev.target.tagName === "INPUT" || ev.target.tagName === "SELECT" || ev.target.tagName === "TEXTAREA" || !$("modal").hidden) return;
    const f = S.spec.frame, step = f ? L.tuneStep(f.rateHz) : 1000;
    if (ev.key === "ArrowLeft") tune(S.tuneHz - step * (ev.shiftKey ? 10 : 1), false);
    else if (ev.key === "ArrowRight") tune(S.tuneHz + step * (ev.shiftKey ? 10 : 1), false);
    else if (ev.key === "m" || ev.key === "M") $("modebtn").click();
    else if (ev.key === "s" || ev.key === "S") $("startbtn").click();
    else if (ev.key === "l" || ev.key === "L") $("listenbtn").click();
    else if (ev.key === "f" || ev.key === "F") $("freqbox").click();
    else if (ev.key === "a" || ev.key === "A") $("advanced").click();
  });

  // ---- modes
  async function loadModes() {
    S.modes = await api("modes");
    S.schemas = await api("schema");
  }
  function showModes() {
    if (!S.modes) return;
    const box = el("div");
    const search = el("input", "search"); search.placeholder = "Search modes: name, band, what it decodes..."; search.type = "search";
    const list = el("div", "modes");
    const cur = S.status && S.status.row ? S.status.row.family + "/" + S.status.row.sub : "";
    const draw = () => {
      list.textContent = "";
      const q = search.value;
      const found = L.modeSearch(S.modes.modes, S.modes.groups, q);
      let lastG = -1;
      for (const m of (q ? found : S.modes.modes)) {
        if (!q && m.group !== lastG) { lastG = m.group; list.appendChild(el("div", "grp", S.modes.groups[m.group] || "")); }
        const b = el("button", (m.family + "/" + m.sub === cur ? "cur" : "") + (m.schema ? "" : " noschema"));
        b.append(el("span", "", m.name), el("small", "", m.blurb || ""));
        b.title = m.blurb || "";
        b.onclick = () => { closeModal(); setMode(m); };
        list.appendChild(b);
      }
      if (!list.children.length) list.appendChild(el("div", "empty", "No mode matches."));
    };
    search.oninput = draw;
    search.onkeydown = (ev) => { if (ev.key === "Enter") { const first = list.querySelector("button"); if (first) first.click(); } };
    box.append(search, list);
    draw();
    modal("Choose a mode (" + S.modes.modes.length + ")", box);
    setTimeout(() => search.focus(), 50);
  }
  async function setMode(m) {
    try { await api("mode", { family: m.family, sub: m.sub || "" }); toast(m.name); } catch (e) { toast(e.message, true); }
  }
  $("modebtn").onclick = showModes;

  // ---- radio controls
  $("startbtn").onclick = async () => {
    try {
      if (S.status && (S.status.running || S.status.wanted)) await api("stop", {});
      else await api("start", { device: $("dev").value === "" ? -1 : Number($("dev").value) });
    } catch (e) { toast(e.message, true); }
  };
  $("gain").onchange = () => api("gain", { db: Number($("gain").value) }).catch(e => toast(e.message, true));
  $("autogain").onchange = () => toast($("autogain").checked ? "Automatic gain is set in the desktop app; the server keeps a fixed gain" : "Fixed gain");
  $("demod").onchange = () => api("demod", { demod: $("demod").value }).catch(e => toast(e.message, true));
  $("autostart").onchange = () => api("autostart", { on: $("autostart").checked }).catch(e => toast(e.message, true));
  $("rate").onchange = () => api("samplerate", { hz: Number($("rate").value) * 1e6 }).catch(e => toast(e.message, true));
  $("bw").onchange = () => api("bandwidth", { hz: Number($("bw").value) * 1e6 }).catch(e => toast(e.message, true));
  $("dev").onchange = () => { if (S.status && S.status.running) api("start", { device: Number($("dev").value) }).catch(e => toast(e.message, true)); };
  $("fps").onchange = () => { S.fps = Number($("fps").value); sub(); };
  $("bins").onchange = () => { S.bins = Number($("bins").value); sub(); };
  $("specon").onchange = () => { sub(); if (!$("specon").checked) S.spec.clear(); };
  $("panelrate").onchange = () => { S.panelMs = Math.round(1000 / Number($("panelrate").value)); sub(); };
  $("arate").onchange = () => { S.arate = Number($("arate").value); if (S.listening) { send({ type: "sub", audio: false }); sub(); } };
  async function loadDevices() {
    try {
      const d = await api("devices");
      const s = $("dev"); s.textContent = "";
      for (const x of d) { const o = el("option", "", x.name + (x.kind ? " (" + x.kind + ")" : "")); o.value = x.index; if (x.selected) o.selected = true; s.appendChild(o); }
    } catch (e) {}
  }

  // ---- audio
  $("listenbtn").onclick = async () => {
    if (!S.audio) S.audio = new window.AudioOut();
    S.listening = !S.listening;
    if (S.listening) { try { await S.audio.start(); S.audio.setGain(Number($("vol").value) / 100); } catch (e) { toast("Audio could not start: " + e.message, true); S.listening = false; } }
    else { S.audio.stop(); }
    $("listenbtn").className = S.listening ? "on" : "";
    $("listenbtn").textContent = S.listening ? "Mute" : "Listen";
    sub();
  };
  $("vol").oninput = () => { if (S.audio) S.audio.setGain(Number($("vol").value) / 100); };
  $("audiorec").onclick = () => {
    if (!S.audio || !S.listening) { toast("Press Listen first", true); return; }
    if (!S.recAudio) { S.audio.recordStart(); S.recAudio = true; $("audiorec").textContent = "Stop and save audio"; $("audiorec").className = "on"; return; }
    const blob = S.audio.recordStop(); S.recAudio = false; $("audiorec").textContent = "Save audio in the browser"; $("audiorec").className = "";
    if (!blob) { toast("nothing recorded", true); return; }
    const a = el("a"); a.href = URL.createObjectURL(blob); a.download = "onair-" + L.formatMhz(S.tuneHz) + "MHz-" + new Date().toISOString().replace(/[:.]/g, "-") + ".wav"; a.click();
    setTimeout(() => URL.revokeObjectURL(a.href), 10000);
  };
  // ---- IQ recording on the server (owner)
  async function toggleRec() {
    try {
      if (S.recording) { await api("record/stop", {}); S.recording = false; toast("Recording stopped"); }
      else { await api("record/start", { kind: "iq" }); S.recording = true; toast("Recording IQ on the server"); }
    } catch (e) { toast(e.message, true); }
    $("recbtn").className = $("recbtn2").className = S.recording ? "on" : "";
    $("recinfo").textContent = S.recording ? "recording on the server" : "";
  }
  $("recbtn").onclick = $("recbtn2").onclick = toggleRec;

  // ---- settings (owner)
  async function showSettings() {
    let m;
    try { m = await api("settings"); } catch (e) { toast(e.message, true); return; }
    const box = el("div", "settings");
    const pw = el("div", "it");
    pw.append(el("div", "", m.hasPassword ? "Owner password" : "Owner password (none yet)"));
    const pwb = el("button", "", m.hasPassword ? "Change the password" : "Set a password"); pwb.onclick = showChangePassword;
    pw.append(pwb, el("div", "h", "Needed to log in from the internet (through the relay or a forwarded port). Stored hashed; the plain password is kept nowhere."));
    box.appendChild(el("h3", "", "Access")); box.appendChild(pw);
    const inputs = {};
    for (const g of L.groupSettings(m.items, m.groups)) {
      if (g.group !== "Access") box.appendChild(el("h3", "", g.group));
      for (const it of g.items) {
        const row = el("div", "it");
        row.appendChild(el("div", "", it.label));
        let inp;
        if (it.kind === "bool") { inp = el("input"); inp.type = "checkbox"; inp.checked = it.value === "1" || it.value === "true"; }
        else if (it.kind === "int") { inp = el("input"); inp.type = "number"; inp.value = it.value; }
        else if (it.kind.startsWith("choice:")) { inp = el("select"); for (const c of it.kind.slice(7).split("|")) { const o = el("option", "", c); o.value = c; if (c === it.value) o.selected = true; inp.appendChild(o); } }
        else { inp = el("input"); inp.type = "text"; inp.value = it.value; }
        inputs[it.key] = { inp, it };
        row.appendChild(inp);
        row.appendChild(el("div", "h", it.help + (it.restart ? " " : "")));
        if (it.restart) row.appendChild(el("div", "r", "Takes effect when the server is restarted."));
        box.appendChild(row);
      }
    }
    const save = el("button", "p", "Save"), err = el("div", "err");
    save.onclick = async () => {
      const body = {};
      for (const k in inputs) {
        const { inp, it } = inputs[k];
        const raw = it.kind === "bool" ? inp.checked : inp.value;
        const v = L.settingValue(it.kind, raw);
        const cur = it.kind === "bool" ? (it.value === "1" || it.value === "true") : it.kind === "int" ? Number(it.value) : it.value;
        if (v !== cur) body[k] = v;
      }
      if (!Object.keys(body).length) { closeModal(); return; }
      try { const r = await api("settings", body); closeModal(); toast("Saved: " + r.changed.join(", ")); await loadSession(); } catch (e) { err.textContent = e.message; }
    };
    box.append(save, err);
    modal("Settings", box);
  }
  function showChangePassword() {
    const box = el("div", "login");
    const old = el("input"); old.type = "password"; old.placeholder = "Current password (if you logged in with one)"; old.autocomplete = "current-password";
    const pw = el("input"); pw.type = "password"; pw.placeholder = "New password (8 or more)"; pw.autocomplete = "new-password";
    const pw2 = el("input"); pw2.type = "password"; pw2.placeholder = "Again"; pw2.autocomplete = "new-password";
    const btn = el("button", "p", "Change"), err = el("div", "err");
    btn.onclick = async () => {
      if (pw.value !== pw2.value) { err.textContent = "the two passwords differ"; return; }
      try {
        const hasPw = S.info && S.info.hasPassword;
        await api(hasPw ? "password" : "setup", hasPw ? { old: old.value, password: pw.value } : { password: pw.value });
        closeModal(); toast("Password changed; log in again where needed"); await loadSession();
      } catch (e) { err.textContent = e.message; }
    };
    box.append(old, pw, pw2, btn, err);
    modal("Owner password", box);
  }
  $("settingsbtn").onclick = showSettings;
  $("modalClose").onclick = closeModal;
  $("modal").addEventListener("click", (ev) => { if (ev.target === $("modal")) closeModal(); });
  document.addEventListener("keydown", (ev) => { if (ev.key === "Escape" && !$("modal").hidden) closeModal(); });

  // ---- phone tabs
  for (const b of document.querySelectorAll("#tabs button")) b.onclick = () => {
    for (const x of document.querySelectorAll("#tabs button")) x.classList.toggle("on", x === b);
    for (const s of document.querySelectorAll("main .tab")) s.classList.toggle("on", s.id === b.dataset.tab + "Sec");
  };

  // ---- start
  async function main() {
    S.spec = new window.SpectrumView($("spec"), $("wf"), $("tuneline"), (hz, live) => tune(hz, live));
    S.panel = new window.PanelView($("panel"));
    try { await loadSession(); } catch (e) { setState("", "server unreachable"); setTimeout(main, 3000); return; }
    if (S.session.role !== "none") {
      try { await loadModes(); await loadDevices(); } catch (e) { toast(e.message, true); }
      connect();
    }
  }
  main();
})();
