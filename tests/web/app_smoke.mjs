// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// OnAir Web 3.9: a headless browser smoke test of the page (opt-in: it needs Playwright's Chromium, which CI does not download).
//   ONAIR_PLAYWRIGHT=1 node tests/web/app_smoke.mjs <path to onair-server>
// Starts onair-server on the test signal with a temporary settings file and an isolated browser profile (never the owner's browser),
// opens /app/, checks the page loads with no console errors, the status reaches "receiving", the mode picker lists more than 250 modes,
// a search finds ADS-B, selecting it changes the title, the panel shows tiles, and the Settings page opens for the LAN owner. No sound is
// played: the Listen button is not pressed. Exit 77 = skipped (no Playwright or not asked for), so ctest treats it as a skip.
import { spawn } from "node:child_process";
import { mkdtempSync, writeFileSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";

const PLAYWRIGHT_VERSION = "1.49.1";   // pinned: install with `npm i playwright@1.49.1 && npx playwright install chromium`
if (process.env.ONAIR_PLAYWRIGHT !== "1") { console.log("app_smoke: skipped (set ONAIR_PLAYWRIGHT=1 and install playwright@" + PLAYWRIGHT_VERSION + ")"); process.exit(77); }
let chromium;
try { ({ chromium } = await import("playwright")); } catch (e) { console.log("app_smoke: skipped (playwright not installed): " + e.message); process.exit(77); }
const server = process.argv[2];
if (!server) { console.error("usage: node tests/web/app_smoke.mjs <onair-server>"); process.exit(2); }

const dir = mkdtempSync(join(tmpdir(), "onair-smoke-"));
const cfg = join(dir, "server.conf");
writeFileSync(cfg, "device=test\nmode=fm\nweb=1\nweb_port=0\nweb_admin_token=0123456789abcdef0123456789abcdef\napp_root=" + join(process.cwd(), "web", "onair-app") + "\n");
const proc = spawn(server, ["--config", cfg], { stdio: ["ignore", "pipe", "pipe"] });
let port = 0, out = "";
proc.stdout.on("data", (d) => { out += d; const m = /browser app is at http:\/\/[^:]+:(\d+)\/app\//.exec(out); if (m) port = +m[1]; });
proc.stderr.on("data", (d) => { out += d; });
for (let i = 0; i < 100 && !port; i++) await new Promise(r => setTimeout(r, 100));
if (!port) { console.error("server did not start:\n" + out); proc.kill(); process.exit(1); }

let fails = 0;
const check = (c, m) => { if (!c) { fails++; console.error("FAIL " + m); } };
const browser = await chromium.launch({ headless: true, args: ["--mute-audio", "--autoplay-policy=user-gesture-required"] });
const ctx = await browser.newContext({ viewport: { width: 1200, height: 800 } });
const page = await ctx.newPage();
const errors = [];
page.on("console", (m) => { if (m.type() === "error") errors.push(m.text()); });
page.on("pageerror", (e) => errors.push(String(e)));
try {
  await page.goto("http://127.0.0.1:" + port + "/app/", { waitUntil: "load" });
  check((await page.title()).includes("OnAir"), "title");
  // the first-start password prompt (LAN owner, no password): press Later
  const later = page.getByRole("button", { name: "Later" });
  if (await later.isVisible({ timeout: 3000 }).catch(() => false)) await later.click();
  await page.getByRole("button", { name: "Start" }).click();
  await page.waitForFunction(() => document.getElementById("state").textContent.startsWith("receiving"), null, { timeout: 15000 });
  check(true, "receiving");
  await page.waitForFunction(() => document.querySelectorAll("#panel .tile").length > 3, null, { timeout: 10000 }).catch(() => check(false, "panel tiles"));
  await page.waitForFunction(() => document.getElementById("specinfo").textContent.includes("bins"), null, { timeout: 10000 }).catch(() => check(false, "spectrum rows arrive"));
  await page.getByRole("button", { name: /Mode|FM/ }).first().click();
  await page.waitForSelector(".modes button");
  const count = await page.locator(".modes button").count();
  check(count > 250, "mode picker lists " + count + " modes");
  await page.locator(".search").fill("ads-b");
  await page.waitForFunction(() => document.querySelectorAll(".modes button").length < 10);
  await page.locator(".modes button").first().click();
  await page.waitForFunction(() => document.getElementById("panelTitle").textContent === "ADS-B", null, { timeout: 10000 }).catch(() => check(false, "switching to ADS-B"));
  await page.getByRole("button", { name: "Settings" }).click();
  await page.waitForSelector(".settings");
  check(await page.locator(".settings .it").count() > 10, "settings items");
  await page.keyboard.press("Escape");
  // phone layout: the tabs appear
  await page.setViewportSize({ width: 390, height: 800 });
  check(await page.locator("#tabs").isVisible(), "phone tabs");
  await page.locator("#tabs button[data-tab=panel]").click();
  check(await page.locator("#panelSec").isVisible(), "panel tab");
  check(errors.length === 0, "console errors: " + errors.join(" | "));
} catch (e) { check(false, "exception: " + e.message); }
await browser.close();
proc.kill("SIGTERM");
await new Promise(r => setTimeout(r, 500));
rmSync(dir, { recursive: true, force: true });
console.log("app_smoke: " + (fails ? "FAIL (" + fails + ")" : "ok"));
process.exit(fails ? 1 : 0);
