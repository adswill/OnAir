// auth.js - GitHub sign-in for the map (docs/MAP-BADGES-4.0.md). Pure WebCrypto (crypto.subtle, in Workers and Node), covered by
// test/auth.test.js.
//
// One OnAir sign-in for the app and the website:
// - The app signs in once through the plugin marketplace's GitHub device flow (core/src/pstore/market.cpp, tools/marketplace/server) and
//   sends that session token as "Authorization: Bearer ..." with its map posts. The map checks it with the same SESSION_SECRET (a Worker
//   secret with the same value on both Workers). The token format is the marketplace's: base64url(JSON {uid, login, iat, exp}) "." HMAC.
// - The website uses GitHub's web flow (/auth/login -> github.com -> /auth/callback). Its session cookie is the same format but signed with
//   a key derived for the map website only, so a website cookie is never accepted by the marketplace, and an app token never as a cookie.
// - The GitHub token is used once to read who signed in, then dropped. Only the SHA-256 of our own session tokens is stored (revocation).
// - OAuth state: a random value in an HttpOnly, SameSite=Lax cookie signed with HMAC and bound to its expiry and the return path; the
//   callback needs the cookie and the query to match. Cookie-authenticated changes need the X-CSRF-Token header (an HMAC of the session)
//   and an Origin header from the map's own origin. Bearer-authenticated calls carry no ambient credentials and need neither.

const enc = new TextEncoder();
export const SESSION_DAYS = 30;
export const STATE_TTL_S = 600;
export const STATE_COOKIE = "oa_state";
export const SESSION_COOKIE = "oa_s";

export function b64url(u) {
  let s = "";
  for (const x of u) s += String.fromCharCode(x);
  return btoa(s).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "");
}
export function unb64url(s) {
  if (typeof s !== "string" || !/^[A-Za-z0-9_-]*$/.test(s)) return null;
  try {
    const b = atob(s.replace(/-/g, "+").replace(/_/g, "/") + "===".slice((s.length + 3) % 4));
    return Uint8Array.from(b, (c) => c.charCodeAt(0));
  } catch { return null; }
}
export function toHex(u) { let s = ""; for (const x of new Uint8Array(u)) s += x.toString(16).padStart(2, "0"); return s; }

const keyCache = new Map();
function hmacKey(secret) {
  let k = keyCache.get(secret);
  if (!k) {
    k = crypto.subtle.importKey("raw", enc.encode(secret), { name: "HMAC", hash: "SHA-256" }, false, ["sign"]);
    if (keyCache.size > 8) keyCache.clear();
    keyCache.set(secret, k);
  }
  return k;
}
export async function hmac(secret, msg) {
  return new Uint8Array(await crypto.subtle.sign("HMAC", await hmacKey(secret), enc.encode(msg)));
}
export async function sha256Hex(s) { return toHex(await crypto.subtle.digest("SHA-256", enc.encode(s))); }

/** Constant-time equality of two strings. */
export function sameText(a, b) {
  a = String(a); b = String(b);
  if (a.length !== b.length) return false;
  let d = 0;
  for (let i = 0; i < a.length; i++) d |= a.charCodeAt(i) ^ b.charCodeAt(i);
  return d === 0;
}

/** The key the website's session cookies are signed with (never the marketplace's own key). */
export function webSecret(secret) { return secret + "\u0000onair-map-web"; }

export function secretOk(secret) { return typeof secret === "string" && secret.length >= 16; }

export async function makeToken(secret, payload) {
  const p = b64url(enc.encode(JSON.stringify(payload)));
  return p + "." + b64url(await hmac(secret, p));
}

/** { uid, login, iat, exp } when the token is signed with `secret` and not expired (seconds), else null. */
export async function readToken(secret, token, nowSec) {
  if (!secretOk(secret) || typeof token !== "string" || token.length > 2000) return null;
  const dot = token.indexOf(".");
  if (dot <= 0) return null;
  const p = token.slice(0, dot), mac = unb64url(token.slice(dot + 1));
  if (!mac) return null;
  const want = await hmac(secret, p);
  if (mac.length !== want.length) return null;
  let diff = 0;
  for (let i = 0; i < want.length; i++) diff |= mac[i] ^ want[i];
  if (diff) return null;
  const raw = unb64url(p);
  if (!raw) return null;
  try {
    const s = JSON.parse(new TextDecoder().decode(raw));
    if (typeof s.uid !== "number" || !Number.isFinite(s.uid) || s.uid <= 0) return null;
    if (typeof s.login !== "string" || !/^[A-Za-z0-9-]{1,39}$/.test(s.login)) return null;
    if (typeof s.exp !== "number" || s.exp < nowSec) return null;
    return { uid: s.uid, login: s.login, iat: Number(s.iat) || 0, exp: s.exp };
  } catch { return null; }
}

/** What is stored for a token: the first 32 hex digits of its SHA-256 (never the token). */
export async function tokenHash(token) { return (await sha256Hex("onair-map-token\u0000" + token)).slice(0, 32); }

export function bearerOf(req) {
  const h = req.headers.get("Authorization") || "";
  const m = /^Bearer ([A-Za-z0-9_.-]{20,2000})$/.exec(h.trim());
  return m ? m[1] : "";
}

