// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

'use strict';
// End to end on 127.0.0.1: the real relay, a fake OnAir host (signs the hello like onair-server) and a fake browser. Nothing leaves this computer.
const test = require('node:test');
const assert = require('node:assert');
const net = require('net');
const crypto = require('crypto');
const { createRelay, idFromPub, labelFromPub } = require('../relay.js');

const DOMAIN = 'relay.test';
const wait = (ms) => new Promise((r) => setTimeout(r, ms));

function keypair() {
  const { publicKey, privateKey } = crypto.generateKeyPairSync('ed25519');
  const pub = publicKey.export({ format: 'der', type: 'spki' }).subarray(12);
  return { priv: privateKey, pub, pubHex: pub.toString('hex') };
}
function wsEncode(op, payload) {   // client frames are masked
  const p = Buffer.from(payload), m = crypto.randomBytes(4);
  const h = p.length < 126 ? Buffer.from([128 | op, 128 | p.length]) : (() => { const b = Buffer.alloc(4); b[0] = 128 | op; b[1] = 128 | 126; b.writeUInt16BE(p.length, 2); return b; })();
  const x = Buffer.from(p); for (let i = 0; i < x.length; i++) x[i] ^= m[i & 3];
  return Buffer.concat([h, m, x]);
}
// A fake host: connects, answers the challenge. `opts.tamper` breaks the signature, `opts.ts` shifts the clock.
function fakeHost(port, kp, opts = {}) {
  return new Promise((resolve) => {
    const h = { frames: [], msgs: [], ready: null, error: null, closed: false };
    const s = net.connect(port, '127.0.0.1');
    let buf = Buffer.alloc(0), upgraded = false;
    h.send = (type, id, payload) => { const hd = Buffer.alloc(5); hd[0] = type; hd.writeUInt32BE(id, 1); s.write(wsEncode(2, Buffer.concat([hd, Buffer.from(payload || '')]))); };
    h.destroy = () => s.destroy();
    s.on('close', () => { h.closed = true; resolve(h); });
    s.write(`GET /relay/v1/host HTTP/1.1\r\nHost: ${DOMAIN}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ${crypto.randomBytes(16).toString('base64')}\r\nSec-WebSocket-Version: 13\r\n\r\n`);
    s.on('data', (d) => {
      buf = Buffer.concat([buf, d]);
      if (!upgraded) { const e = buf.indexOf('\r\n\r\n'); if (e < 0) return; if (!/^HTTP\/1.1 101/.test(buf.toString('latin1', 0, 12))) { h.error = { code: 'http', status: buf.toString('latin1', 0, 12) }; return; } buf = buf.subarray(e + 4); upgraded = true; }
      for (;;) {
        if (buf.length < 2) break;
        let len = buf[1] & 127, o = 2; if (len === 126) { if (buf.length < 4) break; len = buf.readUInt16BE(2); o = 4; }
        if (buf.length < o + len) break;
        const op = buf[0] & 15, p = buf.subarray(o, o + len); buf = buf.subarray(o + len);
        if (op === 1) {
          const m = JSON.parse(p.toString());
          if (m.type === 'challenge') {
            const id = opts.badId ? 'onair-0000000000000000' : idFromPub(kp.pub);
            const ts = Math.floor(Date.now() / 1000) + (opts.ts || 0);
            const canon = `OnAir-Relay-v1\nid=${id}\npub=${kp.pubHex}\nts=${ts}\nnonce=${m.nonce}\n`;
            let sig = crypto.sign(null, Buffer.from(canon), kp.priv).toString('hex');
            if (opts.tamper) sig = (sig[0] === '0' ? '1' : '0') + sig.slice(1);
            s.write(wsEncode(1, JSON.stringify({ type: 'hello', v: 1, id, pub: kp.pubHex, ts, nonce: m.nonce, name: opts.name || 'Roof antenna', max_conns: opts.maxConns || 64, client: 'test', public: !!opts.public, sig })));
          } else if (m.type === 'ready') { h.ready = m; resolve(h); } else if (m.type === 'error') { h.error = m; }
        } else if (op === 2) {
          const f = { type: p[0], id: p.readUInt32BE(1), payload: Buffer.from(p.subarray(5)) }; h.frames.push(f);
          if (opts.onFrame) opts.onFrame(h, f);
        } else if (op === 9) s.write(wsEncode(10, p));
      }
    });
  });
}
// A fake web server inside the host: answers every request head with a body naming the forwarded address.
const echo = (h, f) => {
  if (f.type === 2 && f.payload.includes('\r\n\r\n') && !/upgrade:\s*websocket/i.test(f.payload.toString())) {
    const body = 'hello from host, peer ' + h.peers.get(f.id);
    h.send(2, f.id, `HTTP/1.1 200 OK\r\nContent-Length: ${body.length}\r\nConnection: close\r\n\r\n${body}`); h.send(3, f.id);
  } else if (f.type === 1) { (h.peers = h.peers || new Map()).set(f.id, f.payload.toString()); }
  else if (f.type === 2) h.send(2, f.id, 'ws-echo:' + f.payload.toString());
};
function browser(port, host, req, keepOpen) {
  return new Promise((resolve) => {
    const s = net.connect(port, '127.0.0.1'); const chunks = [];
    s.on('data', (d) => chunks.push(d)); s.on('close', () => resolve({ text: Buffer.concat(chunks).toString('latin1'), s }));
    s.write(req || `GET / HTTP/1.1\r\nHost: ${host}\r\n\r\n`);
    if (keepOpen) resolve({ s, chunks });
  });
}
async function setup(opts) {
  const r = createRelay(Object.assign({ domain: DOMAIN, hostAttemptsPerMin: 50, now: Date.now }, opts));
  const port = await r.listen(0, '127.0.0.1');
  return { r, port };
}

