// OnAir directory Worker: servers register themselves with Ed25519-signed heartbeats; a public JSON API lists them; the static page in
// public/ shows them. Storage: D1 (schema.sql). The first registration of an id fixes its public key; everything after must be signed by it.
//
// Safety rules in this file:
//  * the directory never fetches an address a visitor sends. The only outgoing requests are (a) DNS-over-HTTPS lookups of a host name to a
//    fixed resolver, and (b) the opt-in reachability probe: GET <registered https/http url>/web/v1/info, redirects not followed, 8 KB read
//    at most, answer reduced to one yes/no. Both only for URLs that passed validateUrl and resolve to public addresses.
//  * no IP address is stored; rate limits use a salted hash that expires within a day.
import {
  LIMITS, validateRecord, validateUrl, canonicalize, parseLoc, expiresAt, windowIndex, filterServers, isPublicIp, timingSafeEqualStr, byteLen,
} from "./logic.js";
import { verifySig, sha256Hex } from "./crypto.js";

const JSON_HEADERS = { "content-type": "application/json; charset=utf-8", "cache-control": "no-store", "x-content-type-options": "nosniff" };
const CORS = { "access-control-allow-origin": "*", "access-control-allow-methods": "GET, OPTIONS", "access-control-max-age": "86400" };

const json = (obj, status = 200, extra = {}) => new Response(JSON.stringify(obj), { status, headers: { ...JSON_HEADERS, ...extra } });
const err = (status, message, extra = {}) => json({ ok: false, error: message }, status, extra);
const publicJson = (obj) => json(obj, 200, { ...CORS, "cache-control": "public, max-age=20" });

// ---- rate limits (fixed window in D1) ---------------------------------------------------------------------------------------------------
async function hit(env, key, limit, windowSec, now) {
  const win = windowIndex(now, windowSec);
  const row = await env.DB.prepare(
    "INSERT INTO rl(k, win, n, exp) VALUES(?1, ?2, 1, ?3) ON CONFLICT(k) DO UPDATE SET n = CASE WHEN win = excluded.win THEN n + 1 ELSE 1 END, win = excluded.win, exp = excluded.exp RETURNING n"
  ).bind(key, win, now + windowSec * 2).first();
  return (row ? row.n : 1) <= limit;
}
// Without the IP_SALT secret the hash must not be a fixed function of the address (a fixed one can be reversed over the whole IPv4 range from
// the rows of the rl table): a random salt per Worker instance is used instead, so the rate limits still work but the keys mean nothing later.
let fallbackSalt = null;
function ipSalt(env) {
  if (env.IP_SALT) return env.IP_SALT;
  if (!fallbackSalt) fallbackSalt = Array.from(crypto.getRandomValues(new Uint8Array(16)), (b) => b.toString(16).padStart(2, "0")).join("");
  return fallbackSalt;
}
async function ipKey(env, request) {
  const ip = request.headers.get("cf-connecting-ip") || "unknown";
  return (await sha256Hex(ipSalt(env) + "|" + ip)).slice(0, 24);
}

// ---- DNS and the reachability probe ----------------------------------------------------------------------------------------------------
// Resolves a host name through a fixed DoH resolver; every A / AAAA answer must be a public address (an answer-less name is refused too).
export async function resolvesPublic(host, fetchFn = fetch) {
  let any = false;
  for (const type of ["A", "AAAA"]) {
    let r;
    try {
      r = await fetchFn(`https://cloudflare-dns.com/dns-query?name=${encodeURIComponent(host)}&type=${type}`, { headers: { accept: "application/dns-json" }, redirect: "manual" });
    } catch { return false; }
    if (!r.ok) return false;
    let j;
    try { j = await r.json(); } catch { return false; }
    for (const a of j.Answer || []) {
      if (a.type === 1 || a.type === 28) { any = true; if (!isPublicIp(String(a.data))) return false; }
    }
  }
  return any;
}

// The probe fetches a path that the directory itself fixes; the result is a boolean, never the content.
export async function probeReachable(url, fetchFn = fetch) {
  const u = validateUrl(url);
  if (!u.ok) return false;
  try {
    const ctl = new AbortController();
    const t = setTimeout(() => ctl.abort(), 4000);
    const r = await fetchFn(u.url + "web/v1/info", { redirect: "manual", signal: ctl.signal, headers: { accept: "application/json", "user-agent": "OnAir-Directory-Check/1" } });
    clearTimeout(t);
    if (r.status !== 200) return false;
    const text = (await r.text()).slice(0, 8192);
    const j = JSON.parse(text);
    return typeof j.name === "string" && typeof j.proto === "number";
  } catch { return false; }
}

