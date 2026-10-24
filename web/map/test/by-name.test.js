// by-name.test.js - the optional public name: validation, server-side sanitising, last-writer-wins, search.
import test from "node:test";
import assert from "node:assert/strict";
import { validateBy, cleanBy, searchKey, fold } from "../src/by-name.js";
import { TrackBook, sanitize } from "../src/track-book.js";

const NOW = 1_800_000_000_000;
const ac = (o = {}) => ({ kind: "aircraft", id: "4CA123", label: "TST1", lat: 50, lon: 8, age: 1, src: "adsb", ...o });
const feed = (b, raw, now, by) => b.accept(sanitize(raw, now, "stationstation", by), now);

test("good names pass, trimmed and with inner spaces collapsed", () => {
  assert.deepEqual(validateBy("  Kris_9  "), { ok: true, name: "Kris_9" });
  assert.equal(validateBy("Anna  Maria-K.").name, "Anna Maria-K.");
  assert.equal(validateBy("").name, "");
  assert.equal(validateBy(null).name, "");
  assert.equal(validateBy("   ").name, "");
});

test("length 2 to 24", () => {
  assert.equal(validateBy("a").code, "length");
  assert.equal(validateBy("ab").ok, true);
  assert.equal(validateBy("x".repeat(24)).ok, true);
  assert.equal(validateBy("x".repeat(25)).code, "length");
});

test("only letters, digits, space and _-. ; control characters, markup and non-ASCII are refused", () => {
  for (const s of ["a<b>c", "bob\nsmith", "bob\u0000x", "café x", "a/b/c", "x;y", "‮evil", "emoji \u{1F600}", "a\tb"]) assert.equal(validateBy(s).ok, false, JSON.stringify(s));
});

test("no URLs or e-mail addresses", () => {
  for (const s of ["http example", "www.site", "me.com", "foo.bar.org", "name@mail", "https"]) assert.equal(validateBy(s).ok, false, s);
  assert.equal(validateBy("v.2 radio").ok, true);
});

test("reserved and banned names, including look-alikes", () => {
  for (const s of ["admin", "Admin", "0nAir", "On-Air", "support", "adm1n", "admin_01", "S u p p o r t"]) assert.equal(validateBy(s).code, "reserved", s);
  assert.equal(validateBy("Hitler fan").code, "banned");
  assert.equal(validateBy("nazi_1").code, "banned");
  assert.equal(validateBy("Adminton").ok, true);
  assert.equal(fold("0n-A1r"), "onair");
});

test("cleanBy: invalid or non-string becomes anonymous", () => {
  assert.equal(cleanBy("admin"), "");
  assert.equal(cleanBy({ x: 1 }), "");
  assert.equal(cleanBy("Kris"), "Kris");
});

test("sanitize re-validates by; old clients send none", () => {
  assert.equal(sanitize(ac(), NOW, "s").by, "");
  assert.equal(sanitize(ac(), NOW, "s", "Kris").by, "Kris");
  assert.equal(sanitize(ac(), NOW, "s", "<script>").by, "");
  assert.equal(sanitize(ac(), NOW, "s", "onair").by, "");
});

test("by and heard_at are on the wire; last writer wins; anonymous update clears the name", () => {
  const b = new TrackBook(NOW);
  feed(b, ac(), NOW, "Anna");
  assert.equal(JSON.parse(b.wire(b.rows.get("aircraft:4CA123"))).by, "Anna");
  assert.equal(JSON.parse(b.wire(b.rows.get("aircraft:4CA123"))).heard_at, NOW);
  const v = b.version;
  const r = feed(b, ac(), NOW + 2000, "Ben");
  assert.equal(r.visible, true);
  assert.ok(b.version > v);
  const w = JSON.parse(b.wire(r));
  assert.equal(w.by, "Ben");
  assert.equal(w.heard_at, NOW + 2000);
  const r2 = feed(b, ac(), NOW + 4000);
  assert.equal(JSON.parse(b.wire(r2)).by, undefined, "an anonymous report is the last one: no name");
});

test("same name again is not a visible change", () => {
  const b = new TrackBook(NOW);
  feed(b, ac(), NOW, "Anna");
  assert.equal(feed(b, ac(), NOW + 1000, "Anna").visible, false);
});

test("search by name: case-insensitive, exact or prefix, newest first, limit 200", () => {
  const b = new TrackBook(NOW);
  feed(b, ac({ id: "4CA001" }), NOW, "Anna");
  feed(b, ac({ id: "4CA002" }), NOW + 1000, "anna");
  feed(b, ac({ id: "4CA003" }), NOW + 2000, "Annabel");
  feed(b, ac({ id: "4CA004" }), NOW + 3000, "Ben");
  feed(b, ac({ id: "4CA005" }), NOW + 4000);
  assert.deepEqual(b.byUser("anna", false).map((r) => r.id), ["aircraft:4CA002", "aircraft:4CA001"]);
  assert.equal(b.byUser("anna", true).length, 3);
  assert.equal(b.byUser("ben").length, 1);
  assert.equal(b.byUser("zzz").length, 0);
  for (let i = 0; i < 250; i++) feed(b, ac({ id: "5" + String(i).padStart(5, "0") }), NOW + 5000 + i, "Bulk");
  assert.equal(b.byUser("bulk").length, 200);
});

test("persisted rows keep the name", () => {
  const b = new TrackBook(NOW);
  b.load([{ id: "aircraft:4CA123", kind: "aircraft", label: "X", sub: "", lat: 1, lon: 2, t: NOW, by: "Anna", path: "[]" }]);
  assert.equal(b.byUser("anna").length, 1);
});

test("searchKey", () => {
  assert.equal(searchKey("Anna%20K"), "anna k");
  assert.equal(searchKey("%E0%A4%A"), "");
  assert.equal(searchKey("a<b"), "");
  assert.equal(searchKey("x".repeat(30)), "");
});