test('registration, signed hello, routing, real client address, close', async () => {
  const { r, port } = await setup();
  const kp = keypair();
  const h = await fakeHost(port, kp, { onFrame: echo });
  const label = labelFromPub(kp.pub);
  assert.strictEqual(h.ready.url, `https://${label}.${DOMAIN}/`);
  const b = await browser(port, `${label}.${DOMAIN}`, `GET / HTTP/1.1\r\nHost: ${label}.${DOMAIN}\r\nX-Forwarded-For: 6.6.6.6\r\n\r\n`);
  assert.match(b.text, /^HTTP\/1.1 200/);
  assert.match(b.text, /hello from host, peer 127\.0\.0\.1/);   // OPEN carries the socket address, never the header
  assert.ok(h.frames.some((f) => f.type === 2 && f.payload.toString().includes('X-Forwarded-For: 6.6.6.6')));   // head passed as it arrived; the host's client rewrites it
  r.close(); h.destroy();
});

test('stream after the head (websocket-like bytes), both ways', async () => {
  const { r, port } = await setup();
  const kp = keypair(); const h = await fakeHost(port, kp, { onFrame: echo });
  const host = `${labelFromPub(kp.pub)}.${DOMAIN}`;
  const b = await browser(port, host, `GET /ws HTTP/1.1\r\nHost: ${host}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n\r\n`, true);
  await wait(100); b.s.write('ping1'); await wait(150);
  assert.match(Buffer.concat(b.chunks).toString(), /ws-echo:ping1/);
  b.s.destroy(); await wait(150);
  assert.ok(h.frames.some((f) => f.type === 3), 'CLOSE reaches the host when the browser leaves');
  r.close(); h.destroy();
});

test('offline page, status, bad host', async () => {
  const { r, port } = await setup();
  assert.match((await browser(port, `abcdefghij.${DOMAIN}`)).text, /^HTTP\/1.1 503[\s\S]*offline/i);
  assert.match((await browser(port, DOMAIN, `GET /relay/v1/status HTTP/1.1\r\nHost: ${DOMAIN}\r\n\r\n`)).text, /\{"v":1,"ok":true\}/);
  assert.match((await browser(port, 'evil.example.com')).text, /^HTTP\/1.1 404/);
  r.close();
});

