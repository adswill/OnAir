// Every endpoint of the Worker against real SQLite (schema.sql) and an in-memory R2, including the abuse cases.
import test from "node:test";
import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { cleanup, LIMITS } from "../src/app.js";
import { ADMIN, call, opened, pkg, setup, token, uploadForm, verifySigned, type Ctx } from "./helpers.js";

const INSTALL_A = "a".repeat(32), INSTALL_B = "b".repeat(32);

async function publish(c: Ctx, over: Record<string, unknown> = {}, fields?: Record<string, string>, who: [number, string] = [4242, "alice"]) {
  const r = await call(c, "POST", "/api/v1/plugins", { form: uploadForm(pkg(over), fields), auth: await token(c, who[0], who[1]) });
  return r;
}

test("status and the signing key", async () => {
  const c = await setup();
  const r = await call(c, "GET", "/api/v1/status");
  assert.equal(r.status, 200);
  assert.equal(r.j.signing, true);
  assert.equal(r.j.public_key, c.publicHex);
  const none = await setup({ SIGNING_KEY: undefined });
  assert.equal((await call(none, "GET", "/api/v1/plugins")).status, 503);   // nothing unsigned is served as a list
});

test("an empty list is signed and names its request", async () => {
  const c = await setup();
  const r = await call(c, "GET", "/api/v1/plugins?sort=likes");
  const s = opened(c, r);
  assert.equal(s.kind, "list");
  assert.equal(s.request, "/api/v1/plugins?sort=likes");
  assert.equal(s.time, c.deps.clock);
  assert.deepEqual(s.plugins, []);
  // a changed byte breaks the signature
  const tampered = { ...r.j, signed: r.j.signed.replace('"total":0', '"total":1') };
  assert.equal(verifySigned(c.publicHex, tampered), false);
});

test("upload needs a GitHub sign-in", async () => {
  const c = await setup();
  assert.equal((await call(c, "POST", "/api/v1/plugins", { form: uploadForm(pkg()) })).status, 401);
  assert.equal((await call(c, "POST", "/api/v1/plugins", { form: uploadForm(pkg()), auth: "garbage.token" })).status, 401);
  const forged = (await token(c)).replace(/.$/, (x) => (x === "A" ? "B" : "A"));
  assert.equal((await call(c, "POST", "/api/v1/plugins", { form: uploadForm(pkg()), auth: forged })).status, 401);
  const expired = await token(c, 4242, "alice", -10);
  assert.equal((await call(c, "POST", "/api/v1/plugins", { form: uploadForm(pkg()), auth: expired })).status, 401);
  const other = await setup({ SESSION_SECRET: "a-different-secret-0123456789" });
  assert.equal((await call(other, "POST", "/api/v1/plugins", { form: uploadForm(pkg()), auth: await token(c) })).status, 401);
});

test("upload, list, page, tags, download", async () => {
  const c = await setup();
  const bytes = pkg();
  const up = await call(c, "POST", "/api/v1/plugins", { form: uploadForm(bytes, { title: "Tone finder", description: "Finds steady tones and logs them.", repo: "https://github.com/alice/tone", tags: "ft8, Tones #sdr" }), auth: await token(c) });
  assert.equal(up.status, 201, up.text);
  const sha = createHash("sha256").update(bytes).digest("hex");
  assert.equal(up.j.sha256, sha);
  assert.equal(up.j.review, "unreviewed");
  assert.ok(c.r2.m.has(`p/tone-finder/1.0.0/${sha}.onairplugin`));

  const l = opened(c, await call(c, "GET", "/api/v1/plugins"));
  assert.equal(l.total, 1);
  assert.deepEqual(l.plugins[0].tags, ["ft8", "tones", "sdr"]);
  assert.equal(l.plugins[0].owner, "alice");
  assert.equal(l.plugins[0].review, "unreviewed");

  const p = opened(c, await call(c, "GET", "/api/v1/plugins/tone-finder")).plugin;
  assert.equal(p.repo, "https://github.com/alice/tone");
  assert.equal(p.versions[0].sha256, sha);
  assert.deepEqual(p.permissions, ["record"]);
  assert.deepEqual(p.platforms, ["macos", "linux", "windows"]);

  const t = opened(c, await call(c, "GET", "/api/v1/tags"));
  assert.deepEqual(t.tags.map((x: { tag: string }) => x.tag).sort(), ["ft8", "sdr", "tones"]);

  const d = opened(c, await call(c, "GET", "/api/v1/plugins/tone-finder/download?format=json&install=" + INSTALL_A));
  assert.equal(d.kind, "download");
  assert.equal(d.sha256, sha);
  assert.equal(d.size, bytes.length);
  assert.equal(d.url, `https://plugins.example/files/p/tone-finder/1.0.0/${sha}.onairplugin`);
  const f = await call(c, "GET", new URL(d.url).pathname);
  assert.equal(f.status, 200);
  assert.equal(createHash("sha256").update(Buffer.from(f.text, "latin1")).digest("hex").length, 64);
  const rd = await call(c, "GET", "/api/v1/plugins/tone-finder/download");
  assert.equal(rd.status, 302);
  assert.equal(rd.res.headers.get("location"), d.url);
  assert.equal(rd.res.headers.get("x-onair-sha256"), sha);
});

