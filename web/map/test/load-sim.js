// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// load-sim.js - load simulator for the map hub. Not a unit test: it only runs with --run.
//
//   node test/load-sim.js --run --tree /path/to/checkout [--secs 60] [--warm 15] [--stations 70] [--live 200] [--ws 50]
//
// It starts `wrangler dev --local` (real workerd, real Durable Object SQLite, no login, no network use of Cloudflare)
// on a wrapper entry that counts what Cloudflare bills: every Durable Object fetch / alarm, and the rows read and
// written reported by the SQL cursors. The hub in `--tree` is untouched, so the same run measures before and after.
//
// Load: N stations POST /ingest every 2-5 s with 150-300 tracks (the OnAir app payload: path of 60 points, label, ...),
// V viewers poll /live every 5 s, W viewers hold a WebSocket. Prints a JSON report; per-minute figures.
import { spawn, execFileSync } from "node:child_process";
import { mkdtempSync, writeFileSync, mkdirSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, resolve, dirname } from "node:path";
import { createRequire } from "node:module";
import { fileURLToPath } from "node:url";

const args = process.argv.slice(2);
const opt = (name, def) => { const i = args.indexOf("--" + name); return i >= 0 && args[i + 1] && !args[i + 1].startsWith("--") ? args[i + 1] : def; };
if (!args.includes("--run")) {
  console.log("load-sim.js: pass --run --tree <checkout> to run (see the header comment). Skipped.");
  process.exit(0);
}

const HERE = dirname(fileURLToPath(import.meta.url));
const TREE = resolve(opt("tree", resolve(HERE, "../../..")));
const MAPDIR = join(TREE, "web/map");
const SECS = +opt("secs", 60), WARM = +opt("warm", 15);
const N_ST = +opt("stations", 70), N_LIVE = +opt("live", 200), N_WS = +opt("ws", 50);
const PORT = +opt("port", 8791);
const WRANGLER = resolve(HERE, "../node_modules/.bin/wrangler");
const require = createRequire(resolve(HERE, "../node_modules/"));
const { WebSocket } = require("ws");

// ---- wrapper worker: counts Durable Object requests, alarms and SQL rows ----
const dir = mkdtempSync(join(tmpdir(), "mapsim-"));
const entry = join(dir, "entry.js");
writeFileSync(entry, `
import worker, { MapHub as Base } from ${JSON.stringify(join(MAPDIR, "src/index.js"))};
const S = { doFetch: 0, alarms: 0, wsMsgs: 0, stmts: 0, rowsRead: 0, rowsWritten: 0 };
const cursors = [];
function drain() { for (const c of cursors.splice(0)) { try { S.rowsRead += c.rowsRead || 0; S.rowsWritten += c.rowsWritten || 0; } catch {} } }
function wrapSql(sql) {
  return new Proxy(sql, { get(t, k) {
    if (k === "exec") return (...a) => {
      const c = t.exec(...a); S.stmts++;
      if (!/^\\s*select|^\\s*pragma/i.test(String(a[0]))) c.toArray();
      cursors.push(c); return c;
    };
    const v = t[k]; return typeof v === "function" ? v.bind(t) : v;
  } });
}
function wrapCtx(ctx) {
  const st = new Proxy(ctx.storage, { get(t, k) {
    if (k === "sql") return wrapSql(t.sql);
    const v = t[k]; return typeof v === "function" ? v.bind(t) : v;
  } });
  return new Proxy(ctx, { get(t, k) {
    if (k === "storage") return st;
    const v = t[k]; return typeof v === "function" ? v.bind(t) : v;
  } });
}
export class MapHub extends Base {
  constructor(ctx, env) { super(wrapCtx(ctx), env); }
  async fetch(req) {
    const u = new URL(req.url);
    if (u.pathname === "/__stats") { drain(); const o = { ...S }; if (u.searchParams.get("reset")) for (const k in S) S[k] = 0; return new Response(JSON.stringify(o)); }
    S.doFetch++;
    return super.fetch(req);
  }
  async alarm() { S.alarms++; return super.alarm ? super.alarm() : undefined; }
  async webSocketMessage(...a) { S.wsMsgs++; return super.webSocketMessage ? super.webSocketMessage(...a) : undefined; }
}
export default {
  fetch(req, env, ctx) {
    const u = new URL(req.url);
    if (u.pathname === "/__stats") return env.MAP.get(env.MAP.idFromName("hub-v2")).fetch(req);
    return worker.fetch(req, env, ctx);
  },
};
`);
writeFileSync(join(dir, "wrangler.jsonc"), JSON.stringify({
  name: "mapsim", main: entry, compatibility_date: "2025-04-01",
  assets: { directory: join(MAPDIR, "public"), binding: "ASSETS", not_found_handling: "single-page-application" },
  durable_objects: { bindings: [{ name: "MAP", class_name: "MapHub" }] },
  migrations: [{ tag: "v1", new_sqlite_classes: ["MapHub"] }],
}));
mkdirSync(join(dir, "state"));

