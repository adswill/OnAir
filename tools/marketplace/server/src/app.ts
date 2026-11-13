// OnAir plugin marketplace (the handlers; src/index.ts is the Worker entry): one Cloudflare Worker, D1 for the listings, R2 for the package files. See README.md in this folder (setup) and
// docs/PLUGIN-MARKETPLACE-4.0.md (how it works, privacy, moderation, security).
//
// Every answer the app trusts for what to install (the list, a plugin page, a download address with its sha256) is signed with the Ed25519
// key in the SIGNING_KEY secret: {"signed": "<JSON text>", "sig": "<128 hex>"}. The JSON text names the request it answers and the time, so
// the app refuses an answer that was changed on the way, swapped for another request's answer or replayed much later.
import type { D1PreparedStatement, Deps, Env } from "./types.js";
import { ed25519PublicHex, ed25519Sign, makeSession, readSession, saltedHash, sameText, sha256Hex, signingKeyValid, type Session } from "./crypto.js";
import { checkPackage, compareVersions, ManifestError, policyText, validId } from "./manifest.js";

export const API = "/api/v1";
const DEFAULT_MAX_PACKAGE = 25 * 1024 * 1024;
const MAX_UNPACKED = 64 * 1024 * 1024;
const SESSION_DAYS = 30;
const REPORT_REASONS = ["malware", "broken", "licence", "spam", "other"];

// rate limits: [window seconds, count]
export const LIMITS = {
  uploadUser: [86400, 10], uploadIp: [86400, 20],             // accepted uploads
  uploadTryUser: [86400, 60], uploadTryIp: [86400, 120],      // attempts, refused ones included
  likeInstall: [3600, 30], likeIp: [3600, 120],
  reportUser: [86400, 10],
  authIp: [3600, 30], pollIp: [3600, 400],
  downloadCountIp: [3600, 300],
} as const;

// ---------------------------------------------------------------- answers
const JSON_HEADERS = { "content-type": "application/json; charset=utf-8", "cache-control": "no-store", "x-content-type-options": "nosniff" };
function json(status: number, body: unknown, extra: Record<string, string> = {}): Response {
  return new Response(JSON.stringify(body), { status, headers: { ...JSON_HEADERS, ...extra } });
}
function fail(status: number, code: string, error: string): Response { return json(status, { error, code }); }
class HttpError extends Error { constructor(public status: number, public code: string, msg: string) { super(msg); } }

async function signed(env: Env, d: Deps, req: Request, kind: string, payload: Record<string, unknown>): Promise<Response> {
  if (!signingKeyValid(env.SIGNING_KEY)) return fail(503, "not_configured", "The marketplace has no signing key yet.");
  const u = new URL(req.url);
  const text = JSON.stringify({ v: 1, kind, request: u.pathname + u.search, time: d.now(), ...payload });
  return json(200, { signed: text, sig: await ed25519Sign(env.SIGNING_KEY!, text) });
}