test("served file bytes are exactly the upload", async () => {
  const c = await setup();
  const bytes = pkg();
  await call(c, "POST", "/api/v1/plugins", { form: uploadForm(bytes), auth: await token(c) });
  const sha = createHash("sha256").update(bytes).digest("hex");
  const res = await (await import("../src/app.js")).handle(new Request(`https://plugins.example/files/p/tone-finder/1.0.0/${sha}.onairplugin`), c.env, c.deps);
  const got = new Uint8Array(await res.arrayBuffer());
  assert.equal(createHash("sha256").update(got).digest("hex"), sha);
  assert.equal(res.headers.get("content-type"), "application/zip");
  assert.equal((await call(c, "GET", "/files/p/tone-finder/1.0.0/" + "0".repeat(64) + ".onairplugin")).status, 404);
  assert.equal((await call(c, "GET", "/files/../../etc/passwd")).status, 404);
});

test("FILES_BASE_URL: downloads point at the public bucket", async () => {
  const c = await setup({ FILES_BASE_URL: "https://files.example/" });
  await publish(c);
  const d = opened(c, await call(c, "GET", "/api/v1/plugins/tone-finder/download?format=json"));
  assert.match(d.url, /^https:\/\/files\.example\/p\/tone-finder\/1\.0\.0\/[0-9a-f]{64}\.onairplugin$/);
});

test("required fields and their limits", async () => {
  const c = await setup();
  const t = await token(c);
  const send = (f: Record<string, string>) => call(c, "POST", "/api/v1/plugins", { form: uploadForm(pkg(), f), auth: t });
  assert.equal((await send({ description: "Long enough description." })).j.code, "no_title");
  assert.equal((await send({ title: "T" })).j.code, "no_description");
  assert.equal((await send({ title: "x".repeat(81), description: "Long enough description." })).j.code, "bad_title");
  assert.equal((await send({ title: "Two\nlines", description: "Long enough description." })).j.code, "bad_title");
  assert.equal((await send({ title: "T", description: "short" })).j.code, "bad_description");
  assert.equal((await send({ title: "T", description: "Long enough description.", repo: "http://insecure.example" })).j.code, "bad_repo");
  assert.equal((await send({ title: "T", description: "Long enough description.", repo: "javascript:alert(1)" })).j.code, "bad_repo");
  assert.equal((await send({ title: "T", description: "Long enough description.", tags: "a b c d e f g h i" })).j.code, "bad_tags");
  assert.equal((await send({ title: "T", description: "Long enough description.", tags: "<script>" })).j.code, "bad_tags");
  assert.equal((await send({ title: "AMBE decoder", description: "Long enough description." })).j.code, "policy");
  const noFile = new FormData();
  noFile.append("title", "T");
  noFile.append("description", "Long enough description.");
  assert.equal((await call(c, "POST", "/api/v1/plugins", { form: noFile, auth: t })).j.code, "no_package");
  assert.equal((await call(c, "POST", "/api/v1/plugins", { body: { title: "T" }, auth: t })).j.code, "bad_form");
});

