// Unit tests of the Worker's pure helpers, cross-checked against the Node relay (relay/relay.js) where they must agree.
import test from 'node:test';
import assert from 'node:assert';
import crypto from 'node:crypto';
import { createRequire } from 'node:module';
import * as L from '../src/lib.js';

const require = createRequire(import.meta.url);
const NodeRelay = require('../../../relay/relay.js');

function keypair() {
  const { publicKey, privateKey } = crypto.generateKeyPairSync('ed25519');
  const pub = publicKey.export({ format: 'der', type: 'spki' }).subarray(12);
  return { priv: privateKey, pub: new Uint8Array(pub), pubHex: pub.toString('hex') };
}

test('id and address label match the Node relay for many keys', async () => {
  for (let i = 0; i < 200; i++) {
    const k = keypair();
    assert.strictEqual(await L.idFromPub(k.pub), NodeRelay.idFromPub(Buffer.from(k.pub)));
    assert.strictEqual(await L.labelFromPub(k.pub), NodeRelay.labelFromPub(Buffer.from(k.pub)));
  }
  const r = crypto.randomBytes(37);
  assert.strictEqual(L.base32(r), NodeRelay.base32(r));
});

test('hello check: good, tampered, wrong id, wrong nonce, clock, version, junk', async () => {
  const k = keypair(), nonce = L.randomHex(16), now = Math.floor(Date.now() / 1000);
  const mk = (o = {}) => {
    const h = { type: 'hello', v: 1, id: NodeRelay.idFromPub(Buffer.from(k.pub)), pub: k.pubHex, ts: now, nonce, name: 'Roof\u0007 antenna', max_conns: 9, public: true, ...o };
    h.sig = crypto.sign(null, Buffer.from(L.canonHello(h)), k.priv).toString('hex');
    if (o.tamper) h.sig = (h.sig[0] === '0' ? '1' : '0') + h.sig.slice(1);
    return JSON.stringify(h);
  };
  const ok = await L.checkHello(mk(), nonce, now);
  assert.ok(ok.ok); assert.strictEqual(ok.name, 'Roof antenna'); assert.strictEqual(ok.maxConns, 9); assert.strictEqual(ok.listed, true);
  assert.strictEqual((await L.checkHello(mk({ tamper: true }), nonce, now)).code, 'protocol');
  assert.strictEqual((await L.checkHello(mk({ id: 'onair-0000000000000000' }), nonce, now)).code, 'protocol');
  assert.strictEqual((await L.checkHello(mk(), 'ff' + nonce.slice(2), now)).code, 'protocol');
  for (const skew of [121, -121, 600]) assert.strictEqual((await L.checkHello(mk({ ts: now + skew }), nonce, now)).code, 'clock');
  for (const skew of [119, -119, 0]) assert.ok((await L.checkHello(mk({ ts: now + skew }), nonce, now)).ok);
  assert.strictEqual((await L.checkHello(mk({ v: 2 }), nonce, now)).code, 'version');
  for (const junk of ['{', '[]', 'null', 'x'.repeat(5000), JSON.stringify({ type: 'hello', v: 1, pub: 'zz', sig: 'yy' })]) assert.strictEqual((await L.checkHello(junk, nonce, now)).ok, false);
});

test('relay frames round trip for all types and sizes', () => {
  for (const t of [1, 2, 3]) for (const n of [0, 1, 125, 65536]) for (const id of [0, 1, 0x7fffffff, 0xffffffff]) {
    const p = crypto.randomBytes(n), f = L.parseFrame(L.frame(t, id, new Uint8Array(p)));
    assert.strictEqual(f.t, t); assert.strictEqual(f.id, id >>> 0); assert.deepStrictEqual(Buffer.from(f.payload), p);
  }
  assert.strictEqual(L.parseFrame(new Uint8Array(4)), null);
});

// A server-side (unmasked) frame encoder for the tests
function srv(op, p, fin = true) {
  const n = p.length, h = n < 126 ? Buffer.from([(fin ? 128 : 0) | op, n]) : n < 65536 ? Buffer.from([(fin ? 128 : 0) | op, 126, n >> 8, n & 255]) : (() => { const b = Buffer.alloc(10); b[0] = (fin ? 128 : 0) | op; b[1] = 127; b.writeBigUInt64BE(BigInt(n), 2); return b; })();
  return Buffer.concat([h, p]);
}
function unmaskClient(buf) {   // decode the relay's masked client frame
  const b = Buffer.from(buf); assert.ok(b[1] & 128, 'client frames are masked');
  let len = b[1] & 127, o = 2; if (len === 126) { len = b.readUInt16BE(2); o = 4; } else if (len === 127) { len = Number(b.readBigUInt64BE(2)); o = 10; }
  const m = b.subarray(o, o + 4), p = Buffer.from(b.subarray(o + 4, o + 4 + len)); for (let i = 0; i < p.length; i++) p[i] ^= m[i & 3];
  return { op: b[0] & 15, fin: !!(b[0] & 128), p };
}

test('client frames: masked, every length class, decodable', () => {
  for (const n of [0, 1, 125, 126, 65535, 65536, 200000]) {
    const p = crypto.randomBytes(n), f = unmaskClient(L.wsClientFrame(2, new Uint8Array(p)));
    assert.strictEqual(f.op, 2); assert.ok(f.fin); assert.deepStrictEqual(f.p, p);
  }
  assert.strictEqual(unmaskClient(L.wsClientFrame(1, 'héllo')).p.toString(), 'héllo');
});

