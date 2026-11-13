// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

import test from "node:test";
import assert from "node:assert/strict";
import {
  canonicalize, validateRecord, validateUrl, parseLoc, isPublicIp, expiresAt, isExpired, windowIndex, filterServers, bandRange, timingSafeEqualStr, LIMITS,
} from "../src/logic.js";
import { verifySig } from "../src/crypto.js";
import { resolvesPublic, probeReachable } from "../src/index.js";

// Shared with tests/server/test_web_directory.cpp: same text, same key, same signature.
const VEC = {
  pub: "79b5562e8fe654f94078b112e8a98ba7901f853ae695bed7e0e3910bad049664",
  sig: "e9cb580456c8a6b9e4624da86c4432d5e127b103787337150f94bfcdc4098e63175e721095bee0d123318d0adbfc9dfc2a7d50bce2cd8ab8e963897b7c4d5d08",
  text: "OnAir-Directory-v1\naction=heartbeat\nid=test-server-01\npub=79b5562e8fe654f94078b112e8a98ba7901f853ae695bed7e0e3910bad049664\nts=1790000000\nname=Vilnius roof\nurl=https://sdr.example.org/\nloc=54.69,25.28\nfreq=88000000-108000000;1000000-30000000\nradio=HackRF Pro\ncap=20\nusers=3\ninterval=300\nver=3.0.0\ncontact=\ndesc=Wideband receiver\nreach=1\n",
  rec: { v: 1, action: "heartbeat", id: "test-server-01", pub: "79b5562e8fe654f94078b112e8a98ba7901f853ae695bed7e0e3910bad049664", ts: 1790000000, name: "Vilnius roof",
    url: "https://sdr.example.org/", loc: "54.69,25.28", freq: [[88000000, 108000000], [1000000, 30000000]], radio: "HackRF Pro", cap: 20, users: 3, interval: 300,
    ver: "3.0.0", contact: "", desc: "Wideband receiver", reach: 1 },
};
const NOW = 1790000010;

test("canonical text and signature match the C++ vector", async () => {
  const v = validateRecord({ ...VEC.rec, sig: VEC.sig }, NOW);
  assert.deepEqual(v.errors, []);
  assert.equal(canonicalize(v.rec), VEC.text);
  assert.equal(await verifySig(VEC.pub, VEC.sig, VEC.text), true);
  assert.equal(await verifySig(VEC.pub, VEC.sig, VEC.text.replace("users=3", "users=4")), false);
  assert.equal(await verifySig(VEC.pub, "00".repeat(64), VEC.text), false);
  assert.equal(await verifySig("zz", VEC.sig, VEC.text), false);
});

test("delete text is short and needs no fields", () => {
  const t = canonicalize({ action: "delete", id: "test-server-01", pub: VEC.pub, ts: 5 });
  assert.equal(t, `OnAir-Directory-v1\naction=delete\nid=test-server-01\npub=${VEC.pub}\nts=5\n`);
  const v = validateRecord({ v: 1, action: "delete", id: "test-server-01", pub: VEC.pub, ts: NOW, sig: VEC.sig }, NOW);
  assert.deepEqual(v.errors, []);
});

test("record validation: sizes, types, clock, unknown fields", () => {
  const bad = (patch, re) => {
    const v = validateRecord({ ...VEC.rec, sig: VEC.sig, ...patch }, NOW);
    assert.equal(v.ok, false, JSON.stringify(patch));
    assert.match(v.errors.join("|"), re, JSON.stringify(patch));
  };
  bad({ name: "" }, /name/);
  bad({ name: "x".repeat(LIMITS.name + 1) }, /name/);
  bad({ name: "a\nb" }, /control/);
  bad({ desc: "x".repeat(LIMITS.desc + 1) }, /desc/);
  bad({ desc: "tab\there" }, /control/);
  bad({ ts: NOW + 1000 }, /ts/);
  bad({ ts: NOW - 1000 }, /ts/);
  bad({ ts: 1.5 }, /ts/);
  bad({ cap: -1 }, /cap/);
  bad({ users: 21 }, /users/);
  bad({ interval: 5 }, /interval/);
  bad({ reach: 2 }, /reach/);
  bad({ extra: 1 }, /unknown/);
  bad({ id: "UPPER-case-id" }, /id/);
  bad({ id: "short" }, /id/);
  bad({ pub: "ab" }, /pub/);
  bad({ sig: "ab" }, /sig/);
  bad({ v: 2 }, /v must/);
  bad({ action: "steal" }, /action/);
  bad({ loc: "99,200" }, /loc/);
  bad({ loc: "somewhere" }, /loc/);
  bad({ freq: [[5, 1]] }, /freq/);
  bad({ freq: new Array(9).fill([1, 2]) }, /freq/);
  bad({ freq: [[1.5, 2]] }, /freq/);
  bad({ name: " padded" }, /spaces/);
  bad({ url: "https://sdr.example.org" }, /normal form/);
  assert.equal(validateRecord(null, NOW).ok, false);
  assert.equal(validateRecord([], NOW).ok, false);
});