test("oversized upload is refused before and after reading", async () => {
  const c = await setup({ MAX_PACKAGE_BYTES: "4096" });
  const t = await token(c);
  const big = pkg({}, [["blob.bin", new Uint8Array(8192).map((_, i) => (i * 7919) & 255)]]);
  const r = await call(c, "POST", "/api/v1/plugins", { form: uploadForm(big), auth: t });
  assert.equal(r.status, 413);
  const lied = await call(c, "POST", "/api/v1/plugins", { form: uploadForm(pkg()), auth: t, headers: { "content-length": String(10 << 20) } });
  assert.equal(lied.status, 413);
  assert.equal(c.r2.m.size, 0);
});

test("bad packages are refused with the reason", async () => {
  const c = await setup();
  const t = await token(c);
  const cases: [Uint8Array, RegExp][] = [
    [new TextEncoder().encode("this is plainly not a zip file at all"), /not a zip/],
    [pkg({ abi_version: 7 }), /ABI 7/],
    [pkg({ permissions: ["everything"] }), /unknown permission/],
    [pkg({ id: "Bad Id" }), /"id" must be/],
    [pkg({ license: "All rights reserved" }), /not open/],
  ];
  for (const [b, re] of cases) {
    const r = await call(c, "POST", "/api/v1/plugins", { form: uploadForm(b), auth: t });
    assert.equal(r.status, 400, r.text);
    assert.match(r.j.error, re);
  }
  assert.equal(c.r2.m.size, 0);
  assert.equal(opened(c, await call(c, "GET", "/api/v1/plugins")).total, 0);
});

test("ids: taken, yours, reserved", async () => {
  const c = await setup();
  assert.equal((await publish(c)).status, 201);
  assert.equal((await publish(c)).j.code, "exists_yours");
  assert.equal((await publish(c, {}, undefined, [7, "mallory"])).j.code, "taken");
  assert.equal((await publish(c, { id: "onair-dmr" }, undefined, [7, "mallory"])).j.code, "reserved");
  assert.equal((await publish(c, { id: "onair-dmr" }, undefined, [1, "owner"])).status, 201);
});

test("new versions of your own plugin", async () => {
  const c = await setup();
  await publish(c, {}, { title: "Tone finder", description: "Finds steady tones and logs them.", tags: "tones" });
  const t = await token(c);
  const send = (over: Record<string, unknown>, f: Record<string, string> = {}, auth = t) => call(c, "POST", "/api/v1/plugins/tone-finder/versions", { form: uploadForm(pkg(over), f), auth });
  assert.equal((await send({ version: "1.0.0" })).j.code, "version");
  assert.equal((await send({ version: "0.9.0" })).j.code, "version");
  assert.equal((await send({ version: "1.1.0", kind: "action" })).j.code, "kind");
  assert.equal((await send({ version: "1.1.0", id: "other-id" })).j.code, "wrong_id");
  assert.equal((await send({ version: "1.1.0" }, {}, await token(c, 7, "mallory"))).j.code, "not_owner");
  const ok = await send({ version: "1.1.0", permissions: ["record", "network"] }, { description: "Now also sends spots to a server." });
  assert.equal(ok.status, 200, ok.text);
  const p = opened(c, await call(c, "GET", "/api/v1/plugins/tone-finder")).plugin;
  assert.equal(p.version, "1.1.0");
  assert.equal(p.title, "Tone finder");                       // kept
  assert.equal(p.description, "Now also sends spots to a server.");
  assert.deepEqual(p.tags, ["tones"]);                        // kept (no tags field sent)
  assert.deepEqual(p.permissions, ["record", "network"]);
  assert.deepEqual(p.versions.map((v: { version: string }) => v.version).sort(), ["1.0.0", "1.1.0"]);
  const old = opened(c, await call(c, "GET", "/api/v1/plugins/tone-finder/download?format=json&version=1.0.0"));
  assert.equal(old.version, "1.0.0");
  const me = await call(c, "GET", "/api/v1/me", { auth: t });
  assert.equal(me.j.login, "alice");
  assert.equal(me.j.plugins[0].id, "tone-finder");
});

