// OnAir receiver directory page. Fetches /api/v1/servers once a minute and filters, sorts and draws everything in the browser.
// Server values go in with textContent only (never innerHTML); links only for http(s) URLs the Worker already validated.
// Location, if the visitor shares it, is used for distances in this page only and is never sent anywhere. No analytics, no cookies.
// Tabs: #receivers (default), #listen, #list, #run, #faq. Plain hash routing, every page is in index.html.
function route() {
  const t = (location.hash || "#receivers").slice(1);
  const k = ["receivers", "listen", "list", "run", "faq"].includes(t) ? t : "receivers";
  for (const p of document.querySelectorAll(".page")) p.hidden = p.id !== "p-" + k;
  for (const a of document.querySelectorAll("#tabs a")) a.setAttribute("aria-selected", String(a.dataset.t === k));
  if (k !== "receivers" || !location.hash) window.scrollTo(0, 0);
}
addEventListener("hashchange", route);
route();
const $ = (id) => document.getElementById(id);
const M = 1e6;
// Bands in Hz, as the API's band filter defines them (approximate for the chips).
const BANDS = [
  ["lf", "LF", 3e4, 3e5], ["mw", "MW", 5.3e5, 1.71e6], ["hf", "HF", 3e6, 3e7], ["80m", "80 m", 3.5e6, 3.8e6], ["40m", "40 m", 7e6, 7.2e6], ["20m", "20 m", 14e6, 14.35e6],
  ["fm", "FM", 87.5e6, 108e6], ["air", "Air", 108e6, 137e6], ["marine", "Marine", 156e6, 162.025e6], ["2m", "2 m", 144e6, 148e6], ["dab", "DAB", 174e6, 240e6],
  ["70cm", "70 cm", 430e6, 440e6], ["uhf", "UHF", 3e8, 3e9], ["tv", "TV", 470e6, 862e6], ["l", "L band", 1e9, 2e9],
];
const SCALES = {
  log: { lo: Math.log10(0.1 * M), hi: Math.log10(6e9), log: true, ticks: [[0.1 * M, "0.1 MHz"], [1 * M, "1"], [10 * M, "10"], [100 * M, "100"], [1e9, "1G"], [6e9, "6 GHz"]] },
  hf: { lo: 0, hi: 30 * M, log: false, ticks: [[0, "0"], [10 * M, "10"], [20 * M, "20"], [30 * M, "30 MHz"]] },
  vu: { lo: 30 * M, hi: 1e9, log: false, ticks: [[30 * M, "30"], [250 * M, "250"], [500 * M, "500"], [750 * M, "750"], [1e9, "1 GHz"]] },
};
let all = [], band = "", here = null, scaleKey = "log";
let loaded = false, failed = false, repId = null, timer = null;

function el(tag, cls, text) { const e = document.createElement(tag); if (cls) e.className = cls; if (text !== undefined) e.textContent = text; return e; }
function safeHref(u) { try { const x = new URL(u); return x.protocol === "https:" || x.protocol === "http:" ? x.href : null; } catch { return null; } }
function fmtHz(hz) {
  if (hz >= 1e9) return +(hz / 1e9).toFixed(3) + " GHz";
  if (hz >= 1e6) return +(hz / 1e6).toFixed(3) + " MHz";
  return +(hz / 1e3).toFixed(1) + " kHz";
}
function coverageText(r) { return (r || []).map((x) => fmtHz(x[0]) + " to " + fmtHz(x[1])).join(", ") || "coverage not stated"; }
function ago(ts) {
  const d = Math.max(0, Math.round(Date.now() / 1000 - ts));
  if (d < 90) return "now";
  if (d < 3600) return Math.round(d / 60) + " min ago";
  if (d < 86400) return Math.round(d / 3600) + " h ago";
  return Math.round(d / 86400) + " d ago";
}
function km(a, b, c, d) {
  const R = 6371, r = Math.PI / 180, dl = (c - a) * r, dn = (d - b) * r;
  const x = Math.sin(dl / 2) ** 2 + Math.cos(a * r) * Math.cos(c * r) * Math.sin(dn / 2) ** 2;
  return 2 * R * Math.asin(Math.sqrt(x));
}
function overlaps(s, lo, hi) { return (s.freq || []).some((r) => r[0] <= hi && r[1] >= lo); }
function free(s) { return Math.max(0, s.capacity - s.users); }

