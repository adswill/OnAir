// Sign-in pieces of src/auth.js: the marketplace-compatible session token, OAuth state, CSRF, cookies and return paths.
import test from "node:test";
import assert from "node:assert/strict";
import { createHmac } from "node:crypto";
import {
  makeToken, readToken, webSecret, makeState, checkState, csrfFor, checkCsrf, safeNext, parseCookies, tokenHash, bearerOf, githubUser,
  authorizeUrl, STATE_TTL_S,
} from "../src/auth.js";

const SECRET = "test-secret-0123456789abcdef";
const NOW = 1800000000;

/** A token made exactly like tools/marketplace/server/src/crypto.ts makeSession() (node:crypto here, so the format is checked independently). */
function marketToken(secret, payload) {
  const p = Buffer.from(JSON.stringify(payload)).toString("base64url");
  return p + "." + createHmac("sha256", secret).update(p).digest("base64url");
}

test("a marketplace session token is accepted as is (one sign-in for the app)", async () => {
  const tok = marketToken(SECRET, { uid: 42, login: "Alice-1", iat: NOW - 10, exp: NOW + 3600 });
  assert.deepEqual(await readToken(SECRET, tok, NOW), { uid: 42, login: "Alice-1", iat: NOW - 10, exp: NOW + 3600 });
  assert.equal(await readToken(SECRET, tok, NOW + 3601), null, "expired");
  assert.equal(await readToken("another-secret-0123456789", tok, NOW), null, "other key");
  assert.equal(await readToken(SECRET, tok.slice(0, -2) + "AA", NOW), null, "changed mac");
  const [p, mac] = tok.split(".");
  const forged = Buffer.from(JSON.stringify({ uid: 1, login: "admin", iat: NOW, exp: NOW + 9e6 })).toString("base64url");
  assert.equal(await readToken(SECRET, forged + "." + mac, NOW), null, "changed payload");
  assert.equal(await readToken(SECRET, p, NOW), null, "no mac");
  assert.equal(await readToken("short", tok, NOW), null, "a short secret is refused");
  assert.equal(await readToken(SECRET, marketToken(SECRET, { uid: 42, login: "bad login!", iat: NOW, exp: NOW + 60 }), NOW), null);
  // our own maker is the same format
  assert.ok(await readToken(SECRET, await makeToken(SECRET, { uid: 7, login: "bob", iat: NOW, exp: NOW + 60 }), NOW));
});

test("website cookies use their own key: never valid as marketplace tokens, and the reverse", async () => {
  const web = await makeToken(webSecret(SECRET), { uid: 7, login: "bob", iat: NOW, exp: NOW + 60 });
  assert.equal(await readToken(SECRET, web, NOW), null);
  const app = marketToken(SECRET, { uid: 7, login: "bob", iat: NOW, exp: NOW + 60 });
  assert.equal(await readToken(webSecret(SECRET), app, NOW), null);
});

test("stored token hashes are not the token", async () => {
  const tok = marketToken(SECRET, { uid: 42, login: "a", iat: NOW, exp: NOW + 60 });
  const h = await tokenHash(tok);
  assert.match(h, /^[0-9a-f]{32}$/);
  assert.ok(!tok.includes(h));
  assert.equal(h, await tokenHash(tok));
});

test("OAuth state: round trip, wrong state, forged or expired cookie", async () => {
  const { state, cookie } = await makeState(SECRET, NOW, "/u/alice");
  assert.match(state, /^[0-9a-f]{32}$/);
  assert.match(cookie, /^oa_state=[^;]+; Path=\/auth; Max-Age=600; HttpOnly; Secure; SameSite=Lax$/);
  const v = parseCookies(cookie.split(";")[0]).oa_state;
  assert.equal(await checkState(SECRET, v, state, NOW + 5), "/u/alice");
  assert.equal(await checkState(SECRET, v, "0".repeat(32), NOW + 5), null, "another state (CSRF on the callback)");
  assert.equal(await checkState(SECRET, v, state, NOW + STATE_TTL_S + 1), null, "expired");
  assert.equal(await checkState(SECRET, undefined, state, NOW), null, "no cookie (the flow was not started in this browser)");
  const [s, exp, nx, mac] = v.split(".");
  assert.equal(await checkState(SECRET, [s, String(Number(exp) + 9999), nx, mac].join("."), state, NOW), null, "a longer expiry breaks the mac");
  const evil = Buffer.from("https://evil.example/").toString("base64url");
  assert.equal(await checkState(SECRET, [s, exp, evil, mac].join("."), state, NOW), null, "another return path breaks the mac");
  assert.equal(await checkState("other-secret-0123456789", v, state, NOW), null);
  // two logins in a row: each state only matches its own cookie
  const b = await makeState(SECRET, NOW, "/");
  assert.notEqual(b.state, state);
  assert.equal(await checkState(SECRET, v, b.state, NOW), null);
});