test("upload rate limit per account", async () => {
  const c = await setup();
  const t = await token(c);
  for (let i = 0; i < LIMITS.uploadUser[1]; i++) {
    const r = await call(c, "POST", "/api/v1/plugins", { form: uploadForm(pkg({ id: "p" + i })), auth: t, ip: "203.0.113." + i });
    assert.equal(r.status, 201, r.text);
  }
  const r = await call(c, "POST", "/api/v1/plugins", { form: uploadForm(pkg({ id: "one-more" })), auth: t, ip: "203.0.113.99" });
  assert.equal(r.status, 429);
  c.deps.clock += 86400;
  assert.equal((await call(c, "POST", "/api/v1/plugins", { form: uploadForm(pkg({ id: "next-day" })), auth: t })).status, 201);
});

test("downloads count once per install per day", async () => {
  const c = await setup();
  await publish(c);
  const dl = (q: string, ip?: string) => call(c, "GET", "/api/v1/plugins/tone-finder/download?format=json" + q, { ip });
  await dl("&install=" + INSTALL_A);
  await dl("&install=" + INSTALL_A);
  await dl("&install=" + INSTALL_B);
  await dl("", "192.0.2.1");
  await dl("", "192.0.2.1");
  let p = opened(c, await call(c, "GET", "/api/v1/plugins/tone-finder")).plugin;
  assert.equal(p.downloads, 3);
  c.deps.clock += 86400;
  await dl("&install=" + INSTALL_A);
  p = opened(c, await call(c, "GET", "/api/v1/plugins/tone-finder")).plugin;
  assert.equal(p.downloads, 4);
  // download flooding from one address with fresh install ids stops counting (the files are still served)
  for (let i = 0; i < LIMITS.downloadCountIp[1] + 20; i++) await dl("&install=" + i.toString(16).padStart(32, "0"), "192.0.2.99");
  p = opened(c, await call(c, "GET", "/api/v1/plugins/tone-finder")).plugin;
  assert.equal(p.downloads, 4 + LIMITS.downloadCountIp[1]);
  assert.equal((await dl("", "192.0.2.99")).status, 200);
  assert.equal((await call(c, "GET", "/api/v1/plugins/tone-finder/download?version=9.9.9")).status, 404);
  assert.equal((await call(c, "GET", "/api/v1/plugins/nope/download")).status, 404);
});

test("likes: once per install, unlike, flooding", async () => {
  const c = await setup();
  await publish(c);
  const like = (install: string, ip?: string, on = true) => call(c, on ? "POST" : "DELETE", "/api/v1/plugins/tone-finder/like", { body: { install }, ip });
  assert.equal((await like(INSTALL_A)).j.likes, 1);
  assert.equal((await like(INSTALL_A)).j.likes, 1);
  assert.equal((await like(INSTALL_B)).j.likes, 2);
  let p = opened(c, await call(c, "GET", "/api/v1/plugins/tone-finder?install=" + INSTALL_A)).plugin;
  assert.equal(p.liked, true);
  assert.equal(p.likes, 2);
  assert.equal((await like(INSTALL_A, undefined, false)).j.likes, 1);
  assert.equal((await like(INSTALL_A, undefined, false)).j.likes, 1);
  assert.equal((await call(c, "POST", "/api/v1/plugins/tone-finder/unlike", { body: { install: INSTALL_B } })).j.likes, 0);
  p = opened(c, await call(c, "GET", "/api/v1/plugins/tone-finder?install=" + INSTALL_A)).plugin;
  assert.equal(p.liked, false);
  assert.equal((await like("not-hex")).status, 400);
  assert.equal((await like("")).status, 400);
  assert.equal((await call(c, "POST", "/api/v1/plugins/nope/like", { body: { install: INSTALL_A } })).status, 404);
  // the install id is stored only as a salted hash
  const voters = c.db.raw.exec("SELECT voter FROM likes UNION SELECT voter FROM dl_seen") as { values: string[][] }[];
  for (const row of voters.flatMap((x) => x.values)) assert.ok(!String(row[0]).includes(INSTALL_A) && !String(row[0]).includes(INSTALL_B));
  // one install toggling: limited
  let last = 0;
  for (let i = 0; i < LIMITS.likeInstall[1] + 5; i++) last = (await like("c".repeat(32), "198.51.100.50", i % 2 === 0)).status;
  assert.equal(last, 429);
  // one address with many fresh install ids: limited
  let ok = 0;
  for (let i = 0; i < LIMITS.likeIp[1] + 30; i++) if ((await like((i + 1000).toString(16).padStart(32, "0"), "198.51.100.60")).status === 200) ok++;
  assert.equal(ok, LIMITS.likeIp[1]);
  p = opened(c, await call(c, "GET", "/api/v1/plugins/tone-finder")).plugin;
  assert.equal(p.likes, LIMITS.likeIp[1]);   // the toggling install ended unliked, so only the flood's accepted likes are left
});

