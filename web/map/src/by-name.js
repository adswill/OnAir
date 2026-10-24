// by-name.js - the optional public name a station can put on the icons it was the last to hear. Pure (no Cloudflare APIs),
// covered by test/by-name.test.js. The client (app/map/map_name.h) applies the same rules; the server never trusts it.

export const BY_MIN = 2;
export const BY_MAX = 24;
export const BY_SEARCH_LIMIT = 200;

// Names nobody may take: they would pass for the site or its staff.
const RESERVED = ["admin", "administrator", "onair", "support", "moderator", "staff", "official", "system", "root", "owner", "operator", "server", "anonymous", "webmaster", "mapteam"];
// Slurs (kept short on purpose; a server-side list in code is the hook, extend it as needed). Matched inside the folded name.
const BANNED = ["nigger", "nigga", "faggot", "kike", "chink", "tranny", "retard", "nazi", "hitler"];

const MSG = {
  length: "The name must be 2 to 24 characters.",
  chars: "Use only letters, digits, spaces and _ - . in the name.",
  link: "The name cannot be a web address or e-mail address.",
  reserved: "That name is reserved. Please choose another.",
  banned: "That name is not allowed. Please choose another.",
};

/** Lower case, look-alike digits to letters, separators removed: "0n-Air" and "admin_" fold to "onair" and "admin". */
export function fold(s) {
  return String(s).toLowerCase().replace(/[01345$@]/g, (c) => ({ 0: "o", 1: "i", 3: "e", 4: "a", 5: "s", $: "s", "@": "a" })[c]).replace(/[^a-z0-9]/g, "");
}

/**
 * Check a name. Returns { ok: true, name } (trimmed, inner runs of spaces folded to one) or { ok: false, code, message }.
 * An empty or missing name is { ok: true, name: "" } (anonymous).
 */
export function validateBy(raw) {
  if (raw == null) return { ok: true, name: "" };
  if (typeof raw !== "string") return bad("chars");
  const name = raw.trim().replace(/ {2,}/g, " ");
  if (!name) return { ok: true, name: "" };
  if (name.length < BY_MIN || name.length > BY_MAX) return bad("length");
  if (!/^[A-Za-z0-9 _.-]+$/.test(name)) return bad("chars");   // also rules out control characters, @, / and non-ASCII look-alikes
  if (/https?|www|[a-z0-9]\.[a-z]{2,}/i.test(name)) return bad("link");
  if (!/[A-Za-z0-9]{2}/.test(name.replace(/[ _.-]/g, "")) ) return bad("chars");
  const f = fold(name);
  if (RESERVED.some((r) => f === r || f.startsWith(r) && f.length - r.length <= 2)) return bad("reserved");
  if (BANNED.some((w) => f.includes(w))) return bad("banned");
  return { ok: true, name };
}

function bad(code) { return { ok: false, code, message: MSG[code] }; }

/** The name to store: the checked name, or "" for none or an invalid one (the track itself is still accepted). */
export function cleanBy(raw) {
  const v = validateBy(raw);
  return v.ok ? v.name : "";
}

/** Search text for GET /api/by/<name>: lower case, same character limits, 1 to 24 chars. Returns "" when unusable. */
export function searchKey(raw) {
  let s = String(raw || "");
  try { s = decodeURIComponent(s); } catch { return ""; }
  s = s.trim().replace(/ {2,}/g, " ").toLowerCase();
  if (s.length < 1 || s.length > BY_MAX || !/^[a-z0-9 _.-]+$/.test(s)) return "";
  return s;
}
