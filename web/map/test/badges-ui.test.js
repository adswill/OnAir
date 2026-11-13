// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Badge art (public/badges/art.js), the page helpers (public/badges/page.js) and the map-entry badge (defs.js parseShowcase).
import test from "node:test";
import assert from "node:assert/strict";
import { badgeSvg, GLYPHS, MEDAL, TIER_COLOURS } from "../public/badges/art.js";
import { BADGES, parseShowcase } from "../public/badges/defs.js";
import { badgeRows, statTiles, loginFromPath, fmtNum, bragText, boardValue } from "../public/badges/page.js";
import { publicProfile, BadgeBook, MemoryBadgeBackend } from "../src/badge-stats.js";

test("every badge has art in every tier, self-contained and well formed", () => {
  for (const b of BADGES) for (let t = 0; t <= 4; t++) {
    const s = badgeSvg(b.icon, t, { size: 40, title: b.name + " <x>" });
    assert.match(s, /^<svg [^>]*viewBox="-32 -32 64 64"[^>]*>.*<\/svg>$/s);
    assert.ok(!/href|url\(|<script|on[a-z]+=/i.test(s), "no external references or scripts: " + b.id);
    assert.ok(s.includes(TIER_COLOURS[t]));
    assert.ok(s.includes("&lt;x&gt;"), "the title is escaped");
    assert.equal((s.match(/<svg/g) || []).length, 1);
  }
  assert.ok(MEDAL.startsWith("M") && MEDAL.endsWith("Z"));
  for (const [k, g] of Object.entries(GLYPHS)) for (const d of [...(g.body || []), ...(g.lines || []), ...(g.detail || [])]) assert.match(d, /^M[-0-9., MLHVZCAaz]+$/, k);
  const icons = new Set(BADGES.map((b) => b.icon));
  assert.equal(icons.size, BADGES.length, "one icon per badge");
});

test("map entries: the showcase string is parsed strictly", () => {
  assert.deepEqual(Object.keys(parseShowcase("plane_spotter:3:alice")), ["badge", "tier", "login", "title"]);
  assert.equal(parseShowcase("plane_spotter:3:alice").title, "Plane spotter (Gold)");
  assert.equal(parseShowcase("first_light:4:bob").tier, 1, "a one-tier badge stays at its tier");
  for (const bad of ["", "nope:1:alice", "plane_spotter:9:alice", "plane_spotter:2:<b>", "plane_spotter:2:a/b", null]) assert.equal(parseShowcase(bad), null);
});

test("page helpers", () => {
  assert.equal(loginFromPath("/u/Alice-1"), "Alice-1");
  assert.equal(loginFromPath("/u/a/b"), "");
  assert.equal(fmtNum(1234.4), "1,234");
  assert.equal(fmtNum(3.25, "h"), "3.3 h");
  assert.equal(boardValue("range", 321.4), "321 km");
  const b = new BadgeBook(new MemoryBadgeBackend());
  b.setSettings(1, "alice", { badges: true });
  const u = b.users.get(1);
  u.s.c.aircraft = 150; u.s.shares = 3; u.s.secs = 7200; u.s.first = Date.UTC(2026, 10, 1);
  u.s.earned = { plane_spotter: [[1, Date.UTC(2026, 10, 2)]], first_light: [[1, Date.UTC(2026, 10, 1)]] };
  const p = publicProfile(u, Date.UTC(2026, 10, 3));
  const rows = badgeRows(p);
  assert.equal(rows.length, BADGES.length);
  assert.equal(rows[0].id, "plane_spotter", "earned first, newest first");
  assert.equal(rows[1].id, "first_light");
  assert.ok(rows.slice(2).every((r) => r.tier === 0));
  assert.equal(rows[0].progress, "150 of 1,000");
  assert.ok(statTiles(p).some(([k, v]) => k === "Aircraft" && v === "150"));
  assert.match(bragText(p, "https://map.on-air.dev/u/alice"), /^alice on the OnAir live map: 2 badges, 2 hours/);
});
