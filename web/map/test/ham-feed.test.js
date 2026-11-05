// ham-feed.test.js - "ham contacts are always 0": the payload the desktop app posts (map_send.cpp + ham_mapfeed.cpp) goes through the hub's
// ingest rules and the page's contact code, end to end. Each case below was a cause of the zero.
import test from "node:test";
import assert from "node:assert/strict";
import { createHub, sampleHamRaw } from "./dev-server.js";
import { buildContacts, filterContacts, contactFeatures, hamHints } from "../public/ham.js";

const NOW = 1_800_000_000_000;
const contactsOf = (hub, now = NOW) => buildContacts(hub.live().tracks.filter((t) => t.kind === "ham"), now);

test("a ham contact posted by the app reaches the page as a contact with a line", () => {
  let clock = NOW;
  const hub = createHub(() => clock);
  const r = hub.ingest({ tracks: [sampleHamRaw(20)[0]] });
  assert.equal(r.body.kept, 1);
  const c = contactsOf(hub);
  assert.equal(c.length, 1);
  assert.equal(c[0].from, "LY2XX");
  assert.equal(c[0].to, "DL1ABC");
  assert.equal(c[0].band, "20m");
  assert.deepEqual(c[0].b, [11.0, 48.5], "partner position from the hints");
  assert.equal(contactFeatures(c).length, 1);
});

test("CAUSE: the hub dropped every ham track older than 120 s, so a WSPR contact (2 minute slot, decoded after it) and any queued contact never arrived", () => {
  const hub = createHub(() => NOW);
  const wspr = sampleHamRaw()[3];
  assert.ok(wspr.age > 120);
  assert.equal(hub.ingest({ tracks: [wspr] }).body.kept, 1);
  const aged = { ...sampleHamRaw()[0], id: "OH9AA", label: "OH9AA", age: 1500 };   // waited in the app's rate-limited queue
  assert.equal(hub.ingest({ tracks: [aged] }).body.kept, 1);
  assert.equal(hub.ingest({ tracks: [{ ...aged, id: "OH9AB", label: "OH9AB", age: 4000 }] }).body.kept, 0, "older than an hour is still refused");
});

test("the contact time is the time it was heard (report age), not the time the hub received it", () => {
  const hub = createHub(() => NOW);
  hub.ingest({ tracks: [{ ...sampleHamRaw()[0], age: 600 }] });
  const c = contactsOf(hub);
  assert.ok(Math.abs(c[0].ageMin - 10) < 0.01, "ageMin " + c[0].ageMin);
  // so the default "1 h" filter keeps it and a 5 minute filter drops it
  assert.equal(filterContacts(c, { maxAgeMin: 60 }).length, 1);
  assert.equal(filterContacts(c, { maxAgeMin: 5 }).length, 0);
});

test("the whole sample feed arrives: six stations, six contacts, all bands and modes the filters offer", () => {
  const hub = createHub(() => NOW);
  const r = hub.ingest({ tracks: sampleHamRaw() });
  assert.equal(r.body.kept, 6);
  const c = contactsOf(hub);
  assert.equal(c.length, 6);
  assert.deepEqual([...new Set(c.map((x) => x.mode))].sort(), ["FT4", "FT8", "JS8", "QSO", "WSPR"]);
  assert.equal(filterContacts(c, { modes: new Set(["WSPR"]) }).length, 1);
});

test("a post of more than 250 tracks is refused whole by the hub: the app must keep room for its contacts under that limit", () => {
  const hub = createHub(() => NOW);
  const many = Array.from({ length: 251 }, (_, i) => ({ kind: "aprs", id: "AA" + i, label: "AA" + i, lat: 50, lon: 8 + i / 1000, age: 1 }));
  assert.equal(hub.ingest({ tracks: many }).status, 413);
  assert.equal(hub.ingest({ tracks: many.slice(0, 240).concat(sampleHamRaw().slice(0, 6)) }).body.kept, 246);
});

test("a station that works a second partner replaces its contact (one track per callsign); stale contacts leave after the 45 minute TTL", () => {
  let clock = NOW;
  const hub = createHub(() => clock);
  hub.ingest({ tracks: [sampleHamRaw()[0]] });
  clock += 60_000;
  hub.ingest({ tracks: [{ ...sampleHamRaw()[0], extra: '{"m":"FT8","f":14075200,"b":"20m","to":"SM5CC"}', age: 1 }] });
  const c = contactsOf(hub, clock);
  assert.equal(c.length, 1);
  assert.equal(c[0].to, "SM5CC");
  clock += 46 * 60_000;
  assert.equal(hub.live().tracks.filter((t) => t.kind === "ham").length, 0);
});

test("hints survive odd input without throwing", () => {
  for (const extra of ["", "{", "null", "[]", '{"to":5}', '{"tlat":"x","tlon":1}', undefined]) assert.equal(hamHints({ extra }), null);
});

test("ROOT CAUSE of the page-side zero: the page passes Map.values() (an iterator); buildContacts walked it twice, so the second pass saw nothing", () => {
  const hub = createHub(() => NOW);
  hub.ingest({ tracks: sampleHamRaw() });
  const byId = new Map(hub.live().tracks.map((t) => [t.id, t]));
  assert.equal(byId.size, 6);
  const c = buildContacts(byId.values(), NOW);   // exactly what ham-ui.js does
  assert.equal(c.length, 6, "contacts from an iterator");
  assert.equal(buildContacts(new Set(byId.values()), NOW).length, 6, "and from a Set");
  assert.equal(buildContacts(byId, NOW).length, 0, "a Map itself yields [k, v] pairs, not tracks: callers must pass .values()");
});
