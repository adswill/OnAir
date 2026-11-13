// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The relay Worker under `wrangler dev --local` (workerd + local Durable Objects), on 127.0.0.1 only.
// A fake OnAir host signs the hello like onair-server and carries each virtual connection to a small local web server
// (HTTP + a WebSocket echo), the way server/web/web_relay.cpp does. Exits as skipped when wrangler is not installed.
import test from 'node:test';
import assert from 'node:assert';
import crypto from 'node:crypto';
import net from 'node:net';
import http from 'node:http';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { createRequire } from 'node:module';

const require = createRequire(import.meta.url);
const NodeRelay = require('../../../relay/relay.js');
const DIR = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const WR = path.join(DIR, 'node_modules', '.bin', 'wrangler');
const CFG = path.join(DIR, '.wrangler-test.jsonc');
const SUFFIX = '.relay.localhost', ADMIN = 'a'.repeat(32);
const wait = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });

function keypair() {
  const { publicKey, privateKey } = crypto.generateKeyPairSync('ed25519');
  const pub = publicKey.export({ format: 'der', type: 'spki' }).subarray(12);
  return { priv: privateKey, pub, pubHex: pub.toString('hex') };
}
function wsEncode(op, payload) {
  const p = Buffer.from(payload), m = crypto.randomBytes(4), n = p.length;
  const h = n < 126 ? Buffer.from([128 | op, 128 | n]) : n < 65536 ? Buffer.from([128 | op, 128 | 126, n >> 8, n & 255]) : (() => { const b = Buffer.alloc(10); b[0] = 128 | op; b[1] = 128 | 127; b.writeBigUInt64BE(BigInt(n), 2); return b; })();
  const x = Buffer.from(p); for (let i = 0; i < n; i++) x[i] ^= m[i & 3];
  return Buffer.concat([h, m, x]);
}
// Reads server frames from a socket buffer; returns [frames, rest].
function wsDecode(buf) {
  const out = [];
  for (;;) {
    if (buf.length < 2) break;
    let len = buf[1] & 127, o = 2;
    if (len === 126) { if (buf.length < 4) break; len = buf.readUInt16BE(2); o = 4; } else if (len === 127) { if (buf.length < 10) break; len = Number(buf.readBigUInt64BE(2)); o = 10; }
    if (buf.length < o + len) break;
    out.push({ op: buf[0] & 15, p: Buffer.from(buf.subarray(o, o + len)) }); buf = buf.subarray(o + len);
  }
  return [out, buf];
}

