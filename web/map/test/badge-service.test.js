// The sign-in flow and badge API of src/badge-service.js against a memory backend and a scripted GitHub.
import test from "node:test";
import assert from "node:assert/strict";
import { createHmac } from "node:crypto";
import { BadgeService } from "../src/badge-service.js";
import { MemoryBadgeBackend } from "../src/badge-stats.js";
import { sanitize } from "../src/track-book.js";

const SECRET = "test-secret-0123456789abcdef";
const ORIGIN = "https://map.on-air.dev";
let clock = Date.UTC(2026, 10, 10, 12);
const env = { SESSION_SECRET: SECRET, GITHUB_CLIENT_ID: "Ov23li3cNdYJUxpl572P", GITHUB_CLIENT_SECRET: "cs", BADGE_ADMIN_TOKEN: "admin-token-0123456789abcdef" };

function marketToken(payload) {
  const p = Buffer.from(JSON.stringify(payload)).toString("base64url");
  return p + "." + createHmac("sha256", SECRET).update(p).digest("base64url");
}
const appToken = (uid, login, iat = Math.floor(clock / 1000) - 5) => marketToken({ uid, login, iat, exp: Math.floor(clock / 1000) + 86400 });

function service(users = { "code-a": { id: 1, login: "alice" }, "code-b": { id: 2, login: "bob" } }) {
  const gh = async (url, init) => {
    if (url.endsWith("/access_token")) { const c = JSON.parse(init.body).code; return users[c] ? Response.json({ access_token: "ghu_" + c }) : Response.json({ error: "bad_verification_code" }); }
    if (url.endsWith("/user")) { const c = init.headers.authorization.slice(11); return Response.json(users[c]); }
    return new Response("", { status: 404 });
  };
  return new BadgeService(new MemoryBadgeBackend(), env, { fetchFn: gh, now: () => clock });
}
const call = (svc, method, path, { headers = {}, body } = {}) => {
  const req = new Request(ORIGIN + path, { method, headers, body: body == null ? undefined : JSON.stringify(body) });
  return svc.handle(req, new URL(req.url));
};
const setCookies = (res) => res.headers.getSetCookie().map((c) => c.split(";")[0]);

/** Signs in through the web flow; returns the Cookie header and the CSRF token. */
async function webSignIn(svc, code, next = "/u/alice") {
  const a = await call(svc, "GET", "/auth/login?next=" + encodeURIComponent(next));
  assert.equal(a.status, 302);
  const loc = new URL(a.headers.get("Location"));
  assert.equal(loc.origin + loc.pathname, "https://github.com/login/oauth/authorize");
  assert.equal(loc.searchParams.get("redirect_uri"), "https://map.on-air.dev/auth/callback");
  const st = setCookies(a)[0];
  const b = await call(svc, "GET", `/auth/callback?code=${code}&state=${loc.searchParams.get("state")}`, { headers: { Cookie: st } });
  assert.equal(b.status, 302);
  assert.equal(b.headers.get("Location"), next);
  const ck = setCookies(b).find((c) => c.startsWith("oa_s="));
  assert.ok(ck);
  assert.ok(b.headers.getSetCookie().some((c) => c.startsWith("oa_s=") && /HttpOnly/.test(c) && /Secure/.test(c) && /SameSite=Lax/.test(c)));
  const me = await (await call(svc, "GET", "/api/me", { headers: { Cookie: ck } })).json();
  assert.equal(me.signedIn, true);
  return { cookie: ck, csrf: me.csrf };
}

test("web flow: login, callback, session cookie; a callback without the state cookie or with another state is refused", async () => {
  const svc = service();
  const { cookie, csrf } = await webSignIn(svc, "code-a");
  assert.ok(csrf && csrf.length >= 20);
  const login = await call(svc, "GET", "/auth/login");
  const state = new URL(login.headers.get("Location")).searchParams.get("state");
  assert.equal((await call(svc, "GET", `/auth/callback?code=code-a&state=${state}`)).status, 400, "no state cookie: a login forced on the user from another site");
  assert.equal((await call(svc, "GET", `/auth/callback?code=code-a&state=${"0".repeat(32)}`, { headers: { Cookie: setCookies(login)[0] } })).status, 400);
  assert.equal((await call(svc, "GET", `/auth/callback?code=nope&state=${state}`, { headers: { Cookie: setCookies(login)[0] } })).status, 502);
  // only the hash of the session is stored
  const stored = [...svc.db.tokens.keys()];
  assert.ok(stored.length >= 1 && !stored.some((h) => cookie.includes(h)));
});