test("reports need a sign-in and reach the admin list", async () => {
  const c = await setup();
  await publish(c);
  assert.equal((await call(c, "POST", "/api/v1/plugins/tone-finder/report", { body: { reason: "malware" } })).status, 401);
  const t = await token(c, 99, "bob");
  assert.equal((await call(c, "POST", "/api/v1/plugins/tone-finder/report", { body: { reason: "boring" }, auth: t })).status, 400);
  assert.equal((await call(c, "POST", "/api/v1/plugins/nope/report", { body: { reason: "spam" }, auth: t })).status, 404);
  assert.equal((await call(c, "POST", "/api/v1/plugins/tone-finder/report", { body: { reason: "malware", text: "It opens a shell.", version: "1.0.0" }, auth: t })).status, 201);
  assert.equal((await call(c, "GET", "/api/v1/admin/reports")).status, 401);
  assert.equal((await call(c, "GET", "/api/v1/admin/reports", { auth: "wrong-token-wrong-token-wrong" })).status, 401);
  const list = await call(c, "GET", "/api/v1/admin/reports", { auth: ADMIN });
  assert.equal(list.j.reports.length, 1);
  assert.equal(list.j.reports[0].reporter_login, "bob");
  assert.equal(list.j.reports[0].reason, "malware");
  assert.equal((await call(c, "POST", `/api/v1/admin/reports/${list.j.reports[0].n}/close`, { auth: ADMIN })).status, 200);
  assert.equal((await call(c, "GET", "/api/v1/admin/reports", { auth: ADMIN })).j.reports.length, 0);
  assert.equal((await call(c, "GET", "/api/v1/admin/reports?status=all", { auth: ADMIN })).j.reports.length, 1);
  for (let i = 0; i < LIMITS.reportUser[1] - 1; i++) await call(c, "POST", "/api/v1/plugins/tone-finder/report", { body: { reason: "spam" }, auth: t });
  assert.equal((await call(c, "POST", "/api/v1/plugins/tone-finder/report", { body: { reason: "spam" }, auth: t })).status, 429);
});

test("a short admin token disables the admin endpoints", async () => {
  const c = await setup({ ADMIN_TOKEN: "short" });
  assert.equal((await call(c, "GET", "/api/v1/admin/reports", { auth: "short" })).status, 401);
});

for (const filesBase of ["", "https://files.example"]) {
  test("pull and restore" + (filesBase ? " (public files domain)" : ""), async () => {
    const c = await setup({ FILES_BASE_URL: filesBase || undefined });
    await publish(c);
    const d = opened(c, await call(c, "GET", "/api/v1/plugins/tone-finder/download?format=json"));
    const key = new URL(d.url).pathname.replace(/^\/(files\/)?/, "");
    assert.equal((await call(c, "POST", "/api/v1/admin/plugins/tone-finder/pull", { body: { reason: "malware report confirmed" } })).status, 401);
    assert.equal((await call(c, "POST", "/api/v1/admin/plugins/tone-finder/pull", { body: { reason: "malware report confirmed" }, auth: ADMIN })).j.status, "pulled");
    const l = opened(c, await call(c, "GET", "/api/v1/plugins"));
    assert.equal(l.total, 0);
    assert.deepEqual(l.pulled, [{ id: "tone-finder", reason: "malware report confirmed" }]);
    assert.equal(opened(c, await call(c, "GET", "/api/v1/plugins/tone-finder")).plugin.status, "pulled");
    assert.equal((await call(c, "GET", "/api/v1/plugins/tone-finder/download")).status, 410);
    assert.equal((await call(c, "GET", "/api/v1/plugins/tone-finder/download?format=json")).status, 410);
    assert.equal((await call(c, "GET", "/files/" + key)).status, 410);
    assert.equal((await call(c, "POST", "/api/v1/plugins/tone-finder/like", { body: { install: INSTALL_A } })).status, 410);
    assert.equal((await call(c, "POST", "/api/v1/plugins/tone-finder/versions", { form: uploadForm(pkg({ version: "2.0.0" })), auth: await token(c) })).j.code, "pulled");
    if (filesBase) { assert.ok(!c.r2.m.has(key)); assert.ok(c.r2.m.has("pulled/" + key)); }
    assert.equal((await call(c, "POST", "/api/v1/admin/plugins/tone-finder/restore", { auth: ADMIN })).j.status, "live");
    assert.ok(c.r2.m.has(key));
    assert.equal(opened(c, await call(c, "GET", "/api/v1/plugins")).total, 1);
    assert.equal((await call(c, "GET", "/api/v1/plugins/tone-finder/download")).status, 302);
  });
}

