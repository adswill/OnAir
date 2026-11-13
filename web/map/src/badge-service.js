// badge-service.js - accounts, sign-in and the badge pages' API for the hub (docs/MAP-BADGES-4.0.md). The MapHub owns one instance; the
// tests run it against MemoryBadgeBackend and a scripted GitHub (test/badge-service.test.js).
//
// Routes (all on map.on-air.dev):
//   GET  /auth/login?next=/u/name     -> GitHub (web flow)          GET /auth/callback?code&state -> session cookie, back to `next`
//   POST /auth/logout                 (cookie + CSRF)
//   GET  /api/me                      who is signed in (cookie or Bearer), settings, CSRF token for the page, own profile
//   POST /api/me/settings             { badges, public, by } (cookie + CSRF, or Bearer from the app)
//   POST /api/me/apps/revoke          sign out every app that uses this account on the map
//   POST /api/me/delete               delete every stat, badge and setting kept for this account
//   GET  /api/u/<login>               a public profile (or your own, private or not)
//   GET  /api/leaderboard?board=hours&period=week
//   POST /api/badges/admin            { login, hidden } with "Authorization: Bearer <BADGE_ADMIN_TOKEN>": hide a cheat from the boards

import {
  bearerOf, checkCsrf, checkState, cookie, csrfFor, githubUser, makeState, makeToken, parseCookies, readToken, sameText, secretOk,
  SESSION_COOKIE, SESSION_DAYS, STATE_COOKIE, tokenHash, webSecret, authorizeUrl,
} from "./auth.js";
import { BadgeBook, periodValue, publicProfile, showcase } from "./badge-stats.js";
import { fold, validateBy } from "./by-name.js";
import { BADGE_BY_ID, BOARDS, badgeTitle } from "../public/badges/defs.js";

export const DEFAULT_REDIRECT = "https://map.on-air.dev/auth/callback";
export const ORIGINS = ["https://map.on-air.dev"];
const BOARD_CACHE_MS = 60_000;
const BOARD_SIZE = 50;
const TOKEN_CACHE_MS = 10 * 60_000;

function jres(obj, status = 200, headers = {}) {
  return new Response(JSON.stringify(obj), { status, headers: { "Content-Type": "application/json; charset=utf-8", "Cache-Control": "no-store", ...headers } });
}
function redirect(to, cookies = []) {
  const h = new Headers({ Location: to, "Cache-Control": "no-store" });
  for (const c of cookies) h.append("Set-Cookie", c);
  return new Response(null, { status: 302, headers: h });
}
function page(status, text) {
  return new Response(`<!doctype html><meta charset="utf-8"><title>OnAir sign-in</title><body style="font:16px system-ui;padding:2em;background:#0b1220;color:#dde">` +
    `<p>${text.replace(/[<>&]/g, "")}</p><p><a style="color:#8cf" href="/">Back to the map</a></p>`, { status, headers: { "Content-Type": "text/html; charset=utf-8", "Cache-Control": "no-store" } });
}

export class BadgeService {
  /** env: SESSION_SECRET, GITHUB_CLIENT_ID, GITHUB_CLIENT_SECRET, GITHUB_REDIRECT_URI, BADGE_ADMIN_TOKEN, MAP_ORIGINS (comma list, dev). */
  constructor(backend, env = {}, { fetchFn = (...a) => fetch(...a), now = () => Date.now() } = {}) {
    this.db = backend;
    this.env = env;
    this.fetch = fetchFn;
    this.now = now;
    this.book = new BadgeBook(backend);
    this.claims = new Map();                 // folded linked name -> uid
    for (const [name, uid] of backend.claims()) this.claims.set(fold(name), uid);
    this.tokCache = new Map();               // hash -> { sess, until }
    this.boardCache = new Map();             // "board|period" -> { at, body }
    this.origins = (env.MAP_ORIGINS ? String(env.MAP_ORIGINS).split(",").map((s) => s.trim()).filter(Boolean) : ORIGINS);
  }

  get secret() { return this.env.SESSION_SECRET || ""; }
  get configured() { return secretOk(this.secret); }
  get webConfigured() { return this.configured && !!this.env.GITHUB_CLIENT_ID && !!this.env.GITHUB_CLIENT_SECRET; }
  get redirectUri() { return this.env.GITHUB_REDIRECT_URI || DEFAULT_REDIRECT; }

  // ---------------------------------------------------------------- who is calling

