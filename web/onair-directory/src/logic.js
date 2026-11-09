// Pure functions of the OnAir directory: the signing text, record validation, URL / address checks, expiry, rate-limit windows, band and
// distance filters. No I/O, so node tests cover all of it (test/logic.test.js). The C++ side (server/web/web_directory_proto.cpp) builds the
// same signing text; test vectors in both test suites keep the two in step.

export const PROTO = 1;
export const HEADER = "OnAir-Directory-v1";
export const LIMITS = {
  name: 48, url: 200, loc: 24, radio: 48, ver: 24, contact: 80, desc: 280, id: 32,
  maxRanges: 8, maxCap: 100000, maxUsers: 100000, minInterval: 60, maxInterval: 3600, defaultInterval: 300,
  missedBeats: 3,           // an entry expires after this many missed heartbeats
  clockSkewSec: 300,        // |ts - now| must be inside this
  maxBody: 4096,
};

const enc = new TextEncoder();
export const byteLen = (s) => enc.encode(s).length;
const CONTROL = new RegExp("[\\x00-\\x1f\\x7f-\\x9f\\u2028\\u2029]");
const hasControl = (s) => CONTROL.test(s);

export function toHex(u8) { return Array.from(u8, (b) => b.toString(16).padStart(2, "0")).join(""); }
export function fromHex(h, bytes) {
  if (typeof h !== "string" || h.length !== bytes * 2 || !/^[0-9a-f]+$/.test(h)) return null;
  const o = new Uint8Array(bytes);
  for (let i = 0; i < bytes; i++) o[i] = parseInt(h.substr(i * 2, 2), 16);
  return o;
}

// ---- the text that is signed ---------------------------------------------------------------------------------------------------------
// Fixed key order, "key=value" lines, every value is a string or an integer already in its final form, so both languages produce the same bytes.
const HEARTBEAT_KEYS = ["action", "id", "pub", "ts", "name", "url", "loc", "freq", "radio", "cap", "users", "interval", "ver", "contact", "desc", "reach"];
const DELETE_KEYS = ["action", "id", "pub", "ts"];

export function freqText(ranges) { return (ranges || []).map((r) => `${r[0]}-${r[1]}`).join(";"); }

export function canonicalize(rec) {
  const keys = rec.action === "delete" ? DELETE_KEYS : HEARTBEAT_KEYS;
  let s = HEADER + "\n";
  for (const k of keys) {
    let v = rec[k];
    if (k === "freq") v = freqText(v);
    if (v === undefined || v === null) v = "";
    s += `${k}=${v}\n`;
  }
  return s;
}

// ---- locations -------------------------------------------------------------------------------------------------------------------------
// "lat,lon" (decimal degrees, 0 to 5 decimals) or a Maidenhead locator (4, 6 or 8 characters). Returns {lat, lon} or null.
export function parseLoc(s) {
  if (typeof s !== "string") return null;
  let m = /^(-?\d{1,2}(?:\.\d{1,5})?),(-?\d{1,3}(?:\.\d{1,5})?)$/.exec(s);
  if (m) {
    const lat = Number(m[1]), lon = Number(m[2]);
    if (lat >= -90 && lat <= 90 && lon >= -180 && lon <= 180) return { lat, lon };
    return null;
  }
  m = /^([A-R]{2})(\d{2})(?:([A-X]{2})(?:(\d{2}))?)?$/i.exec(s);
  if (!m) return null;
  const A = (c) => c.toUpperCase().charCodeAt(0) - 65;
  let lon = A(m[1][0]) * 20 - 180, lat = A(m[1][1]) * 10 - 90;
  lon += Number(m[2][0]) * 2; lat += Number(m[2][1]);
  let w = 2, h = 1;
  if (m[3]) { lon += A(m[3][0]) * (2 / 24); lat += A(m[3][1]) * (1 / 24); w = 2 / 24; h = 1 / 24; }
  if (m[4]) { lon += Number(m[4][0]) * (w / 10); lat += Number(m[4][1]) * (h / 10); w /= 10; h /= 10; }
  return { lat: Math.round((lat + h / 2) * 1e4) / 1e4, lon: Math.round((lon + w / 2) * 1e4) / 1e4 };
}

export function haversineKm(a, b) {
  const R = 6371, r = Math.PI / 180;
  const dLat = (b.lat - a.lat) * r, dLon = (b.lon - a.lon) * r;
  const h = Math.sin(dLat / 2) ** 2 + Math.cos(a.lat * r) * Math.cos(b.lat * r) * Math.sin(dLon / 2) ** 2;
  return 2 * R * Math.asin(Math.min(1, Math.sqrt(h)));
}