test("ban a GitHub user", async () => {
  const c = await setup();
  await publish(c, {}, undefined, [7, "Mallory"]);
  const r = await call(c, "POST", "/api/v1/admin/users/mallory/ban", { body: { reason: "malware", pull_plugins: true }, auth: ADMIN });
  assert.equal(r.j.github_id, 7);
  assert.equal(r.j.pulled, 1);
  assert.equal(opened(c, await call(c, "GET", "/api/v1/plugins")).total, 0);
  const t = await token(c, 7, "Mallory");
  assert.equal((await call(c, "POST", "/api/v1/plugins", { form: uploadForm(pkg({ id: "again" })), auth: t })).status, 403);
  // a renamed account is still caught by its numeric id
  assert.equal((await call(c, "POST", "/api/v1/plugins", { form: uploadForm(pkg({ id: "again" })), auth: await token(c, 7, "mallory2") })).status, 403);
  assert.equal((await call(c, "POST", "/api/v1/plugins/again/report", { body: { reason: "spam" }, auth: t })).status, 403);
  assert.equal((await call(c, "GET", "/api/v1/admin/bans", { auth: ADMIN })).j.bans.length, 1);
  assert.equal((await call(c, "POST", "/api/v1/admin/users/mallory/unban", { auth: ADMIN })).status, 200);
  assert.equal((await call(c, "POST", "/api/v1/plugins", { form: uploadForm(pkg({ id: "again" })), auth: t })).status, 201);
  assert.equal((await call(c, "POST", "/api/v1/admin/users/not a login/ban", { auth: ADMIN })).status, 400);
});

test("GitHub device flow sign-in", async () => {
  const c = await setup();
  const s = await call(c, "POST", "/api/v1/auth/device");
  assert.equal(s.status, 200);
  assert.equal(s.j.user_code, "ABCD-1234");
  assert.equal(s.j.verification_uri, "https://github.com/login/device");
  assert.equal((await call(c, "POST", "/api/v1/auth/poll", { body: { device_code: s.j.device_code } })).j.status, "pending");
  const ok = await call(c, "POST", "/api/v1/auth/poll", { body: { device_code: s.j.device_code } });
  assert.equal(ok.j.status, "ok");
  assert.equal(ok.j.login, "alice");
  const me = await call(c, "GET", "/api/v1/me", { auth: ok.j.token });
  assert.equal(me.j.login, "alice");
  // the GitHub token went to api.github.com once and is stored nowhere
  const issued = c.deps.gh.tokensIssued[0];
  assert.ok(!ok.text.includes(issued));
  const dump = JSON.stringify(c.db.raw.exec("SELECT * FROM rl"));
  assert.ok(!dump.includes(issued));
  assert.equal((await call(c, "POST", "/api/v1/auth/poll", { body: { device_code: "x" } })).status, 400);
  const off = await setup({ GITHUB_CLIENT_ID: undefined });
  assert.equal((await call(off, "POST", "/api/v1/auth/device")).status, 503);
  // a banned account gets no session
  await call(c, "POST", "/api/v1/admin/users/alice/ban", { body: { reason: "x" }, auth: ADMIN });
  c.deps.gh.pending = 0;
  assert.equal((await call(c, "POST", "/api/v1/auth/poll", { body: { device_code: s.j.device_code } })).status, 403);
});

