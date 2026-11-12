// Hashes, the Ed25519 signature on every listing answer, the session tokens and the salted hashes of install ids and addresses.
// Only WebCrypto (crypto.subtle), which both Cloudflare Workers and Node have.

const enc = new TextEncoder();

export function toHex(b: ArrayBuffer | Uint8Array): string {
  const u = b instanceof Uint8Array ? b : new Uint8Array(b);
  let s = "";
  for (const x of u) s += x.toString(16).padStart(2, "0");
  return s;
}
export function fromHex(s: string): Uint8Array | null {
  if (!/^([0-9a-fA-F]{2})*$/.test(s)) return null;
  const o = new Uint8Array(s.length / 2);
  for (let i = 0; i < o.length; i++) o[i] = parseInt(s.substr(i * 2, 2), 16);
  return o;
}
export async function sha256Hex(data: ArrayBuffer | Uint8Array | string): Promise<string> {
  const d = typeof data === "string" ? enc.encode(data) : data;
  return toHex(await crypto.subtle.digest("SHA-256", d));
}

// The salted hash under which an install id (likes, download counting) or an address (rate limits) is stored. The salt is a Worker secret,
// so a stored hash cannot be matched to an install id or an address by someone who only has the database.
export async function saltedHash(salt: string, kind: string, value: string): Promise<string> {
  return (await sha256Hex(salt + "\u0000" + kind + "\u0000" + value)).slice(0, 32);
}

// Ed25519 from a 32-byte seed (64 hex digits). WebCrypto imports private Ed25519 keys only as PKCS#8: the fixed 16-byte prefix plus the seed.
const PKCS8_PREFIX = fromHex("302e020100300506032b657004220420")!;
const keyCache = new Map<string, Promise<CryptoKey>>();
function signingKey(seedHex: string): Promise<CryptoKey> {
  let k = keyCache.get(seedHex);
  if (!k) {
    const seed = fromHex(seedHex.trim());
    if (!seed || seed.length !== 32) throw new Error("SIGNING_KEY must be 64 hex digits");
    const der = new Uint8Array(48);
    der.set(PKCS8_PREFIX, 0);
    der.set(seed, 16);
    k = crypto.subtle.importKey("pkcs8", der, { name: "Ed25519" }, true, ["sign"]);
    keyCache.set(seedHex, k);
  }
  return k;
}
export function signingKeyValid(seedHex: string | undefined): boolean {
  return !!seedHex && /^[0-9a-fA-F]{64}$/.test(seedHex.trim());
}
export async function ed25519Sign(seedHex: string, text: string): Promise<string> {
  const sig = await crypto.subtle.sign({ name: "Ed25519" }, await signingKey(seedHex), enc.encode(text));
  return toHex(sig);
}
export async function ed25519PublicHex(seedHex: string): Promise<string> {
  const jwk = (await crypto.subtle.exportKey("jwk", await signingKey(seedHex))) as JsonWebKey;
  const b = atob((jwk.x || "").replace(/-/g, "+").replace(/_/g, "/"));
  return toHex(Uint8Array.from(b, (c) => c.charCodeAt(0)));
}

// Session tokens: base64url(JSON payload) "." base64url(HMAC-SHA256). Issued after the GitHub device flow; the GitHub token itself is
// never stored, only the user's numeric id and login name are in the payload.
function b64url(u: Uint8Array): string {
  let s = "";
  for (const x of u) s += String.fromCharCode(x);
  return btoa(s).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "");
}
function unb64url(s: string): Uint8Array | null {
  if (!/^[A-Za-z0-9_-]*$/.test(s)) return null;
  try {
    const b = atob(s.replace(/-/g, "+").replace(/_/g, "/") + "===".slice((s.length + 3) % 4));
    return Uint8Array.from(b, (c) => c.charCodeAt(0));
  } catch {
    return null;
  }
}
async function hmac(secret: string, msg: string): Promise<Uint8Array> {
  const k = await crypto.subtle.importKey("raw", enc.encode(secret), { name: "HMAC", hash: "SHA-256" }, false, ["sign"]);
  return new Uint8Array(await crypto.subtle.sign("HMAC", k, enc.encode(msg)));
}
export interface Session { uid: number; login: string; iat: number; exp: number }
export async function makeSession(secret: string, s: Session): Promise<string> {
  const p = b64url(enc.encode(JSON.stringify(s)));
  return p + "." + b64url(await hmac(secret, p));
}
export async function readSession(secret: string, token: string, now: number): Promise<Session | null> {
  const dot = token.indexOf(".");
  if (dot <= 0 || token.length > 2000) return null;
  const p = token.slice(0, dot), mac = unb64url(token.slice(dot + 1));
  if (!mac) return null;
  const want = await hmac(secret, p);
  if (mac.length !== want.length) return null;
  let diff = 0;
  for (let i = 0; i < want.length; i++) diff |= mac[i] ^ want[i];
  if (diff) return null;
  const raw = unb64url(p);
  if (!raw) return null;
  try {
    const s = JSON.parse(new TextDecoder().decode(raw)) as Session;
    if (typeof s.uid !== "number" || typeof s.login !== "string" || typeof s.exp !== "number" || s.exp < now) return null;
    return s;
  } catch {
    return null;
  }
}

// constant-time comparison of two strings (the admin token)
export function sameText(a: string, b: string): boolean {
  if (a.length !== b.length) return false;
  let d = 0;
  for (let i = 0; i < a.length; i++) d |= a.charCodeAt(i) ^ b.charCodeAt(i);
  return d === 0;
}