// ---- block list ---------------------------------------------------------------------------------------------------------------------------
async function isBlocked(env, rec, host) {
  const rows = await env.DB.prepare("SELECT 1 AS x FROM blocks WHERE (kind='id' AND value=?1) OR (kind='pub' AND value=?2) OR (kind='host' AND (value=?3 OR ?3 LIKE '%.' || value)) LIMIT 1")
    .bind(rec.id, rec.pub, host || "").first();
  return !!rows;
}

// ---- row <-> API ---------------------------------------------------------------------------------------------------------------------------
function rowToApi(r) {
  return {
    id: r.id, name: r.name, url: r.url, location: r.loc, lat: r.lat, lon: r.lon, freq: JSON.parse(r.freq || "[]"), radio: r.radio,
    capacity: r.cap, users: r.users, version: r.ver, contact: r.contact, description: r.descr,
    reachable: r.reach_opt ? (r.reach_ok === null ? null : !!r.reach_ok) : null,
    first_seen: r.first_seen, last_seen: r.last_seen, expires_at: r.expires_at,
  };
}

// ---- handlers ---------------------------------------------------------------------------------------------------------------------------
async function readBody(request) {
  const len = Number(request.headers.get("content-length") || "0");
  if (len > LIMITS.maxBody) return { error: err(413, "body too large") };
  const ct = request.headers.get("content-type") || "";
  if (!ct.toLowerCase().startsWith("application/json")) return { error: err(415, "content-type must be application/json") };
  const text = await request.text();
  if (byteLen(text) > LIMITS.maxBody) return { error: err(413, "body too large") };
  try { return { body: JSON.parse(text) }; } catch { return { error: err(400, "body is not JSON") }; }
}

async function heartbeat(request, env, ctx, deps) {
  const now = deps.now();
  const ik = await ipKey(env, request);
  if (!(await hit(env, "hb:" + ik, 60, 60, now))) return err(429, "too many requests from this address");
  const rb = await readBody(request);
  if (rb.error) return rb.error;
  const devLocal = env.ONAIR_DEV_LOCAL === "1";   // the local test stack only; absent in wrangler.jsonc
  const v = validateRecord(rb.body, now, devLocal);
  if (!v.ok) return json({ ok: false, error: "invalid", details: v.errors.slice(0, 8) }, 400);
  const rec = v.rec;
  if (!(await verifySig(rec.pub, rb.body.sig, canonicalize(rec)))) return err(401, "signature does not verify");
  const u = rec.action === "heartbeat" ? validateUrl(rec.url, devLocal) : { host: "" };
  if (await isBlocked(env, rec, u.host)) return err(403, "this server or host is blocked");
  if (!(await hit(env, "id:" + rec.id, 8, 300, now))) return err(429, "too many requests for this id");

  const row = await env.DB.prepare("SELECT * FROM servers WHERE id = ?1").bind(rec.id).first();
  if (row && row.pub !== rec.pub) return err(403, "id is registered with another key");
  if (row && rec.ts <= row.last_ts) return err(409, "ts is not newer than the last accepted request");

  // a signed delete leaves a 10-minute tombstone (the ts of the delete, kept in the rl table): a captured heartbeat replayed inside the 5-minute
  // clock window cannot list the server again
  if (!row && rec.action !== "delete") {
    const tomb = await env.DB.prepare("SELECT win FROM rl WHERE k = ?1 AND exp > ?2").bind("tomb:" + rec.id, now).first();
    if (tomb && rec.ts <= tomb.win) return err(409, "ts is not newer than the last accepted request");
  }

  if (rec.action === "delete") {
    if (!row) return err(404, "not registered");
    await env.DB.prepare("DELETE FROM servers WHERE id = ?1").bind(rec.id).run();
    await env.DB.prepare("INSERT OR REPLACE INTO rl(k, win, n, exp) VALUES(?1, ?2, 1, ?3)").bind("tomb:" + rec.id, rec.ts, now + 600).run();
    return json({ ok: true, deleted: true });
  }

  let urlChanged = !row || row.url !== rec.url;
  if (!row) {
    if (!(await hit(env, "new:" + ik, 5, 86400, now))) return err(429, "too many new registrations from this address today");
    const c = await env.DB.prepare("SELECT COUNT(*) AS n FROM servers WHERE expires_at > ?1").bind(now).first();
    if (c && c.n >= Number(env.MAX_SERVERS || 2000)) return err(503, "the directory is full");
  }
  if (urlChanged) {
    const other = await env.DB.prepare("SELECT id FROM servers WHERE url = ?1 AND id != ?2").bind(rec.url, rec.id).first();
    if (other) return err(409, "another server is listed with this url");
    if (!u.literal && !u.devLocal && !(await deps.resolvesPublic(u.host))) return err(422, "url host does not resolve to public addresses only");
  }

  const loc = parseLoc(rec.loc);
  let reachOk = urlChanged ? null : (row ? row.reach_ok : null);
  let reachAt = row ? row.reach_at : 0;
  if (rec.reach && (urlChanged || now - reachAt >= 3600)) {
    reachOk = (await deps.probeReachable(rec.url)) ? 1 : 0;
    reachAt = now;
  }
  if (!rec.reach) { reachOk = null; reachAt = 0; }
  await env.DB.prepare(
    `INSERT INTO servers(id, pub, name, url, host, loc, lat, lon, freq, radio, cap, users, ver, contact, descr, reach_opt, reach_ok, reach_at, interval_s, first_seen, last_seen, last_ts, expires_at)
     VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,?18,?19,?20,?20,?21,?22)
     ON CONFLICT(id) DO UPDATE SET name=excluded.name, url=excluded.url, host=excluded.host, loc=excluded.loc, lat=excluded.lat, lon=excluded.lon, freq=excluded.freq,
       radio=excluded.radio, cap=excluded.cap, users=excluded.users, ver=excluded.ver, contact=excluded.contact, descr=excluded.descr, reach_opt=excluded.reach_opt,
       reach_ok=excluded.reach_ok, reach_at=excluded.reach_at, interval_s=excluded.interval_s, last_seen=excluded.last_seen, last_ts=excluded.last_ts, expires_at=excluded.expires_at`
  ).bind(rec.id, rec.pub, rec.name, rec.url, u.host, rec.loc, loc ? loc.lat : null, loc ? loc.lon : null, JSON.stringify(rec.freq), rec.radio, rec.cap, rec.users, rec.ver,
    rec.contact, rec.desc, rec.reach, reachOk, reachAt, rec.interval, now, rec.ts, expiresAt(now, rec.interval)).run();
  return json({ ok: true, new: !row, expires_at: expiresAt(now, rec.interval), next_sec: rec.interval });
}