test("sign-in attempts are rate limited per address", async () => {
  const c = await setup();
  let last = 0;
  for (let i = 0; i <= LIMITS.authIp[1]; i++) last = (await call(c, "POST", "/api/v1/auth/device", { ip: "192.0.2.200" })).status;
  assert.equal(last, 429);
});

test("search, sorting and filters", async () => {
  const c = await setup();
  const t = await token(c);
  const up = async (id: string, title: string, description: string, tags: string, over: Record<string, unknown> = {}) => {
    const r = await call(c, "POST", "/api/v1/plugins", { form: uploadForm(pkg({ id, ...over }), { title, description, tags }), auth: t });
    assert.equal(r.status, 201, r.text);
    c.deps.clock += 60;
  };
  await up("ft8-spotter", "FT8 spotter", "Shows FT8 spots on a map with filters.", "ft8,map");
  await up("map-tools", "Map tools", "Extra layers for the map; also handy for FT8 spots.", "map");
  await up("wav-export", "WAV export", "Writes the audio to WAV files.", "audio", { kind: "action", modes: undefined });
  await up("pct-test", "100% tone", "Percent in the title.", "test", { library: { linux: "libx.so" } });
  for (let i = 0; i < 3; i++) await call(c, "POST", "/api/v1/plugins/map-tools/like", { body: { install: String(i).repeat(32) } });
  for (let i = 0; i < 2; i++) await call(c, "GET", "/api/v1/plugins/wav-export/download?install=" + String(i).repeat(32));
  const ids = async (q: string) => opened(c, await call(c, "GET", "/api/v1/plugins?" + q)).plugins.map((p: { id: string }) => p.id);
  assert.deepEqual(await ids("q=ft8"), ["ft8-spotter", "map-tools"]);   // title and tag beat a mention in the description
  assert.deepEqual(await ids("q=ft8%20map"), ["ft8-spotter", "map-tools"]);
  assert.deepEqual(await ids("q=nothing-matches"), []);
  assert.deepEqual(await ids("q=%25"), ["pct-test"]);                    // % is a literal, not a wildcard
  assert.deepEqual(await ids("q=_"), []);
  assert.deepEqual((await ids("sort=likes")).slice(0, 1), ["map-tools"]);
  assert.deepEqual((await ids("sort=downloads")).slice(0, 1), ["wav-export"]);
  assert.deepEqual((await ids("sort=newest")).slice(0, 1), ["pct-test"]);
  assert.deepEqual(await ids("tag=map&sort=newest"), ["map-tools", "ft8-spotter"]);
  assert.deepEqual(await ids("kind=action"), ["wav-export"]);
  assert.deepEqual((await ids("platform=windows")).includes("pct-test"), false);
  assert.deepEqual(await ids("platform=linux&q=percent"), ["pct-test"]);
  const pg = opened(c, await call(c, "GET", "/api/v1/plugins?sort=newest&limit=2&page=2"));
  assert.equal(pg.total, 4);
  assert.deepEqual(pg.plugins.map((p: { id: string }) => p.id), ["map-tools", "ft8-spotter"]);
  assert.equal((await call(c, "GET", "/api/v1/plugins?tag=%3Cx%3E")).status, 400);
  assert.equal((await call(c, "GET", "/api/v1/plugins?kind=virus")).status, 400);
  assert.equal((await call(c, "GET", "/api/v1/plugins?platform=amiga")).status, 400);
});

test("unknown routes and methods", async () => {
  const c = await setup();
  assert.equal((await call(c, "GET", "/api/v1/nothing")).status, 404);
  assert.equal((await call(c, "PUT", "/api/v1/plugins")).status, 404);
  assert.equal((await call(c, "GET", "/api/v1/plugins/..%2F..%2Fetc")).status, 404);
  assert.equal((await call(c, "GET", "/")).status, 200);
});

test("the hourly clean-up removes old rate-limit windows", async () => {
  const c = await setup();
  await call(c, "POST", "/api/v1/auth/device");
  const count = () => (c.db.raw.exec("SELECT COUNT(*) FROM rl") as { values: number[][] }[])[0].values[0][0];
  assert.equal(count(), 1);
  await cleanup(c.env, c.deps.clock + 7200);
  assert.equal(count(), 0);
});