test('server frame decoder: any split of the stream, fragments, control frames between fragments, oversize', () => {
  const msgs = [];
  const parts = [srv(1, Buffer.from('{"type":"welcome"}')), srv(2, crypto.randomBytes(70000)), srv(9, Buffer.from('k')),
    srv(2, Buffer.from('frag-a'), false), srv(9, Buffer.from('mid')), srv(0, Buffer.from('frag-b'), false), srv(0, Buffer.from('frag-c')), srv(8, Buffer.from([0x0f, 0xa1]))];
  const all = Buffer.concat(parts);
  for (const step of [1, 2, 3, 7, 100, 4096, all.length]) {
    msgs.length = 0;
    const d = new L.WsDecoder((op, p) => msgs.push([op, Buffer.from(p)]));
    for (let i = 0; i < all.length; i += step) d.push(new Uint8Array(all.subarray(i, i + step)));
    assert.deepStrictEqual(msgs.map((m) => m[0]), [1, 2, 9, 9, 2, 8], 'step ' + step);
    assert.strictEqual(msgs[1][1].length, 70000);
    assert.strictEqual(msgs[4][1].toString(), 'frag-afrag-bfrag-c');
    assert.strictEqual(msgs[5][1].readUInt16BE(0), 4001);
  }
  let dead = false; const d = new L.WsDecoder((op) => { if (op === -1) dead = true; }, 1000);
  d.push(new Uint8Array(srv(2, Buffer.alloc(2000)))); assert.ok(dead);
});

test('request head: forged forwarding headers and Cloudflare headers dropped, Host kept, Connection forced', () => {
  const hs = new Headers({ 'x-forwarded-for': '6.6.6.6', 'x-real-ip': '6.6.6.6', forwarded: 'for=6.6.6.6', 'cf-connecting-ip': '1.2.3.4', 'cf-ray': 'x', 'x-relay-ip': '1.2.3.4', origin: 'https://abc.example', cookie: 'a=b', connection: 'keep-alive', 'transfer-encoding': 'chunked' });
  const h = L.requestHead('POST', '/web/v1/info?x=1', 'abcdefghij-r.on-air.dev', hs, { bodyLen: 5 });
  assert.match(h, /^POST \/web\/v1\/info\?x=1 HTTP\/1\.1\r\nHost: abcdefghij-r\.on-air\.dev\r\n/);
  for (const bad of ['6.6.6.6', '1.2.3.4', 'cf-ray', 'keep-alive', 'chunked']) assert.ok(!h.includes(bad), bad);
  assert.match(h, /origin: https:\/\/abc\.example\r\n/); assert.match(h, /cookie: a=b\r\n/); assert.match(h, /connection: close\r\n/); assert.match(h, /content-length: 5\r\n\r\n$/);
  const w = L.requestHead('GET', '/web/v1/ws', 'h', new Headers({ 'sec-websocket-protocol': 'onair-web.v1', 'sec-websocket-extensions': 'permessage-deflate', 'sec-websocket-key': 'browser' }), { wsKey: 'abc=' });
  assert.match(w, /sec-websocket-protocol: onair-web\.v1/); assert.ok(!w.includes('permessage-deflate')); assert.ok(!w.includes('browser'));
  assert.match(w, /upgrade: websocket\r\nconnection: Upgrade\r\nsec-websocket-version: 13\r\nsec-websocket-key: abc=\r\n\r\n$/);
});

test('response head and chunked body in every split', () => {
  const enc = new TextEncoder();
  assert.strictEqual(L.parseResponseHead(enc.encode('HTTP/1.1 200 OK\r\nA: b')), null);
  assert.ok(L.parseResponseHead(enc.encode('x'.repeat(70000))).error);
  assert.ok(L.parseResponseHead(enc.encode('SMTP 200\r\n\r\n')).error);
  const r = L.parseResponseHead(enc.encode('HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nSec-WebSocket-Protocol: onair-web.v1\r\nX-A: 1\r\n\r\nREST'));
  assert.strictEqual(r.status, 101); assert.strictEqual(r.proto, 'onair-web.v1'); assert.deepStrictEqual(r.headers, [['x-a', '1']]); assert.strictEqual(new TextDecoder().decode(r.rest), 'REST');
  const body = crypto.randomBytes(5000);
  const chunked = Buffer.concat([Buffer.from('7d0;ext=1\r\n'), body.subarray(0, 2000), Buffer.from('\r\nBB8\r\n'), body.subarray(2000), Buffer.from('\r\n0\r\n\r\n')]);
  for (const step of [1, 3, 64, 2001, chunked.length]) {
    const d = new L.ChunkedDecoder(); let out = Buffer.alloc(0);
    for (let i = 0; i < chunked.length; i += step) out = Buffer.concat([out, Buffer.from(d.push(new Uint8Array(chunked.subarray(i, i + step))))]);
    assert.ok(d.done, 'step ' + step); assert.deepStrictEqual(out, body);
  }
  const bad = new L.ChunkedDecoder(); bad.push(enc.encode('zz\r\n')); assert.ok(bad.bad);
});

test('attempt limiter: per key, sliding minute', () => {
  const a = new L.Attempts(3);
  for (let i = 0; i < 3; i++) assert.ok(!a.tooMany('ip:1', 1000 + i));
  assert.ok(a.tooMany('ip:1', 1004)); assert.ok(!a.tooMany('ip:2', 1004));
  assert.ok(!a.tooMany('ip:1', 70000));
});
