// ham-ui.js - the "Ham contacts" rail section: band / mode / age filters, a list of contacts (newest first) and the map lines between
// the two stations of a contact, coloured by mode. Data comes from the ham tracks the site already receives (see ham.js).
import { BAND_NAMES, MODE_NAMES, buildContacts, filterContacts, contactFeatures, modeColour } from "./ham.js";

const $ = (id) => document.getElementById(id);

export function initHam({ map, tracks, selectById, esc, REDUCED }) {
  const panel = $("hamPanel");
  if (!panel) return;
  const bandSel = $("hamBand"), modeSel = $("hamMode"), ageSel = $("hamAge"), list = $("hamList"), count = $("hamN"), lines = $("hamLines");
  bandSel.innerHTML = '<option value="">All bands</option>' + BAND_NAMES.map((b) => `<option>${b}</option>`).join("");
  modeSel.innerHTML = '<option value="">All modes</option>' + MODE_NAMES.map((m) => `<option>${m}</option>`).join("");
  const sets = (v) => (v ? new Set([v]) : null);

  map.addSource("hamlines", { type: "geojson", data: { type: "FeatureCollection", features: [] } });
  map.addLayer({
    id: "hamlines", type: "line", source: "hamlines", layout: { "line-cap": "round", visibility: lines.checked ? "visible" : "none" },
    paint: { "line-color": ["get", "color"], "line-width": 1.8, "line-opacity": 0.8 },
  }, "halo");

  function refresh() {
    const all = buildContacts(tracks().values(), Date.now());
    const f = filterContacts(all, { bands: sets(bandSel.value), modes: sets(modeSel.value), maxAgeMin: ageSel.value ? Number(ageSel.value) : null });
    count.textContent = String(f.length);
    const src = map.getSource("hamlines");
    if (src) src.setData({ type: "FeatureCollection", features: contactFeatures(f) });
    map.setLayoutProperty("hamlines", "visibility", lines.checked ? "visible" : "none");
    if (panel.open) {
      list.innerHTML = f.slice(0, 60).map((c) => {
        const mins = c.ageMin < 1 ? "now" : c.ageMin < 90 ? Math.round(c.ageMin) + " min" : Math.round(c.ageMin / 60) + " h";
        const meta = [c.mode, c.band, c.freq ? (c.freq / 1e6).toFixed(3) + " MHz" : ""].filter(Boolean).join(" · ");
        return `<li data-id="${esc(c.id)}"><i style="background:${modeColour(c.mode)}"></i><b>${esc(c.from)}${c.to ? " &rarr; " + esc(c.to) : ""}</b><span>${esc(meta)}</span><em>${mins}</em></li>`;
      }).join("") || '<li class="empty">No contacts match. Stations that report who they worked appear here.</li>';
    }
  }
  for (const el of [bandSel, modeSel, ageSel, lines]) el.addEventListener("change", refresh);
  panel.addEventListener("toggle", refresh);
  list.addEventListener("click", (e) => {
    const li = e.target.closest("li[data-id]");
    if (li) selectById(li.dataset.id);
  });
  let soon = 0;   // the data changes every few seconds: follow it, at most twice a second, and keep the 5 s tick for the ageing
  document.addEventListener("onair:tracks", () => { if (!soon) soon = setTimeout(() => { soon = 0; refresh(); }, 500); });
  setInterval(refresh, 5000);
  refresh();
}
