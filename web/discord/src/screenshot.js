// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

const TTL_MS = 20000;
let mem = null; // { png: ArrayBuffer, at: ms }

export function _resetCache() { mem = null; }

async function render(env, mapUrl, fetchFn) {
  const endpoint = `https://api.cloudflare.com/client/v4/accounts/${env.CF_ACCOUNT_ID}/browser-rendering/screenshot`;
  const base = {
    url: mapUrl,
    viewport: { width: 1280, height: 720 },
    gotoOptions: { waitUntil: "networkidle0", timeout: 30000 },
    screenshotOptions: { type: "png" },
  };
  // Extra wait for map tiles; some API versions may reject the field, so retry without it.
  for (const body of [{ ...base, waitForTimeout: 3000 }, base]) {
    const r = await fetchFn(endpoint, {
      method: "POST",
      headers: { Authorization: "Bearer " + env.CF_API_TOKEN, "Content-Type": "application/json" },
      body: JSON.stringify(body),
    });
    if (r.ok) {
      const buf = await r.arrayBuffer();
      if (buf.byteLength > 1000) return buf;
      throw new Error("empty screenshot");
    }
    if (r.status !== 400) throw new Error("Browser Rendering " + r.status + " " + (await r.text()).slice(0, 120));
  }
  throw new Error("Browser Rendering rejected the request");
}

/** Returns { png, ageSec }. Reuses a PNG younger than 20 s (memory, then Cache API). */
export async function getScreenshot(env, mapUrl, fetchFn = fetch, now = Date.now()) {
  if (mem && now - mem.at < TTL_MS) return { png: mem.png, ageSec: Math.round((now - mem.at) / 1000) };
  const key = new Request("https://onair-discord.invalid/screenshot");
  try {
    const hit = typeof caches !== "undefined" && (await caches.default.match(key));
    if (hit) {
      const at = Number(hit.headers.get("X-Shot-At")) || 0;
      if (now - at < TTL_MS) {
        mem = { png: await hit.arrayBuffer(), at };
        return { png: mem.png, ageSec: Math.round((now - at) / 1000) };
      }
    }
  } catch { /* cache is best effort */ }
  const png = await render(env, mapUrl, fetchFn);
  mem = { png, at: now };
  try {
    if (typeof caches !== "undefined") {
      await caches.default.put(key, new Response(png.slice(0), {
        headers: { "Cache-Control": "max-age=20", "X-Shot-At": String(now), "Content-Type": "image/png" },
      }));
    }
  } catch { /* ignore */ }
  return { png, ageSec: 0 };
}