export function parseCookies(header) {
  const out = {};
  for (const part of String(header || "").split(";")) {
    const i = part.indexOf("=");
    if (i <= 0) continue;
    const k = part.slice(0, i).trim(), v = part.slice(i + 1).trim();
    if (k && !(k in out)) out[k] = v;
  }
  return out;
}

export function cookie(name, value, maxAge, path = "/") {
  return `${name}=${value}; Path=${path}; Max-Age=${maxAge}; HttpOnly; Secure; SameSite=Lax`;
}

/** A return path that stays on this site: "/u/name", "/leaders"; anything else (absolute, protocol-relative, odd characters) is "/". */
export function safeNext(next) {
  const s = String(next || "");
  if (!/^\/[A-Za-z0-9/_.?=&%-]{0,200}$/.test(s) || s.startsWith("//") || s.includes("/\\") || s.includes("..")) return "/";
  return s;
}

function randomHex(n) { const u = new Uint8Array(n); crypto.getRandomValues(u); return toHex(u); }

/** A new OAuth state: { state (for the GitHub URL), cookie (the Set-Cookie value) }. */
export async function makeState(secret, nowSec, next) {
  const state = randomHex(16);
  const exp = nowSec + STATE_TTL_S;
  const nx = b64url(enc.encode(safeNext(next)));
  const body = `${state}.${exp}.${nx}`;
  const mac = b64url(await hmac(webSecret(secret), "state\u0000" + body));
  return { state, cookie: cookie(STATE_COOKIE, `${body}.${mac}`, STATE_TTL_S, "/auth") };
}

/** The return path when the state cookie is ours, unexpired and matches the query's state; else null. */
export async function checkState(secret, cookieValue, queryState, nowSec) {
  if (!secretOk(secret) || typeof cookieValue !== "string" || typeof queryState !== "string") return null;
  if (!/^[0-9a-f]{32}$/.test(queryState)) return null;
  const parts = cookieValue.split(".");
  if (parts.length !== 4) return null;
  const [state, expS, nx, mac] = parts;
  const want = b64url(await hmac(webSecret(secret), "state\u0000" + `${state}.${expS}.${nx}`));
  if (!sameText(mac, want)) return null;
  const exp = Number(expS);
  if (!Number.isFinite(exp) || exp < nowSec) return null;
  if (!sameText(state, queryState)) return null;
  const raw = unb64url(nx);
  return raw ? safeNext(new TextDecoder().decode(raw)) : "/";
}

/** The CSRF token of a website session (the page reads it from /api/me and sends it back in X-CSRF-Token). */
export async function csrfFor(secret, sessionHash) {
  return b64url(await hmac(webSecret(secret), "csrf\u0000" + sessionHash)).slice(0, 32);
}

/**
 * Cookie-authenticated change: the Origin must be the map's own (a page on another site cannot send it) and X-CSRF-Token must match.
 * Returns "" when it passes, else the reason.
 */
export async function checkCsrf(req, secret, sessionHash, allowedOrigins) {
  const origin = req.headers.get("Origin") || "";
  if (!origin || !allowedOrigins.includes(origin)) return "origin";
  const got = req.headers.get("X-CSRF-Token") || "";
  if (!got || !sameText(got, await csrfFor(secret, sessionHash))) return "csrf";
  return "";
}

/** The GitHub authorize URL (no scopes: only the public id and login are read). */
export function authorizeUrl(clientId, redirectUri, state) {
  const q = new URLSearchParams({ client_id: clientId, redirect_uri: redirectUri, state, allow_signup: "true" });
  return "https://github.com/login/oauth/authorize?" + q.toString();
}

/**
 * Code -> GitHub user { id, login }; the GitHub token is used for the one /user call and then dropped (also its refresh token, when the
 * app issues expiring tokens). `fetchFn` is fetch (the tests pass a fake). Throws Error(message) on any failure.
 */
export async function githubUser(fetchFn, clientId, clientSecret, code, redirectUri) {
  if (!/^[A-Za-z0-9_-]{4,100}$/.test(String(code || ""))) throw new Error("bad code");
  const r = await fetchFn("https://github.com/login/oauth/access_token", {
    method: "POST",
    headers: { accept: "application/json", "content-type": "application/json", "user-agent": "OnAir-Map" },
    body: JSON.stringify({ client_id: clientId, client_secret: clientSecret, code, redirect_uri: redirectUri }),
  });
  if (!r.ok) throw new Error("GitHub did not answer (HTTP " + r.status + ")");
  const g = await r.json();
  const tok = typeof g.access_token === "string" ? g.access_token : "";
  if (!tok) throw new Error("GitHub refused the sign-in" + (g.error ? " (" + String(g.error).slice(0, 40) + ")" : ""));
  const ur = await fetchFn("https://api.github.com/user", { headers: { authorization: "Bearer " + tok, accept: "application/vnd.github+json", "user-agent": "OnAir-Map" } });
  if (!ur.ok) throw new Error("GitHub did not say who signed in (HTTP " + ur.status + ")");
  const u = await ur.json();
  if (typeof u.id !== "number" || typeof u.login !== "string" || !/^[A-Za-z0-9-]{1,39}$/.test(u.login)) throw new Error("GitHub sent an unexpected user");
  return { id: u.id, login: u.login };
}