  /** The app's Bearer token (the marketplace session): { uid, login, hash, via: "bearer" }, null when absent, or { error } when bad. */
  async fromBearer(req) {
    const tok = bearerOf(req);
    if (!tok) return null;
    if (!this.configured) return { error: "Badges are not set up on this server yet." };
    const nowMs = this.now(), nowSec = Math.floor(nowMs / 1000);
    const h = await tokenHash(tok);
    let c = this.tokCache.get(h);
    if (!c || c.until < nowMs) {
      const sess = await readToken(this.secret, tok, nowSec);
      if (!sess) return { error: "Your OnAir sign-in has expired or is not valid. Sign in with GitHub again." };
      c = { sess, until: nowMs + TOKEN_CACHE_MS };
      if (this.tokCache.size > 5000) this.tokCache.clear();
      this.tokCache.set(h, c);
      this.db.tokenPut(h, sess.uid, "app", sess.exp);
    }
    const t = this.db.tokenGet(h);
    if (t && t.revoked) return { error: "This app was signed out from your profile page. Sign in with GitHub again." };
    const u = this.book.user(c.sess.uid, c.sess.login);
    if (u.appsBefore && c.sess.iat && c.sess.iat * 1000 < u.appsBefore) return { error: "This app was signed out from your profile page. Sign in with GitHub again." };
    return { uid: c.sess.uid, login: c.sess.login, hash: h, via: "bearer" };
  }

  /** The website's session cookie: { uid, login, hash, via: "cookie" } or null. */
  async fromCookie(req) {
    if (!this.configured) return null;
    const v = parseCookies(req.headers.get("Cookie"))[SESSION_COOKIE];
    if (!v) return null;
    const sess = await readToken(webSecret(this.secret), v, Math.floor(this.now() / 1000));
    if (!sess) return null;
    const h = await tokenHash(v);
    const t = this.db.tokenGet(h);
    if (!t || t.revoked || t.kind !== "web" || t.uid !== sess.uid) return null;
    return { uid: sess.uid, login: sess.login, hash: h, via: "cookie" };
  }

  async caller(req) {
    const b = await this.fromBearer(req);
    if (b) return b;
    return this.fromCookie(req);
  }

  /** A change request: Bearer callers pass; cookie callers need the map's Origin and the CSRF token. Returns an error Response or null. */
  async guardChange(req, who) {
    if (!who || who.error) return jres({ error: who && who.error ? who.error : "Sign in with GitHub first." }, 401);
    if (who.via === "bearer") return null;
    const why = await checkCsrf(req, this.secret, who.hash, this.origins);
    return why ? jres({ error: "This request did not come from the map page (" + why + ")." }, 403) : null;
  }

  // ---------------------------------------------------------------- ingest hooks

  /** May this post use the public name `by`? A name linked to an account is only for that account. */
  byAllowed(by, uid) {
    if (!by) return true;
    const owner = this.claims.get(fold(by));
    return !owner || owner === uid;
  }

  /** The badge to draw next to the name on the map ("id:tier") for a signed-in sender, or "". */
  showcaseFor(uid, by) {
    const u = this.book.users.get(uid);
    if (!u || !u.public || !u.badges || u.hidden || !by || !u.by || fold(u.by) !== fold(by)) return "";
    const sc = showcase(u.s);
    return sc ? sc + ":" + u.login : "";
  }

  /** Credit a post; returns the ingest reply's additions ({ account, awards }). */
  creditPost(who, args) {
    const r = this.book.credit({ uid: who.uid, login: who.login, ...args });
    const u = this.book.users.get(who.uid);
    const out = { account: { login: who.login, badges: !!(u && u.badges), public: !!(u && u.public) } };
    if (r.awards.length) {
      out.awards = r.awards.map((a) => ({ id: a.id, tier: a.tier, name: badgeTitle(BADGE_BY_ID[a.id], a.tier), icon: BADGE_BY_ID[a.id].icon }));
      this.boardCache.clear();
    }
    if (!r.credited && r.reason && r.reason !== "rate") out.account.note = r.reason;
    return out;
  }

  // ---------------------------------------------------------------- routes

  /** Handles /auth/* and the badge API; null for anything else. */
  async handle(req, url) {
    const p = url.pathname, m = req.method;
    if (p === "/auth/login" && m === "GET") return this.login(url);
    if ((p === "/auth/callback" || p === "/auth/") && m === "GET") return this.callback(req, url);
    if (p === "/auth/logout" && m === "POST") return this.logout(req);
    if (p === "/api/me" && m === "GET") return this.me(req);
    if (p === "/api/me/settings" && m === "POST") return this.settings(req);
    if (p === "/api/me/apps/revoke" && m === "POST") return this.revokeApps(req);
    if (p === "/api/me/delete" && m === "POST") return this.deleteMe(req);
    if (p.startsWith("/api/u/") && m === "GET") return this.profile(req, decodeURIComponent(p.slice(7)));
    if (p === "/api/leaderboard" && m === "GET") return this.leaderboard(url);
    if (p === "/api/badges/admin" && m === "POST") return this.admin(req);
    return null;
  }