const base = `http://127.0.0.1:${PORT}`;
const child = spawn(WRANGLER, ["dev", "--local", "--config", join(dir, "wrangler.jsonc"), "--port", String(PORT), "--persist-to", join(dir, "state"), "--log-level", "error"],
  { cwd: dir, stdio: ["ignore", "inherit", "inherit"], detached: true });
const killAll = () => { try { process.kill(-child.pid, "SIGTERM"); } catch { /* gone */ } };
process.on("exit", killAll);
process.on("SIGINT", () => { killAll(); process.exit(1); });

/** CPU seconds used by wrangler and every descendant (workerd included), from ps. */
function cpuMs() {
  const out = execFileSync("ps", ["-axo", "pid=,ppid=,time="], { encoding: "utf8" });
  const rows = out.trim().split("\n").map((l) => l.trim().split(/\s+/)).map(([pid, ppid, time]) => ({ pid: +pid, ppid: +ppid, time }));
  const kids = new Set([child.pid]);
  for (let grew = true; grew;) { grew = false; for (const r of rows) if (!kids.has(r.pid) && kids.has(r.ppid)) { kids.add(r.pid); grew = true; } }
  let ms = 0;
  for (const r of rows) {
    if (!kids.has(r.pid)) continue;
    const m = r.time.match(/^(?:(\d+):)?(\d+):(\d+\.\d+|\d+)$/);
    if (m) ms += ((+(m[1] || 0)) * 3600 + (+m[2]) * 60 + parseFloat(m[3])) * 1000;
  }
  return ms;
}

// ---- synthetic world ----
let seed = 12345;
const rnd = () => { seed = (seed * 1664525 + 1013904223) >>> 0; return seed / 4294967296; };
const N_AIR = 1400;
const world = [];
for (let i = 0; i < N_AIR; i++) {
  const k = i < N_AIR * 0.86 ? "aircraft" : i < N_AIR * 0.95 ? "vessel" : i < N_AIR * 0.98 ? "aprs" : "sonde";
  const hex = (0x400000 + i * 977).toString(16).toUpperCase().padStart(6, "0");
  world.push({
    kind: k, id: k === "vessel" ? String(244000000 + i * 31) : k === "aircraft" ? hex : k === "aprs" ? "OZ" + i + "-9" : "S" + (1000 + i),
    lat: 35 + rnd() * 25, lon: -10 + rnd() * 40, hdg: rnd() * 360,
    spd: k === "aircraft" ? 250 + rnd() * 250 : k === "vessel" ? 5 + rnd() * 15 : k === "sonde" ? 5 : 20,
    alt: k === "aircraft" ? 3000 + rnd() * 35000 : 0, path: [],
  });
}
world.sort((a, b) => a.lon - b.lon);
let lastStep = Date.now();
function stepWorld() {
  const now = Date.now(), dt = (now - lastStep) / 1000; lastStep = now;
  for (const w of world) {
    const nm = w.spd * dt / 3600, r = w.hdg * Math.PI / 180;
    w.lat += nm / 60 * Math.cos(r); w.lon += nm / 60 * Math.sin(r) / Math.cos(w.lat * Math.PI / 180);
    w.path.push([+w.lat.toFixed(5), +w.lon.toFixed(5)]);
    if (w.path.length > 60) w.path.shift();
  }
}
const stations = [];
for (let i = 0; i < N_ST; i++) {
  const n = 150 + Math.floor(rnd() * 150);
  const from = Math.floor(rnd() * (world.length - n));
  stations.push({ id: "sim-station-" + String(i).padStart(3, "0"), from, n, every: 2000 + Math.floor(rnd() * 3000) });
}
function payload(st) {
  const tracks = [];
  for (const w of world.slice(st.from, st.from + st.n)) {
    const t = {
      kind: w.kind, id: w.id, label: w.kind === "aircraft" ? "SIM" + w.id.slice(-3) : w.id, sub: w.kind === "aircraft" ? "ADS-B" : "AIS",
      lat: +w.lat.toFixed(6), lon: +w.lon.toFixed(6), age: +(rnd() * 5).toFixed(1), heading: +w.hdg.toFixed(1), speed: +w.spd.toFixed(1),
      src: w.kind === "aircraft" ? "adsb" : w.kind === "vessel" ? "ais" : w.kind, cat: w.kind === "aircraft" ? "A3" : "", extra: "",
    };
    if (w.kind === "aircraft") { t.icao = w.id; t.alt = Math.round(w.alt); t.vrate = 0; t.squawk = "1000"; }
    if (w.kind === "vessel") t.mmsi = w.id;
    t.path = w.path.map(([la, lo]) => [la, lo]);
    tracks.push(t);
  }
  return JSON.stringify({ station: st.id, tracks });
}