test("return paths stay on the map", () => {
  assert.equal(safeNext("/u/alice"), "/u/alice");
  assert.equal(safeNext("/leaders?board=hours"), "/leaders?board=hours");
  for (const bad of ["https://evil.example", "//evil.example", "/\\evil", "javascript:alert(1)", "/u/../../x", "", null, "/a\nb"]) assert.equal(safeNext(bad), "/", String(bad));
  assert.equal(new URL(authorizeUrl("Ov23li3cNdYJUxpl572P", "https://map.on-air.dev/auth/callback", "ab")).searchParams.get("scope"), null, "no scopes asked");
});

test("CSRF: the map's Origin and the session's token are both needed", async () => {
  const h = "0123456789abcdef0123456789abcdef";
  const tok = await csrfFor(SECRET, h);
  const origins = ["https://map.on-air.dev"];
  const req = (hdr) => new Request("https://map.on-air.dev/api/me/settings", { method: "POST", headers: hdr });
  assert.equal(await checkCsrf(req({ Origin: "https://map.on-air.dev", "X-CSRF-Token": tok }), SECRET, h, origins), "");
  assert.equal(await checkCsrf(req({ Origin: "https://evil.example", "X-CSRF-Token": tok }), SECRET, h, origins), "origin");
  assert.equal(await checkCsrf(req({ "X-CSRF-Token": tok }), SECRET, h, origins), "origin");
  assert.equal(await checkCsrf(req({ Origin: "https://map.on-air.dev" }), SECRET, h, origins), "csrf");
  assert.equal(await checkCsrf(req({ Origin: "https://map.on-air.dev", "X-CSRF-Token": await csrfFor(SECRET, "f".repeat(32)) }), SECRET, h, origins), "csrf", "another session's token");
});

test("cookies and bearer headers are parsed strictly", () => {
  assert.deepEqual(parseCookies("a=1; oa_s=xyz; a=2"), { a: "1", oa_s: "xyz" });
  const r = (v) => new Request("https://x/", { headers: { Authorization: v } });
  assert.equal(bearerOf(r("Bearer abcdefghijklmnopqrstuvwxyz.ABC")), "abcdefghijklmnopqrstuvwxyz.ABC");
  assert.equal(bearerOf(r("Bearer short")), "");
  assert.equal(bearerOf(r("Basic abcdefghijklmnopqrstuvwxyz")), "");
});

test("GitHub code exchange: the token is used once for /user and not returned", async () => {
  const calls = [];
  const gh = async (url, init) => {
    calls.push({ url, init });
    if (url.endsWith("/access_token")) return Response.json({ access_token: "ghu_secret", expires_in: 28800, refresh_token: "ghr_secret", token_type: "bearer" });
    if (url.endsWith("/user")) return Response.json({ id: 99, login: "alice" });
    return new Response("no", { status: 404 });
  };
  const u = await githubUser(gh, "cid", "csecret", "code123", "https://map.on-air.dev/auth/callback");
  assert.deepEqual(u, { id: 99, login: "alice" });
  assert.equal(calls.length, 2);
  assert.equal(JSON.parse(calls[0].init.body).redirect_uri, "https://map.on-air.dev/auth/callback");
  assert.ok(!JSON.stringify(u).includes("ghu_") && !JSON.stringify(u).includes("ghr_"));
  await assert.rejects(githubUser(gh, "cid", "cs", "bad code!", "x"));
  await assert.rejects(githubUser(async () => Response.json({ error: "bad_verification_code" }), "cid", "cs", "code123", "x"), /bad_verification_code/);
});
