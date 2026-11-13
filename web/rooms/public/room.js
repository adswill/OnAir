// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Room viewer: waterfall canvas, decoded text, chat. Every string from the server goes in through textContent.
const id = location.pathname.split("/").pop();
const $ = (x) => document.getElementById(x);
const BINS = 512, H = 240;
const cv = $("wf"), g = cv.getContext("2d");
const lut = new Uint32Array(256);
for (let i = 0; i < 256; i++) {   // dark blue -> cyan -> yellow -> white
  const v = i / 255, r = Math.min(255, Math.max(0, (v - 0.5) * 510)), gg = Math.min(255, Math.max(0, (v - 0.15) * 300)), b = Math.min(255, 60 + v * 400 - Math.max(0, (v - 0.5) * 700));
  lut[i] = (255 << 24) | (Math.max(0, Math.min(255, b)) << 16) | (gg << 8) | r;
}
let img = g.createImageData(BINS, H), px = new Uint32Array(img.data.buffer);
function addLines(buf) {
  const n = buf[1];
  for (let l = 0; l < n; l++) {
    px.copyWithin(BINS, 0, BINS * (H - 1));   // scroll down one row, newest on top
    for (let i = 0; i < BINS; i++) px[i] = lut[buf[2 + l * BINS + i]];
  }
  g.putImageData(img, 0, 0);
}
let hostToken = "";
async function mod(action, vid) {
  const r = await fetch("/api/r/" + encodeURIComponent(id) + "/mod", { method: "POST", headers: { Authorization: "Bearer " + hostToken }, body: JSON.stringify({ action, vid }) });
  $("hostnote").textContent = r.ok ? action + " done" : "failed (" + r.status + ")";
}
function addItem(ul, parts, seq) {
  const li = document.createElement("li");
  if (parts.nick != null) { const n = document.createElement("span"); n.className = "n"; n.textContent = parts.nick + ": "; li.append(n); }
  li.append(document.createTextNode(parts.text));
  if (seq != null) { const b = document.createElement("button"); b.className = "rep"; b.textContent = "report"; b.onclick = () => { send({ t: "report", seq }); b.disabled = true; }; li.append(b); }
  if (hostToken && parts.id) for (const a of ["mute", "kick", "ban"]) { const b = document.createElement("button"); b.className = "rep"; b.textContent = a; b.onclick = () => mod(a, parts.id); li.append(b); }
  ul.append(li);
  while (ul.children.length > 200) ul.firstChild.remove();
  ul.scrollTop = ul.scrollHeight;
}
function meta(m) {
  $("title").textContent = m.title;
  document.title = m.title + " - OnAir Live room";
  $("sub").textContent = [m.mode, m.freq ? m.freq + " MHz" : "", m.transcript ? "" : "decoded text not shared"].filter(Boolean).join("  |  ");
  $("viewers").textContent = m.viewers != null ? m.viewers + " watching" : "";
}
let ws, tries = 0;
const send = (o) => { if (ws && ws.readyState === 1) ws.send(JSON.stringify(o)); };
const store = { get: (k) => { try { return sessionStorage.getItem(k) || ""; } catch { return ""; } }, set: (k, v) => { try { sessionStorage.setItem(k, v); } catch {} } };
$("nick").value = store.get("nick");
function connect() {
  ws = new WebSocket((location.protocol === "https:" ? "wss://" : "ws://") + location.host + "/api/r/" + encodeURIComponent(id) + "/ws");
  ws.binaryType = "arraybuffer";
  ws.onopen = () => { tries = 0; send({ t: "hello", nick: $("nick").value, vt: store.get("vt-" + id) }); };
  ws.onmessage = (e) => {
    if (typeof e.data !== "string") { const b = new Uint8Array(e.data); if (b[0] === 1) addLines(b); return; }
    let m; try { m = JSON.parse(e.data); } catch { return; }
    if (m.t === "welcome") {
      store.set("vt-" + id, m.vt); meta(m.meta);
      $("chat").replaceChildren(); $("texts").replaceChildren();
      for (const c of m.chat) addItem($("chat"), c, c.seq);
      for (const t of m.texts) addItem($("texts"), { text: t });
      if (m.muted) $("note").textContent = "You are muted in this room.";
    } else if (m.t === "chat") addItem($("chat"), m, m.seq);
    else if (m.t === "texts") for (const t of m.items) addItem($("texts"), { text: t });
    else if (m.t === "meta") meta(m);
    else if (m.t === "v") $("viewers").textContent = m.n + " watching";
    else if (m.t === "muted") $("note").textContent = "You are muted in this room.";
    else if (m.t === "slow") $("note").textContent = "Slow down a little.";
    else if (m.t === "reported") $("note").textContent = "Thanks, the host has been told.";
    else if (m.t === "kicked") { $("note").textContent = "You were removed from this room."; tries = 99; }
    else if (m.t === "end") { $("note").textContent = "This room has ended."; tries = 99; }
  };
  ws.onclose = () => { if (tries < 6) setTimeout(connect, Math.min(15000, 1000 * 2 ** tries++)); else $("title").textContent = $("title").textContent; };
}
$("form").onsubmit = (e) => {
  e.preventDefault();
  const t = $("msg").value.trim(); if (!t) return;
  store.set("nick", $("nick").value);
  send({ t: "nick", nick: $("nick").value });
  send({ t: "chat", text: t }); $("msg").value = ""; $("note").textContent = "";
};
fetch("/api/r/" + encodeURIComponent(id) + "/info").then((r) => r.ok ? r.json() : Promise.reject()).then((m) => { meta(m); connect(); }).catch(() => { $("title").textContent = "This room is not live."; });

async function hostPoll() {
  if (!hostToken) return;
  const r = await fetch("/api/r/" + encodeURIComponent(id) + "/hostview", { headers: { Authorization: "Bearer " + hostToken } });
  if (!r.ok) return;
  const v = await r.json();
  $("reports").replaceChildren();
  for (const x of v.reports) {
    const li = document.createElement("li");
    li.append(document.createTextNode(x.nick + ": " + x.text));
    for (const a of ["mute", "kick", "ban"]) { const b = document.createElement("button"); b.className = "rep"; b.textContent = a; b.onclick = () => mod(a, x.vid); li.append(b); }
    $("reports").append(li);
  }
}
$("hostform").onsubmit = async (e) => {
  e.preventDefault();
  const r = await fetch("/api/r/" + encodeURIComponent(id) + "/hostlogin", { method: "POST", body: JSON.stringify({ pin: $("pin").value }) });
  $("pin").value = "";
  if (!r.ok) { $("hostnote").textContent = r.status === 429 ? "Too many tries, wait a minute." : "Wrong PIN."; return; }
  hostToken = (await r.json()).hostToken;   // kept in this page's memory only
  $("hostnote").textContent = "Moderation on: mute, kick and ban buttons appear next to chat lines.";
  hostPoll(); setInterval(hostPoll, 5000);
};