async function listServers(url, env, now) {
  const { results } = await env.DB.prepare("SELECT * FROM servers WHERE expires_at > ?1 ORDER BY users DESC, name LIMIT 2100").bind(now).all();
  const p = url.searchParams;
  const num = (k) => (p.has(k) && p.get(k) !== "" ? Number(p.get(k)) : NaN);
  const rows = filterServers((results || []).map(rowToApi), {
    band: p.get("band") || "", freq_mhz: p.get("freq_mhz") || "", q: p.get("q") || "", lat: num("lat"), lon: num("lon"), radius_km: num("radius_km"), free: p.get("free") === "1",
  });
  const limit = Math.min(500, Math.max(1, Number(p.get("limit")) || 200));
  return publicJson({ ok: true, count: rows.length, servers: rows.slice(0, limit), generated: now });
}

async function report(request, env, deps) {
  const now = deps.now();
  const ik = await ipKey(env, request);
  if (!(await hit(env, "rep:" + ik, 5, 3600, now))) return err(429, "too many reports from this address");
  if (!(await hit(env, "repall", 200, 3600, now))) return err(429, "too many reports");
  const rb = await readBody(request);
  if (rb.error) return rb.error;
  const b = rb.body || {};
  const reason = ["abuse", "illegal", "offline", "wrong-info", "other"].includes(b.reason) ? b.reason : null;
  if (typeof b.id !== "string" || !/^[a-z0-9][a-z0-9-]{7,31}$/.test(b.id) || !reason) return err(400, "id and reason (abuse, illegal, offline, wrong-info, other) are required");
  const text = typeof b.text === "string" ? b.text.replace(/[\u0000-\u001f\u007f]/g, " ").slice(0, 500) : "";
  const exists = await env.DB.prepare("SELECT 1 AS x FROM servers WHERE id = ?1").bind(b.id).first();
  if (!exists) return err(404, "no such server");
  await env.DB.prepare("INSERT INTO reports(server_id, reason, text, at) VALUES(?1, ?2, ?3, ?4)").bind(b.id, reason, text, now).run();
  return json({ ok: true });
}