function pos(hz, sc) {
  const v = sc.log ? Math.log10(Math.max(hz, 1e5)) : hz;
  return Math.min(100, Math.max(0, ((v - sc.lo) / (sc.hi - sc.lo)) * 100));
}
function drawAxis() {
  const sc = SCALES[scaleKey], ax = $("axis");
  ax.replaceChildren();
  for (const [hz, t] of sc.ticks) { const s = el("span", "", t); s.style.left = pos(hz, sc) + "%"; ax.append(s); }
  $("axis-title").textContent = "Coverage";
}
function covBar(s) {
  const sc = SCALES[scaleKey], box = el("div", "cov");
  box.setAttribute("role", "img");
  box.setAttribute("aria-label", "Coverage " + coverageText(s.freq));
  for (const [a, b] of s.freq || []) {
    if (!sc.log && (b < sc.lo || a > sc.hi)) continue;
    const l = pos(a, sc), r = pos(b, sc), i = el("i");
    i.style.left = l + "%"; i.style.width = Math.max(r - l, 0.4) + "%";
    box.append(i);
  }
  return box;
}

function visible() {
  const q = $("q").value.trim().toLowerCase(), bd = BANDS.find((b) => b[0] === band), onlyFree = $("free").checked;
  const out = all.filter((s) => {
    if (bd && !overlaps(s, bd[2], bd[3])) return false;
    if (onlyFree && !free(s)) return false;
    if (q && ![s.name, s.radio, s.location, s.description].some((t) => (t || "").toLowerCase().includes(q))) return false;
    return true;
  });
  const by = $("sort").value;
  const cmp = {
    users: (a, b) => b.users - a.users || a.name.localeCompare(b.name),
    free: (a, b) => free(b) - free(a) || a.name.localeCompare(b.name),
    name: (a, b) => a.name.localeCompare(b.name),
    seen: (a, b) => b.last_seen - a.last_seen,
    freq: (a, b) => ((a.freq[0] || [1e12])[0]) - ((b.freq[0] || [1e12])[0]),
    dist: (a, b) => (a.d ?? 1e9) - (b.d ?? 1e9),
  }[by];
  return out.sort(cmp);
}

function drawChips() {
  const box = $("chips");
  box.replaceChildren();
  const mk = (id, label, n) => {
    const b = el("button", "chip", n === null ? label : `${label} ${n}`);
    b.type = "button"; b.setAttribute("aria-pressed", String(band === id));
    b.addEventListener("click", () => { band = band === id && id !== "" ? "" : id; render(); });
    box.append(b);
  };
  mk("", "All", all.length);
  for (const [id, label, lo, hi] of BANDS) {
    const n = all.filter((s) => overlaps(s, lo, hi)).length;
    if (n || band === id) mk(id, label, n);
  }
}

function row(s) {
  const tr = document.createElement("tr");
  const nm = el("td", "c-name");
  const t = el("div", "nm");
  const dot = el("span", "dot" + (s.reachable === true ? " ok" : s.reachable === false ? " bad" : ""));
  dot.title = s.reachable === true ? "Answered when the directory checked" : s.reachable === false ? "Did not answer when the directory checked" : "Not checked";
  dot.setAttribute("role", "img");
  dot.setAttribute("aria-label", dot.title);
  t.append(dot, document.createTextNode(s.name));
  nm.append(t);
  const bits = [s.radio, s.location].filter(Boolean).join(" · ");
  if (bits) nm.append(el("div", "sub", bits));
  if (s.description) nm.append(el("div", "sub", s.description));
  const cov = el("td", "c-cov");
  cov.append(covBar(s), el("div", "sub", coverageText(s.freq)));
  const u = el("td", "c-num", `${s.users}/${s.capacity}`);
  u.dataset.l = "Listeners";
  if (!free(s)) { u.classList.add("full"); u.title = "No free slots"; }
  const tds = [nm, cov, u];
  if (here) { const d = el("td", "c-num", s.d === undefined ? "n/a" : s.d < 10 ? s.d.toFixed(1) + " km" : Math.round(s.d) + " km"); d.dataset.l = "Distance"; tds.push(d); }
  const sn = el("td", "c-num", ago(s.last_seen)); sn.dataset.l = "Seen"; sn.title = new Date(s.last_seen * 1000).toLocaleString(); tds.push(sn);
  const act = el("td", "c-act"), box = el("div", "act");
  const href = safeHref(s.url);
  if (href) { const a = el("a", "listen", "Listen"); a.href = href; a.target = "_blank"; a.rel = "noopener noreferrer nofollow"; a.setAttribute("aria-label", "Listen to " + s.name + " (opens its own page)"); box.append(a); }
  const rb = el("button", "rep", "Report"); rb.type = "button"; rb.setAttribute("aria-label", "Report " + s.name);
  rb.addEventListener("click", () => openReport(s));
  box.append(rb); act.append(box); tds.push(act);
  tr.append(...tds);
  return tr;
}

