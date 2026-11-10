// OnAir relay on Cloudflare: pure helpers shared by the Worker, the Durable Objects and the Node tests.
// Protocol: docs/ONAIR-WEB-RELAY.md (version 1). Nothing here touches the network.

export const T_OPEN = 1, T_DATA = 2, T_CLOSE = 3;
const B32 = 'abcdefghijklmnopqrstuvwxyz234567';
const te = new TextEncoder(), td = new TextDecoder();

export const hex = (u8) => Array.from(u8, (b) => b.toString(16).padStart(2, '0')).join('');
export const unhex = (s) => { const o = new Uint8Array(s.length >> 1); for (let i = 0; i < o.length; i++) o[i] = parseInt(s.substr(i * 2, 2), 16); return o; };
export function base32(buf) {
  let bits = 0, val = 0, out = '';
  for (const b of buf) { val = ((val << 8) | b) & 0xffff; bits += 8; while (bits >= 5) { out += B32[(val >>> (bits - 5)) & 31]; bits -= 5; } }
  if (bits > 0) out += B32[(val << (5 - bits)) & 31];
  return out;
}
export const sha256 = async (u8) => new Uint8Array(await crypto.subtle.digest('SHA-256', u8));
export const idFromPub = async (pub) => 'onair-' + hex(await sha256(pub)).slice(0, 16);
export const labelFromPub = async (pub) => base32(await sha256(pub)).slice(0, 10);
export const clean = (s, n) => String(s == null ? '' : s).replace(/[^\x20-\x7e -￿]/g, '').slice(0, n);
export const canonHello = (h) => `OnAir-Relay-v1\nid=${h.id}\npub=${h.pub}\nts=${h.ts}\nnonce=${h.nonce}\n`;
export const randomHex = (n) => hex(crypto.getRandomValues(new Uint8Array(n)));

export async function verifyEd25519(pubHex, msg, sigHex) {
  try {
    const key = await crypto.subtle.importKey('raw', unhex(pubHex), { name: 'Ed25519' }, false, ['verify']);
    return await crypto.subtle.verify({ name: 'Ed25519' }, key, unhex(sigHex), te.encode(msg));
  } catch { return false; }
}

// Checks a host's hello against the challenge. Returns {ok, id, label, pub} or {ok:false, code, message}.
export async function checkHello(text, nonce, nowS, skewS = 120) {
  const bad = (code, message) => ({ ok: false, code, message });
  if (typeof text !== 'string' || text.length > 4096) return bad('protocol', 'bad hello');
  let h; try { h = JSON.parse(text); } catch { return bad('protocol', 'bad hello'); }
  if (!h || h.type !== 'hello') return bad('protocol', 'bad hello');
  if (h.v !== 1) return bad('version', 'this relay speaks protocol 1');
  if (!/^[0-9a-f]{64}$/.test(h.pub || '') || !/^[0-9a-f]{128}$/.test(h.sig || '')) return bad('protocol', 'bad key');
  const pub = unhex(h.pub);
  if (h.id !== await idFromPub(pub)) return bad('protocol', 'id does not match key');
  if (h.nonce !== nonce) return bad('protocol', 'wrong nonce');
  if (!(Math.abs(nowS - Number(h.ts)) <= skewS)) return bad('clock', 'your clock is more than 2 minutes off');
  if (!await verifyEd25519(h.pub, canonHello(h), h.sig)) return bad('protocol', 'bad signature');
  return { ok: true, id: h.id, label: await labelFromPub(pub), pub: h.pub, name: clean(h.name, 60), listed: h.public === true,
    client: clean(h.client, 40), maxConns: parseInt(h.max_conns, 10) || 0 };
}

// ---- relay frames: [type 1][id 4 BE][payload]
export function frame(t, id, payload) {
  const p = payload ? (payload instanceof Uint8Array ? payload : te.encode(payload)) : new Uint8Array(0);
  const o = new Uint8Array(5 + p.length);
  o[0] = t; new DataView(o.buffer).setUint32(1, id >>> 0); o.set(p, 5);
  return o;
}
export function parseFrame(buf) {
  const u = buf instanceof Uint8Array ? buf : new Uint8Array(buf);
  if (u.length < 5) return null;
  return { t: u[0], id: new DataView(u.buffer, u.byteOffset, u.byteLength).getUint32(1), payload: u.subarray(5) };
}