// ---------------------------------------------------------------- helpers
const clientIp = (req: Request) => req.headers.get("cf-connecting-ip") || req.headers.get("x-real-ip") || "0.0.0.0";
const salt = (env: Env) => {
  if (!env.HASH_SALT || env.HASH_SALT.length < 16) throw new HttpError(503, "not_configured", "The marketplace is not set up yet (HASH_SALT).");
  return env.HASH_SALT;
};
// true when the request is still inside its limit
export async function rateHit(env: Env, now: number, key: string, [win, limit]: readonly [number, number]): Promise<boolean> {
  const w = Math.floor(now / win);
  const r = await env.DB.prepare(
    "INSERT INTO rl (k, win, n, exp) VALUES (?, ?, 1, ?) ON CONFLICT(k) DO UPDATE SET n = CASE WHEN rl.win = excluded.win THEN rl.n + 1 ELSE 1 END, win = excluded.win, exp = excluded.exp RETURNING n",
  ).bind(key, w, (w + 1) * win).first<{ n: number }>();
  return !!r && r.n <= limit;
}
async function limitOrThrow(env: Env, now: number, key: string, lim: readonly [number, number], what: string) {
  if (!(await rateHit(env, now, key, lim))) throw new HttpError(429, "rate_limited", `Too many ${what} for now. Try again later.`);
}
async function readJson(req: Request, max = 8192): Promise<Record<string, unknown>> {
  const t = await req.text();
  if (t.length > max) throw new HttpError(413, "too_large", "The request is too large.");
  if (!t) return {};
  try {
    const v = JSON.parse(t);
    if (v && typeof v === "object" && !Array.isArray(v)) return v as Record<string, unknown>;
  } catch { /* below */ }
  throw new HttpError(400, "bad_json", "The request is not a JSON object.");
}
const str = (v: unknown) => (typeof v === "string" ? v : "");
function cleanLine(s: string, maxLen: number, multiline = false): boolean {
  if (s.length > maxLen) return false;
  for (let i = 0; i < s.length; i++) { const c = s.charCodeAt(i); if (c < 32 && !(multiline && (c === 10 || c === 9 || c === 13))) return false; }
  return true;
}
const csv = (s: string) => s.split(",").filter(Boolean);
const tagColumn = (t: string[]) => (t.length ? "," + t.join(",") + "," : ",");
export function parseTags(raw: string): string[] {
  const out: string[] = [];
  for (const t of raw.toLowerCase().split(/[\s,;]+/)) {
    const x = t.replace(/^#/, "");
    if (!x) continue;
    if (!/^[a-z0-9][a-z0-9+.-]{0,23}$/.test(x)) throw new HttpError(400, "bad_tags", `Tag "${x.slice(0, 30)}" is not allowed: use a-z, 0-9, + . - (at most 24 characters).`);
    if (!out.includes(x)) out.push(x);
  }
  if (out.length > 8) throw new HttpError(400, "bad_tags", "At most 8 tags.");
  return out;
}
function fileUrl(env: Env, req: Request, key: string): string {
  const base = (env.FILES_BASE_URL || "").replace(/\/+$/, "");
  if (base) return base + "/" + key;
  return (env.PUBLIC_URL || new URL(req.url).origin).replace(/\/+$/, "") + "/files/" + key;
}

async function session(env: Env, d: Deps, req: Request): Promise<Session> {
  const h = req.headers.get("authorization") || "";
  const m = /^Bearer\s+(\S+)$/i.exec(h);
  if (!m) throw new HttpError(401, "sign_in", "Sign in with GitHub first.");
  if (!env.SESSION_SECRET || env.SESSION_SECRET.length < 16) throw new HttpError(503, "not_configured", "The marketplace is not set up yet (SESSION_SECRET).");
  const s = await readSession(env.SESSION_SECRET, m[1], d.now());
  if (!s) throw new HttpError(401, "sign_in", "Your sign-in has expired or is not valid. Sign in with GitHub again.");
  if (await banned(env, s.uid, s.login)) throw new HttpError(403, "banned", "This GitHub account may not upload or report on the OnAir marketplace.");
  return s;
}
async function banned(env: Env, uid: number, login: string): Promise<boolean> {
  return !!(await env.DB.prepare("SELECT 1 AS x FROM bans WHERE login = ? OR (github_id != 0 AND github_id = ?)").bind(login.toLowerCase(), uid).first());
}
function admin(env: Env, req: Request) {
  const t = env.ADMIN_TOKEN || "";
  const m = /^Bearer\s+(\S+)$/i.exec(req.headers.get("authorization") || "");
  if (t.length < 24 || !m || !sameText(m[1], t)) throw new HttpError(401, "admin", "Admin token required.");
}

// ---------------------------------------------------------------- rows to answers
interface PluginRow {
  id: string; title: string; description: string; repo: string; tags: string; kind: string; platforms: string; permissions: string; license: string;
  author: string; version: string; owner_id: number; owner_login: string; likes: number; downloads: number; created_at: number; updated_at: number;
  status: string; pulled_reason: string;
}
interface VersionRow { plugin_id: string; version: string; r2_key: string; size: number; sha256: string; min_onair: string; platforms: string; permissions: string; created_at: number }
const summaryOf = (r: PluginRow) => ({
  id: r.id, title: r.title, summary: r.description.length > 200 ? r.description.slice(0, 197) + "..." : r.description, owner: r.owner_login,
  version: r.version, kind: r.kind, tags: csv(r.tags), platforms: csv(r.platforms), likes: r.likes, downloads: r.downloads, updated: r.updated_at,
  review: "unreviewed",
});

// ---------------------------------------------------------------- browse
const SORTS = ["relevance", "likes", "downloads", "newest"];
const likeEsc = (s: string) => s.replace(/[\\%_]/g, (c) => "\\" + c);

async function listPlugins(env: Env, d: Deps, req: Request): Promise<Response> {
  const u = new URL(req.url), p = u.searchParams;
  const q = (p.get("q") || "").toLowerCase().trim().slice(0, 100);
  const sort = SORTS.includes(p.get("sort") || "") ? p.get("sort")! : "relevance";
  const tag = (p.get("tag") || "").toLowerCase(), kind = p.get("kind") || "", platform = p.get("platform") || "";
  const limit = Math.min(50, Math.max(1, parseInt(p.get("limit") || "30", 10) || 30));
  const page = Math.min(1000, Math.max(1, parseInt(p.get("page") || "1", 10) || 1));
  if (tag && !/^[a-z0-9][a-z0-9+.-]{0,23}$/.test(tag)) return fail(400, "bad_tag", "Unknown tag format.");
  if (kind && !["mode", "action", "source", "panel-lite"].includes(kind)) return fail(400, "bad_kind", "kind is mode, action, source or panel-lite.");
  if (platform && !["macos", "linux", "windows"].includes(platform)) return fail(400, "bad_platform", "platform is macos, linux or windows.");

  const where: string[] = ["status = 'live'"], wArgs: unknown[] = [];
  if (tag) { where.push("tags LIKE ? ESCAPE '\\'"); wArgs.push("%," + likeEsc(tag) + ",%"); }
  if (kind) { where.push("kind = ?"); wArgs.push(kind); }
  if (platform) { where.push("platforms LIKE ?"); wArgs.push("%," + platform + ",%"); }
  const tokens = q.split(/\s+/).filter(Boolean).slice(0, 5).map((t) => t.slice(0, 40));
  const score: string[] = [], sArgs: unknown[] = [];
  for (const t of tokens) {
    const pat = "%" + likeEsc(t) + "%";
    where.push("(lower(title) LIKE ? ESCAPE '\\' OR id LIKE ? ESCAPE '\\' OR tags LIKE ? ESCAPE '\\' OR lower(description) LIKE ? ESCAPE '\\' OR lower(owner_login) LIKE ? ESCAPE '\\' OR lower(author) LIKE ? ESCAPE '\\')");
    wArgs.push(pat, pat, pat, pat, pat, pat);
    score.push("(CASE WHEN lower(title) = ? THEN 20 ELSE 0 END) + (CASE WHEN lower(title) LIKE ? ESCAPE '\\' THEN 10 ELSE 0 END) + (CASE WHEN tags LIKE ? ESCAPE '\\' THEN 8 ELSE 0 END)" +
      " + (CASE WHEN id LIKE ? ESCAPE '\\' THEN 5 ELSE 0 END) + (CASE WHEN lower(owner_login) LIKE ? ESCAPE '\\' OR lower(author) LIKE ? ESCAPE '\\' THEN 3 ELSE 0 END)" +
      " + (CASE WHEN lower(description) LIKE ? ESCAPE '\\' THEN 2 ELSE 0 END)");
    sArgs.push(t, pat, "%," + likeEsc(t) + ",%", pat, pat, pat, pat);
  }
  const scoreSql = score.length ? score.join(" + ") : "0";
  const order = sort === "likes" ? "likes DESC, downloads DESC, updated_at DESC"
    : sort === "downloads" ? "downloads DESC, likes DESC, updated_at DESC"
    : sort === "newest" ? "updated_at DESC, id"
    : "score DESC, likes DESC, downloads DESC, updated_at DESC";
  const w = where.join(" AND ");
  const rows = await env.DB.prepare(`SELECT *, ${scoreSql} AS score FROM plugins WHERE ${w} ORDER BY ${order} LIMIT ? OFFSET ?`)
    .bind(...sArgs, ...wArgs, limit, (page - 1) * limit).all<PluginRow>();
  const total = await env.DB.prepare(`SELECT COUNT(*) AS n FROM plugins WHERE ${w}`).bind(...wArgs).first<{ n: number }>();
  const pulled = await env.DB.prepare("SELECT id, pulled_reason FROM plugins WHERE status = 'pulled' ORDER BY updated_at DESC LIMIT 500").all<{ id: string; pulled_reason: string }>();
  return signed(env, d, req, "list", {
    total: total?.n ?? 0, page, limit, sort, plugins: rows.results.map(summaryOf),
    pulled: pulled.results.map((r) => ({ id: r.id, reason: r.pulled_reason })),
  });
}

async function listTags(env: Env, d: Deps, req: Request): Promise<Response> {
  const rows = await env.DB.prepare("SELECT tags FROM plugins WHERE status = 'live' LIMIT 5000").all<{ tags: string }>();
  const n = new Map<string, number>();
  for (const r of rows.results) for (const t of csv(r.tags)) n.set(t, (n.get(t) || 0) + 1);
  const tags = [...n.entries()].sort((a, b) => b[1] - a[1] || a[0].localeCompare(b[0])).slice(0, 40).map(([tag, count]) => ({ tag, count }));
  return signed(env, d, req, "tags", { tags });
}

async function pluginRow(env: Env, id: string): Promise<PluginRow | null> {
  if (!validId(id)) return null;
  return env.DB.prepare("SELECT * FROM plugins WHERE id = ?").bind(id).first<PluginRow>();
}
const installOk = (s: string) => /^[0-9a-f]{32,64}$/.test(s);

async function pluginPage(env: Env, d: Deps, req: Request, id: string): Promise<Response> {
  const r = await pluginRow(env, id);
  if (!r) return fail(404, "not_found", "No such plugin.");
  if (r.status !== "live") return signed(env, d, req, "plugin", { plugin: { id: r.id, status: r.status, pulled_reason: r.pulled_reason } });
  const vs = await env.DB.prepare("SELECT * FROM versions WHERE plugin_id = ? ORDER BY created_at DESC LIMIT 20").bind(id).all<VersionRow>();
  const install = new URL(req.url).searchParams.get("install") || "";
  let liked = false;
  if (install && installOk(install)) liked = !!(await env.DB.prepare("SELECT 1 AS x FROM likes WHERE plugin_id = ? AND voter = ?").bind(id, await saltedHash(salt(env), "install", install)).first());
  return signed(env, d, req, "plugin", {
    plugin: {
      ...summaryOf(r), description: r.description, repo: r.repo, license: r.license, author: r.author, permissions: csv(r.permissions), status: r.status,
      created: r.created_at, liked,
      versions: vs.results.map((v) => ({ version: v.version, size: v.size, sha256: v.sha256, min_onair: v.min_onair, platforms: csv(v.platforms), permissions: csv(v.permissions), created: v.created_at })),
    },
  });
}

async function download(env: Env, d: Deps, req: Request, id: string): Promise<Response> {
  const r = await pluginRow(env, id);
  if (!r) return fail(404, "not_found", "No such plugin.");
  if (r.status !== "live") return fail(410, "pulled", "This plugin was withdrawn" + (r.pulled_reason ? ": " + r.pulled_reason : "."));
  const p = new URL(req.url).searchParams;
  const version = p.get("version") || r.version;
  const v = await env.DB.prepare("SELECT * FROM versions WHERE plugin_id = ? AND version = ?").bind(id, version).first<VersionRow>();
  if (!v) return fail(404, "no_version", "No such version.");
  // one count per install (or, without an install id, per address) per plugin per day
  const now = d.now(), s = salt(env), ipKey = await saltedHash(s, "ip", clientIp(req));
  const install = p.get("install") || "";
  const voter = install && installOk(install) ? "i:" + (await saltedHash(s, "install", install)) : "a:" + ipKey;
  if (await rateHit(env, now, "dl:" + ipKey, LIMITS.downloadCountIp)) {
    const ins = await env.DB.prepare("INSERT OR IGNORE INTO dl_seen (plugin_id, voter, day) VALUES (?, ?, ?)").bind(id, voter, Math.floor(now / 86400)).run();
    if (ins.meta?.changes) await env.DB.prepare("UPDATE plugins SET downloads = downloads + 1 WHERE id = ?").bind(id).run();
  }
  const url = fileUrl(env, req, v.r2_key);
  if (p.get("format") === "json") return signed(env, d, req, "download", { id, version: v.version, url, size: v.size, sha256: v.sha256 });
  return new Response(null, { status: 302, headers: { location: url, "x-onair-sha256": v.sha256, "x-onair-size": String(v.size), "cache-control": "no-store" } });
}

async function serveFile(env: Env, key: string): Promise<Response> {
  const m = /^p\/([a-z0-9][a-z0-9._-]{0,63})\/([0-9A-Za-z.+-]{1,32})\/([0-9a-f]{64})\.onairplugin$/.exec(key);
  if (!m) return fail(404, "not_found", "No such file.");
  const row = await env.DB.prepare("SELECT p.status AS status FROM versions v JOIN plugins p ON p.id = v.plugin_id WHERE v.r2_key = ?").bind(key).first<{ status: string }>();
  if (!row) return fail(404, "not_found", "No such file.");
  if (row.status !== "live") return fail(410, "pulled", "This plugin was withdrawn.");
  const o = await env.FILES.get(key);
  if (!o) return fail(404, "not_found", "No such file.");
  return new Response(o.body, {
    headers: { "content-type": "application/zip", "content-length": String(o.size), "content-disposition": `attachment; filename="${m[1]}-${m[2]}.onairplugin"`, "cache-control": "public, max-age=300", "x-onair-sha256": m[3] },
  });
}

// ---------------------------------------------------------------- likes
async function like(env: Env, d: Deps, req: Request, id: string, on: boolean): Promise<Response> {
  const body = await readJson(req);
  const install = str(body.install) || new URL(req.url).searchParams.get("install") || "";
  if (!installOk(install)) return fail(400, "bad_install", "A like needs the install id (32-64 lower-case hex digits).");
  const r = await pluginRow(env, id);
  if (!r) return fail(404, "not_found", "No such plugin.");
  if (r.status !== "live") return fail(410, "pulled", "This plugin was withdrawn.");
  const now = d.now(), s = salt(env), voter = await saltedHash(s, "install", install);
  await limitOrThrow(env, now, "like:" + voter, LIMITS.likeInstall, "likes");
  await limitOrThrow(env, now, "likeip:" + (await saltedHash(s, "ip", clientIp(req))), LIMITS.likeIp, "likes from this address");
  if (on) {
    const x = await env.DB.prepare("INSERT OR IGNORE INTO likes (plugin_id, voter, at) VALUES (?, ?, ?)").bind(id, voter, now).run();
    if (x.meta?.changes) await env.DB.prepare("UPDATE plugins SET likes = likes + 1 WHERE id = ?").bind(id).run();
  } else {
    const x = await env.DB.prepare("DELETE FROM likes WHERE plugin_id = ? AND voter = ?").bind(id, voter).run();
    if (x.meta?.changes) await env.DB.prepare("UPDATE plugins SET likes = MAX(0, likes - 1) WHERE id = ?").bind(id).run();
  }
  const n = await env.DB.prepare("SELECT likes FROM plugins WHERE id = ?").bind(id).first<{ likes: number }>();
  return json(200, { id, likes: n?.likes ?? 0, liked: on });
}

// ---------------------------------------------------------------- GitHub sign-in (device flow, through this Worker)
async function ghForm(d: Deps, url: string, form: Record<string, string>): Promise<Record<string, unknown>> {
  const r = await d.fetch(url, {
    method: "POST", body: new URLSearchParams(form).toString(),
    headers: { accept: "application/json", "content-type": "application/x-www-form-urlencoded", "user-agent": "OnAir-Marketplace" },
  });
  if (!r.ok) throw new HttpError(502, "github", "GitHub did not answer (HTTP " + r.status + ").");
  return (await r.json()) as Record<string, unknown>;
}
async function authDevice(env: Env, d: Deps, req: Request): Promise<Response> {
  if (!env.GITHUB_CLIENT_ID) return fail(503, "not_configured", "GitHub sign-in is not set up yet.");
  await limitOrThrow(env, d.now(), "auth:" + (await saltedHash(salt(env), "ip", clientIp(req))), LIMITS.authIp, "sign-in attempts");
  const g = await ghForm(d, "https://github.com/login/device/code", { client_id: env.GITHUB_CLIENT_ID, scope: "" });
  if (!g.device_code || !g.user_code) return fail(502, "github", "GitHub refused the sign-in request" + (g.error_description ? ": " + str(g.error_description) : "."));
  return json(200, { device_code: g.device_code, user_code: g.user_code, verification_uri: g.verification_uri || "https://github.com/login/device", expires_in: g.expires_in ?? 900, interval: g.interval ?? 5 });
}
async function authPoll(env: Env, d: Deps, req: Request): Promise<Response> {
  if (!env.GITHUB_CLIENT_ID) return fail(503, "not_configured", "GitHub sign-in is not set up yet.");
  if (!env.SESSION_SECRET || env.SESSION_SECRET.length < 16) return fail(503, "not_configured", "The marketplace is not set up yet (SESSION_SECRET).");
  const body = await readJson(req);
  const dc = str(body.device_code);
  if (!/^[A-Za-z0-9_-]{8,100}$/.test(dc)) return fail(400, "bad_code", "device_code missing.");
  await limitOrThrow(env, d.now(), "poll:" + (await saltedHash(salt(env), "ip", clientIp(req))), LIMITS.pollIp, "sign-in checks");
  const g = await ghForm(d, "https://github.com/login/oauth/access_token", { client_id: env.GITHUB_CLIENT_ID, device_code: dc, grant_type: "urn:ietf:params:oauth:grant-type:device_code" });
  const e = str(g.error);
  if (e === "authorization_pending") return json(200, { status: "pending" });
  if (e === "slow_down") return json(200, { status: "slow_down", interval: g.interval ?? 10 });
  if (e === "expired_token") return json(200, { status: "expired" });
  if (e === "access_denied") return json(200, { status: "denied" });
  const tok = str(g.access_token);
  if (!tok) return fail(502, "github", "GitHub sign-in failed" + (e ? " (" + e + ")" : "") + ".");
  // who signed in; the GitHub token is used for this one call and then dropped (it is never stored)
  const ur = await d.fetch("https://api.github.com/user", { headers: { authorization: "Bearer " + tok, accept: "application/vnd.github+json", "user-agent": "OnAir-Marketplace" } });
  if (!ur.ok) return fail(502, "github", "GitHub did not say who signed in (HTTP " + ur.status + ").");
  const user = (await ur.json()) as { id?: number; login?: string };
  if (typeof user.id !== "number" || !user.login || !/^[A-Za-z0-9-]{1,39}$/.test(user.login)) return fail(502, "github", "GitHub sent an unexpected user.");
  if (await banned(env, user.id, user.login)) return fail(403, "banned", "This GitHub account may not upload or report on the OnAir marketplace.");
  const now = d.now();
  const token = await makeSession(env.SESSION_SECRET, { uid: user.id, login: user.login, iat: now, exp: now + SESSION_DAYS * 86400 });
  return json(200, { status: "ok", token, login: user.login, expires: now + SESSION_DAYS * 86400 });
}
async function me(env: Env, d: Deps, req: Request): Promise<Response> {
  const s = await session(env, d, req);
  const rows = await env.DB.prepare("SELECT * FROM plugins WHERE owner_id = ? ORDER BY updated_at DESC LIMIT 200").bind(s.uid).all<PluginRow>();
  return json(200, { login: s.login, plugins: rows.results.map((r) => ({ ...summaryOf(r), status: r.status, pulled_reason: r.pulled_reason })) });
}

// ---------------------------------------------------------------- upload
interface UploadForm { bytes: Uint8Array; title: string; description: string; repo: string; tags: string[] | null }
async function readUpload(env: Env, req: Request, requireText: boolean): Promise<UploadForm> {
  const max = Math.max(1024, parseInt(env.MAX_PACKAGE_BYTES || "", 10) || DEFAULT_MAX_PACKAGE);
  const cl = parseInt(req.headers.get("content-length") || "0", 10);
  if (cl > max + 65536) throw new HttpError(413, "too_large", `The package is larger than ${Math.round(max / 1048576)} MiB.`);
  if (!(req.headers.get("content-type") || "").toLowerCase().startsWith("multipart/form-data")) throw new HttpError(400, "bad_form", "Send the upload as multipart/form-data.");
  let fd: FormData;
  try { fd = await req.formData(); } catch { throw new HttpError(400, "bad_form", "The upload form could not be read."); }
  const pkg = fd.get("package");
  if (!pkg || typeof pkg === "string") throw new HttpError(400, "no_package", "Choose the .onairplugin file.");
  const file = pkg as unknown as Blob;
  if (file.size > max) throw new HttpError(413, "too_large", `The package is larger than ${Math.round(max / 1048576)} MiB.`);
  if (file.size < 22) throw new HttpError(400, "bad_package", "The file is not an .onairplugin package (a zip file).");
  const field = (k: string) => { const v = fd.get(k); return typeof v === "string" ? v.trim() : ""; };
  const title = field("title"), description = field("description").replace(/\r\n/g, "\n"), repo = field("repo");
  if (requireText && !title) throw new HttpError(400, "no_title", "A title is required.");
  if (requireText && !description) throw new HttpError(400, "no_description", "A description is required.");
  if (title && !cleanLine(title, 80)) throw new HttpError(400, "bad_title", "The title must be one line of at most 80 characters.");
  if (description && (!cleanLine(description, 4000, true) || description.length < 10)) throw new HttpError(400, "bad_description", "The description must be 10 to 4000 characters.");
  if (repo && (!/^https:\/\/[^\s<>"']{3,200}$/.test(repo) || repo.length > 200)) throw new HttpError(400, "bad_repo", "The link must be an https:// address (at most 200 characters).");
  const pol = policyText(title, description);
  if (pol) throw new HttpError(400, "policy", "Not accepted: the text " + pol);
  const tagsRaw = fd.get("tags");
  return { bytes: new Uint8Array(await file.arrayBuffer()), title, description, repo, tags: typeof tagsRaw === "string" ? parseTags(tagsRaw) : null };
}

async function upload(env: Env, d: Deps, req: Request, existingId: string | null): Promise<Response> {
  const s = await session(env, d, req);
  const now = d.now(), sl = salt(env);
  const ipKey = await saltedHash(sl, "ip", clientIp(req));
  await limitOrThrow(env, now, "uptry:" + s.uid, LIMITS.uploadTryUser, "upload attempts from this account today");
  await limitOrThrow(env, now, "uptryip:" + ipKey, LIMITS.uploadTryIp, "upload attempts from this address today");
  const f = await readUpload(env, req, existingId === null);
  let checked;
  try { checked = await checkPackage(f.bytes, MAX_UNPACKED); } catch (e) {
    if (e instanceof ManifestError) return fail(400, "bad_package", "The package was refused: " + e.message + ".");
    throw e;
  }
  const m = checked.manifest;
  const owners = (env.OWNER_LOGINS || "").toLowerCase().split(",").map((x) => x.trim()).filter(Boolean);
  if (/^onair/.test(m.id) && !owners.includes(s.login.toLowerCase())) return fail(403, "reserved", 'Plugin ids that start with "onair" are kept for the OnAir project. Change the id in manifest.json.');
  const cur = await pluginRow(env, m.id);
  if (existingId === null) {
    if (cur) return cur.owner_id === s.uid
      ? fail(409, "exists_yours", `You already published "${m.id}". Use "Upload a new version" for it.`)
      : fail(409, "taken", `The id "${m.id}" is taken by another author. Change the id in manifest.json.`);
  } else {
    if (m.id !== existingId) return fail(400, "wrong_id", `This package is "${m.id}", not "${existingId}".`);
    if (!cur) return fail(404, "not_found", "No such plugin.");
    if (cur.owner_id !== s.uid) return fail(403, "not_owner", "Only the author who published this plugin can upload new versions.");
    if (cur.status !== "live") return fail(403, "pulled", "This plugin was withdrawn by the moderators" + (cur.pulled_reason ? ": " + cur.pulled_reason : "") + ".");
    if (compareVersions(m.version, cur.version) <= 0) return fail(409, "version", `The new version must be higher than ${cur.version} (the package says ${m.version}).`);
    if (m.kind !== cur.kind) return fail(400, "kind", `The kind changed from ${cur.kind} to ${m.kind}: publish it as a new plugin instead.`);
  }
  if (await env.DB.prepare("SELECT 1 AS x FROM versions WHERE plugin_id = ? AND version = ?").bind(m.id, m.version).first())
    return fail(409, "version", `Version ${m.version} was published already.`);

  // only a package that passed every check counts against the daily upload limits
  await limitOrThrow(env, now, "up:" + s.uid, LIMITS.uploadUser, "uploads from this account today");
  await limitOrThrow(env, now, "upip:" + ipKey, LIMITS.uploadIp, "uploads from this address today");
  const sha = await sha256Hex(f.bytes);
  const key = `p/${m.id}/${m.version}/${sha}.onairplugin`;
  await env.FILES.put(key, f.bytes, { httpMetadata: { contentType: "application/zip" }, customMetadata: { uploader: s.login } });
  const plats = "," + checked.platforms.join(",") + ",", perms = m.permissions.join(",");
  const stmts: D1PreparedStatement[] = [
    env.DB.prepare("INSERT INTO versions (plugin_id, version, r2_key, size, sha256, min_onair, platforms, permissions, created_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)")
      .bind(m.id, m.version, key, f.bytes.length, sha, m.minOnAir, plats, perms, now),
  ];
  if (existingId === null) {
    stmts.push(env.DB.prepare(
      "INSERT INTO plugins (id, title, description, repo, tags, kind, platforms, permissions, license, author, version, owner_id, owner_login, created_at, updated_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
    ).bind(m.id, f.title, f.description, f.repo, tagColumn(f.tags || []), m.kind, plats, perms, m.license, m.author, m.version, s.uid, s.login, now, now));
  } else {
    stmts.push(env.DB.prepare(
      "UPDATE plugins SET version = ?, platforms = ?, permissions = ?, license = ?, author = ?, owner_login = ?, updated_at = ?, title = COALESCE(NULLIF(?, ''), title), description = COALESCE(NULLIF(?, ''), description), repo = CASE WHEN ? THEN ? ELSE repo END, tags = CASE WHEN ? THEN ? ELSE tags END WHERE id = ?",
    ).bind(m.version, plats, perms, m.license, m.author, s.login, now, f.title, f.description, f.repo ? 1 : 0, f.repo, f.tags ? 1 : 0, tagColumn(f.tags || []), m.id));
  }
  await env.DB.batch(stmts);
  return json(existingId === null ? 201 : 200, { ok: true, id: m.id, version: m.version, size: f.bytes.length, sha256: sha, platforms: checked.platforms, review: "unreviewed" });
}

// ---------------------------------------------------------------- reports
async function report(env: Env, d: Deps, req: Request, id: string): Promise<Response> {
  const s = await session(env, d, req);
  const body = await readJson(req);
  const reason = str(body.reason), text = str(body.text).trim(), version = str(body.version).slice(0, 32);
  if (!REPORT_REASONS.includes(reason)) return fail(400, "bad_reason", "reason is one of " + REPORT_REASONS.join(", ") + ".");
  if (!cleanLine(text, 2000, true)) return fail(400, "bad_text", "The text is limited to 2000 characters.");
  const r = await pluginRow(env, id);
  if (!r) return fail(404, "not_found", "No such plugin.");
  await limitOrThrow(env, d.now(), "rep:" + s.uid, LIMITS.reportUser, "reports from this account today");
  await env.DB.prepare("INSERT INTO reports (plugin_id, version, reporter_id, reporter_login, reason, text, at) VALUES (?, ?, ?, ?, ?, ?, ?)")
    .bind(id, version, s.uid, s.login, reason, text, d.now()).run();
  return json(201, { ok: true });
}

// ---------------------------------------------------------------- admin
async function moveFiles(env: Env, id: string, toPulled: boolean) {
  // with a public files domain, a pulled plugin's packages move under pulled/ so their public address stops working
  if (!env.FILES_BASE_URL) return;
  const vs = await env.DB.prepare("SELECT r2_key FROM versions WHERE plugin_id = ?").bind(id).all<{ r2_key: string }>();
  for (const v of vs.results) {
    const from = toPulled ? v.r2_key : "pulled/" + v.r2_key, to = toPulled ? "pulled/" + v.r2_key : v.r2_key;
    const o = await env.FILES.get(from);
    if (!o) continue;
    await env.FILES.put(to, await o.arrayBuffer(), { httpMetadata: { contentType: "application/zip" } });
    await env.FILES.delete(from);
  }
}
async function adminRoute(env: Env, d: Deps, req: Request, parts: string[]): Promise<Response> {
  admin(env, req);
  const now = d.now();
  if (req.method === "GET" && parts[0] === "reports" && parts.length === 1) {
    const st = new URL(req.url).searchParams.get("status") || "open";
    const rows = await env.DB.prepare("SELECT * FROM reports WHERE (? = 'all' OR status = ?) ORDER BY n DESC LIMIT 500").bind(st, st).all();
    return json(200, { reports: rows.results });
  }
  if (req.method === "POST" && parts[0] === "reports" && parts.length === 3 && parts[2] === "close") {
    const x = await env.DB.prepare("UPDATE reports SET status = 'closed' WHERE n = ?").bind(parseInt(parts[1], 10) || 0).run();
    return x.meta?.changes ? json(200, { ok: true }) : fail(404, "not_found", "No such report.");
  }
  if (req.method === "POST" && parts[0] === "plugins" && parts.length === 3 && (parts[2] === "pull" || parts[2] === "restore")) {
    const id = decodeURIComponent(parts[1]);
    const r = await pluginRow(env, id);
    if (!r) return fail(404, "not_found", "No such plugin.");
    const body = await readJson(req);
    const pull = parts[2] === "pull";
    if (pull === (r.status === "pulled")) return json(200, { ok: true, id, status: r.status });
    await env.DB.prepare("UPDATE plugins SET status = ?, pulled_reason = ?, updated_at = ? WHERE id = ?").bind(pull ? "pulled" : "live", pull ? str(body.reason).slice(0, 300) : "", now, id).run();
    await moveFiles(env, id, pull);
    return json(200, { ok: true, id, status: pull ? "pulled" : "live" });
  }
  if (req.method === "POST" && parts[0] === "users" && parts.length === 3 && (parts[2] === "ban" || parts[2] === "unban")) {
    const login = decodeURIComponent(parts[1]).toLowerCase();
    if (!/^[a-z0-9-]{1,39}$/.test(login)) return fail(400, "bad_login", "Not a GitHub login.");
    if (parts[2] === "unban") { await env.DB.prepare("DELETE FROM bans WHERE login = ?").bind(login).run(); return json(200, { ok: true }); }
    const body = await readJson(req);
    const known = await env.DB.prepare("SELECT owner_id AS uid FROM plugins WHERE lower(owner_login) = ? UNION SELECT reporter_id AS uid FROM reports WHERE lower(reporter_login) = ? LIMIT 1").bind(login, login).first<{ uid: number }>();
    await env.DB.prepare("INSERT OR REPLACE INTO bans (login, github_id, reason, at) VALUES (?, ?, ?, ?)").bind(login, known?.uid ?? 0, str(body.reason).slice(0, 300), now).run();
    let pulled = 0;
    if (body.pull_plugins === true && known) {
      const ids = await env.DB.prepare("SELECT id FROM plugins WHERE owner_id = ? AND status = 'live'").bind(known.uid).all<{ id: string }>();
      for (const x of ids.results) {
        await env.DB.prepare("UPDATE plugins SET status = 'pulled', pulled_reason = ?, updated_at = ? WHERE id = ?").bind("author banned", now, x.id).run();
        await moveFiles(env, x.id, true);
        pulled++;
      }
    }
    return json(200, { ok: true, login, github_id: known?.uid ?? 0, pulled });
  }
  if (req.method === "GET" && parts[0] === "bans") return json(200, { bans: (await env.DB.prepare("SELECT * FROM bans ORDER BY at DESC").all()).results });
  return fail(404, "not_found", "No such admin action.");
}

// ---------------------------------------------------------------- router
export async function handle(req: Request, env: Env, d: Deps): Promise<Response> {
  try {
    const u = new URL(req.url);
    const path = u.pathname.replace(/\/+$/, "") || "/";
    if (path.startsWith("/files/") && (req.method === "GET" || req.method === "HEAD")) return await serveFile(env, decodeURIComponent(path.slice(7)));
    if (path === "/" || path === API) return json(200, { service: "OnAir plugin marketplace", api: API, docs: "https://github.com/adswill/OnAir/blob/main/docs/PLUGIN-MARKETPLACE-4.0.md" });
    if (!path.startsWith(API + "/")) return fail(404, "not_found", "Not found.");
    const parts = path.slice(API.length + 1).split("/");
    const m = req.method;
    if (parts[0] === "status" && m === "GET") {
      return json(200, { ok: true, signing: signingKeyValid(env.SIGNING_KEY), public_key: signingKeyValid(env.SIGNING_KEY) ? await ed25519PublicHex(env.SIGNING_KEY!) : "", github: !!env.GITHUB_CLIENT_ID });
    }
    if (parts[0] === "tags" && parts.length === 1 && m === "GET") return await listTags(env, d, req);
    if (parts[0] === "auth" && parts[1] === "device" && m === "POST") return await authDevice(env, d, req);
    if (parts[0] === "auth" && parts[1] === "poll" && m === "POST") return await authPoll(env, d, req);
    if (parts[0] === "me" && m === "GET") return await me(env, d, req);
    if (parts[0] === "admin") return await adminRoute(env, d, req, parts.slice(1));
    if (parts[0] === "plugins") {
      if (parts.length === 1 && m === "GET") return await listPlugins(env, d, req);
      if (parts.length === 1 && m === "POST") return await upload(env, d, req, null);
      const id = decodeURIComponent(parts[1] || "");
      if (!validId(id)) return fail(404, "not_found", "No such plugin.");
      if (parts.length === 2 && m === "GET") return await pluginPage(env, d, req, id);
      if (parts.length === 3) {
        if (parts[2] === "download" && m === "GET") return await download(env, d, req, id);
        if (parts[2] === "like" && m === "POST") return await like(env, d, req, id, true);
        if ((parts[2] === "like" && m === "DELETE") || (parts[2] === "unlike" && m === "POST")) return await like(env, d, req, id, false);
        if (parts[2] === "versions" && m === "POST") return await upload(env, d, req, id);
        if (parts[2] === "report" && m === "POST") return await report(env, d, req, id);
      }
    }
    return fail(404, "not_found", "Not found.");
  } catch (e) {
    if (e instanceof HttpError) return fail(e.status, e.code, e.message);
    console.error("marketplace error", e);
    return fail(500, "internal", "Something went wrong on the server.");
  }
}

// the hourly cron: rate-limit windows that are over, download-dedupe rows older than two days
export async function cleanup(env: Env, now: number) {
  await env.DB.prepare("DELETE FROM rl WHERE exp < ?").bind(now).run();
  await env.DB.prepare("DELETE FROM dl_seen WHERE day < ?").bind(Math.floor(now / 86400) - 2).run();
}
