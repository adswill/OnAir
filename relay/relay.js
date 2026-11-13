#!/usr/bin/env node
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// OnAir relay service (docs/ONAIR-WEB-RELAY.md, protocol v1). One file, no dependencies, Node 18+.
// It speaks plain TCP/HTTP: put TLS in front (Caddy, nginx, Cloudflare proxy) for the wildcard certificate.
//   RELAY_DOMAIN=relay.on-air.dev RELAY_PORT=8080 node relay.js
'use strict';
const net = require('net');
const crypto = require('crypto');

const GUID = '258EAFA5-E914-47DA-95CA-C5AB0DC85B11';
const T_OPEN = 1, T_DATA = 2, T_CLOSE = 3;
const B32 = 'abcdefghijklmnopqrstuvwxyz234567';

function base32(buf) {
  let bits = 0, val = 0, out = '';
  for (const b of buf) { val = (val << 8) | b; bits += 8; while (bits >= 5) { out += B32[(val >>> (bits - 5)) & 31]; bits -= 5; } }
  if (bits > 0) out += B32[(val << (5 - bits)) & 31];
  return out;
}
const idFromPub = (pubBytes) => 'onair-' + crypto.createHash('sha256').update(pubBytes).digest('hex').slice(0, 16);
const labelFromPub = (pubBytes) => base32(crypto.createHash('sha256').update(pubBytes).digest()).slice(0, 10);
const clean = (s, n) => String(s == null ? '' : s).replace(/[^\x20-\x7e -￿]/g, '').slice(0, n);

function verifyEd25519(pubHex, msg, sigHex) {
  try {
    const der = Buffer.concat([Buffer.from('302a300506032b6570032100', 'hex'), Buffer.from(pubHex, 'hex')]);
    const key = crypto.createPublicKey({ key: der, format: 'der', type: 'spki' });
    return crypto.verify(null, Buffer.from(msg), key, Buffer.from(sigHex, 'hex'));
  } catch { return false; }
}

// ---- minimal server-side WebSocket (only what the host link needs)
class WS {
  constructor(sock, onMsg, onClose) {
    this.sock = sock; this.buf = Buffer.alloc(0); this.onMsg = onMsg; this.onClose = onClose; this.closed = false;
    this.frag = []; this.fragOp = 0; this.lastRx = Date.now();
    sock.on('data', (d) => { this.lastRx = Date.now(); this.buf = Buffer.concat([this.buf, d]); this.pump(); });
    sock.on('close', () => this.fin()); sock.on('error', () => this.fin());
  }
  fin() { if (!this.closed) { this.closed = true; this.onClose(); } }
  pump() {
    for (;;) {
      const b = this.buf; if (b.length < 2) return;
      const op = b[0] & 15, fin = !!(b[0] & 128), masked = !!(b[1] & 128);
      let len = b[1] & 127, o = 2;
      if (len === 126) { if (b.length < 4) return; len = b.readUInt16BE(2); o = 4; }
      else if (len === 127) { if (b.length < 10) return; len = Number(b.readBigUInt64BE(2)); o = 10; }
      if (!masked || len > (4 << 20)) return this.close();
      if (b.length < o + 4 + len) return;
      const mask = b.subarray(o, o + 4), p = Buffer.from(b.subarray(o + 4, o + 4 + len));
      for (let i = 0; i < p.length; i++) p[i] ^= mask[i & 3];
      this.buf = b.subarray(o + 4 + len);
      if (op === 8) return this.close();
      if (op === 9) { this.send(10, p); continue; }
      if (op === 10) continue;
      if (op === 1 || op === 2) { this.fragOp = op; this.frag = [p]; } else if (op === 0) this.frag.push(p); else return this.close();
      if (fin) { const m = Buffer.concat(this.frag); this.frag = []; this.onMsg(this.fragOp === 1 ? m.toString('utf8') : m); }
    }
  }
  send(op, payload) {
    if (this.closed) return true;
    const p = Buffer.isBuffer(payload) ? payload : Buffer.from(payload);
    let h;
    if (p.length < 126) h = Buffer.from([128 | op, p.length]);
    else if (p.length < 65536) { h = Buffer.alloc(4); h[0] = 128 | op; h[1] = 126; h.writeUInt16BE(p.length, 2); }
    else { h = Buffer.alloc(10); h[0] = 128 | op; h[1] = 127; h.writeBigUInt64BE(BigInt(p.length), 2); }
    return this.sock.write(Buffer.concat([h, p]));
  }
  text(o) { return this.send(1, JSON.stringify(o)); }
  close() { if (!this.closed) { try { this.send(8, Buffer.alloc(0)); } catch {} this.sock.end(); setTimeout(() => this.sock.destroy(), 500).unref(); } }
}