test('directory lists only hosts that opted in', async () => {
  const { r, port } = await setup();
  const a = await fakeHost(port, keypair(), { public: true, name: 'Public one' });
  const b = await fakeHost(port, keypair(), { public: false, name: 'Private one' });
  const j = JSON.parse((await browser(port, DOMAIN, `GET /api/v1/servers HTTP/1.1\r\nHost: ${DOMAIN}\r\n\r\n`)).text.split('\r\n\r\n')[1]);
  assert.strictEqual(j.servers.length, 1); assert.strictEqual(j.servers[0].name, 'Public one');
  assert.match(j.servers[0].url, /^https:\/\//);
  a.destroy(); await wait(100);
  const j2 = JSON.parse((await browser(port, DOMAIN, `GET /api/v1/servers HTTP/1.1\r\nHost: ${DOMAIN}\r\n\r\n`)).text.split('\r\n\r\n')[1]);
  assert.strictEqual(j2.servers.length, 0);
  r.close(); b.destroy();
});

test('hello is refused: bad signature, wrong id, clock off, banned', async () => {
  const { r, port } = await setup({ bansIds: [idFromPub(Buffer.from('ab'.repeat(16), 'hex'))] });
  assert.strictEqual((await fakeHost(port, keypair(), { tamper: true })).error.code, 'protocol');
  assert.strictEqual((await fakeHost(port, keypair(), { badId: true })).error.code, 'protocol');
  assert.strictEqual((await fakeHost(port, keypair(), { ts: 600 })).error.code, 'clock');
  r.bans.ids.add(idFromPub(keypair().pub));   // unrelated id stays unaffected
  const kp = keypair(); r.bans.ids.add(idFromPub(kp.pub));
  assert.strictEqual((await fakeHost(port, kp)).error.code, 'banned');
  r.close();
});

test('one live link per id: a new link replaces the old one', async () => {
  const { r, port } = await setup();
  const kp = keypair();
  const a = await fakeHost(port, kp); const b = await fakeHost(port, kp);
  await wait(150);
  assert.ok(a.closed); assert.ok(!b.closed); assert.strictEqual(r.rooms.size, 1);
  r.close(); b.destroy();
});

test('limits: head size, connections per host, rate of host links', async () => {
  const { r, port } = await setup({ maxHeadBytes: 2048 });
  const kp = keypair(); const h = await fakeHost(port, kp, { maxConns: 2 });
  const host = `${labelFromPub(kp.pub)}.${DOMAIN}`;
  assert.match((await browser(port, host, `GET / HTTP/1.1\r\nHost: ${host}\r\nX: ${'a'.repeat(3000)}\r\n\r\n`)).text, /^HTTP\/1.1 431/);
  const c1 = await browser(port, host, `GET / HTTP/1.1\r\nHost: ${host}\r\n\r\n`, true);
  const c2 = await browser(port, host, `GET / HTTP/1.1\r\nHost: ${host}\r\n\r\n`, true);
  await wait(100);
  assert.match((await browser(port, host)).text, /^HTTP\/1.1 503[\s\S]*full/);   // max_conns 2 reached
  c1.s.destroy(); c2.s.destroy(); r.close(); h.destroy();

  const t = await setup({ hostAttemptsPerMin: 3 });
  let busy = 0; for (let i = 0; i < 6; i++) { const x = await fakeHost(t.port, keypair()); if (x.error) busy++; x.destroy(); }
  assert.ok(busy >= 2, 'later attempts from the same address are refused');
  t.r.close();
});

test('byte rate cap closes a connection that exceeds it', async () => {
  const { r, port } = await setup({ maxBytesPerSecHost: 50000 });
  const kp = keypair(); const h = await fakeHost(port, kp, { onFrame: echo });
  const host = `${labelFromPub(kp.pub)}.${DOMAIN}`;
  const b = await browser(port, host, `GET /ws HTTP/1.1\r\nHost: ${host}\r\nUpgrade: websocket\r\n\r\n`, true);
  await wait(50); b.s.write(Buffer.alloc(60000, 1)); await wait(200);
  assert.ok(b.s.destroyed, 'connection closed by the relay');
  r.close(); h.destroy();
});

test('admin ban needs the token and drops the host', async () => {
  const { r, port } = await setup({ adminToken: 'x'.repeat(24) });
  const kp = keypair(); const h = await fakeHost(port, kp);
  const call = (tok) => browser(port, DOMAIN, `POST /relay/v1/admin/ban HTTP/1.1\r\nHost: ${DOMAIN}\r\nAuthorization: Bearer ${tok}\r\nContent-Length: 2\r\n\r\n{}`.replace('{}', JSON.stringify({ id: idFromPub(kp.pub) })).replace('Content-Length: 2', 'Content-Length: ' + JSON.stringify({ id: idFromPub(kp.pub) }).length));
  assert.match((await call('y'.repeat(24))).text, /^HTTP\/1.1 403/);
  assert.match((await call('x'.repeat(24))).text, /^HTTP\/1.1 200/);
  await wait(150); assert.ok(h.closed); assert.strictEqual(r.rooms.size, 0);
  r.close();
});