export function concat(a, b) { if (!a || !a.length) return b; if (!b.length) return a; const o = new Uint8Array(a.length + b.length); o.set(a); o.set(b, a.length); return o; }
export const toU8 = (m) => (typeof m === 'string' ? te.encode(m) : m instanceof Uint8Array ? m : new Uint8Array(m));

// ---- WebSocket framing toward the host's web server: the relay is the client there, so its frames are masked.
export function wsClientFrame(op, payload) {
  const p = toU8(payload || new Uint8Array(0)), n = p.length;
  const hl = n < 126 ? 2 : n < 65536 ? 4 : 10;
  const o = new Uint8Array(hl + 4 + n);
  o[0] = 128 | op;
  if (n < 126) o[1] = 128 | n;
  else if (n < 65536) { o[1] = 128 | 126; o[2] = n >> 8; o[3] = n & 255; }
  else { o[1] = 128 | 127; new DataView(o.buffer).setBigUint64(2, BigInt(n)); }
  const m = crypto.getRandomValues(new Uint8Array(4)); o.set(m, hl);
  for (let i = 0; i < n; i++) o[hl + 4 + i] = p[i] ^ m[i & 3];
  return o;
}
// Server frames (unmasked) from the host, fed in pieces. onFrame(op, payload) gets whole messages (fragments joined) and control frames.
export class WsDecoder {
  constructor(onFrame, maxMsg = 4 << 20) { this.buf = new Uint8Array(0); this.frag = []; this.fragOp = 0; this.fragLen = 0; this.onFrame = onFrame; this.max = maxMsg; this.dead = false; }
  push(chunk) {
    if (this.dead) return;
    this.buf = concat(this.buf, chunk);
    for (;;) {
      const b = this.buf; if (b.length < 2) return;
      const op = b[0] & 15, fin = !!(b[0] & 128), masked = !!(b[1] & 128);
      let len = b[1] & 127, o = 2;
      if (len === 126) { if (b.length < 4) return; len = (b[2] << 8) | b[3]; o = 4; }
      else if (len === 127) { if (b.length < 10) return; len = Number(new DataView(b.buffer, b.byteOffset).getBigUint64(2)); o = 10; }
      if (len > this.max) { this.dead = true; this.onFrame(-1, null); return; }
      const mo = masked ? 4 : 0;
      if (b.length < o + mo + len) return;
      let p = b.slice(o + mo, o + mo + len);
      if (masked) { const m = b.subarray(o, o + 4); for (let i = 0; i < p.length; i++) p[i] ^= m[i & 3]; }
      this.buf = b.subarray(o + mo + len);
      if (op >= 8) { this.onFrame(op, p); continue; }
      if (op === 1 || op === 2) { this.fragOp = op; this.frag = [p]; this.fragLen = p.length; }
      else if (op === 0) { this.frag.push(p); this.fragLen += p.length; if (this.fragLen > this.max) { this.dead = true; this.onFrame(-1, null); return; } }
      else { this.dead = true; this.onFrame(-1, null); return; }
      if (fin) {
        let m = this.frag[0] || new Uint8Array(0);
        for (let i = 1; i < this.frag.length; i++) m = concat(m, this.frag[i]);
        this.frag = []; this.fragLen = 0;
        this.onFrame(this.fragOp, m);
      }
    }
  }
}

// ---- HTTP/1.1 toward the host's web server
const DROP_REQ = new Set(['x-forwarded-for', 'x-real-ip', 'forwarded', 'x-forwarded-host', 'x-forwarded-proto', 'connection', 'keep-alive',
  'transfer-encoding', 'content-length', 'upgrade', 'te', 'trailer', 'proxy-connection', 'sec-websocket-key', 'sec-websocket-extensions',
  'sec-websocket-version', 'sec-websocket-accept', 'cdn-loop', 'true-client-ip', 'host']);