  async login(url) {
    if (!this.webConfigured) return page(503, "GitHub sign-in is not set up on this server yet.");
    const nowSec = Math.floor(this.now() / 1000);
    const { state, cookie: c } = await makeState(this.secret, nowSec, url.searchParams.get("next") || "/");
    return redirect(authorizeUrl(this.env.GITHUB_CLIENT_ID, this.redirectUri, state), [c]);
  }

  async callback(req, url) {
    if (!this.webConfigured) return page(503, "GitHub sign-in is not set up on this server yet.");
    const clear = cookie(STATE_COOKIE, "", 0, "/auth");
    if (url.searchParams.get("error")) return redirectBack("/", clear, "cancelled");
    const nowSec = Math.floor(this.now() / 1000);
    const next = await checkState(this.secret, parseCookies(req.headers.get("Cookie"))[STATE_COOKIE], url.searchParams.get("state") || "", nowSec);
    if (!next) return page(400, "The sign-in link is old or was not started on this page. Please press Sign in again.");
    let gh;
    try { gh = await githubUser(this.fetch, this.env.GITHUB_CLIENT_ID, this.env.GITHUB_CLIENT_SECRET, url.searchParams.get("code"), this.redirectUri); }
    catch (e) { return page(502, "GitHub sign-in failed: " + String(e.message || e).slice(0, 120)); }
    const exp = nowSec + SESSION_DAYS * 86400;
    const tok = await makeToken(webSecret(this.secret), { uid: gh.id, login: gh.login, iat: nowSec, exp, aud: "map-web" });
    this.db.tokenPut(await tokenHash(tok), gh.id, "web", exp);
    this.book.user(gh.id, gh.login);
    this.book.users.get(gh.id)._dirty = true;
    return redirect(next, [clear, cookie(SESSION_COOKIE, tok, SESSION_DAYS * 86400)]);
  }

  async logout(req) {
    const who = await this.fromCookie(req);
    const clear = cookie(SESSION_COOKIE, "", 0);
    if (!who) return jres({ ok: true }, 200, { "Set-Cookie": clear });
    const bad = await this.guardChange(req, who);
    if (bad) return bad;
    this.db.tokenRevoke(who.hash);
    return jres({ ok: true }, 200, { "Set-Cookie": clear });
  }

  async me(req) {
    const who = await this.caller(req);
    if (!who || who.error) return jres({ signedIn: false, configured: this.configured, web: this.webConfigured, error: who && who.error ? who.error : undefined });
    const u = this.book.user(who.uid, who.login);
    const nowMs = this.now();
    return jres({
      signedIn: true, login: u.login, badges: u.badges, public: u.public, by: u.by || "", hidden: u.hidden,
      apps: this.db.tokenCount(u.uid, "app", Math.floor(nowMs / 1000)),
      csrf: who.via === "cookie" ? await csrfFor(this.secret, who.hash) : undefined,
      profile: publicProfile(u, nowMs),
    });
  }

  async readBody(req) {
    try { const t = await req.text(); return t.length > 4000 ? null : JSON.parse(t || "{}"); } catch { return null; }
  }

  async settings(req) {
    const who = await this.caller(req);
    const bad = await this.guardChange(req, who);
    if (bad) return bad;
    const body = await this.readBody(req);
    if (!body || typeof body !== "object") return jres({ error: "bad json" }, 400);
    const ch = {};
    if (typeof body.badges === "boolean") ch.badges = body.badges;
    if (typeof body.public === "boolean") ch.public = body.public;
    if (typeof body.by === "string") {
      const v = validateBy(body.by.slice(0, 80));
      if (!v.ok) return jres({ error: v.message }, 400);
      const owner = v.name ? this.claims.get(fold(v.name)) : 0;
      if (owner && owner !== who.uid) return jres({ error: "That name is linked to another OnAir account. Please choose another." }, 409);
      ch.by = v.name;
    }
    const u = this.book.user(who.uid, who.login);
    if ("by" in ch) {
      for (const [k, id] of this.claims) if (id === who.uid) this.claims.delete(k);
      if (ch.by) this.claims.set(fold(ch.by), who.uid);
    }
    this.book.setSettings(who.uid, who.login, ch);
    if ("public" in ch || "badges" in ch) this.boardCache.clear();
    return jres({ ok: true, badges: u.badges, public: u.public, by: u.by || "" });
  }

