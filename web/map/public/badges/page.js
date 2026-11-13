// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// badges/page.js - the profile page (/u/<github-login>) and the leaderboard (/leaders). Everything from the server is written with
// textContent; the only markup inserted is the badge art from art.js (our own strings). The pure helpers are tested in test/badges-ui.test.js.

import { BADGES, BADGE_BY_ID, BOARDS, RARE, TIER_NAMES, progressFor, badgeTitle } from "./defs.js";
import { badgeSvg } from "./art.js";

// ---------------------------------------------------------------- pure helpers

export function fmtNum(v, unit = "") {
  const n = Number(v) || 0;
  const s = n >= 100 || Number.isInteger(n) ? Math.round(n).toLocaleString("en") : n.toFixed(1);
  return unit ? `${s} ${unit}` : s;
}
export function fmtDate(ms) {
  if (!ms) return "";
  return new Date(ms).toISOString().slice(0, 10);
}
/** The login from a /u/<login> path, or "". */
export function loginFromPath(p) {
  const m = /^\/u\/([A-Za-z0-9-]{1,39})\/?$/.exec(String(p || ""));
  return m ? m[1] : "";
}
/** Rows for the badge grid: earned first (highest tier first), then the locked ones with progress. */
export function badgeRows(profile) {
  const rows = (profile.badges || []).map((b) => {
    const def = BADGE_BY_ID[b.id];
    if (!def) return null;
    const pr = progressFor(def, b.value);
    const last = b.earned && b.earned.length ? b.earned[b.earned.length - 1] : null;
    return {
      id: b.id, icon: def.icon, tier: b.tier, title: badgeTitle(def, b.tier), desc: def.desc,
      earnedAt: last ? last.at : 0, earned: b.earned || [],
      progress: pr.next == null ? "Top tier" : `${fmtNum(b.value, def.unit)} of ${fmtNum(pr.next, def.unit)}`, frac: pr.frac,
    };
  }).filter(Boolean);
  rows.sort((a, b) => (b.tier > 0) - (a.tier > 0) || b.tier - a.tier || b.earnedAt - a.earnedAt || BADGES.findIndex((x) => x.id === a.id) - BADGES.findIndex((x) => x.id === b.id));
  return rows;
}
/** The tiles of the stats grid. */
export function statTiles(p) {
  const s = p.stats || {}, c = s.counts || {};
  return [
    ["Hours shared", fmtNum(s.hours)], ["Days", fmtNum(s.days)], ["Streak (best)", `${fmtNum(s.streak)} (${fmtNum(s.bestStreak)})`],
    ["Objects", fmtNum(s.objects)], ["Aircraft", fmtNum(c.aircraft)], ["Ships", fmtNum(c.vessel)], ["Sondes", fmtNum(c.sonde)],
    ["APRS stations", fmtNum(c.aprs)], ["Satellites", fmtNum(c.sat)], ["Ham contacts", fmtNum(c.ham)], ["DF fixes", fmtNum(c.fix)],
    ["ACARS messages", fmtNum(c.acars)], ["Record range", s.rangeKm ? fmtNum(s.rangeKm, "km") : "-"],
    ["Countries", fmtNum((s.countries || []).length)], ["Fields", fmtNum((s.fields || []).length)], ["This week", fmtNum(s.week && s.week.hours, "h")],
  ];
}
/** A board's value written with its unit. */
export function boardValue(boardId, v) {
  const b = BOARDS.find((x) => x.id === boardId);
  return fmtNum(v, b ? b.unit : "");
}
/** The brag text for the share button. */
export function bragText(p, url) {
  const earned = (p.badges || []).filter((b) => b.tier > 0).length;
  const h = p.stats ? fmtNum(p.stats.hours) : "0";
  return `${p.login} on the OnAir live map: ${earned} badges, ${h} hours on the air, ${fmtNum(p.stats && p.stats.objects)} objects shared. ${url}`;
}

// ---------------------------------------------------------------- DOM