// ---- addresses and URLs ----------------------------------------------------------------------------------------------------------------
export function parseIPv4(s) {
  const m = /^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})$/.exec(s);
  if (!m) return null;
  const o = m.slice(1).map(Number);
  return o.every((x) => x <= 255) ? o : null;
}

export function isPublicIPv4(o) {
  const [a, b, c] = o;
  if (a === 0 || a === 10 || a === 127 || a >= 224) return false;
  if (a === 100 && b >= 64 && b <= 127) return false;               // carrier-grade NAT
  if (a === 169 && b === 254) return false;
  if (a === 172 && b >= 16 && b <= 31) return false;
  if (a === 192 && b === 0 && (c === 0 || c === 2)) return false;
  if (a === 192 && b === 168) return false;
  if (a === 192 && b === 88 && c === 99) return false;
  if (a === 198 && (b === 18 || b === 19)) return false;
  if (a === 198 && b === 51 && c === 100) return false;
  if (a === 203 && b === 0 && c === 113) return false;
  return true;
}

// Expands an IPv6 text address to 8 groups, or null.
export function parseIPv6(s) {
  s = s.replace(/^\[|\]$/g, "").toLowerCase();
  if (!/^[0-9a-f:.]+$/.test(s) || !s.includes(":")) return null;
  const m = /^(.*:)(\d+\.\d+\.\d+\.\d+)$/.exec(s);
  if (m) {
    const v4 = parseIPv4(m[2]);
    if (!v4) return null;
    s = m[1] + ((v4[0] << 8) | v4[1]).toString(16) + ":" + ((v4[2] << 8) | v4[3]).toString(16);
  }
  const halves = s.split("::");
  if (halves.length > 2) return null;
  const part = (t) => (t === "" ? [] : t.split(":"));
  const head = part(halves[0]), tail = halves.length === 2 ? part(halves[1]) : [];
  if (halves.length === 1 && head.length !== 8) return null;
  if (halves.length === 2 && head.length + tail.length > 7) return null;
  const mid = halves.length === 2 ? new Array(8 - head.length - tail.length).fill("0") : [];
  const g = [...head, ...mid, ...tail];
  if (g.length !== 8 || !g.every((x) => /^[0-9a-f]{1,4}$/.test(x))) return null;
  return g.map((x) => parseInt(x, 16));
}

// Only global unicast 2000::/3 is public; 6to4, Teredo, documentation and the NAT64 / IPv4-mapped forms are refused (they can hide private v4 addresses).
export function isPublicIPv6(g) {
  if ((g[0] & 0xe000) !== 0x2000) return false;
  if (g[0] === 0x2001 && (g[1] === 0x0000 || g[1] === 0x0db8 || (g[1] & 0xfff0) === 0x0010)) return false;
  if (g[0] === 0x2002) return false;
  if (g[0] === 0x3fff) return false;
  return true;
}

export function isPublicIp(text) {
  const v4 = parseIPv4(text);
  if (v4) return isPublicIPv4(v4);
  const v6 = parseIPv6(text);
  return v6 ? isPublicIPv6(v6) : false;
}

const BAD_SUFFIX = [".local", ".localhost", ".internal", ".lan", ".home", ".home.arpa", ".corp", ".intranet", ".private", ".test", ".example", ".invalid", ".onion", ".arpa"];
const BAD_PORTS = new Set([21, 22, 23, 25, 53, 110, 111, 135, 137, 139, 143, 389, 445, 465, 587, 993, 995, 1433, 2049, 3306, 3389, 5432, 5900, 6379, 9200, 11211, 27017]);