// ---- admin (secret token in the Worker secret ADMIN_TOKEN) ----------------------------------------------------------------------------
async function admin(request, env, url, deps) {
  const now = deps.now();
  const ik = await ipKey(env, request);
  if (!env.ADMIN_TOKEN) return err(404, "not found");
  if (!(await hit(env, "adm:" + ik, 30, 60, now))) return err(429, "too many requests");
  const auth = request.headers.get("authorization") || "";
  if (!auth.startsWith("Bearer ") || !timingSafeEqualStr(auth.slice(7), env.ADMIN_TOKEN)) {
    await hit(env, "admfail:" + ik, 1000, 600, now);
    return err(401, "unauthorized");
  }
  const parts = url.pathname.split("/").filter(Boolean);   // api v1 admin <what> [<id>]
  const what = parts[3], arg = parts[4];
  if (what === "servers" && request.method === "DELETE" && arg) {
    const r = await env.DB.prepare("DELETE FROM servers WHERE id = ?1").bind(arg).run();
    return json({ ok: true, deleted: r.meta ? r.meta.changes : 0 });
  }
  if (what === "block" && request.method === "POST") {
    const rb = await readBody(request);
    if (rb.error) return rb.error;
    const { kind, value, note } = rb.body || {};
    if (!["host", "id", "pub"].includes(kind) || typeof value !== "string" || !value || value.length > 253) return err(400, "kind (host, id, pub) and value are required");
    const v = kind === "host" ? value.toLowerCase() : value;
    await env.DB.prepare("INSERT OR REPLACE INTO blocks(kind, value, note, at) VALUES(?1, ?2, ?3, ?4)").bind(kind, v, String(note || "").slice(0, 200), now).run();
    const col = { host: "host", id: "id", pub: "pub" }[kind];
    const r = kind === "host"
      ? await env.DB.prepare("DELETE FROM servers WHERE host = ?1 OR host LIKE '%.' || ?1").bind(v).run()
      : await env.DB.prepare(`DELETE FROM servers WHERE ${col} = ?1`).bind(v).run();
    return json({ ok: true, removed: r.meta ? r.meta.changes : 0 });
  }
  if (what === "block" && request.method === "DELETE") {
    const kind = url.searchParams.get("kind"), value = url.searchParams.get("value");
    await env.DB.prepare("DELETE FROM blocks WHERE kind = ?1 AND value = ?2").bind(kind, value).run();
    return json({ ok: true });
  }
  if (what === "reports" && request.method === "GET") {
    const { results } = await env.DB.prepare("SELECT * FROM reports ORDER BY n DESC LIMIT 200").all();
    return json({ ok: true, reports: results });
  }
  if (what === "reports" && request.method === "DELETE" && arg) {
    await env.DB.prepare("DELETE FROM reports WHERE n = ?1").bind(Number(arg)).run();
    return json({ ok: true });
  }
  return err(404, "not found");
}

// ---- router ---------------------------------------------------------------------------------------------------------------------------
const defaultDeps = { now: () => Math.floor(Date.now() / 1000), resolvesPublic, probeReachable };

export async function handle(request, env, ctx, deps = defaultDeps) {
  const url = new URL(request.url);
  const p = url.pathname;
  if (!p.startsWith("/api/")) return env.ASSETS.fetch(request);
  try {
    if (request.method === "OPTIONS") return new Response(null, { status: 204, headers: { ...CORS, "access-control-allow-headers": "content-type" } });
    if (p === "/api/v1/servers" && request.method === "GET") return await listServers(url, env, deps.now());
    const m = /^\/api\/v1\/servers\/([a-z0-9-]{8,32})$/.exec(p);
    if (m && request.method === "GET") {
      const r = await env.DB.prepare("SELECT * FROM servers WHERE id = ?1 AND expires_at > ?2").bind(m[1], deps.now()).first();
      return r ? publicJson({ ok: true, server: rowToApi(r) }) : err(404, "no such server", CORS);
    }
    if (p === "/api/v1/heartbeat" && request.method === "POST") return await heartbeat(request, env, ctx, deps);
    if (p === "/api/v1/report" && request.method === "POST") return await report(request, env, deps);
    if (p.startsWith("/api/v1/admin/")) return await admin(request, env, url, deps);
    if (p === "/api/v1/health") return publicJson({ ok: true, proto: 1 });
    return err(404, "not found", CORS);
  } catch (e) {
    return err(500, "internal error");
  }
}

export default {
  fetch: (request, env, ctx) => handle(request, env, ctx),
  async scheduled(_event, env) {
    const now = Math.floor(Date.now() / 1000);
    await env.DB.prepare("DELETE FROM servers WHERE expires_at < ?1").bind(now - 86400).run();
    await env.DB.prepare("DELETE FROM rl WHERE exp < ?1").bind(now).run();
    await env.DB.prepare("DELETE FROM reports WHERE at < ?1").bind(now - 90 * 86400).run();
  },
};