const $ = (sel) => document.querySelector(sel);
function el(tag, props = {}, ...kids) {
  const e = document.createElement(tag);
  for (const [k, v] of Object.entries(props)) {
    if (k === "text") e.textContent = v; else if (k === "cls") e.className = v; else if (k.startsWith("on")) e.addEventListener(k.slice(2), v); else e.setAttribute(k, v);
  }
  for (const c of kids) if (c != null) e.append(c);
  return e;
}
function svgNode(markup) { const t = document.createElement("template"); t.innerHTML = markup; return t.content.firstChild; }   // our own badge art only
async function getJson(url) { const r = await fetch(url, { credentials: "same-origin" }); return { status: r.status, body: await r.json().catch(() => ({})) }; }

let me = null;
async function loadMe() { me = (await getJson("/api/me")).body; return me; }
async function change(path, body) {
  const r = await fetch(path, { method: "POST", credentials: "same-origin", headers: { "Content-Type": "application/json", "X-CSRF-Token": me && me.csrf ? me.csrf : "" }, body: JSON.stringify(body || {}) });
  return { status: r.status, body: await r.json().catch(() => ({})) };
}

function signInBox(next) {
  const box = $("#account");
  box.textContent = "";
  if (!me || !me.signedIn) {
    if (me && me.configured === false) { box.append(el("span", { cls: "dim", text: "Sign-in is not set up on this server yet." })); return; }
    box.append(el("a", { cls: "btn", href: "/auth/login?next=" + encodeURIComponent(next), text: "Sign in with GitHub" }));
    return;
  }
  box.append(el("span", { text: "Signed in as " }), el("a", { href: "/u/" + me.login, text: me.login }), " ",
    el("button", { type: "button", text: "Sign out", onclick: async () => { await change("/auth/logout"); location.reload(); } }));
}

function settingsCard() {
  const host = $("#settings");
  if (!host) return;
  host.textContent = "";
  if (!me || !me.signedIn) return;
  const msg = el("div", { cls: "msg", hidden: "" });
  const say = (t) => { msg.textContent = t; msg.hidden = !t; };
  const toggle = (key, label, help) => {
    const cb = el("input", { type: "checkbox" });
    cb.checked = !!me[key];
    cb.addEventListener("change", async () => {
      const r = await change("/api/me/settings", { [key]: cb.checked });
      if (r.status !== 200) { cb.checked = !cb.checked; say(r.body.error || "Could not save."); } else { me[key] = cb.checked; say(""); }
    });
    return el("div", {}, el("label", {}, cb, el("span", { text: label })), el("div", { cls: "dim", text: help }));
  };
  const name = el("input", { type: "text", maxlength: "24", placeholder: "empty = not linked", value: me.by || "" });
  host.append(el("h2", { text: "Your settings" }), el("div", { cls: "card" },
    toggle("badges", "Earn badges", "Count what you share from OnAir for stats and badges. Off: nothing is counted."),
    toggle("public", "Public profile", "Show this page, your badges and stats to everyone, list you on the leaderboard, and show your top badge next to your map name."),
    el("div", { cls: "row", style: "margin-top:8px" }, el("span", { text: "Map name linked to this account:" }), name,
      el("button", { type: "button", text: "Save", onclick: async () => { const r = await change("/api/me/settings", { by: name.value }); say(r.status === 200 ? "Saved." : r.body.error || "Could not save."); } })),
    el("div", { cls: "dim", text: "A linked name can only be used by stations signed in with this account." }),
    el("div", { cls: "row", style: "margin-top:10px" },
      el("button", { type: "button", text: `Sign out OnAir apps (${me.apps || 0})`, onclick: async () => { const r = await change("/api/me/apps/revoke"); say(r.status === 200 ? "Every OnAir app was signed out of the map. Sign in again in the app to keep earning." : r.body.error || "Failed."); } }),
      el("button", { type: "button", cls: "danger", text: "Delete my badge data", onclick: async () => {
        if (!confirm("Delete every stat, badge and setting the map keeps for your account? This cannot be undone.")) return;
        const r = await change("/api/me/delete"); if (r.status === 200) location.href = "/"; else say(r.body.error || "Failed.");
      } })),
    msg));
}