// Literal host and scheme / port rules. A hostname still has to resolve to public addresses only (the Worker checks the answers with isPublicIp).
// Returns {ok, error, url (normalised, no query), host, literal, port}.
// devLocal: only for the local test stack (`wrangler dev --var ONAIR_DEV_LOCAL:1`), lets http://*.localhost:port be listed. Never set on the real directory.
export function validateUrl(text, devLocal = false) {
  if (typeof text !== "string" || text.length > LIMITS.url) return { ok: false, error: "url too long or missing" };
  if (hasControl(text) || /\s/.test(text)) return { ok: false, error: "url has spaces or control characters" };
  let u;
  try { u = new URL(text); } catch { return { ok: false, error: "url does not parse" }; }
  if (u.protocol !== "https:" && u.protocol !== "http:") return { ok: false, error: "url must be http or https" };
  if (u.username || u.password) return { ok: false, error: "url must not contain a user name or password" };
  if (u.search || u.hash) return { ok: false, error: "url must not have a query or fragment" };
  if (!/^\/[A-Za-z0-9._~\-\/]*$/.test(u.pathname)) return { ok: false, error: "url path has odd characters" };
  if (devLocal && u.protocol === "http:" && u.hostname.toLowerCase().endsWith(".localhost")) {
    const p0 = u.pathname.endsWith("/") ? u.pathname : u.pathname + "/";
    return { ok: true, host: u.hostname.toLowerCase(), literal: false, devLocal: true, port: Number(u.port || 80), url: `${u.protocol}//${u.host}${p0}` };
  }
  const port = u.port ? Number(u.port) : (u.protocol === "https:" ? 443 : 80);
  if (BAD_PORTS.has(port) || (port < 1024 && port !== 80 && port !== 443)) return { ok: false, error: "url port not allowed" };
  const host = u.hostname.toLowerCase();
  const literal = host.startsWith("[") || /^[\d.]+$/.test(host);
  if (literal) {
    if (!isPublicIp(host)) return { ok: false, error: "url host is not a public address" };
  } else {
    if (!host.includes(".") || host.length > 253 || host === "localhost") return { ok: false, error: "url host is not a public name" };
    if (BAD_SUFFIX.some((s) => host.endsWith(s))) return { ok: false, error: "url host is not a public name" };
    if (!host.split(".").every((l) => /^[a-z0-9\-]+$/.test(l))) return { ok: false, error: "url host has odd characters" };
  }
  const path = u.pathname.endsWith("/") ? u.pathname : u.pathname + "/";
  return { ok: true, host, literal, port, url: `${u.protocol}//${u.host}${path}` };
}

// ---- records ---------------------------------------------------------------------------------------------------------------------------
function str(rec, k, max, errs, required = false) {
  let v = rec[k];
  if (v === undefined || v === null) v = "";
  if (typeof v !== "string") { errs.push(`${k} must be a string`); return ""; }
  if (hasControl(v)) { errs.push(`${k} has control characters`); return ""; }
  v = v.trim();
  if (byteLen(v) > max) errs.push(`${k} is longer than ${max} bytes`);
  if (required && !v) errs.push(`${k} is required`);
  return v;
}
function int(rec, k, lo, hi, errs, def) {
  const v = rec[k];
  if (v === undefined || v === null) { if (def !== undefined) return def; errs.push(`${k} is required`); return 0; }
  if (!Number.isInteger(v) || v < lo || v > hi) { errs.push(`${k} must be an integer from ${lo} to ${hi}`); return 0; }
  return v;
}

// Checks a heartbeat or delete body (already JSON-parsed). Returns {ok, errors, rec}; rec has exactly the signed fields in final form.
// `sig` is checked for shape only here; the signature itself is verified by the caller with WebCrypto.
export function validateRecord(body, now, devLocal = false) {
  const errs = [];
  if (!body || typeof body !== "object" || Array.isArray(body)) return { ok: false, errors: ["body must be a JSON object"] };
  const allowed = new Set([...HEARTBEAT_KEYS, "sig", "v"]);
  for (const k of Object.keys(body)) if (!allowed.has(k)) errs.push(`unknown field ${k}`);
  if (body.v !== PROTO) errs.push(`v must be ${PROTO}`);
  const action = body.action;
  if (action !== "heartbeat" && action !== "delete") errs.push("action must be heartbeat or delete");
  let id = "";
  if (typeof body.id === "string" && /^[a-z0-9][a-z0-9-]{7,31}$/.test(body.id)) id = body.id; else errs.push("id must be 8 to 32 characters of a-z 0-9 -");
  let pub = "";
  if (fromHex(body.pub, 32)) pub = body.pub; else errs.push("pub must be 64 hex characters");
  if (!fromHex(body.sig, 64)) errs.push("sig must be 128 hex characters");
  const ts = int(body, "ts", 1, 4102444800, errs);
  if (ts && Math.abs(ts - now) > LIMITS.clockSkewSec) errs.push("ts is too far from the directory's clock");
  const rec = { action, id, pub, ts };
  if (action === "heartbeat") {
    rec.name = str(body, "name", LIMITS.name, errs, true);
    const u = validateUrl(body.url, devLocal);
    if (!u.ok) errs.push(u.error);
    rec.url = u.ok ? u.url : "";
    if (u.ok && body.url !== u.url) errs.push("url must be written in its normal form: " + u.url);
    rec.loc = str(body, "loc", LIMITS.loc, errs);
    if (rec.loc && !parseLoc(rec.loc)) errs.push("loc must be \"lat,lon\" or a Maidenhead locator");
    const ranges = [];
    if (body.freq !== undefined && body.freq !== null) {
      if (!Array.isArray(body.freq) || body.freq.length > LIMITS.maxRanges) errs.push(`freq must be a list of at most ${LIMITS.maxRanges} ranges`);
      else for (const r of body.freq) {
        if (!Array.isArray(r) || r.length !== 2 || !Number.isInteger(r[0]) || !Number.isInteger(r[1]) || r[0] < 0 || r[1] < r[0] || r[1] > 3e11) { errs.push("freq ranges are [low_hz, high_hz]"); break; }
        ranges.push([r[0], r[1]]);
      }
    }
    rec.freq = ranges;
    rec.radio = str(body, "radio", LIMITS.radio, errs);
    rec.cap = int(body, "cap", 0, LIMITS.maxCap, errs);
    rec.users = int(body, "users", 0, LIMITS.maxUsers, errs);
    rec.interval = int(body, "interval", LIMITS.minInterval, LIMITS.maxInterval, errs, LIMITS.defaultInterval);
    rec.ver = str(body, "ver", LIMITS.ver, errs);
    rec.contact = str(body, "contact", LIMITS.contact, errs);
    rec.desc = str(body, "desc", LIMITS.desc, errs);
    rec.reach = int(body, "reach", 0, 1, errs, 0);
    if (rec.users > rec.cap && rec.cap > 0) errs.push("users is more than cap");
    for (const k of ["name", "loc", "radio", "ver", "contact", "desc"]) if (typeof body[k] === "string" && body[k] !== rec[k]) errs.push(`${k} has spaces at its ends`);
  }
  return { ok: errs.length === 0, errors: errs, rec };
}