// ---- measuring ----
const m = { ingestN: 0, ingest429: 0, ingestBytesIn: 0, ingestBytesOut: 0, liveN: 0, liveBytesOut: 0, wsBytesOut: 0, wsMsgs: 0, ingestMs: [], liveMs: [] };
let measuring = false;
let stop = false;
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const pct = (a, p) => { if (!a.length) return 0; const s = [...a].sort((x, y) => x - y); return s[Math.min(s.length - 1, Math.floor(s.length * p))]; };

async function stationLoop(st) {
  await sleep(rnd() * st.every);
  while (!stop) {
    const body = payload(st);
    const t0 = performance.now();
    try {
      const r = await fetch(base + "/ingest", { method: "POST", body, headers: { "content-type": "application/json" } });
      const txt = await r.text();
      if (measuring) {
        m.ingestN++; if (r.status === 429) m.ingest429++;
        m.ingestBytesIn += Buffer.byteLength(body); m.ingestBytesOut += Buffer.byteLength(txt); m.ingestMs.push(performance.now() - t0);
      }
    } catch { /* hub busy */ }
    await sleep(st.every);
  }
}
async function liveLoop() {
  await sleep(rnd() * 5000);
  while (!stop) {
    const t0 = performance.now();
    try {
      const r = await fetch(base + "/live");
      const txt = await r.text();
      if (measuring) { m.liveN++; m.liveBytesOut += Buffer.byteLength(txt); m.liveMs.push(performance.now() - t0); }
    } catch { /* ignore */ }
    await sleep(5000);
  }
}
const sockets = [];
function wsViewer() {
  const ws = new WebSocket(`ws://127.0.0.1:${PORT}/ws`);
  ws.on("message", (d) => { if (measuring) { m.wsMsgs++; m.wsBytesOut += d.length; } });
  ws.on("error", () => {});
  sockets.push(ws);
}

async function waitUp() {
  for (let i = 0; i < 120; i++) {
    try { const r = await fetch(base + "/__stats"); if (r.ok) return; } catch { /* starting */ }
    await sleep(500);
  }
  throw new Error("wrangler did not start");
}

await waitUp();
const worldTimer = setInterval(stepWorld, 1000);
for (const st of stations) stationLoop(st);
for (let i = 0; i < N_LIVE; i++) liveLoop();
for (let i = 0; i < N_WS; i++) wsViewer();
console.error(`warming up ${WARM}s ...`);
await sleep(WARM * 1000);
await fetch(base + "/__stats?reset=1").then((r) => r.text());
const cpu0 = cpuMs();
measuring = true;
const t0 = Date.now();
console.error(`measuring ${SECS}s ...`);
await sleep(SECS * 1000);
measuring = false;
const elapsedMin = (Date.now() - t0) / 60000;
const cpu1 = cpuMs();
const S = await fetch(base + "/__stats").then((r) => r.json());
stop = true; clearInterval(worldTimer);
for (const ws of sockets) try { ws.terminate(); } catch { /* closed */ }

const per = (x) => Math.round(x / elapsedMin);
const doReq = S.doFetch + S.alarms;
const report = {
  tree: TREE, secs: SECS, stations: N_ST, liveViewers: N_LIVE, wsViewers: N_WS,
  perMinute: {
    ingestPosts: per(m.ingestN), ingest429: per(m.ingest429), liveGets: per(m.liveN),
    doRequests: per(doReq), doFetches: per(S.doFetch), doAlarms: per(S.alarms),
    sqlStatements: per(S.stmts), sqlRowsRead: per(S.rowsRead), sqlRowsWritten: per(S.rowsWritten),
    bytesIn: per(m.ingestBytesIn), bytesOut: per(m.ingestBytesOut + m.liveBytesOut + m.wsBytesOut),
    wsMessages: per(m.wsMsgs),
  },
  cpuMsPerDoRequest: +((cpu1 - cpu0) / Math.max(1, doReq)).toFixed(2),
  cpuMsPerMinute: Math.round((cpu1 - cpu0) / elapsedMin),
  latencyMs: { ingestP50: +pct(m.ingestMs, 0.5).toFixed(1), ingestP95: +pct(m.ingestMs, 0.95).toFixed(1), liveP50: +pct(m.liveMs, 0.5).toFixed(1), liveP95: +pct(m.liveMs, 0.95).toFixed(1) },
  bytesPerResponse: { live: Math.round(m.liveBytesOut / Math.max(1, m.liveN)), ingestIn: Math.round(m.ingestBytesIn / Math.max(1, m.ingestN)) },
};
console.log(JSON.stringify(report, null, 2));
killAll();
setTimeout(() => process.exit(0), 500);