test("urls: scheme, credentials, private and odd hosts, ports", () => {
  const ok = (u) => assert.equal(validateUrl(u).ok, true, u);
  const no = (u) => assert.equal(validateUrl(u).ok, false, u);
  ok("https://sdr.example.org/");
  ok("http://sdr.example.org:8073/");
  ok("https://sdr.example.org:8443/radio/");
  ok("https://93.184.216.34/");
  ok("https://[2606:4700:4700::1111]/");
  no("ftp://sdr.example.org/");
  no("javascript:alert(1)");
  no("https://user:pw@sdr.example.org/");
  no("https://sdr.example.org/?a=1");
  no("https://sdr.example.org/#x");
  no("https://localhost/");
  no("https://printer/");
  no("https://nas.local/");
  no("https://box.lan:8080/");
  no("https://a.internal/");
  no("https://127.0.0.1/");
  no("https://2130706433/");          // decimal form of 127.0.0.1
  no("https://0x7f.1/");
  no("https://10.1.2.3/");
  no("https://172.16.0.1/");
  no("https://192.168.1.1/");
  no("https://169.254.169.254/");     // cloud metadata
  no("https://100.64.0.1/");
  no("https://[::1]/");
  no("https://[fe80::1]/");
  no("https://[fd00::1]/");
  no("https://[::ffff:127.0.0.1]/");
  no("https://[2002:7f00:1::]/");
  no("https://sdr.example.org:22/");
  no("https://sdr.example.org:6379/");
  no("https://sdr.example.org:81/");
  no("https://sdr.exa mple.org/");
  no("https://" + "a".repeat(250) + ".org/");
  no("x".repeat(300));
  assert.equal(validateUrl("HTTPS://SDR.Example.ORG").url, "https://sdr.example.org/");
});

test("public address test", () => {
  for (const a of ["8.8.8.8", "1.1.1.1", "2606:4700:4700::1111", "2a00:1450:4001:81b::200e"]) assert.equal(isPublicIp(a), true, a);
  for (const a of ["0.0.0.0", "10.0.0.1", "127.0.0.2", "172.31.255.255", "192.168.0.1", "169.254.1.1", "224.0.0.1", "255.255.255.255", "::", "::1", "fe80::1", "fc00::1", "2001:db8::1", "::ffff:10.0.0.1", "not an ip", "999.1.1.1"]) assert.equal(isPublicIp(a), false, a);
});

test("locations", () => {
  assert.deepEqual(parseLoc("54.69,25.28"), { lat: 54.69, lon: 25.28 });
  assert.deepEqual(parseLoc("-33.87,151.21"), { lat: -33.87, lon: 151.21 });
  assert.equal(parseLoc("91,0"), null);
  assert.equal(parseLoc("0,181"), null);
  assert.equal(parseLoc("1.123456,2"), null);          // more than 5 decimals is refused
  const j = parseLoc("KO24");                            // Lithuania
  assert.ok(Math.abs(j.lat - 54.5) < 0.6 && Math.abs(j.lon - 25) < 1.1, JSON.stringify(j));
  const k = parseLoc("KO24pq");
  assert.ok(Math.abs(k.lat - 54.7) < 0.1 && Math.abs(k.lon - 25.3) < 0.2, JSON.stringify(k));
  assert.equal(parseLoc("ZZ99"), null);
  assert.equal(parseLoc(5), null);
});

test("expiry after a few missed heartbeats", () => {
  assert.equal(expiresAt(1000, 300), 1000 + 300 * LIMITS.missedBeats);
  assert.equal(expiresAt(1000, 1), 1000 + LIMITS.minInterval * LIMITS.missedBeats);          // clamped up
  assert.equal(expiresAt(1000, 99999), 1000 + LIMITS.maxInterval * LIMITS.missedBeats);      // clamped down
  assert.equal(isExpired(2000, 1999), false);
  assert.equal(isExpired(2000, 2000), true);
});

test("rate-limit windows", () => {
  assert.equal(windowIndex(119, 60), windowIndex(60, 60));
  assert.notEqual(windowIndex(120, 60), windowIndex(119, 60));
});