test("not configured: no secrets means a clear 503, and the ingest path stays anonymous", async () => {
  const svc = new BadgeService(new MemoryBadgeBackend(), {}, { now: () => clock });
  assert.equal((await call(svc, "GET", "/auth/login")).status, 503);
  const me = await (await call(svc, "GET", "/api/me")).json();
  assert.equal(me.signedIn, false);
  assert.equal(me.configured, false);
  const r = await svc.fromBearer(new Request(ORIGIN + "/ingest", { headers: { Authorization: "Bearer " + appToken(1, "alice") } }));
  assert.ok(r.error);
});

test("settings: cookie changes need Origin and CSRF; the app's Bearer token needs neither", async () => {
  const svc = service();
  const { cookie, csrf } = await webSignIn(svc, "code-a");
  const good = { Cookie: cookie, Origin: ORIGIN, "X-CSRF-Token": csrf, "Content-Type": "application/json" };
  assert.equal((await call(svc, "POST", "/api/me/settings", { headers: { Cookie: cookie, Origin: ORIGIN }, body: { badges: true } })).status, 403);
  assert.equal((await call(svc, "POST", "/api/me/settings", { headers: { ...good, Origin: "https://evil.example" }, body: { badges: true } })).status, 403);
  assert.equal((await call(svc, "POST", "/api/me/settings", { body: { badges: true } })).status, 401);
  const ok = await call(svc, "POST", "/api/me/settings", { headers: good, body: { badges: true, public: true } });
  assert.equal(ok.status, 200);
  assert.deepEqual(await ok.json(), { ok: true, badges: true, public: true, by: "" });
  const app = await call(svc, "POST", "/api/me/settings", { headers: { Authorization: "Bearer " + appToken(1, "alice") }, body: { public: false } });
  assert.equal(app.status, 200);
  assert.equal((await app.json()).public, false);
});

test("linking the map name: one account per name; a linked name is refused to others on ingest", async () => {
  const svc = service();
  const A = { Authorization: "Bearer " + appToken(1, "alice") }, B = { Authorization: "Bearer " + appToken(2, "bob") };
  assert.equal((await call(svc, "POST", "/api/me/settings", { headers: A, body: { by: "Alice Rx" } })).status, 200);
  assert.equal((await call(svc, "POST", "/api/me/settings", { headers: B, body: { by: "alice  rx" } })).status, 409);
  assert.equal((await call(svc, "POST", "/api/me/settings", { headers: B, body: { by: "admin" } })).status, 400, "the name rules still apply");
  assert.equal(svc.byAllowed("Alice Rx", 1), true);
  assert.equal(svc.byAllowed("ALICE RX", 2), false);
  assert.equal(svc.byAllowed("Alice Rx", 0), false, "signed out");
  assert.equal(svc.byAllowed("Someone Else", 0), true, "unlinked names work as before");
  await call(svc, "POST", "/api/me/settings", { headers: A, body: { by: "" } });
  assert.equal(svc.byAllowed("Alice Rx", 2), true, "unlinked again");
});

test("profiles are private until made public; your own is always visible to you", async () => {
  const svc = service();
  const A = { Authorization: "Bearer " + appToken(1, "alice") };
  await call(svc, "POST", "/api/me/settings", { headers: A, body: { badges: true } });
  const row = sanitize({ kind: "aircraft", id: "3C6444", icao: "3C6444", lat: 54.9, lon: 25.5, src: "adsb" }, clock, "station-abcdef12");
  const add = svc.creditPost({ uid: 1, login: "alice" }, { station: "station-abcdef12", rows: [row], prev: new Map(), home: null, extraKeys: [], now: clock });
  assert.ok(add.awards.some((a) => a.id === "first_light" && a.name === "First light"));
  assert.equal((await call(svc, "GET", "/api/u/alice")).status, 404);
  const mine = await call(svc, "GET", "/api/u/alice", { headers: A });
  assert.equal(mine.status, 200);
  assert.equal((await mine.json()).mine, true);
  await call(svc, "POST", "/api/me/settings", { headers: A, body: { public: true } });
  const pub = await (await call(svc, "GET", "/api/u/ALICE")).json();
  assert.equal(pub.stats.objects, 1);
  assert.equal(pub.mine, false);
  assert.equal((await call(svc, "GET", "/api/u/a%2Fb")).status, 400);
});

