// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

import { verifySignature } from "./verify.js";
import { fetchStats, buildEmbed } from "./stats.js";
import { getScreenshot } from "./screenshot.js";

const json = (o, status = 200) =>
  new Response(JSON.stringify(o), { status, headers: { "Content-Type": "application/json" } });

async function editOriginal(env, token, embed, png, fetchFn) {
  const url = `https://discord.com/api/v10/webhooks/${env.DISCORD_APP_ID}/${token}/messages/@original`;
  let body;
  if (png) {
    body = new FormData();
    body.append("payload_json", JSON.stringify({
      embeds: [embed],
      attachments: [{ id: 0, filename: "map.png" }],
    }));
    body.append("files[0]", new Blob([png], { type: "image/png" }), "map.png");
  } else {
    body = JSON.stringify({ embeds: [embed] });
  }
  const r = await fetchFn(url, { method: "PATCH", headers: png ? {} : { "Content-Type": "application/json" }, body });
  if (!r.ok) console.error("discord edit failed", r.status, (await r.text()).slice(0, 200));
}

export async function runCommand(name, env, token, fetchFn = fetch) {
  const mapUrl = env.MAP_URL || "https://map.on-air.dev";
  let stats = null, statsErr = "";
  let shot = null, shotErr = "";
  const jobs = [
    fetchStats(mapUrl, fetchFn).then((s) => { stats = s; }).catch((e) => { statsErr = String(e.message || e); }),
  ];
  if (name === "map") {
    jobs.push(getScreenshot(env, mapUrl, fetchFn).then((s) => { shot = s; }).catch((e) => { shotErr = String(e.message || e); }));
  }
  await Promise.all(jobs);
  if (!stats) {
    await editOriginal(env, token, {
      title: "OnAir live map", url: mapUrl, color: 0xd73a49,
      description: "Could not read live stats: " + statsErr,
    }, shot && shot.png, fetchFn);
    return;
  }
  const embed = buildEmbed(stats, {
    mapUrl,
    withImage: !!shot,
    shotAgeSec: shot ? shot.ageSec : null,
    note: name === "map" && !shot ? "Screenshot unavailable: " + shotErr : undefined,
  });
  await editOriginal(env, token, embed, shot && shot.png, fetchFn);
}

export async function handle(req, env, ctx, fetchFn = fetch) {
  if (req.method !== "POST") return new Response("OnAir Discord bot", { status: 200 });
  const body = await req.text();
  const ok = await verifySignature(
    env.DISCORD_PUBLIC_KEY,
    req.headers.get("X-Signature-Ed25519"),
    req.headers.get("X-Signature-Timestamp"),
    body,
  );
  if (!ok) return new Response("invalid request signature", { status: 401 });
  let it;
  try { it = JSON.parse(body); } catch { return new Response("bad json", { status: 400 }); }
  if (it.type === 1) return json({ type: 1 });
  if (it.type === 2) {
    const name = it.data && it.data.name;
    if (name === "map" || name === "stats") {
      ctx.waitUntil(runCommand(name, env, it.token, fetchFn).catch((e) => console.error("command failed", e)));
      return json({ type: 5 });
    }
  }
  return json({ type: 4, data: { content: "Unknown command.", flags: 64 } });
}

export default { fetch: (req, env, ctx) => handle(req, env, ctx) };