// ---- the host's web server: plain HTTP + a WebSocket that echoes and reports the forwarded address
function webServer() {
  const srv = http.createServer((req, res) => {
    const peer = req.headers['x-forwarded-for'] || '-';
    if (req.url === '/big') { res.writeHead(200, { 'content-type': 'application/octet-stream' }); res.end(Buffer.alloc(300000, 7)); return; }
    if (req.url === '/chunked') { res.writeHead(200, { 'content-type': 'text/plain' }); res.write('part1-'); setTimeout(() => res.end('part2'), 50); return; }
    if (req.method === 'POST') { const b = []; req.on('data', (d) => b.push(d)); req.on('end', () => { res.writeHead(201, { 'content-type': 'text/plain' }); res.end('got ' + Buffer.concat(b).length); }); return; }
    res.writeHead(200, { 'content-type': 'text/plain', 'x-host-seen': req.headers.host, 'x-xff-count': String((req.rawHeaders.filter((h) => /^x-forwarded-for$/i.test(h))).length) });
    res.end('hello from host, peer ' + peer);
  });
  srv.on('upgrade', (req, sock) => {
    if (req.headers.origin && new URL(req.headers.origin).host !== req.headers.host) { sock.end('HTTP/1.1 403 Forbidden\r\nContent-Length: 6\r\nConnection: close\r\n\r\norigin'); return; }
    if (req.url === '/pw' && !/pw=right/.test(req.headers.cookie || '')) { sock.end('HTTP/1.1 401 Unauthorized\r\nContent-Length: 5\r\nConnection: close\r\n\r\nnope!'); return; }
    const accept = crypto.createHash('sha1').update(req.headers['sec-websocket-key'] + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest('base64');
    sock.write(`HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ${accept}\r\nSec-WebSocket-Protocol: onair-web.v1\r\n\r\n`);
    const srvFrame = (op, p) => { p = Buffer.from(p); const n = p.length; const h = n < 126 ? Buffer.from([128 | op, n]) : n < 65536 ? Buffer.from([128 | op, 126, n >> 8, n & 255]) : (() => { const b = Buffer.alloc(10); b[0] = 128 | op; b[1] = 127; b.writeBigUInt64BE(BigInt(n), 2); return b; })(); return Buffer.concat([h, p]); };
    sock.write(srvFrame(1, JSON.stringify({ type: 'welcome', peer: req.headers['x-forwarded-for'] })));
    sock.write(srvFrame(9, 'k'));   // a ping: the relay must answer it
    let buf = Buffer.alloc(0);
    sock.on('data', (d) => {
      buf = Buffer.concat([buf, d]);
      for (;;) {
        if (buf.length < 6) return;
        let len = buf[1] & 127, o = 2; if (len === 126) { len = buf.readUInt16BE(2); o = 4; } else if (len === 127) { len = Number(buf.readBigUInt64BE(2)); o = 10; }
        if (!(buf[1] & 128)) { sock.destroy(); return; }   // clients must mask
        if (buf.length < o + 4 + len) return;
        const m = buf.subarray(o, o + 4), p = Buffer.from(buf.subarray(o + 4, o + 4 + len)); for (let i = 0; i < p.length; i++) p[i] ^= m[i & 3];
        const op = buf[0] & 15; buf = buf.subarray(o + 4 + len);
        if (op === 10) { srv.pongs = (srv.pongs || 0) + 1; continue; }
        if (op === 8) { sock.end(srvFrame(8, p)); srv.closes = (srv.closes || 0) + 1; return; }
        if (op === 1 && p.toString() === 'stream') { for (let i = 0; i < 40; i++) sock.write(srvFrame(2, Buffer.concat([Buffer.from([i & 1 ? 0x20 : 0x10]), crypto.randomBytes(i & 1 ? 600 : 1100)]))); continue; }
        if (op === 1 && p.toString() === 'bye') { sock.end(srvFrame(8, Buffer.from([0x0f, 0xa8, 0x6b]))); return; }
        sock.write(srvFrame(op, op === 1 ? 'echo:' + p.toString() : p));
      }
    });
    sock.on('error', () => {});
  });
  return new Promise((res) => srv.listen(0, '127.0.0.1', () => res(srv)));
}

// ---- a fake host: the relay client of onair-server in miniature
function fakeHost(relayPort, webPort, kp, opts = {}) {
  return new Promise((resolve) => {
    const h = { ready: null, error: null, closed: false, opens: [], vconns: new Map(), status: 0 };
    const s = net.connect(relayPort, '127.0.0.1'); h.sock = s;
    let buf = Buffer.alloc(0), up = false;
    const sendFrame = (t, id, payload) => { const hd = Buffer.alloc(5); hd[0] = t; hd.writeUInt32BE(id, 1); s.write(wsEncode(2, Buffer.concat([hd, Buffer.from(payload || '')]))); };
    h.destroy = () => s.destroy();
    s.on('error', () => {});
    s.on('close', () => { h.closed = true; for (const v of h.vconns.values()) v.destroy(); resolve(h); });
    s.write(`GET /relay/v1/host HTTP/1.1\r\nHost: 127.0.0.1:${relayPort}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ${crypto.randomBytes(16).toString('base64')}\r\nSec-WebSocket-Version: 13\r\n\r\n`);
    s.on('data', (d) => {
      buf = Buffer.concat([buf, d]);
      if (!up) { const e = buf.indexOf('\r\n\r\n'); if (e < 0) return; h.status = parseInt(buf.toString('latin1', 9, 12), 10); if (h.status !== 101) { h.error = { code: 'http', status: h.status }; s.destroy(); return; } buf = buf.subarray(e + 4); up = true; }
      const [fs_, rest] = wsDecode(buf); buf = rest;
      for (const f of fs_) {
        if (f.op === 1) {
          const m = JSON.parse(f.p.toString());
          if (m.type === 'challenge') {
            const id = NodeRelay.idFromPub(kp.pub), ts = Math.floor(Date.now() / 1000) + (opts.ts || 0);
            let sig = crypto.sign(null, Buffer.from(`OnAir-Relay-v1\nid=${id}\npub=${kp.pubHex}\nts=${ts}\nnonce=${m.nonce}\n`), kp.priv).toString('hex');
            if (opts.tamper) sig = (sig[0] === '0' ? '1' : '0') + sig.slice(1);
            s.write(wsEncode(1, JSON.stringify({ type: 'hello', v: 1, id, pub: kp.pubHex, ts, nonce: m.nonce, name: opts.name || 'Roof antenna', max_conns: opts.maxConns || 64, client: 'test', public: !!opts.public, sig })));
          } else if (m.type === 'ready') { h.ready = m; resolve(h); } else if (m.type === 'error') { h.error = m; }
        } else if (f.op === 2) {
          const t = f.p[0], id = f.p.readUInt32BE(1), payload = f.p.subarray(5);
          if (t === 1) {
            h.opens.push(payload.toString());
            const c = net.connect(webPort, '127.0.0.1'); let first = true;
            c.on('data', (x) => { for (let i = 0; i < x.length; i += 16384) sendFrame(2, id, x.subarray(i, i + 16384)); });
            c.on('close', () => { if (h.vconns.get(id) === c) { h.vconns.delete(id); sendFrame(3, id); } });
            c.on('error', () => {});
            c.rewrite = (x) => {   // like web_relay.cpp: drop forged forwarding headers, add the OPEN address
              if (!first) return x; first = false;
              const t2 = x.toString('latin1'), e = t2.indexOf('\r\n\r\n');
              const lines = t2.slice(0, e).split('\r\n').filter((l, i) => i === 0 || !/^(x-forwarded-for|x-real-ip|forwarded):/i.test(l));
              lines.push('X-Forwarded-For: ' + h.opens[h.opens.length - 1]);
              return Buffer.concat([Buffer.from(lines.join('\r\n') + '\r\n\r\n', 'latin1'), x.subarray(e + 4)]);
            };
            c.peer = payload.toString(); h.vconns.set(id, c);
          } else if (t === 2) { const c = h.vconns.get(id); if (c) c.write(c.rewrite(Buffer.from(payload))); }
          else if (t === 3) { const c = h.vconns.get(id); if (c) { h.vconns.delete(id); c.destroy(); } }
        } else if (f.op === 9) s.write(wsEncode(10, f.p));
        else if (f.op === 8) s.destroy();
      }
    });
    setTimeout(() => resolve(h), 8000);
  });
}

// ---- a browser: raw sockets so Host / Origin can be the relay's address, as a real browser would send them
function rawHttp(port, host, req) {
  return new Promise((res) => {
    const s = net.connect(port, '127.0.0.1', () => s.write(req || `GET / HTTP/1.1\r\nHost: ${host}\r\nConnection: close\r\n\r\n`));
    const ch = []; s.on('data', (d) => ch.push(d)); s.on('error', () => {});
    s.on('close', () => { const b = Buffer.concat(ch), e = b.indexOf('\r\n\r\n'); res({ status: parseInt(b.toString('latin1', 9, 12), 10) || 0, head: b.toString('latin1', 0, Math.max(0, e)), body: e >= 0 ? b.subarray(e + 4) : Buffer.alloc(0) }); });
    s.setTimeout(15000, () => s.destroy());
  });
}
// Decodes a chunked body if the head says so
function bodyOf(r) {
  if (!/transfer-encoding:\s*chunked/i.test(r.head)) return r.body;
  const out = []; let b = r.body;
  for (;;) { const e = b.indexOf('\r\n'); const n = parseInt(b.subarray(0, e).toString(), 16); if (!n) break; out.push(b.subarray(e + 2, e + 2 + n)); b = b.subarray(e + 4 + n); }
  return Buffer.concat(out);
}
class Browser {
  constructor(port, host, p = '/web/v1/ws', extra = '') {
    this.q = []; this.bins = 0; this.closeCode = null; this.status = 0; this.buf = Buffer.alloc(0); this.up = false;
    this.s = net.connect(port, '127.0.0.1', () => this.s.write(`GET ${p} HTTP/1.1\r\nHost: ${host}\r\nOrigin: http://${host}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ${crypto.randomBytes(16).toString('base64')}\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Protocol: onair-web.v1\r\n${extra}\r\n`));
    this.s.on('error', () => {}); this.s.on('close', () => { if (this.closeCode === null) this.closeCode = -1; });
    this.s.on('data', (d) => {
      this.buf = Buffer.concat([this.buf, d]);
      if (this.status && !this.up) return;   // refused: the rest is the body
      if (!this.up) { const e = this.buf.indexOf('\r\n\r\n'); if (e < 0) return; this.head = this.buf.toString('latin1', 0, e); this.status = parseInt(this.head.slice(9, 12), 10); this.buf = this.buf.subarray(e + 4); if (this.status !== 101) { this.closeCode = -2; return; } this.up = true; }
      const [fr, rest] = wsDecode(this.buf); this.buf = rest;
      for (const f of fr) {
        if (f.op === 1) this.q.push(JSON.parse(f.p.toString().replace(/^echo:/, '')));
        else if (f.op === 2) this.bins++;
        else if (f.op === 8) { this.closeCode = f.p.length >= 2 ? f.p.readUInt16BE(0) : 1005; this.s.destroy(); }
        else if (f.op === 9) this.s.write(wsEncode(10, f.p));
      }
    });
  }
  send(o) { this.s.write(wsEncode(1, typeof o === 'string' ? o : JSON.stringify(o))); }
  close() { this.s.destroy(); }
  async wait(pred, ms = 6000) { const t = Date.now(); while (Date.now() - t < ms) { const f = this.q.find(pred); if (f) return f; if (this.closeCode !== null) return null; await wait(25); } return null; }
}

let wr, PORT, web, T;
const startRelay = async () => {
  const log = fs.openSync(process.env.RELAY_TEST_LOG || T + '/wr.log', 'a');
  // the same config without routes: with a route, wrangler dev replaces the Host header with the route's name
  fs.writeFileSync(CFG, fs.readFileSync(path.join(DIR, 'wrangler.jsonc'), 'utf8').replace(/ {2}"routes": \[[\s\S]*?\],\n/, ''));
  wr = spawn(WR, ['dev', '-c', CFG, '--local', '--persist-to', T + '/state', '--port', '' + PORT, '--ip', '127.0.0.1', '--show-interactive-dev-session=false',
    '--var', 'RELAY_DEV_LOCAL:1', '--var', 'RELAY_MAIN_HOST:127.0.0.1', '--var', 'RELAY_LABEL_SUFFIX:' + SUFFIX, '--var', 'RELAY_SCHEME:http',
    '--var', 'RELAY_PUBLIC_PORT:' + PORT, '--var', 'HOST_ATTEMPTS_PER_MIN:40', '--var', 'ADMIN_TOKEN:' + ADMIN, '--var', 'MAX_BPS:2000000'],
  { cwd: DIR, env: { ...process.env, CI: '1', WRANGLER_SEND_METRICS: 'false' }, stdio: ['ignore', log, log], detached: true });
  for (let i = 0; i < 240; i++) { await wait(250); try { const r = await fetch(`http://127.0.0.1:${PORT}/relay/v1/status`, { signal: AbortSignal.timeout(3000) }); if (r.ok) return true; } catch { /* not yet */ } }
  return false;
};
const stopRelay = async () => { try { process.kill(-wr.pid, 'SIGTERM'); } catch { /* gone */ } for (let i = 0; i < 50 && wr.exitCode === null && wr.signalCode === null; i++) await wait(100); };

const skip = !fs.existsSync(WR) && 'wrangler not installed (npm install in web/relay-worker)';
test.describe('relay Worker under wrangler dev', { skip, concurrency: false }, () => {
  test.before(async () => {
    T = fs.mkdtempSync(path.join(os.tmpdir(), 'onair-cfrelay-')); PORT = await freePort(); web = await webServer();
    assert.ok(await startRelay(), 'wrangler dev serves the relay');
  });
  test.after(async () => { await stopRelay(); web.close(); try { fs.unlinkSync(CFG); } catch { /* gone */ } fs.rmSync(T, { recursive: true, force: true }); });

  test('status, offline page, unknown host', async () => {
    assert.match((await rawHttp(PORT, '127.0.0.1', `GET /relay/v1/status HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n`)).body.toString(), /\{"v":1,"ok":true\}/);
    const off = await rawHttp(PORT, 'abcdefghij' + SUFFIX); assert.strictEqual(off.status, 503); assert.match(bodyOf(off).toString(), /offline/);
    assert.strictEqual((await rawHttp(PORT, 'evil.example.com')).status, 404);
    assert.strictEqual((await rawHttp(PORT, 'NOT-A-LABEL' + SUFFIX)).status, 404);
  });

  test('signed hello, stable address, HTTP through the tunnel (real address, forged header dropped), POST, chunked, large body', async () => {
    const kp = keypair(), h = await fakeHost(PORT, web.address().port, kp);
    const label = NodeRelay.labelFromPub(kp.pub), host = label + SUFFIX;
    assert.ok(h.ready, 'ready: ' + JSON.stringify(h.error));
    assert.strictEqual(h.ready.url, `http://${host}:${PORT}/`);
    const r = await rawHttp(PORT, host, `GET /x HTTP/1.1\r\nHost: ${host}\r\nX-Forwarded-For: 6.6.6.6\r\nConnection: close\r\n\r\n`);
    assert.strictEqual(r.status, 200); assert.match(bodyOf(r).toString(), /hello from host, peer 127\.0\.0\.1$/);
    assert.match(r.head, /x-xff-count: 1/i); assert.match(r.head, new RegExp('x-host-seen: ' + host.replace(/\./g, '\\.'), 'i'));
    const p = await rawHttp(PORT, host, `POST /up HTTP/1.1\r\nHost: ${host}\r\nContent-Length: 11\r\nConnection: close\r\n\r\nhello world`);
    assert.strictEqual(p.status, 201); assert.strictEqual(bodyOf(p).toString(), 'got 11');
    assert.strictEqual(bodyOf(await rawHttp(PORT, host, `GET /chunked HTTP/1.1\r\nHost: ${host}\r\nConnection: close\r\n\r\n`)).toString(), 'part1-part2');
    const big = bodyOf(await rawHttp(PORT, host, `GET /big HTTP/1.1\r\nHost: ${host}\r\nConnection: close\r\n\r\n`));
    assert.strictEqual(big.length, 300000); assert.ok(big.every((x) => x === 7));
    await wait(200); assert.strictEqual(h.vconns.size, 0, 'every virtual connection was closed');
    h.destroy(); await wait(300);
    assert.strictEqual((await rawHttp(PORT, host)).status, 503, 'offline once the host link is gone');
  });

  test('two listeners over WebSocket: welcome, streams, echo, the relay answers the host ping; reconnect keeps the address', async () => {
    const kp = keypair(); let h = await fakeHost(PORT, web.address().port, kp, { public: true, name: 'Two listeners' });
    const host = NodeRelay.labelFromPub(kp.pub) + SUFFIX;
    const a = new Browser(PORT, host), b = new Browser(PORT, host);
    assert.ok(await a.wait((m) => m.type === 'welcome') && await b.wait((m) => m.type === 'welcome'), 'both get a welcome');
    assert.strictEqual(a.q[0].peer, '127.0.0.1');
    assert.match(a.head, /sec-websocket-protocol: onair-web\.v1/i);
    a.send('stream'); b.send('stream'); a.send({ type: 'tune', who: 'a' }); b.send({ type: 'tune', who: 'b' });
    assert.ok(await a.wait((m) => m.who === 'a') && await b.wait((m) => m.who === 'b'));
    await wait(500);
    assert.strictEqual(a.bins, 40); assert.strictEqual(b.bins, 40);
    assert.ok(!a.q.some((m) => m.who === 'b') && !b.q.some((m) => m.who === 'a'), 'each listener gets only its own stream');
    assert.ok((web.pongs || 0) >= 2, 'the host\'s WebSocket pings are answered');
    const list = await (await fetch(`http://127.0.0.1:${PORT}/api/v1/servers`, { signal: AbortSignal.timeout(10000) })).json();
    const me = list.servers.find((s) => s.name === 'Two listeners'); assert.ok(me); assert.strictEqual(me.listeners, 2);
    // host's web server closes one listener with a code
    a.send('bye'); for (let i = 0; i < 40 && a.closeCode === null; i++) await wait(50);
    assert.strictEqual(a.closeCode, 4008);
    // the host link drops: listeners are cut off; the host comes back with the same address
    const url = h.ready.url; h.destroy(); for (let i = 0; i < 40 && b.closeCode === null; i++) await wait(50);
    assert.ok(b.closeCode !== null, 'listener cut off with the link');
    h = await fakeHost(PORT, web.address().port, kp, { public: true, name: 'Two listeners' });
    assert.strictEqual(h.ready.url, url, 'same address after the reconnect');
    const c = new Browser(PORT, host); assert.ok(await c.wait((m) => m.type === 'welcome'), 'a listener connects again after the reconnect');
    c.send({ type: 'x' }); await c.wait((m) => m.type === 'x'); c.close(); await wait(300);
    h.destroy();
  });

  test('wrong password / origin: the host\'s refusal reaches the browser as HTTP', async () => {
    const kp = keypair(), h = await fakeHost(PORT, web.address().port, kp), host = NodeRelay.labelFromPub(kp.pub) + SUFFIX;
    const bad = new Browser(PORT, host, '/pw', 'Cookie: pw=wrong\r\n'); for (let i = 0; i < 100 && !bad.status; i++) await wait(50);
    assert.strictEqual(bad.status, 401);
    const good = new Browser(PORT, host, '/pw', 'Cookie: pw=right\r\n'); assert.ok(await good.wait((m) => m.type === 'welcome'));
    good.close(); h.destroy();
  });

  test('one live link per id: a second link replaces the first', async () => {
    const kp = keypair(), a = await fakeHost(PORT, web.address().port, kp); const b = await fakeHost(PORT, web.address().port, kp);
    assert.ok(a.ready && b.ready); for (let i = 0; i < 60 && !a.closed; i++) await wait(50);
    assert.ok(a.closed, 'the old link is closed'); assert.ok(!b.closed);
    const r = await rawHttp(PORT, NodeRelay.labelFromPub(kp.pub) + SUFFIX); assert.strictEqual(r.status, 200, 'the new link serves');
    b.destroy();
  });

  test('refusals: bad signature, clock off, admin ban (needs the token) drops the host and refuses it again', async () => {
    assert.strictEqual((await fakeHost(PORT, web.address().port, keypair(), { tamper: true })).error.code, 'protocol');
    assert.strictEqual((await fakeHost(PORT, web.address().port, keypair(), { ts: 600 })).error.code, 'clock');
    const kp = keypair(), h = await fakeHost(PORT, web.address().port, kp), id = NodeRelay.idFromPub(kp.pub);
    const ban = (tok, what) => fetch(`http://127.0.0.1:${PORT}/relay/v1/admin/${what || 'ban'}`, { method: 'POST', headers: { authorization: 'Bearer ' + tok }, body: JSON.stringify({ id }), signal: AbortSignal.timeout(10000) });
    assert.strictEqual((await ban('b'.repeat(32))).status, 403);
    assert.strictEqual((await ban(ADMIN)).status, 200);
    for (let i = 0; i < 60 && !h.closed; i++) await wait(50);
    assert.ok(h.closed, 'banned host dropped');
    assert.strictEqual((await fakeHost(PORT, web.address().port, kp)).error.code, 'banned');
    assert.strictEqual((await ban(ADMIN, 'unban')).status, 200);
    const again = await fakeHost(PORT, web.address().port, kp); assert.ok(again.ready, 'allowed again after unban'); again.destroy();
  });

  test('limits: listeners per host, request head size, byte rate', async () => {
    const kp = keypair(), h = await fakeHost(PORT, web.address().port, kp, { maxConns: 2 }), host = NodeRelay.labelFromPub(kp.pub) + SUFFIX;
    const a = new Browser(PORT, host), b = new Browser(PORT, host);
    await a.wait((m) => m.type === 'welcome'); await b.wait((m) => m.type === 'welcome');
    const r = await rawHttp(PORT, host); assert.strictEqual(r.status, 503); assert.match(bodyOf(r).toString(), /full/);
    a.close(); b.close(); await wait(400);
    const big = await rawHttp(PORT, host, `GET / HTTP/1.1\r\nHost: ${host}\r\nX-Pad: ${'a'.repeat(17000)}\r\nConnection: close\r\n\r\n`);
    assert.ok(big.status === 431 || big.status === 413 || big.status === 400, 'oversized head refused (' + big.status + ')');
    const f = new Browser(PORT, host); await f.wait((m) => m.type === 'welcome');
    for (let i = 0; i < 40; i++) f.s.write(wsEncode(2, crypto.randomBytes(60000)));   // 2.4 MB in a moment, cap 2 MB/s
    for (let i = 0; i < 80 && f.closeCode === null; i++) await wait(50);
    assert.ok(f.closeCode !== null, 'a listener over the byte budget is closed');
    h.destroy();
  });

  test('host link attempts are rate limited per address', async () => {
    let refused = 0;
    for (let i = 0; i < 45; i++) { const x = await fakeHost(PORT, web.address().port, keypair()); if (x.error && x.error.status === 429) refused++; x.destroy(); }
    assert.ok(refused >= 1, 'refused ' + refused);
  });
});
