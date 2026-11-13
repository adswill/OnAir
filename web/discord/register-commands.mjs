// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Registers the global slash commands. Usage:
//   DISCORD_APP_ID=... DISCORD_BOT_TOKEN=... node register-commands.mjs
const { DISCORD_APP_ID, DISCORD_BOT_TOKEN } = process.env;
if (!DISCORD_APP_ID || !DISCORD_BOT_TOKEN) {
  console.error("Set DISCORD_APP_ID and DISCORD_BOT_TOKEN in the environment.");
  process.exit(1);
}
const commands = [
  { name: "map", description: "Screenshot of the OnAir live map with live stats", type: 1 },
  { name: "stats", description: "OnAir live map stats (no screenshot)", type: 1 },
];
const r = await fetch(`https://discord.com/api/v10/applications/${DISCORD_APP_ID}/commands`, {
  method: "PUT",
  headers: { Authorization: "Bot " + DISCORD_BOT_TOKEN, "Content-Type": "application/json" },
  body: JSON.stringify(commands),
});
console.log(r.status, (await r.text()).slice(0, 500));
process.exit(r.ok ? 0 : 1);