function renderProfile(p) {
  const main = $("#profile");
  main.textContent = "";
  const url = location.origin + "/u/" + p.login;
  main.append(
    el("h1", { text: p.login }),
    el("div", { cls: "dim", text: (p.by ? `Map name ${p.by}. ` : "") + (p.first ? `Sharing since ${fmtDate(p.first)}.` : "No shares yet.") + (p.mine && !p.public ? " Only you can see this page." : "") }),
    el("div", { cls: "row", style: "margin:10px 0" }, el("button", { type: "button", text: "Copy link to brag", onclick: async () => { try { await navigator.clipboard.writeText(bragText(p, url)); } catch { /* no clipboard */ } } })),
  );
  const stats = el("div", { cls: "stats" });
  for (const [k, v] of statTiles(p)) stats.append(el("div", { cls: "stat" }, el("b", { text: v }), el("span", { text: k })));
  main.append(el("h2", { text: "Stats" }), stats);
  const rare = Object.keys((p.stats && p.stats.rare) || {});
  if (rare.length) main.append(el("div", { cls: "dim", style: "margin-top:6px", text: "Rare catches: " + rare.map((k) => RARE[k] || k).join(", ") }));
  const grid = el("div", { cls: "bgrid" });
  for (const b of badgeRows(p)) {
    const when = b.earned.map((e) => `${TIER_NAMES[e.tier] || ""} ${fmtDate(e.at)}`.trim()).join(", ");
    grid.append(el("div", { cls: "bcard" + (b.tier ? "" : " locked") }, svgNode(badgeSvg(b.icon, b.tier, { size: 52, title: b.title })),
      el("div", {}, el("div", { cls: "t", text: b.title }), el("div", { cls: "d", text: b.desc }),
        el("div", { cls: "d", text: b.tier ? "Earned " + when : "Not earned yet" }), el("div", { cls: "d", text: b.progress }),
        el("div", { cls: "bar" }, el("i", { style: `width:${Math.round(b.frac * 100)}%` })))));
  }
  main.append(el("h2", { text: "Badges" }), grid);
}

export async function bootProfile() {
  const login = loginFromPath(location.pathname);
  await loadMe();
  signInBox(location.pathname);
  if (!login) { $("#profile").textContent = "No profile name in the address."; return; }
  document.title = login + " - OnAir map badges";
  if (me && me.signedIn && me.login.toLowerCase() === login.toLowerCase()) settingsCard();
  const r = await getJson("/api/u/" + encodeURIComponent(login));
  if (r.status !== 200) { $("#profile").textContent = r.body.error || "Not found."; return; }
  renderProfile(r.body);
}

export async function bootLeaders() {
  await loadMe();
  signInBox("/leaders");
  const q = new URLSearchParams(location.search);
  let board = BOARDS.find((b) => b.id === q.get("board")) || BOARDS[0];
  let period = board.periods.includes(q.get("period")) ? q.get("period") : board.periods[0];
  const host = $("#leaders");
  const draw = async () => {
    host.textContent = "";
    const tabs = el("div", { cls: "tabs" });
    for (const b of BOARDS) tabs.append(el("button", { type: "button", "aria-pressed": String(b.id === board.id), text: b.name, onclick: () => { board = b; if (!b.periods.includes(period)) period = b.periods[0]; draw(); } }));
    const per = el("div", { cls: "tabs" });
    for (const p of board.periods) per.append(el("button", { type: "button", "aria-pressed": String(p === period), text: p === "all" ? "All time" : p === "week" ? "This week" : "This month", onclick: () => { period = p; draw(); } }));
    host.append(tabs, per);
    history.replaceState(null, "", `/leaders?board=${board.id}&period=${period}`);
    const r = await getJson(`/api/leaderboard?board=${board.id}&period=${period}`);
    const rows = (r.body && r.body.rows) || [];
    if (!rows.length) { host.append(el("p", { cls: "dim", text: "Nobody on this board yet. Turn on Earn badges and Public profile to join." })); return; }
    const tb = el("tbody");
    for (const x of rows) {
      const [bid, bt] = String(x.showcase || "").split(":");
      const def = BADGE_BY_ID[bid];
      tb.append(el("tr", {}, el("td", { cls: "n", text: String(x.rank) }),
        el("td", {}, def ? svgNode(badgeSvg(def.icon, Number(bt), { size: 20, title: badgeTitle(def, Number(bt)) })) : null, " ", el("a", { href: "/u/" + x.login, text: x.login })),
        el("td", { cls: "n", text: boardValue(board.id, x.value) })));
    }
    host.append(el("table", {}, el("thead", {}, el("tr", {}, el("th", { cls: "n", text: "#" }), el("th", { text: "Station" }), el("th", { cls: "n", text: board.name }))), tb));
  };
  draw();
}
