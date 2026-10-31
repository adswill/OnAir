// dev-server.js - run the map page locally against the REAL hub rules, without Cloudflare.
//   node test/dev-server.js --serve [--port 8791] [--seed]
// Serves public/, takes POST /ingest through the same sanitize() + TrackBook the Durable Object uses, and answers /live (full and ?since=),
// /meta, /trail(s), /jam, /msgs, /api/by. `--seed` posts the demo tracks plus a set of ham contacts exactly as the desktop app sends them
// (map_send.cpp: kind "ham", extra = ham_mapfeed.cpp JSON). Not used in production; tests import {createHub} to drive the same code.
import http from "node:http";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { TrackBook, sanitize } from "../src/track-book.js";

const PUB = path.join(path.dirname(fileURLToPath(import.meta.url)), "..", "public");
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
      for (const raw of tracks) {
        const t = sanitize(raw, now, station, "");
        if (!t) continue;
        if (book.accept(t, now)) kept++;
      }
      return { status: 200, body: { ok: true, kept, n: tracks.length } };
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

export function start(port = 8791, seed = false) {
  const hub = createHub();
  if (seed) {
    hub.ingest({ tracks: sampleHamRaw() });
    import("../public/demo.js").then(({ generateDemo }) => {
      const { tracks } = generateDemo(Date.now());
      const raw = tracks.filter((t) => t.kind !== "doa" && t.kind !== "fix").map((t) => ({ ...t, id: t.id.split(":").slice(1).join(":"), path: undefined, age: 0 }));
      for (let i = 0; i < raw.length; i += 200) hub.ingest({ tracks: raw.slice(i, i + 200) });
      console.log("seeded", hub.book.size, "tracks");
    });
  }
  const server = http.createServer((req, res) => {
    const u = new URL(req.url, "http://x");
    const json = (o, status = 200) => { res.writeHead(status, { "Content-Type": "application/json", "Cache-Control": "no-store" }); res.end(JSON.stringify(o)); };
    if (u.pathname === "/ingest" && req.method === "POST") {
      let b = "";
      req.on("data", (c) => (b += c));
      req.on("end", () => { let body; try { body = JSON.parse(b); } catch { return json({ error: "bad json" }, 400); } const r = hub.ingest(body, String(body.station || "devstation1")); json(r.body, r.status); });
      return;
    }
    if (u.pathname === "/live") return json(hub.live(u.searchParams.get("since")));
    if (u.pathname === "/meta") return json({});
    if (u.pathname === "/trail" || u.pathname === "/trails") return json({ now: Date.now(), trails: {} });
    if (u.pathname === "/jam") return json({ cells: [] });
    if (u.pathname === "/msgs") return json({ msgs: [] });
    if (u.pathname.startsWith("/api/by/")) return json({ users: [], tracks: [] });
    if (u.pathname === "/ws") { res.writeHead(404); return res.end(); }
    let f = path.join(PUB, u.pathname === "/" ? "index.html" : u.pathname);
    if (!f.startsWith(PUB) || !fs.existsSync(f) || fs.statSync(f).isDirectory()) { res.writeHead(404); return res.end("not found"); }
    res.writeHead(200, { "Content-Type": MIME[path.extname(f)] || "application/octet-stream", "Cache-Control": "no-store" });
    fs.createReadStream(f).pipe(res);
  });
  server.listen(port, () => console.log("map dev server on http://localhost:" + port));
  return server;
}

if (process.argv[1] === fileURLToPath(import.meta.url) && process.argv.includes("--serve")) {   // `node --test test/` loads this file too: it must not start a server
  const i = process.argv.indexOf("--port");
  start(i > 0 ? Number(process.argv[i + 1]) : 8791, process.argv.includes("--seed"));
}