  async revokeApps(req) {
    const who = await this.caller(req);
    const bad = await this.guardChange(req, who);
    if (bad) return bad;
    const u = this.book.user(who.uid, who.login);
    u.appsBefore = this.now(); u._dirty = true; u._urgent = true;
    this.db.tokenRevokeUser(who.uid, "app");
    this.tokCache.clear();
    return jres({ ok: true });
  }

  async deleteMe(req) {
    const who = await this.caller(req);
    const bad = await this.guardChange(req, who);
    if (bad) return bad;
    for (const [k, id] of this.claims) if (id === who.uid) this.claims.delete(k);
    this.book.forget(who.uid);
    this.tokCache.clear();
    this.boardCache.clear();
    return jres({ ok: true }, 200, who.via === "cookie" ? { "Set-Cookie": cookie(SESSION_COOKIE, "", 0) } : {});
  }

  async profile(req, loginRaw) {
    const lc = String(loginRaw || "").toLowerCase();
    if (!/^[a-z0-9-]{1,39}$/.test(lc)) return jres({ error: "Not a GitHub login." }, 400);
    let uid = 0;
    for (const u of this.book.users.values()) if (u.login.toLowerCase() === lc) { uid = u.uid; break; }
    if (!uid) uid = this.db.findLogin(lc);
    const who = await this.caller(req);
    const mine = who && !who.error && who.uid === uid;
    if (!uid) return jres({ error: "No OnAir profile with that name." }, 404);
    const u = this.book.user(uid);
    if (!mine && (!u.public || !u.badges || u.hidden)) return jres({ error: "This profile is private." }, 404);
    return jres({ ...publicProfile(u, this.now()), mine: !!mine, public: u.public, badgesOn: u.badges });
  }

  leaderboard(url) {
    const board = BOARDS.find((b) => b.id === url.searchParams.get("board")) || BOARDS[0];
    const period = board.periods.includes(url.searchParams.get("period")) ? url.searchParams.get("period") : board.periods[0];
    const key = board.id + "|" + period;
    const nowMs = this.now();
    const hit = this.boardCache.get(key);
    if (hit && nowMs - hit.at < BOARD_CACHE_MS) return jres(hit.body, 200, { "Cache-Control": "public, max-age=60" });
    // rows written back are in SQL; users still in memory are fresher
    const byUid = new Map();
    for (const r of this.db.boardRows()) byUid.set(r.uid, r);
    for (const u of this.book.users.values()) {
      if (u.public && u.badges && !u.hidden) byUid.set(u.uid, u); else byUid.delete(u.uid);
    }
    const rows = [];
    for (const u of byUid.values()) {
      if (!u.s) continue;
      const v = periodValue(u.s, board.stat, period, nowMs);
      if (v > 0) rows.push({ login: u.login, value: Math.round(v * 100) / 100, showcase: showcase(u.s) });
    }
    rows.sort((a, b) => b.value - a.value || a.login.localeCompare(b.login));
    const body = { board: board.id, period, at: nowMs, rows: rows.slice(0, BOARD_SIZE).map((r, i) => ({ rank: i + 1, ...r })) };
    this.boardCache.set(key, { at: nowMs, body });
    return jres(body, 200, { "Cache-Control": "public, max-age=60" });
  }

  async admin(req) {
    const t = this.env.BADGE_ADMIN_TOKEN || "";
    const got = (req.headers.get("Authorization") || "").replace(/^Bearer /, "");
    if (t.length < 24 || !sameText(got, t)) return jres({ error: "Admin token required." }, 401);
    const body = await this.readBody(req);
    const lc = String(body && body.login || "").toLowerCase();
    const uid = this.db.findLogin(lc) || [...this.book.users.values()].find((u) => u.login.toLowerCase() === lc)?.uid;
    if (!uid) return jres({ error: "unknown login" }, 404);
    const u = this.book.user(uid);
    u.hidden = !!body.hidden; u._dirty = true; u._urgent = true;
    this.boardCache.clear();
    return jres({ ok: true, login: u.login, hidden: u.hidden });
  }
}

function redirectBack(to, clear, why) { return redirect(to + (to.includes("?") ? "&" : "?") + "signin=" + why, [clear]); }
