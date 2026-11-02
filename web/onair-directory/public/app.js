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
let sortDir = 1;
let all = [], band = "", here = null, scaleKey = "log", region = "", sdr = "", view = "list", favOnly = false, selId = null;
const FAVK = "onair.dir.fav";
function favs() { try { return new Set(JSON.parse(localStorage.getItem(FAVK) || "[]")); } catch { return new Set(); } }
function saveFavs(f) { try { localStorage.setItem(FAVK, JSON.stringify([...f])); } catch { /* private window */ } }
let FAV = favs();
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

const REGIONS = [
  ["eu", "Europe", 34, 72, -25, 45], ["na", "North America", 5, 84, -170, -50], ["sa", "South America", -57, 13, -92, -30], ["af", "Africa", -36, 36, -20, 52],
  ["me", "Middle East", 12, 42, 34, 63], ["as", "Asia", -11, 78, 63, 180], ["oc", "Oceania", -50, 0, 110, 180],
];
function regionOf(s) {
  if (s.lat === null || s.lat === undefined) return "";
  const r = REGIONS.find((x) => s.lat >= x[2] && s.lat <= x[3] && s.lon >= x[4] && s.lon <= x[5]);
  return r ? r[0] : "other";
}
const regionName = (id) => id === "other" ? "Elsewhere" : (REGIONS.find((r) => r[0] === id) || [0, "Unknown"])[1];
function online(s) { return s.reachable !== false; }