test("filters: band, frequency, text, distance", () => {
  const rows = [
    { id: "a", name: "Vilnius HF", description: "", radio: "KiwiSDR", url: "https://a.example.org/", freq: [[0, 30e6]], capacity: 4, users: 4, lat: 54.69, lon: 25.28 },
    { id: "b", name: "Dubai FM", description: "wideband", radio: "HackRF", url: "https://b.example.org/", freq: [[88e6, 108e6]], capacity: 10, users: 1, lat: 25.2, lon: 55.27 },
    { id: "c", name: "No place", description: "", radio: "", url: "https://c.example.org/", freq: [[400e6, 470e6]], capacity: 1, users: 0, lat: null, lon: null },
  ];
  const ids = (p) => filterServers(rows, p).map((r) => r.id);
  assert.deepEqual(ids({}), ["a", "b", "c"]);
  assert.deepEqual(ids({ band: "hf" }), ["a"]);
  assert.deepEqual(ids({ band: "fm" }), ["b"]);
  assert.deepEqual(ids({ band: "uhf" }), ["c"]);
  assert.deepEqual(ids({ band: "nonsense" }), []);
  assert.deepEqual(ids({ band: "constructor" }), []);
  assert.deepEqual(ids({ freq_mhz: "100.1" }), ["b"]);
  assert.deepEqual(ids({ freq_mhz: "abc" }), []);
  assert.deepEqual(ids({ q: "kiwi" }), ["a"]);
  assert.deepEqual(ids({ q: "WIDEBAND" }), ["b"]);
  assert.deepEqual(ids({ free: true }), ["b", "c"]);
  assert.deepEqual(ids({ lat: 54, lon: 25 }), ["a", "b"]);                       // nearest first, no-location rows dropped
  assert.deepEqual(ids({ lat: 54, lon: 25, radius_km: 200 }), ["a"]);
  assert.ok(bandRange("HF") && !bandRange("zzz"));
});

test("constant-time compare", () => {
  assert.equal(timingSafeEqualStr("abc", "abc"), true);
  assert.equal(timingSafeEqualStr("abc", "abd"), false);
  assert.equal(timingSafeEqualStr("abc", "abcd"), false);
});

// A fake fetch for the DNS and probe helpers.
const dns = (map) => async (url) => {
  const u = new URL(url);
  const recs = (map[u.searchParams.get("name")] || {})[u.searchParams.get("type")] || [];
  return { ok: true, json: async () => ({ Answer: recs.map((d) => ({ type: u.searchParams.get("type") === "A" ? 1 : 28, data: d })) }) };
};

test("DNS check: all answers public, at least one", async () => {
  assert.equal(await resolvesPublic("good.example.org", dns({ "good.example.org": { A: ["93.184.216.34"], AAAA: ["2606:4700:4700::1111"] } })), true);
  assert.equal(await resolvesPublic("mixed.example.org", dns({ "mixed.example.org": { A: ["93.184.216.34", "10.0.0.5"] } })), false);
  assert.equal(await resolvesPublic("v6.example.org", dns({ "v6.example.org": { A: ["93.184.216.34"], AAAA: ["::1"] } })), false);
  assert.equal(await resolvesPublic("none.example.org", dns({})), false);
  assert.equal(await resolvesPublic("x.example.org", async () => { throw new Error("net"); }), false);
});

test("reachability probe: fixed path, no redirects, boolean only", async () => {
  const seen = [];
  const f = (status, body) => async (url, opts) => { seen.push([url, opts.redirect]); return { status, text: async () => body }; };
  assert.equal(await probeReachable("https://sdr.example.org/", f(200, '{"proto":1,"name":"x"}')), true);
  assert.deepEqual(seen[0], ["https://sdr.example.org/web/v1/info", "manual"]);
  assert.equal(await probeReachable("https://sdr.example.org/", f(302, "")), false);       // redirects are not followed
  assert.equal(await probeReachable("https://sdr.example.org/", f(200, "not json")), false);
  assert.equal(await probeReachable("https://sdr.example.org/", f(200, '{"name":"x"}')), false);
  seen.length = 0;
  assert.equal(await probeReachable("http://127.0.0.1/", f(200, '{"proto":1,"name":"x"}')), false);   // private literal: never fetched
  assert.equal(seen.length, 0);
});

test("dev-only *.localhost listing is off unless the flag is given", () => {
  assert.equal(validateUrl("http://abc.relay.localhost:8099/").ok, false);
  assert.equal(validateUrl("http://abc.relay.localhost:8099/", true).ok, true);
  assert.equal(validateUrl("http://abc.example.com/", true).ok, true === validateUrl("http://abc.example.com/").ok);
});