const frame = (t, id, payload) => { const h = Buffer.alloc(5); h[0] = t; h.writeUInt32BE(id >>> 0, 1); return payload && payload.length ? Buffer.concat([h, payload]) : h; };

function reply(sock, code, reason, type, body, extra) {
  const b = Buffer.from(body);
  sock.end(Buffer.concat([Buffer.from(`HTTP/1.1 ${code} ${reason}\r\nContent-Type: ${type}\r\nContent-Length: ${b.length}\r\nConnection: close\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n${extra || ''}\r\n`), b]));
}
const OFFLINE = `<!doctype html><meta charset=utf-8><meta name=viewport content="width=device-width"><title>Radio offline</title><body style="font:16px system-ui;max-width:32em;margin:15vh auto;padding:0 1em"><h1>This radio is offline</h1><p>The person sharing it has stopped, or lost their connection. Try again in a minute.</p><p><a href="https://web.on-air.dev/">Other shared radios</a></p>`;

function createRelay(opt = {}) {
  const o = Object.assign({
    domain: 'relay.on-air.dev', mainHost: null, hostPath: '/relay/v1/host', scheme: 'https', publicPort: '',
    maxHosts: 2000, maxConnsPerHost: 64, maxConnsPerIp: 24, maxTotalConns: 8000,
    maxBytesPerSecHost: 8 * 1024 * 1024, maxHeadBytes: 16384, idleMs: 120000, linkIdleMs: 70000,
    hostAttemptsPerMin: 6, clockSkewS: 120, adminToken: '', bansIds: [], bansIps: [], clientIpHeader: '', now: () => Date.now(),
  }, opt);
  const mainHost = (o.mainHost || o.domain).toLowerCase();
  const rooms = new Map();       // label -> room
  const byId = new Map();        // id -> room
  const bans = { ids: new Set(o.bansIds), ips: new Set(o.bansIps) };
  const ipConns = new Map();     // ip -> count
  const attempts = new Map();    // key -> [timestamps]
  let totalConns = 0;
  const log = opt.log || (() => {});

  const tooMany = (key) => {
    const t = o.now(), a = (attempts.get(key) || []).filter((x) => t - x < 60000);
    a.push(t); attempts.set(key, a);
    if (attempts.size > 20000) for (const [k, v] of attempts) if (!v.length || t - v[v.length - 1] > 60000) attempts.delete(k);
    return a.length > o.hostAttemptsPerMin;
  };
  const rateOk = (room, n) => {
    const t = o.now();
    if (t - room.winStart >= 1000) { room.winStart = t; room.winBytes = 0; }
    room.winBytes += n; room.bytes += n;
    return room.winBytes <= room.maxBps;
  };
  const hostOf = (header) => { const m = /^host:\s*([^\r\n]*)/im.exec(header); return m ? m[1].trim().toLowerCase().replace(/:\d+$/, '') : ''; };

  function clientIp(sock, head) {
    if (o.clientIpHeader) { const m = new RegExp('^' + o.clientIpHeader + ':\\s*([^\\r\\n]*)', 'im').exec(head); if (m) return m[1].split(',')[0].trim().slice(0, 64); }
    return (sock.remoteAddress || '').replace(/^::ffff:/, '');
  }

  function dropRoom(room) {
    if (rooms.get(room.label) === room) rooms.delete(room.label);
    if (byId.get(room.id) === room) byId.delete(room.id);
    for (const c of room.conns.values()) { c.sock.destroy(); }
    room.conns.clear(); room.ws.close();
  }

  // ---- the host link
  function acceptHost(sock, head, ip) {
    const key = /^sec-websocket-key:\s*(\S+)/im.exec(head);
    if (!key || !/^upgrade:\s*websocket/im.test(head)) return reply(sock, 400, 'Bad Request', 'text/plain', 'WebSocket upgrade required\n');
    if (tooMany('ip:' + ip)) return reply(sock, 429, 'Too Many Requests', 'text/plain', 'slow down\n', 'Retry-After: 30\r\n');
    if (bans.ips.has(ip)) return reply(sock, 403, 'Forbidden', 'text/plain', 'banned\n');
    const accept = crypto.createHash('sha1').update(key[1] + GUID).digest('base64');
    sock.write(`HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ${accept}\r\n\r\n`);
    sock.setTimeout(0);
    const nonce = crypto.randomBytes(16).toString('hex');
    let room = null, state = 'hello';
    const ws = new WS(sock, onMsg, onClose);
    const helloTimer = setTimeout(() => { if (state === 'hello') ws.close(); }, 15000);
    const idle = setInterval(() => { if (o.now() - ws.lastRx > o.linkIdleMs) ws.close(); }, 5000);
    idle.unref(); helloTimer.unref();
    ws.text({ type: 'challenge', v: 1, nonce });
    const fail = (code, message) => { ws.text({ type: 'error', code, message }); setTimeout(() => ws.close(), 50); state = 'dead'; };

    function onMsg(m) {
      if (state === 'hello') {
        if (typeof m !== 'string' || m.length > 4096) return fail('protocol', 'bad hello');
        let h; try { h = JSON.parse(m); } catch { return fail('protocol', 'bad hello'); }
        if (!h || h.type !== 'hello') return fail('protocol', 'bad hello');
        if (h.v !== 1) return fail('version', 'this relay speaks protocol 1');
        if (!/^[0-9a-f]{64}$/.test(h.pub || '') || !/^[0-9a-f]{128}$/.test(h.sig || '')) return fail('protocol', 'bad key');
        const pubBytes = Buffer.from(h.pub, 'hex');
        if (h.id !== idFromPub(pubBytes)) return fail('protocol', 'id does not match key');
        if (h.nonce !== nonce) return fail('protocol', 'wrong nonce');
        if (Math.abs(o.now() / 1000 - Number(h.ts)) > o.clockSkewS) return fail('clock', 'your clock is more than 2 minutes off');
        const canon = `OnAir-Relay-v1\nid=${h.id}\npub=${h.pub}\nts=${h.ts}\nnonce=${h.nonce}\n`;
        if (!verifyEd25519(h.pub, canon, h.sig)) return fail('protocol', 'bad signature');
        if (bans.ids.has(h.id)) return fail('banned', 'this server is banned');
        if (tooMany('id:' + h.id)) return fail('busy', 'too many connection attempts, try again soon');
        const label = labelFromPub(pubBytes);
        const old = byId.get(h.id);
        if (!old && rooms.size >= o.maxHosts) return fail('busy', 'the relay is full');
        if (old) dropRoom(old);   // one live link per id
        room = { id: h.id, label, ws, name: clean(h.name, 60), pub: h.pub, listed: h.public === true, client: clean(h.client, 40),
          maxConns: Math.max(1, Math.min(o.maxConnsPerHost, parseInt(h.max_conns, 10) || o.maxConnsPerHost)),
          conns: new Map(), nextId: 1, since: Math.floor(o.now() / 1000), bytes: 0, total: 0, winStart: 0, winBytes: 0, maxBps: o.maxBytesPerSecHost };
        rooms.set(label, room); byId.set(h.id, room); state = 'ready'; clearTimeout(helloTimer);
        const port = o.publicPort ? ':' + o.publicPort : '';
        room.url = `${o.scheme}://${label}.${o.domain}${port}/`;
        ws.text({ type: 'ready', url: room.url, max_conns: room.maxConns });
        log('host up', room.id, label);
        return;
      }
      if (state !== 'ready' || !Buffer.isBuffer(m) || m.length < 5) return;
      const t = m[0], id = m.readUInt32BE(1), c = room.conns.get(id);
      if (!c) return;
      if (t === T_DATA) {
        if (!rateOk(room, m.length - 5)) { c.sock.destroy(); return; }
        if (!c.sock.write(m.subarray(5))) { c.waitDrain = true; c.sock.once('drain', () => { c.waitDrain = false; }); }
      } else if (t === T_CLOSE) { room.conns.delete(id); c.gone = true; c.sock.end(); }
    }
    function onClose() {
      clearInterval(idle); clearTimeout(helloTimer);
      if (room && byId.get(room.id) === room) { log('host down', room.id); dropRoom(room); }
    }
  }

  // ---- a browser connection for a room
  function carry(sock, room, ip, head, first) {
    if (room.conns.size >= room.maxConns || totalConns >= o.maxTotalConns || (ipConns.get(ip) || 0) >= o.maxConnsPerIp) {
      return reply(sock, 503, 'Service Unavailable', 'text/plain', 'This radio is full. Try again in a minute.\n', 'Retry-After: 15\r\n');
    }
    const id = room.nextId++; if (room.nextId > 0x7fffffff) room.nextId = 1;
    const c = { sock, gone: false, waitDrain: false };
    room.conns.set(id, c); totalConns++; room.total++; ipConns.set(ip, (ipConns.get(ip) || 0) + 1);
    sock.setTimeout(o.idleMs, () => sock.destroy());
    const send = (buf) => {
      for (let i = 0; i < buf.length; i += 1 << 20) {
        const part = buf.subarray(i, i + (1 << 20));
        if (!rateOk(room, part.length)) { sock.destroy(); return; }
        if (!room.ws.send(2, frame(T_DATA, id, part))) { sock.pause(); room.ws.sock.once('drain', () => sock.resume()); }
      }
    };
    room.ws.send(2, frame(T_OPEN, id, Buffer.from(ip.slice(0, 64))));
    send(first);
    sock.on('data', (d) => { if (!c.gone) send(d); });
    sock.on('error', () => {});
    sock.on('close', () => {
      totalConns--; const n = (ipConns.get(ip) || 1) - 1; if (n > 0) ipConns.set(ip, n); else ipConns.delete(ip);
      if (room.conns.get(id) === c) { room.conns.delete(id); if (!room.ws.closed) room.ws.send(2, frame(T_CLOSE, id)); }
    });
  }

  function listing() {
    const l = [];
    for (const r of rooms.values()) if (r.listed) l.push({ id: r.id, name: r.name, url: r.url, listeners: r.conns.size, since: r.since });
    l.sort((a, b) => b.listeners - a.listeners || a.name.localeCompare(b.name));
    return l;
  }

  function onConnection(sock) {
    sock.on('error', () => {});
    let buf = Buffer.alloc(0), routed = false;
    sock.setTimeout(30000, () => { if (!routed) sock.destroy(); });
    const onData = (d) => {
      buf = Buffer.concat([buf, d]);
      const end = buf.indexOf('\r\n\r\n');
      if (end < 0) { if (buf.length > o.maxHeadBytes) { sock.removeListener('data', onData); reply(sock, 431, 'Request Header Fields Too Large', 'text/plain', 'head too large\n'); } return; }
      if (end + 4 > o.maxHeadBytes) { sock.removeListener('data', onData); return reply(sock, 431, 'Request Header Fields Too Large', 'text/plain', 'head too large\n'); }
      sock.removeListener('data', onData); routed = true; sock.setTimeout(0);
      route(sock, buf, end);
    };
    sock.on('data', onData);
  }

  function route(sock, buf, end) {
    const head = buf.subarray(0, end + 4).toString('latin1');
    const ip = clientIp(sock, head);
    const host = hostOf(head);
    const line = head.split('\r\n', 1)[0].split(' ');
    const path = (line[1] || '/').split('?')[0];
    if (host === mainHost) {
      if (path === o.hostPath) return acceptHost(sock, head, ip);
      if (path === '/relay/v1/status') return reply(sock, 200, 'OK', 'application/json', JSON.stringify({ v: 1, ok: true }));
      if (path === '/api/v1/servers') return reply(sock, 200, 'OK', 'application/json', JSON.stringify({ v: 1, servers: listing() }), 'Access-Control-Allow-Origin: *\r\n');
      if (path.startsWith('/relay/v1/admin/') && line[0] === 'POST') return admin(sock, head, buf.subarray(end + 4), path);
      return reply(sock, 200, 'OK', 'text/plain', 'OnAir relay\n');
    }
    if (!host.endsWith('.' + o.domain.toLowerCase())) return reply(sock, 404, 'Not Found', 'text/plain', 'unknown host\n');
    const label = host.slice(0, -(o.domain.length + 1));
    const room = rooms.get(label);
    if (bans.ips.has(ip)) return reply(sock, 403, 'Forbidden', 'text/plain', 'banned\n');
    if (!room) return reply(sock, 503, 'Service Unavailable', 'text/html; charset=utf-8', OFFLINE, 'Retry-After: 30\r\n');
    carry(sock, room, ip, head, buf);
  }

  function admin(sock, head, body, path) {
    const t = /^authorization:\s*bearer\s+(\S+)/im.exec(head);
    if (!o.adminToken || !t || t[1].length !== o.adminToken.length || !crypto.timingSafeEqual(Buffer.from(t[1]), Buffer.from(o.adminToken))) return reply(sock, 403, 'Forbidden', 'text/plain', 'no\n');
    let j = {}; try { j = JSON.parse(body.toString() || '{}'); } catch {}
    const unban = path.endsWith('/unban');
    if (typeof j.id === 'string') { unban ? bans.ids.delete(j.id) : bans.ids.add(j.id); const r = byId.get(j.id); if (r && !unban) r.ws.close(); }
    if (typeof j.ip === 'string') { unban ? bans.ips.delete(j.ip) : bans.ips.add(j.ip); }
    reply(sock, 200, 'OK', 'application/json', '{"ok":true}');
  }

  const server = net.createServer(onConnection);
  return {
    server, rooms, bans, listing, stats: () => ({ hosts: rooms.size, conns: totalConns }),
    listen: (port, host) => new Promise((res) => server.listen(port, host || '0.0.0.0', () => res(server.address().port))),
    close: () => { for (const r of [...rooms.values()]) { r.ws.sock.destroy(); dropRoom(r); } server.close(); },
  };
}

module.exports = { createRelay, labelFromPub, idFromPub, base32 };

if (require.main === module) {
  const e = process.env;
  const relay = createRelay({
    domain: e.RELAY_DOMAIN || 'relay.on-air.dev', mainHost: e.RELAY_MAIN_HOST || undefined,
    scheme: e.RELAY_SCHEME || 'https', publicPort: e.RELAY_PUBLIC_PORT || '',
    adminToken: e.RELAY_ADMIN_TOKEN || '', clientIpHeader: e.RELAY_CLIENT_IP_HEADER || '',
    bansIds: (e.RELAY_BAN_IDS || '').split(',').filter(Boolean), bansIps: (e.RELAY_BAN_IPS || '').split(',').filter(Boolean),
    maxBytesPerSecHost: Number(e.RELAY_MAX_BPS) || 8 * 1024 * 1024, maxConnsPerHost: Number(e.RELAY_MAX_CONNS_PER_HOST) || 64,
    log: (...a) => console.log(new Date().toISOString(), ...a),   // ids only, never listener content
  });
  relay.listen(Number(e.RELAY_PORT) || 8080, e.RELAY_BIND || '0.0.0.0').then((p) => console.log('onair relay listening on', p));
}