function visible() {
  const q = $("q").value.trim().toLowerCase(), bd = BANDS.find((b) => b[0] === band), onlyFree = $("free").checked;
  const out = all.filter((s) => {
    if (bd && !overlaps(s, bd[2], bd[3])) return false;
    if (onlyFree && !free(s)) return false;
    if (favOnly && !FAV.has(s.id)) return false;
    if (region && regionOf(s) !== region) return false;
    if (sdr && (s.radio || "") !== sdr) return false;
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
    radio: (a, b) => (a.radio || "").localeCompare(b.radio || "") || a.name.localeCompare(b.name),
  }[by];
  out.sort(cmp);
  return sortDir < 0 ? out.reverse() : out;
}

function drawChips() {
  const box = $("chips");
  box.replaceChildren();
  const mk = (id, label, n) => {
    const b = el("button", "chip", label);
    if (n !== null) b.append(el("span", "n", String(n)));
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
function drawSelects() {
  const rs = $("region"), ss = $("sdr");
  const regs = [...new Set(all.map(regionOf).filter(Boolean))].sort((a, b) => regionName(a).localeCompare(regionName(b)));
  rs.replaceChildren(new Option("All regions", ""), ...regs.map((r) => new Option(regionName(r), r))); if (loaded && region && !regs.includes(region)) region = ""; rs.value = regs.includes(region) ? region : "";
  const sds = [...new Set(all.map((s) => s.radio).filter(Boolean))].sort();
  ss.replaceChildren(new Option("All radios", ""), ...sds.map((r) => new Option(r, r))); if (loaded && sdr && !sds.includes(sdr)) sdr = ""; ss.value = sds.includes(sdr) ? sdr : "";
}

function favBtn(s) {
  const b = el("button", "fav", FAV.has(s.id) ? "★" : "☆"); b.type = "button";
  b.setAttribute("aria-pressed", String(FAV.has(s.id))); b.setAttribute("aria-label", (FAV.has(s.id) ? "Remove " : "Add ") + s.name + (FAV.has(s.id) ? " from favourites" : " to favourites"));
  b.title = FAV.has(s.id) ? "Remove from favourites" : "Add to favourites";
  b.addEventListener("click", () => { if (FAV.has(s.id)) FAV.delete(s.id); else FAV.add(s.id); saveFavs(FAV); render(); });
  return b;
}
function statusDot(s) {
  const dot = el("span", "dot" + (s.reachable === true ? " ok" : s.reachable === false ? " bad" : ""));
  dot.title = s.reachable === true ? "Answered when the directory checked" : s.reachable === false ? "Did not answer when the directory checked" : "Not checked";
  dot.setAttribute("role", "img"); dot.setAttribute("aria-label", dot.title);
  return dot;
}
function loadCell(s) {
  const box = el("span", "load" + (!free(s) ? " full" : "")), bar = el("span", "bar"), i = el("i");
  i.style.width = Math.min(100, Math.round((s.users / Math.max(1, s.capacity)) * 100)) + "%"; bar.append(i);
  box.append(bar, el("span", "mono", `${s.users}/${s.capacity}`));
  box.title = free(s) ? free(s) + " free slot" + (free(s) === 1 ? "" : "s") : "No free slots";
  return box;
}
function listenLink(s) {
  const href = safeHref(s.url);
  if (!href) return null;
  const a = el("a", "listen", "Listen"); a.href = href; a.target = "_blank"; a.rel = "noopener noreferrer nofollow";
  a.setAttribute("aria-label", "Listen to " + s.name + " (opens its own page)");
  return a;
}
function reportBtn(s) {
  const rb = el("button", "rep", "Report"); rb.type = "button"; rb.setAttribute("aria-label", "Report " + s.name);
  rb.addEventListener("click", () => openReport(s)); return rb;
}
function distText(s) { return s.d === undefined ? "n/a" : s.d < 10 ? s.d.toFixed(1) + " km" : Math.round(s.d) + " km"; }

function row(s) {
  const tr = document.createElement("tr");
  if (s.id === selId) tr.className = "sel";
  tr.addEventListener("click", (e) => { if (e.target.closest("a,button")) return; selId = selId === s.id ? null : s.id; for (const r of $("rows").children) r.classList.remove("sel"); if (selId) tr.classList.add("sel"); });
  const fv = el("td", "c-fav"); fv.append(favBtn(s));
  const nm = el("td", "c-name");
  const t = el("div", "nm"); t.append(statusDot(s), document.createTextNode(s.name));
  nm.append(t);
  const bits = [s.location, s.description].filter(Boolean).join(" · ");
  if (bits) nm.append(el("div", "sub", bits));
  const rd = el("td", "c-radio", s.radio || ""); rd.classList.add("sub");
  const cov = el("td", "c-cov");
  cov.append(covBar(s), el("div", "sub mono", coverageText(s.freq)));
  const u = el("td", "c-num"); u.append(loadCell(s)); u.dataset.l = "Listeners";
  const tds = [fv, nm, rd, cov, u];
  if (here) { const d = el("td", "c-num", distText(s)); d.dataset.l = "Distance"; tds.push(d); }
  const sn = el("td", "c-num sub", ago(s.last_seen)); sn.dataset.l = "Seen"; sn.title = new Date(s.last_seen * 1000).toLocaleString(); tds.push(sn);
  const act = el("td", "c-act"), box = el("div", "act");
  if (window.matchMedia("(max-width: 760px)").matches) box.append(favBtn(s));
  box.append(reportBtn(s)); const l = listenLink(s); if (l) box.append(l);
  act.append(box); tds.push(act);
  tr.append(...tds);
  return tr;
}

function card(s) {
  const c = el("article", "card");
  const ct = el("div", "ct"), nm = el("div", "nm"); nm.append(statusDot(s), document.createTextNode(s.name));
  ct.append(nm, favBtn(s)); c.append(ct);
  const dl = el("dl", "meta");
  const add = (k, v) => { if (v) { dl.append(el("dt", "", k), el("dd", "", v)); } };
  add("Radio", s.radio); add("Place", s.location); if (here && s.d !== undefined) add("Distance", distText(s)); add("Seen", ago(s.last_seen));
  c.append(dl);
  if (s.description) c.append(el("p", "desc", s.description));
  const cov = el("div"); cov.append(covBar(s), el("div", "sub mono", coverageText(s.freq))); c.append(cov);
  const cf = el("div", "cf"), lc = el("span"); lc.append(loadCell(s));
  const act = el("div", "act"); act.append(reportBtn(s)); const l = listenLink(s); if (l) act.append(l);
  cf.append(lc, act); c.append(cf);
  return c;
}

// ---- map: plain equirectangular plot of receiver positions on a lat/lon grid (no tiles, nothing is fetched) ----
const SVGNS = "http://www.w3.org/2000/svg";
function svg(tag, attrs, cls) { const e = document.createElementNS(SVGNS, tag); for (const k in attrs) e.setAttribute(k, attrs[k]); if (cls) e.setAttribute("class", cls); return e; }
function drawMap(list) {
  const m = $("map"), W = 720, H = 300; m.replaceChildren();
  const pts = list.filter((s) => s.lat !== null && s.lat !== undefined);
  let la0 = -90, la1 = 90, lo0 = -180, lo1 = 180;
  if (pts.length) {
    la0 = Math.min(...pts.map((s) => s.lat)); la1 = Math.max(...pts.map((s) => s.lat)); lo0 = Math.min(...pts.map((s) => s.lon)); lo1 = Math.max(...pts.map((s) => s.lon));
    let cy = (la0 + la1) / 2, cx = (lo0 + lo1) / 2, hh = Math.max((la1 - la0) * 0.65, 14), hw = Math.max((lo1 - lo0) * 0.65, 28);
    const k = Math.max(hw / 2, hh) ; hh = k; hw = k * 2;
    if (hh > 90) { hh = 90; hw = 180; }
    la0 = Math.max(-90, cy - hh); la1 = Math.min(90, cy + hh); lo0 = Math.max(-180, cx - hw); lo1 = Math.min(180, cx + hw);
  }
  const X = (lo) => ((lo - lo0) / (lo1 - lo0)) * W, Y = (la) => ((la1 - la) / (la1 - la0)) * H;
  const span = Math.max(la1 - la0, (lo1 - lo0) / 2), gs = span > 120 ? 30 : span > 60 ? 15 : span > 30 ? 10 : span > 12 ? 5 : 2;
  for (let lo = Math.ceil(lo0 / gs) * gs; lo <= lo1; lo += gs) { const x = X(lo); m.append(svg("line", { x1: x, y1: 0, x2: x, y2: H }, lo === 0 ? "g g0" : "g")); const t = svg("text", { x: x + 3, y: H - 4 }, "gl"); t.textContent = lo + "°"; m.append(t); }
  for (let la = Math.ceil(la0 / gs) * gs; la <= la1; la += gs) { const y = Y(la); m.append(svg("line", { x1: 0, y1: y, x2: W, y2: y }, la === 0 ? "g g0" : "g")); const t = svg("text", { x: 4, y: y - 3 }, "gl"); t.textContent = la + "°"; m.append(t); }
  const tip = $("maptip");
  for (const s of pts) {
    const cx = X(s.lon), cy = Y(s.lat), r = 5 + Math.sqrt(s.users) * 1.6;
    const g = svg("g", {});
    if (s.id === selId) g.append(svg("circle", { cx, cy, r: r + 7 }, "ring"));
    const c = svg("circle", { cx, cy, r, tabindex: 0, role: "button", "aria-label": `${s.name}, ${s.users} of ${s.capacity} listeners` }, "pt" + (!online(s) ? " off" : !free(s) ? " full" : "") + (FAV.has(s.id) ? " fv" : ""));
    const show = () => {
      tip.hidden = false; tip.replaceChildren(); const b = el("b", "", s.name); tip.append(b, document.createElement("br"), document.createTextNode(`${s.location || ""}${s.location ? " · " : ""}${s.users}/${s.capacity} listeners`));
      const box = m.getBoundingClientRect(), wr = $("mapview").querySelector(".mapbox").getBoundingClientRect();
      tip.style.left = (box.left - wr.left + (cx / W) * box.width) + "px"; tip.style.top = (box.top - wr.top + (cy / H) * box.height) + "px";
    };
    c.addEventListener("pointerenter", show); c.addEventListener("focus", show);
    c.addEventListener("pointerleave", () => { tip.hidden = true; }); c.addEventListener("blur", () => { tip.hidden = true; });
    const pick = () => { selId = s.id; render(); };
    c.addEventListener("click", pick); c.addEventListener("keydown", (e) => { if (e.key === "Enter" || e.key === " ") { e.preventDefault(); pick(); } });
    g.append(c); m.append(g);
  }
  const note = $("mapnote"); note.replaceChildren();
  const sel = list.find((s) => s.id === selId);
  if (sel) { const cd = card(sel); cd.style.maxWidth = "420px"; note.append(cd); }
  const nopos = list.length - pts.length;
  if (!pts.length) note.append(el("p", "sub", "None of these receivers shares a location, so there is nothing to plot. Switch to List or Cards."));
  else if (nopos) note.append(el("p", "sub", `${nopos} receiver${nopos === 1 ? " has" : "s have"} no location and ${nopos === 1 ? "is" : "are"} not on the map.`));
  else if (!sel) note.append(el("p", "sub", "Click a dot for details. Dot size shows listeners, red means full, grey means unreachable."));
}

function syncUrl() {
  const p = new URLSearchParams();
  const put = (k, v) => { if (v) p.set(k, v); };
  put("q", $("q").value.trim()); put("band", band); put("region", region); put("sdr", sdr);
  if ($("sort").value !== "users") put("sort", $("sort").value);
  if (view !== "list") put("view", view); if ($("free").checked) p.set("free", "1"); if (favOnly) p.set("fav", "1"); if (scaleKey !== "log") put("scale", scaleKey);
  const qs = p.toString();
  try { history.replaceState(null, "", location.pathname + (qs ? "?" + qs : "") + location.hash); } catch { /* ignore */ }
}
function readUrl() {
  const p = new URLSearchParams(location.search);
  $("q").value = (p.get("q") || "").slice(0, 60); band = p.get("band") || ""; region = p.get("region") || ""; sdr = p.get("sdr") || "";
  if (["list", "cards", "map"].includes(p.get("view"))) view = p.get("view");
  $("free").checked = p.get("free") === "1"; favOnly = p.get("fav") === "1";
  if (SCALES[p.get("scale")]) { scaleKey = p.get("scale"); $("scale").value = scaleKey; }
  const so = p.get("sort"); if (so && [...$("sort").options].some((o) => o.value === so && !o.disabled)) $("sort").value = so;
}

function render() {
  drawAxis(); drawChips(); drawSelects();
  const st = $("status"), list = visible();
  $("th-dist").hidden = !here;
  for (const b of $("view").children) b.setAttribute("aria-checked", String(b.dataset.v === view));
  $("favOnly").setAttribute("aria-pressed", String(favOnly));
  $("scaleWrap").hidden = view !== "list";
  $("tbl").hidden = !(view === "list" && list.length);
  $("cards").hidden = !(view === "cards" && list.length);
  $("mapview").hidden = !(view === "map" && list.length);
  $("empty").hidden = !(loaded && !failed && !all.length);
  $("stats").hidden = !(loaded && !failed && all.length);
  $("st-n").textContent = String(all.length); $("st-on").textContent = String(all.filter(online).length); $("st-l").textContent = String(all.reduce((a, s) => a + s.users, 0));
  st.classList.toggle("err", failed); st.classList.toggle("load", !loaded);
  if (failed) st.textContent = "The directory could not be reached. Check your connection and try again; the list refreshes every minute.";
  else if (!loaded) st.textContent = "Loading receivers...";
  else if (!all.length) st.textContent = "0 receivers listed.";
  else if (!list.length) st.textContent = `No match among ${all.length} listed. Clear the search or pick All.`;
  else st.textContent = `${list.length} of ${all.length} receivers` + (here ? ", distances measured in your browser" : "") + ".";
  document.querySelector(".zero")?.remove();
  if (loaded && !failed && all.length && !list.length) {
    const z = el("div", "zero"); z.append(el("h2", "", "Nothing matches these filters"), el("p", "", "Try another band or region, or clear the search."));
    const b = el("button", "btn", "Clear filters"); b.type = "button"; b.addEventListener("click", clearFilters); z.append(b); st.after(z);
  }
  for (const th of document.querySelectorAll("#tbl th[data-sort]")) { const on = th.dataset.sort === $("sort").value; th.setAttribute("aria-sort", on ? (sortDir > 0 ? "ascending" : "descending") : "none"); }
  if (view === "list") $("rows").replaceChildren(...list.map(row));
  else if (view === "cards") $("cards").replaceChildren(...list.map(card));
  else if (list.length) drawMap(list);
  syncUrl();
}
function clearFilters() { $("q").value = ""; band = ""; region = ""; sdr = ""; favOnly = false; $("free").checked = false; render(); }

async function load() {
  if (DEMO) { all = demoServers(); loaded = true; failed = false; if (here) setDist(); render(); return; }
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

// ?demo=1 fills the list with made-up receivers so the page can be previewed without the API; production never sets it
const DEMO = /[?&]demo=1\b/.test(location.search);
function demoServers() {
  const now = Math.floor(Date.now() / 1000);
  const mk = (id, name, loc, lat, lon, radio, cap, users, freq, seen, reach, desc) => ({ id, name, url: "https://example.org/" + id, location: loc, lat, lon, radio, capacity: cap, users, freq, last_seen: now - seen, reachable: reach, description: desc });
  return [
    mk("a1", "Vilnius rooftop discone", "Vilnius, Lithuania", 54.69, 25.28, "HackRF Pro", 8, 3, [[1e6, 6e9]], 40, true, "Wideband receiver on the roof"),
    mk("a2", "Kaunas HF loop", "Kaunas, Lithuania", 54.9, 23.9, "Airspy HF+ Discovery", 4, 4, [[1e5, 3e7]], 120, true, "Magnetic loop, quiet rural site"),
    mk("a3", "Tallinn airband", "Tallinn, Estonia", 59.44, 24.75, "RTL-SDR v4", 6, 1, [[118e6, 137e6], [156e6, 163e6]], 30, true, "Approach and ATIS, AIS"),
    mk("a4", "Gdansk coast VHF", "Gdansk, Poland", 54.35, 18.65, "SDRplay RSPdx", 6, 2, [[87.5e6, 108e6], [144e6, 146e6], [156e6, 163e6]], 75, true, "Marine VHF and FM"),
    mk("a5", "Riga ADS-B", "Riga, Latvia", 56.95, 24.1, "RTL-SDR v3", 5, 0, [[1089e6, 1091e6]], 210, null, "Aircraft traffic over the Baltic"),
    mk("a6", "Dubai 522 MHz mux", "Dubai, UAE", 25.2, 55.27, "HackRF Pro", 3, 1, [[470e6, 700e6], [87.5e6, 108e6]], 55, true, "DVB-T2 and FM from a 30th floor balcony"),
    mk("a7", "Ohio shortwave", "Columbus, Ohio, USA", 39.96, -83.0, "Airspy HF+ Discovery", 6, 2, [[1e5, 3e7]], 20, true, "Big wire antenna, evening broadcast band"),
    mk("a8", "Oslo 2 m repeaters", "Oslo, Norway", 59.91, 10.75, "RTL-SDR v4", 4, 0, [[144e6, 146e6], [430e6, 440e6]], 600, false, "Local repeaters"),
    mk("a9", "Cape Town marine", "Cape Town, South Africa", -33.92, 18.42, "SDRplay RSP1A", 4, 1, [[156e6, 163e6], [87.5e6, 108e6]], 95, true, "Table Bay AIS and VHF"),
    mk("b1", "Berlin DAB and FM", "Berlin, Germany", 52.52, 13.4, "HackRF One", 5, 2, [[87.5e6, 108e6], [174e6, 240e6]], 48, true, "Broadcast monitoring"),
    mk("b2", "Poznan APRS", "Poznan, Poland", 52.41, 16.93, "RTL-SDR v3", 3, 0, [[144e6, 146e6]], 33, true, "APRS igate with a receiver"),
    mk("b3", "Roof Aarhus", "Aarhus, Denmark", 56.16, 10.2, "Airspy Mini", 6, 5, [[24e6, 1.8e9]], 25, true, "Wide scanner, 5 slots busy"),
  ];
}

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

// theme: follows the system until the visitor picks one
function setTheme(t) {
  const th = t || (matchMedia("(prefers-color-scheme: light)").matches ? "light" : "dark");
  document.documentElement.dataset.theme = th; { const tb = $("themeBtn"); if (tb) tb.textContent = "Theme: " + (th === "light" ? "Light" : "Dark"); }
  const m = document.querySelector('meta[name="theme-color"]'); if (m) m.content = th === "light" ? "#dadada" : "#252526";
  if (t) { try { localStorage.setItem("onair.dir.theme", t); } catch { /* ignore */ } }
}
let savedTheme = null; try { savedTheme = localStorage.getItem("onair.dir.theme"); } catch { /* ignore */ }
setTheme(savedTheme);
$("themeBtn").addEventListener("click", () => setTheme(document.documentElement.dataset.theme === "light" ? "dark" : "light"));

for (const id of ["q", "sort", "free"]) $(id).addEventListener(id === "q" ? "input" : "change", render);
$("region").addEventListener("change", () => { region = $("region").value; render(); });
$("sdr").addEventListener("change", () => { sdr = $("sdr").value; render(); });
$("favOnly").addEventListener("click", () => { favOnly = !favOnly; render(); });
$("scale").addEventListener("change", () => { scaleKey = $("scale").value; render(); });
for (const b of $("view").children) b.addEventListener("click", () => { view = b.dataset.v; render(); });
$("share").addEventListener("click", async () => {
  syncUrl(); const b = $("share");
  try { await navigator.clipboard.writeText(location.href); b.textContent = "Link copied"; } catch { b.textContent = "Copy it from the address bar"; }
  setTimeout(() => { b.textContent = "Copy link"; }, 1800);
});
for (const th of document.querySelectorAll("#tbl th[data-sort]")) th.addEventListener("click", () => { const k = th.dataset.sort; if ($("sort").value === k) sortDir = -sortDir; else { $("sort").value = k; sortDir = k === "users" || k === "seen" ? 1 : 1; } render(); });
window.__setServers = (a) => { all = a; loaded = true; failed = false; render(); }; // test harness hook, harmless in production
readUrl();
render();
load();
if (!DEMO) timer = setInterval(load, 60000);