const keepReq = (k) => !DROP_REQ.has(k) && !k.startsWith('cf-') && !k.startsWith('x-relay-');
// Builds the request head the host's web server sees. `headers` is an iterable of [name, value] (a Headers object works).
export function requestHead(method, pathQuery, host, headers, opt = {}) {
  let h = `${method} ${pathQuery} HTTP/1.1\r\nHost: ${host}\r\n`;
  for (const [k0, v] of headers) {
    const k = k0.toLowerCase();
    if (!keepReq(k) || /[\r\n]/.test(v) || !/^[!#$%&'*+.^_`|~0-9a-z-]+$/.test(k)) continue;
    h += `${k}: ${v}\r\n`;
  }
  if (opt.wsKey) h += `upgrade: websocket\r\nconnection: Upgrade\r\nsec-websocket-version: 13\r\nsec-websocket-key: ${opt.wsKey}\r\n`;
  else { h += 'connection: close\r\n'; if (opt.bodyLen) h += `content-length: ${opt.bodyLen}\r\n`; }
  return h + '\r\n';
}

const HOP = new Set(['connection', 'keep-alive', 'transfer-encoding', 'content-length', 'upgrade', 'proxy-connection', 'te', 'trailer']);
// Parses an HTTP response head from bytes. Returns null (need more), {error}, or {status, headers:[[k,v]], rest, chunked, length}.
export function parseResponseHead(buf, maxHead = 65536) {
  let end = -1;
  for (let i = 3; i < buf.length; i++) if (buf[i] === 10 && buf[i - 1] === 13 && buf[i - 2] === 10 && buf[i - 3] === 13) { end = i + 1; break; }
  if (end < 0) return buf.length > maxHead ? { error: 'head too large' } : null;
  const lines = td.decode(buf.subarray(0, end - 4)).split('\r\n');
  const m = /^HTTP\/1\.[01] (\d{3})/.exec(lines[0]);
  if (!m) return { error: 'bad status line' };
  const headers = []; let chunked = false, length = -1, proto = '', accept = '';
  for (const l of lines.slice(1)) {
    const c = l.indexOf(':'); if (c <= 0) continue;
    const k = l.slice(0, c).trim().toLowerCase(), v = l.slice(c + 1).trim();
    if (k === 'transfer-encoding' && /chunked/i.test(v)) chunked = true;
    if (k === 'content-length') length = parseInt(v, 10);
    if (k === 'sec-websocket-protocol') proto = v;
    if (k === 'sec-websocket-accept') accept = v;
    if (!HOP.has(k) && !k.startsWith('sec-websocket-')) headers.push([k, v]);
  }
  return { status: +m[1], headers, rest: buf.subarray(end), chunked, length: chunked ? -1 : length, proto, accept };
}

// Chunked transfer decoding, fed in pieces. push() returns the body bytes so far; .done is set at the last chunk.
export class ChunkedDecoder {
  constructor() { this.buf = new Uint8Array(0); this.left = -1; this.done = false; this.bad = false; }
  push(chunk) {
    this.buf = concat(this.buf, chunk); const out = [];
    while (!this.done && !this.bad) {
      if (this.left < 0) {
        let e = -1; for (let i = 1; i < this.buf.length; i++) if (this.buf[i] === 10 && this.buf[i - 1] === 13) { e = i; break; }
        if (e < 0) { if (this.buf.length > 1024) this.bad = true; break; }
        const n = parseInt(td.decode(this.buf.subarray(0, e - 1)).split(';')[0].trim(), 16);
        if (!(n >= 0)) { this.bad = true; break; }
        this.buf = this.buf.subarray(e + 1);
        if (n === 0) { this.done = true; break; }
        this.left = n;
      }
      if (this.left > 0) { const k = Math.min(this.left, this.buf.length); if (!k) break; out.push(this.buf.slice(0, k)); this.buf = this.buf.subarray(k); this.left -= k; }
      if (this.left === 0) { if (this.buf.length < 2) break; this.buf = this.buf.subarray(2); this.left = -1; }
    }
    let r = new Uint8Array(0); for (const o of out) r = concat(r, o); return r;
  }
}

export const OFFLINE = `<!doctype html><meta charset=utf-8><meta name=viewport content="width=device-width"><title>Radio offline</title><body style="font:16px system-ui;max-width:32em;margin:15vh auto;padding:0 1em"><h1>This radio is offline</h1><p>The person sharing it has stopped, or lost their connection. Try again in a minute.</p><p><a href="https://web.on-air.dev/">Other shared radios</a></p>`;

// Per-key attempt counter over a sliding minute (the host link: per address and per id).
export class Attempts {
  constructor(perMin) { this.perMin = perMin; this.m = new Map(); }
  tooMany(key, now) {
    const a = (this.m.get(key) || []).filter((x) => now - x < 60000); a.push(now); this.m.set(key, a);
    if (this.m.size > 20000) for (const [k, v] of this.m) if (!v.length || now - v[v.length - 1] > 60000) this.m.delete(k);
    return a.length > this.perMin;
  }
}
