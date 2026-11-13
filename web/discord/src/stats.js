// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

const LAYERS = [
  ["aircraft", "Aircraft"], ["sonde", "Radiosondes"], ["vessel", "Ships (AIS)"], ["aprs", "APRS stations"],
  ["ham", "Ham"], ["drone", "Drones"], ["flarm", "FLARM"], ["sat", "Satellites"],
  ["buoy", "Buoys"], ["dsc", "DSC"],
];

/** Reduce a /live snapshot to plain numbers. */
export function summarize(live) {
  const tracks = Array.isArray(live && live.tracks) ? live.tracks : [];
  const byKind = {};
  for (const t of tracks) byKind[t.kind] = (byKind[t.kind] || 0) + 1;
  const load = (live && live.load) || {};
  return {
    byKind,
    markers: tracks.length,
    stations: Number(live && live.stations) || 0,
    viewers: load.viewers ?? null,
    ingestPerMin: load.ingestPerMin ?? null,
    throttlePerMin: load.throttlePerMin ?? null,
    loadPct: load.pct ?? null,
  };
}

export async function fetchStats(mapUrl, fetchFn = fetch) {
  const r = await fetchFn(mapUrl.replace(/\/$/, "") + "/live", { headers: { "User-Agent": "OnAirDiscord/1.0" } });
  if (!r.ok) throw new Error("map /live " + r.status);
  return summarize(await r.json());
}

const n = (v) => (v == null ? "n/a" : String(v));

/** Embed fields. Layers with zero markers are hidden except the four main ones. */
export function statsFields(s) {
  const main = new Set(["aircraft", "sonde", "vessel", "aprs"]);
  const fields = [];
  for (const [k, label] of LAYERS) {
    const c = s.byKind[k] || 0;
    if (c || main.has(k)) fields.push({ name: label, value: String(c), inline: true });
  }
  fields.push({ name: "Total markers", value: String(s.markers), inline: true });
  fields.push({ name: "Stations sending", value: n(s.stations) + " (last 30 min)", inline: true });
  fields.push({ name: "Live viewers", value: n(s.viewers), inline: true });
  const rate = s.ingestPerMin == null ? "n/a" : `${s.ingestPerMin} posts/min (${(s.ingestPerMin / 60).toFixed(1)}/s)`;
  fields.push({ name: "Ingest", value: rate, inline: true });
  fields.push({ name: "Server load", value: s.loadPct == null ? "n/a" : s.loadPct + "%", inline: true });
  return fields;
}

export function buildEmbed(s, { shotAgeSec, note, mapUrl = "https://map.on-air.dev", withImage = false, now = new Date() } = {}) {
  const parts = [now.toISOString().replace("T", " ").slice(0, 16) + " UTC", mapUrl.replace(/^https?:\/\//, "")];
  if (shotAgeSec != null && shotAgeSec >= 1) parts.splice(1, 0, `cached ${shotAgeSec} s ago`);
  const embed = {
    title: "OnAir live map",
    url: mapUrl,
    color: 0x2f81f7,
    fields: statsFields(s),
    footer: { text: parts.join(" | ") },
  };
  if (note) embed.description = note;
  if (withImage) embed.image = { url: "attachment://map.png" };
  return embed;
}
