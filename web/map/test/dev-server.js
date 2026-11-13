// dev-server.js - run the map page locally against the REAL hub rules, without Cloudflare.
//   node test/dev-server.js --serve [--port 8791] [--seed] [--still]
// Serves public/, takes POST /ingest through the same sanitize() + TrackBook the Durable Object uses, and answers /live (full and ?since=),
// /meta, /trail(s), /jam, /msgs, /api/by. `--seed` posts the demo tracks plus a set of ham contacts exactly as the desktop app sends them
// (map_send.cpp: kind "ham", extra = ham_mapfeed.cpp JSON). Not used in production; tests import {createHub} to drive the same code.
import http from "node:http";
import fs from "node:fs";
import path from "node:path";
import os from "node:os";
import crypto from "node:crypto";
import { fileURLToPath } from "node:url";
import { TrackBook, sanitize } from "../src/track-book.js";
import { BadgeService } from "../src/badge-service.js";
import { MemoryBadgeBackend } from "../src/badge-stats.js";
import { makeToken, webSecret, tokenHash, cookie, SESSION_COOKIE } from "../src/auth.js";
import { fromLocator } from "../src/badge-geo.js";

const PUB = process.env.MAP_PUBLIC || path.join(path.dirname(fileURLToPath(import.meta.url)), "..", "public");   // MAP_PUBLIC: serve another copy of public/ (to compare versions)
const MIME = { ".html": "text/html", ".js": "text/javascript", ".css": "text/css", ".png": "image/png", ".svg": "image/svg+xml", ".json": "application/json" };

/** The hub's ingest + /live logic without the Worker shell. `now` is injectable so tests can move the clock. */
export function createHub(clock = () => Date.now()) {
  const book = new TrackBook(clock());
  return {
    book,
    ingest(body, station = "devstation1") {
      const tracks = Array.isArray(body.tracks) ? body.tracks : [];
      if (tracks.length > 250) return { status: 413, body: { error: "too many" } };   // the real hub refuses a bigger post
      const now = clock();
      let kept = 0;
      const rows = [];
      for (const raw of tracks) {
        const t = sanitize(raw, now, station, typeof body.by === "string" ? body.by : "");
        if (!t) continue;
        if (book.accept(t, now)) { kept++; rows.push(t); }
      }
      return { status: 200, body: { ok: true, kept, n: tracks.length }, rows };
    },
    live(since) {
      const now = clock();
      book.expire(now);
      if (since != null) {
        const parts = book.deltaParts(Number(since));
        if (parts) return { now, version: book.version, delta: true, stations: 1, tracks: JSON.parse(parts.tracks), removed: parts.removed };
      }
      return { now, version: book.version, stations: 1, tracks: JSON.parse(book.tracksJson()) };
    },
  };
}

/** Ham contacts as map_send.cpp posts them: id/label = the station callsign, sub "ham MODE", extra = the site's hints JSON. */
export function sampleHamRaw(ageSec = 20) {
  const mk = (call, mode, f, to, tlat, tlon, lat, lon, age) => ({
    kind: "ham", id: call, label: call, sub: "ham " + mode, src: "ham", lat, lon, age: age ?? ageSec,
    extra: `{"m":"${mode}","f":${f}${f > 0 ? `,"b":"${f < 15e6 ? "20m" : "15m"}"` : ""},"to":"${to}"${tlat != null ? `,"tlat":${tlat},"tlon":${tlon}` : ""}}`,
  });
  return [
    mk("LY2XX", "FT8", 14075200, "DL1ABC", 48.5, 11.0, 54.5, 25.0),
    mk("DL1ABC", "FT8", 14076100, "LY2XX", 54.5, 25.0, 48.5, 11.0),
    mk("W1AW", "FT4", 14081000, "EA8ZZ", 28.5, -16.0, 41.5, -73.0),
    mk("JA1YY", "WSPR", 14097100, "VK3AA", -37.5, 145.0, 35.5, 139.0, 135),   // WSPR: known only after its 2 minute slot, so older than 120 s on arrival
    mk("OH2BB", "JS8", 7078000, "SM5CC", 59.5, 17.0, 60.5, 25.0),
    mk("ZL1QQ", "QSO", 21300000, "KH6WW", 21.5, -158.0, -37.5, 175.0, 1500),
  ];
}