export function expiresAt(now, intervalSec) {
  const i = Math.min(LIMITS.maxInterval, Math.max(LIMITS.minInterval, intervalSec | 0));
  return now + i * LIMITS.missedBeats;
}
export const isExpired = (expires, now) => expires <= now;

// A fixed window counter: all requests in the same window share one number.
export const windowIndex = (now, windowSec) => Math.floor(now / windowSec);

// ---- filters ---------------------------------------------------------------------------------------------------------------------------
export const BANDS = {
  lf: [30e3, 300e3], mf: [300e3, 3e6], hf: [3e6, 30e6], vhf: [30e6, 300e6], uhf: [300e6, 3e9], shf: [3e9, 30e9],
  mw: [530e3, 1710e3], fm: [87.5e6, 108e6], air: [118e6, 137e6], marine: [156e6, 162.1e6], "2m": [144e6, 146e6], "70cm": [430e6, 440e6],
  "20m": [14e6, 14.35e6], "40m": [7e6, 7.2e6], "80m": [3.5e6, 3.8e6], dab: [174e6, 240e6], tv: [470e6, 862e6], l: [1e9, 2e9],
};

export function bandRange(name) { return Object.hasOwn(BANDS, String(name).toLowerCase()) ? BANDS[String(name).toLowerCase()] : null; }
export function rangesOverlap(ranges, lo, hi) { return (ranges || []).some((r) => r[0] <= hi && r[1] >= lo); }

// p: {band, freq_mhz, q, lat, lon, radius_km, free}. rows: records as the API returns them. Returns filtered rows (with distance_km when near).
export function filterServers(rows, p) {
  let lo = null, hi = null;
  if (p.band) { const b = bandRange(p.band); if (!b) return []; [lo, hi] = b; }
  if (p.freq_mhz !== undefined && p.freq_mhz !== null && p.freq_mhz !== "") {
    const f = Number(p.freq_mhz) * 1e6;
    if (!Number.isFinite(f)) return [];
    lo = f; hi = f;
  }
  const q = p.q ? String(p.q).toLowerCase().slice(0, 60) : "";
  const near = Number.isFinite(p.lat) && Number.isFinite(p.lon) ? { lat: p.lat, lon: p.lon } : null;
  const out = [];
  for (const r of rows) {
    if (lo !== null && !rangesOverlap(r.freq, lo, hi)) continue;
    if (q && !`${r.name} ${r.description} ${r.radio} ${r.url}`.toLowerCase().includes(q)) continue;
    if (p.free && r.capacity - r.users <= 0) continue;
    let d = null;
    if (near) {
      if (r.lat === null || r.lat === undefined) continue;
      d = haversineKm(near, { lat: r.lat, lon: r.lon });
      if (Number.isFinite(p.radius_km) && d > p.radius_km) continue;
    }
    out.push(d === null ? r : { ...r, distance_km: Math.round(d) });
  }
  if (near) out.sort((a, b) => a.distance_km - b.distance_km);
  return out;
}

export function timingSafeEqualStr(a, b) {
  const x = enc.encode(String(a)), y = enc.encode(String(b));
  let d = x.length ^ y.length;
  for (let i = 0; i < Math.max(x.length, y.length); i++) d |= (x[i] || 0) ^ (y[i] || 0);
  return d === 0;
}
