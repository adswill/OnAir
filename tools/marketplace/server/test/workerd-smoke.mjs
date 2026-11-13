// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Smoke test in the real Workers runtime (workerd through Miniflare, with its local D1 and R2): the SQL dialect of D1 (RETURNING, ON CONFLICT,
// LIKE ... ESCAPE), Ed25519 from a PKCS#8 seed, multipart parsing and DecompressionStream as Cloudflare runs them.
// Run after `npm test` (it uses the compiled dist-test/src): node test/workerd-smoke.mjs
import { Miniflare } from "miniflare";
import { readFileSync } from "node:fs";
import { createHash, createPublicKey, randomBytes, verify } from "node:crypto";
import assert from "node:assert/strict";
import { makeZip, manifest } from "../dist-test/test/helpers.js";
import { makeSession, ed25519PublicHex } from "../dist-test/src/crypto.js";

const root = new URL("..", import.meta.url).pathname;
const seed = randomBytes(32).toString("hex");
const env = { SIGNING_KEY: seed, SESSION_SECRET: "s".repeat(40), HASH_SALT: "h".repeat(40), ADMIN_TOKEN: "a".repeat(40), PUBLIC_URL: "https://plugins.example", GITHUB_CLIENT_ID: "x", OWNER_LOGINS: "owner" };
const mf = new Miniflare({
  // the workers list form works with Miniflare 4 old and new (newer releases accept only this form)
  workers: [{
    name: "onair-plugins",
    modules: [
      { type: "ESModule", path: root + "dist-test/src/index.js" },
      { type: "ESModule", path: root + "dist-test/src/app.js" },
      { type: "ESModule", path: root + "dist-test/src/crypto.js" },
      { type: "ESModule", path: root + "dist-test/src/manifest.js" },
      { type: "ESModule", path: root + "dist-test/src/zip.js" },
    ],
    modulesRoot: root + "dist-test/src",
    compatibilityDate: "2026-05-03",
    d1Databases: { DB: "onair-plugins-smoke" },
    r2Buckets: { FILES: "onair-plugins-smoke" },
    bindings: env,
  }],
});
try {
  const db = await mf.getD1Database("DB");
  const sql = readFileSync(root + "schema.sql", "utf8").replace(/--[^\n]*/g, "");
  for (const st of sql.split(";").map((s) => s.trim()).filter(Boolean)) await db.prepare(st).run();
  const pub = await ed25519PublicHex(seed);
  const key = createPublicKey({ key: { kty: "OKP", crv: "Ed25519", x: Buffer.from(pub, "hex").toString("base64url") }, format: "jwk" });
  const open = async (res) => {
    const j = await res.json();
    assert.ok(verify(null, Buffer.from(j.signed), key, Buffer.from(j.sig, "hex")), "signature");
    return JSON.parse(j.signed);
  };
  const st = await (await mf.dispatchFetch("https://plugins.example/api/v1/status")).json();
  assert.equal(st.public_key, pub);

  const m = manifest();
  const bytes = makeZip([["manifest.json", JSON.stringify(m), 8], ["libtone.dylib", "a".repeat(300), 8], ["libtone.so", "b"], ["tone.dll", "c"]]);
  const now = Math.floor(Date.now() / 1000);
  const tok = await makeSession(env.SESSION_SECRET, { uid: 1, login: "alice", iat: now, exp: now + 3600 });
  // the multipart body exactly as OnAir's client writes it (core/src/pstore/market.cpp, multipartBody)
  const B = "----OnAirBoundary" + randomBytes(8).toString("hex");
  const part = (name, value) => `--${B}\r\nContent-Disposition: form-data; name="${name}"\r\n\r\n${value}\r\n`;
  const body = Buffer.concat([
    Buffer.from(part("title", "Tone finder") + part("description", "Finds steady tones.") + part("tags", "ft8 tones")),
    Buffer.from(`--${B}\r\nContent-Disposition: form-data; name="package"; filename="p.onairplugin"\r\nContent-Type: application/zip\r\n\r\n`),
    Buffer.from(bytes), Buffer.from(`\r\n--${B}--\r\n`),
  ]);
  const up = await mf.dispatchFetch("https://plugins.example/api/v1/plugins", { method: "POST", body, headers: { authorization: "Bearer " + tok, "content-type": "multipart/form-data; boundary=" + B } });
  const uj = await up.json();
  assert.equal(up.status, 201, JSON.stringify(uj));
  assert.equal(uj.sha256, createHash("sha256").update(bytes).digest("hex"));

  const l = await open(await mf.dispatchFetch("https://plugins.example/api/v1/plugins?q=tone&sort=relevance"));
  assert.equal(l.plugins[0].id, "tone-finder");
  const like = await mf.dispatchFetch("https://plugins.example/api/v1/plugins/tone-finder/like", { method: "POST", body: JSON.stringify({ install: "a".repeat(32) }), headers: { "content-type": "application/json" } });
  assert.equal((await like.json()).likes, 1);
  const d = await open(await mf.dispatchFetch("https://plugins.example/api/v1/plugins/tone-finder/download?format=json&install=" + "a".repeat(32)));
  const f = await mf.dispatchFetch(d.url);
  assert.equal(createHash("sha256").update(Buffer.from(await f.arrayBuffer())).digest("hex"), d.sha256);
  const p = await open(await mf.dispatchFetch("https://plugins.example/api/v1/plugins/tone-finder?install=" + "a".repeat(32)));
  assert.equal(p.plugin.downloads, 1);
  assert.equal(p.plugin.liked, true);
  console.log("workerd smoke test: ok");
} finally {
  await mf.dispose();
}