const CACHE = path.join(os.tmpdir(), "onair-map-dev-cache");
/** Fetch once, keep on disk (OSM tiles for /tiles, maplibre for /vendor), so repeated runs and screenshots need no network. */
async function cached(key, url) {
  const f = path.join(CACHE, key.replace(/[^a-zA-Z0-9._-]/g, "_"));
  if (fs.existsSync(f)) return fs.readFileSync(f);
  const r = await fetch(url, { headers: { "User-Agent": "OnAirMapDev/1.0 (local development)" } });
  if (!r.ok) throw new Error(String(r.status));
  const b = Buffer.from(await r.arrayBuffer());
  fs.mkdirSync(CACHE, { recursive: true });
  fs.writeFileSync(f, b);
  return b;
}
const VENDOR = { "maplibre-gl.js": "https://unpkg.com/maplibre-gl@5.7.3/dist/maplibre-gl.js", "maplibre-gl.css": "https://unpkg.com/maplibre-gl@5.7.3/dist/maplibre-gl.css" };

/** Badges in the dev server: memory storage, a dev-only secret, and /auth/dev?login=NAME instead of GitHub (never in the Worker). */
const DEV_SECRET = "dev-secret-not-for-production";
export function createBadges(port) {
  return new BadgeService(new MemoryBadgeBackend(), { SESSION_SECRET: DEV_SECRET, MAP_ORIGINS: `http://localhost:${port},http://127.0.0.1:${port}` });
}