function render() {
  drawAxis(); drawChips();
  const st = $("status"), list = visible();
  $("th-dist").hidden = !here;
  $("tbl").hidden = !list.length;
  $("empty").hidden = !(loaded && !failed && !all.length);
  st.classList.toggle("err", failed);
  if (failed) st.textContent = "The directory could not be reached. Check your connection and try again; the list refreshes every minute.";
  else if (!loaded) st.textContent = "Loading receivers...";
  else if (!all.length) st.textContent = "0 receivers listed.";
  else if (!list.length) st.textContent = `No match among ${all.length} listed. Clear the search or pick All.`;
  else st.textContent = `${list.length} of ${all.length} receivers` + (here ? ", distances measured in your browser" : "") + ".";
  $("rows").replaceChildren(...list.map(row));
}

async function load() {
  try {
    const r = await fetch("/api/v1/servers?limit=500");
    const j = await r.json();
    if (!j.ok) throw new Error("bad");
    all = j.servers; failed = false;
    if (here) setDist();
  } catch { failed = true; }
  loaded = true;
  render();
}
function setDist() { for (const s of all) s.d = s.lat === null || s.lat === undefined ? undefined : km(here[0], here[1], s.lat, s.lon); }

$("loc").addEventListener("click", () => {
  const b = $("loc");
  if (here) { here = null; b.setAttribute("aria-pressed", "false"); b.textContent = "Show distance"; $("sort").querySelector("[value=dist]").disabled = true; if ($("sort").value === "dist") $("sort").value = "users"; render(); return; }
  if (!navigator.geolocation) { $("status").textContent = "This browser cannot share a location."; return; }
  b.textContent = "Locating...";
  navigator.geolocation.getCurrentPosition((p) => {
    here = [p.coords.latitude, p.coords.longitude]; setDist();
    b.setAttribute("aria-pressed", "true"); b.textContent = "Hide distance";
    $("sort").querySelector("[value=dist]").disabled = false; $("sort").value = "dist"; render();
  }, () => { b.textContent = "Show distance"; $("status").textContent = "Location was not shared, so no distances are shown. Nothing was sent anywhere."; });
});

const rep = $("rep");
function openReport(s) { repId = s.id; $("repname").textContent = s.name; $("repmsg").textContent = ""; $("reptext").value = ""; rep.showModal(); }
$("repcancel").addEventListener("click", () => rep.close());
$("repform").addEventListener("submit", async (e) => {
  e.preventDefault();
  try {
    const r = await fetch("/api/v1/report", { method: "POST", headers: { "content-type": "application/json" }, body: JSON.stringify({ id: repId, reason: $("repreason").value, text: $("reptext").value }) });
    const j = await r.json();
    $("repmsg").textContent = j.ok ? "Thank you. The report was sent." : "Not sent: " + (j.error || "error");
    if (j.ok) setTimeout(() => rep.close(), 1200);
  } catch { $("repmsg").textContent = "Not sent: network error"; }
});

for (const id of ["q", "sort", "free"]) $(id).addEventListener(id === "q" ? "input" : "change", render);
$("scale").addEventListener("change", () => { scaleKey = $("scale").value; render(); });
window.__setServers = (a) => { all = a; loaded = true; failed = false; render(); }; // test harness hook, harmless in production
render();
load();
timer = setInterval(load, 60000);