test("leaderboard: public users only, per period, ranked", async () => {
  const svc = service();
  const mk = async (uid, login, n, pub) => {
    const H = { Authorization: "Bearer " + appToken(uid, login) };
    await call(svc, "POST", "/api/me/settings", { headers: H, body: { badges: true, public: pub } });
    const rows = [];
    for (let i = 0; i < n; i++) rows.push(sanitize({ kind: "aircraft", id: (0x3c0000 + uid * 100 + i).toString(16).toUpperCase(), icao: (0x3c0000 + uid * 100 + i).toString(16).toUpperCase(), lat: 50 + i * 0.01, lon: 8, src: "adsb" }, clock, "station-" + login + "1234"));
    svc.creditPost({ uid, login }, { station: "station-" + login + "1234", rows, prev: new Map(), home: null, extraKeys: [], now: clock });
  };
  await mk(1, "alice", 3, true);
  await mk(2, "bob", 5, true);
  await mk(3, "carol", 9, false);
  const lb = await (await call(svc, "GET", "/api/leaderboard?board=aircraft&period=week")).json();
  assert.deepEqual(lb.rows.map((r) => [r.rank, r.login, r.value]), [[1, "bob", 5], [2, "alice", 3]]);
  const bad = await (await call(svc, "GET", "/api/leaderboard?board=streak&period=week")).json();
  assert.equal(bad.period, "all", "a board without weekly values falls back to all time");
  clock += 7 * 86400000;
  svc.boardCache.clear();
  const next = await (await call(svc, "GET", "/api/leaderboard?board=aircraft&period=week")).json();
  assert.equal(next.rows.length, 0, "a new week starts empty");
  const all = await (await call(svc, "GET", "/api/leaderboard?board=aircraft&period=all")).json();
  assert.equal(all.rows.length, 2);
  // admin hides a cheat
  assert.equal((await call(svc, "POST", "/api/badges/admin", { body: { login: "bob", hidden: true } })).status, 401);
  assert.equal((await call(svc, "POST", "/api/badges/admin", { headers: { Authorization: "Bearer " + env.BADGE_ADMIN_TOKEN }, body: { login: "bob", hidden: true } })).status, 200);
  const after = await (await call(svc, "GET", "/api/leaderboard?board=aircraft&period=all")).json();
  assert.deepEqual(after.rows.map((r) => r.login), ["alice"]);
});

test("signing out apps and deleting the data", async () => {
  const svc = service();
  const { cookie, csrf } = await webSignIn(svc, "code-a");
  const tok = appToken(1, "alice");
  const req = () => new Request(ORIGIN + "/ingest", { method: "POST", headers: { Authorization: "Bearer " + tok } });
  assert.equal((await svc.fromBearer(req())).uid, 1);
  const H = { Cookie: cookie, Origin: ORIGIN, "X-CSRF-Token": csrf };
  assert.equal((await call(svc, "POST", "/api/me/apps/revoke", { headers: H })).status, 200);
  assert.ok((await svc.fromBearer(req())).error, "the old app token no longer counts");
  clock += 1000;
  assert.equal((await svc.fromBearer(new Request(ORIGIN + "/ingest", { headers: { Authorization: "Bearer " + appToken(1, "alice", Math.floor(clock / 1000) + 1) } }))).uid, 1, "a new sign-in works");
  assert.equal((await call(svc, "POST", "/api/me/delete", { headers: H })).status, 200);
  assert.equal(svc.db.users.size, 0);
  assert.equal((await (await call(svc, "GET", "/api/me", { headers: { Cookie: cookie } })).json()).signedIn, false, "the website session went with it");
  // logout needs CSRF too
  const s2 = await webSignIn(svc, "code-b", "/");
  assert.equal((await call(svc, "POST", "/auth/logout", { headers: { Cookie: s2.cookie, Origin: ORIGIN } })).status, 403);
  assert.equal((await call(svc, "POST", "/auth/logout", { headers: { Cookie: s2.cookie, Origin: ORIGIN, "X-CSRF-Token": s2.csrf } })).status, 200);
  assert.equal((await (await call(svc, "GET", "/api/me", { headers: { Cookie: s2.cookie } })).json()).signedIn, false);
});

test("showcase badge on map entries: only public, opted-in users with a linked name", async () => {
  const svc = service();
  const A = { Authorization: "Bearer " + appToken(1, "alice") };
  await call(svc, "POST", "/api/me/settings", { headers: A, body: { badges: true, by: "AliceRx" } });
  const row = sanitize({ kind: "aircraft", id: "3C6444", icao: "3C6444", lat: 54.9, lon: 25.5, src: "adsb" }, clock, "station-abcdef12");
  svc.creditPost({ uid: 1, login: "alice" }, { station: "station-abcdef12", rows: [row], prev: new Map(), home: null, extraKeys: [], now: clock });
  assert.equal(svc.showcaseFor(1, "AliceRx"), "", "not public yet");
  await call(svc, "POST", "/api/me/settings", { headers: A, body: { public: true } });
  assert.equal(svc.showcaseFor(1, "AliceRx"), "first_light:1:alice");
  assert.equal(svc.showcaseFor(1, "Other"), "");
});