export function start(port = 8791, seed = false, live = true) {
  const hub = createHub();
  const badges = createBadges(port);
  const toRequest = (req, body) => new Request("http://localhost:" + port + req.url, { method: req.method, headers: Object.entries(req.headers).filter(([, v]) => typeof v === "string"), body: body && req.method !== "GET" ? body : undefined });
  const sendResponse = async (res, r) => {
    const h = {};
    r.headers.forEach((v, k) => { if (k !== "set-cookie") h[k] = v; });
    const sc = r.headers.getSetCookie();
    if (sc.length) h["set-cookie"] = sc;
    res.writeHead(r.status, h);
    res.end(Buffer.from(await r.arrayBuffer()));
  };
  const credit = (req, r, body, station) => {
    if (!req.headers.authorization) return Promise.resolve();
    return badges.fromBearer(toRequest(req)).then((who) => {
      if (!who) return;
      if (who.error) { r.body.account_error = who.error; return; }
      const home = typeof body.home === "string" ? fromLocator(body.home) : null;
      Object.assign(r.body, badges.creditPost(who, { station, rows: r.rows || [], prev: new Map(), home, extraKeys: [], now: Date.now() }));
    });
  };
  if (seed) {   // a demo account with a public profile, so /u/demo-station and /leaders show something
    badges.book.setSettings(1001, "demo-station", { badges: true, public: true, by: "DemoRx" });
    badges.claims.set("demorx", 1001);
    setTimeout(() => {
      const r = hub.ingest({ tracks: hub.live().tracks.filter((t) => t.kind !== "fix").slice(0, 200).map((t) => ({ ...t, id: t.id.split(":").slice(1).join(":"), age: 0 })) }, "demo-station-01");
      badges.creditPost({ uid: 1001, login: "demo-station" }, { station: "demo-station-01", rows: r.rows || [], prev: new Map(), home: fromLocator("KO24pq"), extraKeys: [], now: Date.now() });
    }, 1500).unref();
  }
  const sockets = new Set();
  const frame = (str) => { const payload = Buffer.from(str); const head = payload.length < 126 ? Buffer.from([0x81, payload.length]) : payload.length < 65536 ? Buffer.from([0x81, 126, payload.length >> 8, payload.length & 255]) : (() => { const b = Buffer.alloc(10); b[0] = 0x81; b[1] = 127; b.writeBigUInt64BE(BigInt(payload.length), 2); return b; })(); return Buffer.concat([head, payload]); };
  const push = (tracks) => { const f = frame(JSON.stringify({ type: "upsert", tracks })); for (const so of sockets) so.write(f); };
  if (seed) {
    hub.ingest({ tracks: sampleHamRaw() });
    import("../public/demo.js").then(({ generateDemo }) => {
      const { tracks } = generateDemo(Date.now());
      const raw = tracks.filter((t) => t.kind !== "doa" && t.kind !== "fix").map((t) => ({ ...t, id: t.id.split(":").slice(1).join(":"), path: undefined, age: 0 }));
      for (let i = 0; i < raw.length; i += 200) hub.ingest({ tracks: raw.slice(i, i + 200) });
      console.log("seeded", hub.book.size, "tracks");
      // keep the seed fresh (a real station re-reports every few seconds); the ham contacts keep their original ages
      if (live) setInterval(() => {
        for (const r of raw) {   // move aircraft and ships along their heading, climb or descend a little: gives the page real history
          if (!Number.isFinite(r.heading) || !r.speed || (r.kind !== "aircraft" && r.kind !== "vessel")) continue;
          const nm = (r.speed * 8) / 3600, k = Math.PI / 180;
          r.lat += (nm / 60) * Math.cos(r.heading * k); r.lon += (nm / 60) * Math.sin(r.heading * k) / Math.max(0.2, Math.cos(r.lat * k));
          if (r.kind === "aircraft" && Number.isFinite(r.alt)) r.alt = Math.max(0, r.alt + (r.vrate || 0) * 8 / 60 + Math.sin(Date.now() / 20000 + r.lat) * 40);
        }
        hub.ingest({ tracks: sampleHamRaw().map((h, i) => ({ ...h, age: [20, 40, 120, 135, 300, 1500][i] })) });
        for (let i = 0; i < raw.length; i += 200) hub.ingest({ tracks: raw.slice(i, i + 200) });
        push(hub.live().tracks);   // the real hub sends only the changed rows; this sends all, which keeps the page fresh
      }, 8000).unref();
    });
  }
  const server = http.createServer((req, res) => {
    const u = new URL(req.url, "http://x");
    const json = (o, status = 200) => { res.writeHead(status, { "Content-Type": "application/json", "Cache-Control": "no-store" }); res.end(JSON.stringify(o)); };
    if (u.pathname === "/ingest" && req.method === "POST") {
      let b = "";
      req.on("data", (c) => (b += c));
      req.on("end", () => {
        let body; try { body = JSON.parse(b); } catch { return json({ error: "bad json" }, 400); }
        const st = String(body.station || "devstation1");
        const r = hub.ingest(body, st);
        credit(req, r, body, st).then(() => json(r.body, r.status));
      });
      return;
    }
    if (u.pathname === "/auth/dev") {   // dev only: sign in as ?login=NAME (uid from the name) without GitHub
      const login = (u.searchParams.get("login") || "alice").replace(/[^A-Za-z0-9-]/g, "").slice(0, 39) || "alice";
      const uid = 100000 + [...login].reduce((a, c) => (a * 31 + c.charCodeAt(0)) % 900000, 7);
      const nowSec = Math.floor(Date.now() / 1000);
      makeToken(webSecret(DEV_SECRET), { uid, login, iat: nowSec, exp: nowSec + 86400 }).then(async (tok) => {
        badges.db.tokenPut(await tokenHash(tok), uid, "web", nowSec + 86400);
        badges.book.user(uid, login);
        res.writeHead(302, { Location: "/u/" + login, "Set-Cookie": cookie(SESSION_COOKIE, tok, 86400) });
        res.end();
      });
      return;
    }
    if (u.pathname.startsWith("/auth/") || u.pathname === "/api/me" || u.pathname.startsWith("/api/me/") || u.pathname.startsWith("/api/u/") || u.pathname === "/api/leaderboard") {
      let b = "";
      req.on("data", (c) => (b += c));
      req.on("end", async () => {
        const rq = toRequest(req, b);
        const r = await badges.handle(rq, new URL(rq.url));
        if (!r) { res.writeHead(404); return res.end(); }
        await sendResponse(res, r);
      });
      return;
    }
    if (/^\/u\/[A-Za-z0-9-]{1,39}\/?$/.test(u.pathname) || u.pathname === "/leaders") {
      res.writeHead(200, { "Content-Type": "text/html", "Cache-Control": "no-store" });
      return res.end(fs.readFileSync(path.join(PUB, u.pathname === "/leaders" ? "leaders.html" : "profile.html")));
    }
    if (u.pathname === "/live") return json(hub.live(u.searchParams.get("since")));
    if (u.pathname === "/meta") return json({});
    if (u.pathname === "/trail" || u.pathname === "/trails") return json({ now: Date.now(), trails: {} });
    if (u.pathname === "/jam") return json({ cells: [] });
    if (u.pathname === "/msgs") return json({ msgs: [] });
    if (u.pathname.startsWith("/api/by/")) return json({ users: [], tracks: [] });
    if (u.pathname === "/ws") { res.writeHead(404); return res.end(); }
    const tile = u.pathname.match(/^\/tiles\/(\d{1,2})\/(\d+)\/(\d+)\.png$/);
    if (tile) {
      cached(`tile_${tile[1]}_${tile[2]}_${tile[3]}.png`, `https://tile.openstreetmap.org/${tile[1]}/${tile[2]}/${tile[3]}.png`)
        .then((b) => { res.writeHead(200, { "Content-Type": "image/png", "Cache-Control": "max-age=3600" }); res.end(b); })
        .catch(() => { res.writeHead(404); res.end(); });
      return;
    }
    const vend = u.pathname.match(/^\/vendor\/(maplibre-gl\.(?:js|css))$/);
    if (vend) {
      cached("vendor_" + vend[1], VENDOR[vend[1]])
        .then((b) => { res.writeHead(200, { "Content-Type": vend[1].endsWith("js") ? "text/javascript" : "text/css" }); res.end(b); })
        .catch(() => { res.writeHead(502); res.end(); });
      return;
    }
    if (u.pathname === "/icon-sheet") { res.writeHead(200, { "Content-Type": "text/html" }); return res.end(fs.readFileSync(path.join(path.dirname(fileURLToPath(import.meta.url)), "icon-sheet.html"))); }
    if (u.pathname === "/" || u.pathname === "/index.html") {
      let html = fs.readFileSync(path.join(PUB, "index.html"), "utf8").replace(/https:\/\/unpkg\.com\/maplibre-gl@5\.7\.3\/dist\//g, "/vendor/");
      // ?shot=1 (screenshots): the consent popup is hidden and nothing is requested from third parties; ?theme=dark|light|auto sets the theme before the page starts
      if (u.searchParams.has("shot")) {
        const th = u.searchParams.get("theme");
        const js = `try{${th ? `localStorage.setItem("onair.map.theme",${JSON.stringify(th === "dark" || th === "light" ? th : "auto")});` : ""}}catch(e){}`;
        html = html.replace("<head>", "<head><script>" + js + "</script><style>.consent{display:none!important}</style>");   // the photo popup would cover the map; no choice is stored, so no photo is requested
        if (u.searchParams.get("sel")) {   // select a track a few seconds after start (what a click in the list does)
          const id = JSON.stringify(u.searchParams.get("sel"));
          html = html.replace("</body>", `<script>setTimeout(function(){var b=document.querySelector('#list button[data-id='+JSON.stringify(${id})+']');if(b)b.click()},4000)</script></body>`);
        }
        if (u.searchParams.get("css")) html = html.replace("</head>", "<style>" + u.searchParams.get("css").replace(/[<]/g, "") + "</style></head>");   // debugging aid: extra CSS
      }
      res.writeHead(200, { "Content-Type": "text/html", "Cache-Control": "no-store" });
      return res.end(html);
    }
    let f = path.join(PUB, u.pathname === "/" ? "index.html" : u.pathname);
    if (!f.startsWith(PUB) || !fs.existsSync(f) || fs.statSync(f).isDirectory()) { res.writeHead(404); return res.end("not found"); }
    res.writeHead(200, { "Content-Type": MIME[path.extname(f)] || "application/octet-stream", "Cache-Control": "no-store" });
    fs.createReadStream(f).pipe(res);
  });
  // minimal WebSocket (no dependency): the handshake, then the same "hello" snapshot the Durable Object sends on connect
  server.on("upgrade", (req, socket) => {
    if (new URL(req.url, "http://x").pathname !== "/ws") return socket.destroy();
    const accept = crypto.createHash("sha1").update(req.headers["sec-websocket-key"] + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").digest("base64");
    socket.write("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " + accept + "\r\n\r\n");
    socket.write(frame(JSON.stringify({ type: "hello", ...hub.live(), load: { pct: 8, viewers: 1, stations: 1, tracks: hub.book.size } })));
    sockets.add(socket);
    socket.on("close", () => sockets.delete(socket));
    socket.on("error", () => {});
  });
  server.listen(port, () => console.log("map dev server on http://localhost:" + port));
  return server;
}

if (process.argv[1] === fileURLToPath(import.meta.url) && process.argv.includes("--serve")) {   // `node --test test/` loads this file too: it must not start a server
  const i = process.argv.indexOf("--port");
  start(i > 0 ? Number(process.argv[i + 1]) : 8791, process.argv.includes("--seed"), !process.argv.includes("--still"));   // --still: no periodic updates (steady screenshots)
}
