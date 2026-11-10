// One Durable Object per host link (one shared radio). It holds the host's WebSocket with the Hibernation API, so a shared
// radio with nobody listening costs no duration. Browsers' requests arrive here from the Worker: plain HTTP is written to the
// host as a request head + body and the host's answer is parsed back into a Response; a browser WebSocket is accepted here
// (hibernatable too) and bridged to a WebSocket the relay opens, as a client, to the host's web server inside the tunnel.
import { DurableObject } from 'cloudflare:workers';
import { T_OPEN, T_DATA, T_CLOSE, frame, parseFrame, checkHello, randomHex, requestHead, parseResponseHead, ChunkedDecoder,
  WsDecoder, wsClientFrame, concat, toU8, OFFLINE } from './lib.js';

const td = new TextDecoder();
const CHUNK = 65536;              // DATA frames toward the host
const MAX_BODY = 1 << 20;         // a browser's request body
const MAX_HEAD = 16384;
const IDLE_MS = 120000;           // HTTP phase without bytes
const HEAD_MS = 30000;            // the host must start answering within this
const TOUCH_MS = 30 * 60000;      // registry refresh while the link is up

const text = (status, body, extra) => new Response(body, { status, headers: { 'content-type': 'text/plain', 'cache-control': 'no-store', 'x-content-type-options': 'nosniff', ...(extra || {}) } });
const offline = () => new Response(OFFLINE, { status: 503, headers: { 'content-type': 'text/html; charset=utf-8', 'cache-control': 'no-store', 'retry-after': '30', 'x-relay-gone': '1' } });

export class Room extends DurableObject {
  constructor(ctx, env) {
    super(ctx, env);
    this.conns = new Map();   // id -> connection (rebuilt from the sockets' attachments after hibernation)
    this.nextId = 1;
    this.winStart = 0; this.winBytes = 0;
    this.maxBps = Number(env.MAX_BPS) || 8 * 1024 * 1024;
    this.maxPerHost = Number(env.MAX_CONNS_PER_HOST) || 64;
    this.maxPerIp = Number(env.MAX_CONNS_PER_IP) || 24;
    for (const ws of ctx.getWebSockets()) {
      const a = ws.deserializeAttachment() || {};
      if (a.kind === 'l') { this.conns.set(a.id, this.wsConn(a.id, a.ip, ws)); this.nextId = Math.max(this.nextId, a.id + 1); }
    }
  }
  registry() { return this.env.REGISTRY.get(this.env.REGISTRY.idFromName('global')); }
  hostWs() { return this.ctx.getWebSockets('host')[0] || null; }
  hostState() { const h = this.hostWs(); return h ? (h.deserializeAttachment() || {}) : {}; }
  toHost(t, id, payload) { const h = this.hostWs(); if (!h) return false; try { h.send(frame(t, id, payload)); return true; } catch { return false; } }
  rateOk(n) {
    const t = Date.now();
    if (t - this.winStart >= 1000) { this.winStart = t; this.winBytes = 0; }
    this.winBytes += n; return this.winBytes <= this.maxBps;
  }
  sendData(c, bytes) {
    for (let i = 0; i < bytes.length; i += CHUNK) {
      const part = bytes.subarray(i, i + CHUNK);
      if (!this.rateOk(part.length)) { this.drop(c, true); return false; }
      if (!this.toHost(T_DATA, c.id, part)) { this.drop(c, false); return false; }
    }
    return true;
  }
  ipCount(ip) { let n = 0; for (const c of this.conns.values()) if (c.ip === ip) n++; return n; }
  allocId() { for (;;) { const id = this.nextId++; if (this.nextId > 0x7fffffff) this.nextId = 1; if (!this.conns.has(id)) return id; } }

  // ---- RPC from the registry: a newer link for the same id took over, or the id was banned
  async evict(id) {
    const h = this.hostWs(); if (!h) return;
    const a = h.deserializeAttachment() || {};
    if (a.id === id) { this.closeAll(); try { h.close(1000, 'replaced'); } catch { /* gone */ } }
  }

  async fetch(request) {
    const kind = request.headers.get('x-relay-kind');
    if (kind === 'host') return this.acceptHost(request);
    return this.browser(request);
  }

  // ---- the host link
  acceptHost(request) {
    const [client, server] = Object.values(new WebSocketPair());
    this.ctx.acceptWebSocket(server, ['host']);
    const nonce = randomHex(16);
    server.serializeAttachment({ kind: 'host', st: 'hello', nonce, t0: Date.now() });
    server.send(JSON.stringify({ type: 'challenge', v: 1, nonce }));
    this.ctx.storage.setAlarm(Date.now() + 15000);
    return new Response(null, { status: 101, webSocket: client });
  }
  fail(ws, code, message) {
    try { ws.send(JSON.stringify({ type: 'error', code, message })); } catch { /* gone */ }
    const a = ws.deserializeAttachment() || {}; a.st = 'dead'; ws.serializeAttachment(a);
    setTimeout(() => { try { ws.close(1008, code); } catch { /* gone */ } }, 50);
  }
  async hello(ws, a, msg) {
    a.st = 'checking'; ws.serializeAttachment(a);
    const h = await checkHello(typeof msg === 'string' ? msg : '', a.nonce, Date.now() / 1000);
    if (!h.ok) return this.fail(ws, h.code, h.message);
    const e = this.env, port = e.RELAY_PUBLIC_PORT ? ':' + e.RELAY_PUBLIC_PORT : '';
    const url = `${e.RELAY_SCHEME || 'https'}://${h.label}${e.RELAY_LABEL_SUFFIX || '-r.on-air.dev'}${port}/`;
    const room = this.ctx.id.toString();
    const r = await this.registry().claim({ id: h.id, label: h.label, room, name: h.name, url, listed: h.listed });
    if (!r.ok) return this.fail(ws, r.code, r.message);
    const maxConns = Math.max(1, Math.min(this.maxPerHost, h.maxConns || this.maxPerHost));
    ws.serializeAttachment({ kind: 'host', st: 'ready', id: h.id, label: h.label, url, maxConns, room });
    ws.send(JSON.stringify({ type: 'ready', url, max_conns: maxConns }));
    this.ctx.storage.setAlarm(Date.now() + TOUCH_MS);
  }
  async alarm() {
    const h = this.hostWs(); if (!h) return;
    const a = h.deserializeAttachment() || {};
    if (a.st !== 'ready') { if (Date.now() - (a.t0 || 0) >= 14000) try { h.close(1008, 'hello timeout'); } catch { /* gone */ } return; }
    try { await this.registry().touch(a.id, a.room, this.listenerCount()); } catch { /* next time */ }
    this.ctx.storage.setAlarm(Date.now() + TOUCH_MS);
  }
  listenerCount() { let n = 0; for (const c of this.conns.values()) if (c.kind === 'ws') n++; return n; }
  report() { const a = this.hostState(); if (a.st === 'ready') this.ctx.waitUntil(this.registry().touch(a.id, a.room, this.listenerCount()).catch(() => {})); }

  async webSocketMessage(ws, msg) {
    const a = ws.deserializeAttachment() || {};
    if (a.kind === 'host') {
      if (a.st === 'hello') return this.hello(ws, a, msg);
      if (a.st !== 'ready' || typeof msg === 'string') return;
      const f = parseFrame(msg); if (!f) return;
      const c = this.conns.get(f.id); if (!c) return;
      if (f.t === T_DATA) { if (!this.rateOk(f.payload.length)) return this.drop(c, true); c.fromHost(f.payload); }
      else if (f.t === T_CLOSE) { c.gone = true; this.drop(c, false); }
      return;
    }
    if (a.kind === 'l') {
      const c = this.conns.get(a.id); if (!c) { try { ws.close(1011, 'gone'); } catch { /* gone */ } return; }
      this.sendData(c, wsClientFrame(typeof msg === 'string' ? 1 : 2, toU8(msg)));
    }
  }
  async webSocketClose(ws, code) {
    const a = ws.deserializeAttachment() || {};
    if (a.kind === 'host') {
      if (a.st === 'ready') { this.closeAll(); this.ctx.waitUntil(this.registry().release(a.id, a.room).catch(() => {})); }
      return;
    }
    if (a.kind === 'l') {
      const c = this.conns.get(a.id);
      if (c) { c.browserGone = true; if (!c.gone) { const p = new Uint8Array(2); p[0] = (code >= 1000 && code < 5000 && code !== 1005 && code !== 1006 ? code : 1000) >> 8; p[1] = (code >= 1000 && code < 5000 && code !== 1005 && code !== 1006 ? code : 1000) & 255; this.sendData(c, wsClientFrame(8, p)); } this.drop(c, !c.gone); }
    }
  }
  async webSocketError(ws) { return this.webSocketClose(ws, 1006); }

  closeAll() { for (const c of [...this.conns.values()]) { c.gone = true; this.drop(c, false); } }
  // Ends a connection on both sides. `tellHost`: send CLOSE (not when the host closed it).
  drop(c, tellHost) {
    if (this.conns.get(c.id) !== c) return;
    this.conns.delete(c.id);
    if (tellHost && !c.gone) this.toHost(T_CLOSE, c.id);
    c.end();
    if (c.kind === 'ws') this.report();
  }

  // ---- a browser request
  async browser(request) {
    const host = this.hostState();
    if (host.st !== 'ready') return offline();
    const ip = request.headers.get('x-relay-ip') || '0.0.0.0';
    if (this.conns.size >= host.maxConns || this.ipCount(ip) >= this.maxPerIp) return text(503, 'This radio is full. Try again in a minute.\n', { 'retry-after': '15' });
    const u = new URL(request.url), hostName = request.headers.get('x-relay-host') || u.host;
    const isWs = (request.headers.get('upgrade') || '').toLowerCase() === 'websocket';
    let body = null;
    if (!isWs && request.body && request.method !== 'GET' && request.method !== 'HEAD') {
      const len = Number(request.headers.get('content-length') || 0);
      if (len > MAX_BODY) return text(413, 'request too large\n');
      body = new Uint8Array(await request.arrayBuffer());
      if (body.length > MAX_BODY) return text(413, 'request too large\n');
    }
    const wsKey = isWs ? btoa(String.fromCharCode(...crypto.getRandomValues(new Uint8Array(16)))) : '';
    const head = requestHead(request.method, u.pathname + u.search, hostName, request.headers, { wsKey, bodyLen: body ? body.length : 0 });
    if (head.length > MAX_HEAD) return text(431, 'head too large\n');
    const id = this.allocId();
    const c = this.httpConn(id, ip, request.method === 'HEAD');
    this.conns.set(id, c);
    this.toHost(T_OPEN, id, ip.slice(0, 64));
    if (!this.sendData(c, new TextEncoder().encode(head))) return text(502, 'relay link lost\n');
    if (body && body.length && !this.sendData(c, body)) return text(502, 'relay link lost\n');
    const r = await c.headP;
    if (!r) return text(504, 'the radio did not answer\n');
    if (isWs && r.status === 101) return this.becomeWs(c, r);
    return c.response(r, isWs);
  }

  httpConn(id, ip, isHead) {
    const room = this;
    let resolveHead; const headP = new Promise((res) => { resolveHead = res; });
    const c = {
      id, ip, kind: 'http', gone: false, buf: new Uint8Array(0), pend: new Uint8Array(0), head: null, headP, writer: null, left: -1, chunk: null, ended: false, timer: null,
      arm() { clearTimeout(this.timer); this.timer = setTimeout(() => room.drop(this, true), this.head ? IDLE_MS : HEAD_MS); },
      fromHost(p) {
        this.arm();
        if (this.ws) return this.ws(p);
        if (!this.head) {
          this.buf = concat(this.buf, p);
          const r = parseResponseHead(this.buf);
          if (!r) return;
          if (r.error) { resolveHead(null); room.drop(this, true); return; }
          this.head = r; this.buf = new Uint8Array(0); resolveHead(r);
          return;   // the rest is taken by response() / becomeWs()
        }
        this.body(p);
      },
      body(p) {
        if (this.ended) return;
        if (!this.writer) { this.pend = concat(this.pend, p); return; }   // between the head and response(): keep it
        let out = p;
        if (this.chunk) { out = this.chunk.push(p); if (this.chunk.bad) { room.drop(this, true); return; } }
        else if (this.left >= 0) { out = p.subarray(0, this.left); this.left -= out.length; }
        if (out.length) this.writer.write(out).catch(() => room.drop(this, true));
        if ((this.chunk && this.chunk.done) || this.left === 0) { this.finish(); room.drop(this, true); }
      },
      response(r, wasWs) {
        const nullBody = isHead || r.status === 204 || r.status === 304 || r.status < 200;
        const h = new Headers(); for (const [k, v] of r.headers) h.append(k, v);
        if (nullBody || (r.length === 0 && !r.chunked)) { room.drop(this, true); return new Response(null, { status: r.status === 101 || r.status < 200 ? 502 : r.status, headers: h }); }
        const ts = new TransformStream(); this.writer = ts.writable.getWriter();
        if (r.chunked) this.chunk = new ChunkedDecoder(); else this.left = r.length;
        const first = concat(r.rest, this.pend); this.pend = new Uint8Array(0);
        if (first.length) this.body(first);
        if (this.eof) this.finish();   // the host closed before the Response existed
        const enc = h.has('content-encoding') ? 'manual' : 'automatic';
        return new Response(ts.readable, { status: r.status, headers: h, encodeBody: enc });
      },
      finish() { if (this.ended) return; if (!this.writer) { this.eof = true; return; } this.ended = true; this.writer.close().catch(() => {}); },
      end() { clearTimeout(this.timer); resolveHead(null); this.finish(); },
    };
    c.arm();
    return c;
  }

  // The host answered 101: accept the browser's WebSocket here and translate frames both ways.
  becomeWs(c, r) {
    const [client, server] = Object.values(new WebSocketPair());
    this.ctx.acceptWebSocket(server, ['l:' + c.id]);
    server.serializeAttachment({ kind: 'l', id: c.id, ip: c.ip });
    const w = this.wsConn(c.id, c.ip, server);
    clearTimeout(c.timer);
    const first = concat(r.rest, c.pend), closed = this.conns.get(c.id) !== c;   // the host may have closed it already
    this.conns.set(c.id, w);
    if (first.length) w.fromHost(first);
    if (closed) { w.gone = true; this.drop(w, false); }
    this.report();
    const headers = r.proto ? { 'sec-websocket-protocol': r.proto } : {};
    return new Response(null, { status: 101, webSocket: client, headers });
  }
  wsConn(id, ip, lws) {
    const room = this;
    const c = { id, ip, kind: 'ws', gone: false, lws, browserGone: false,
      fromHost(p) { this.dec.push(p); },
      end() { if (!this.browserGone) { try { this.lws.close(1001, 'closed'); } catch { /* gone */ } } },
    };
    c.dec = new WsDecoder((op, p) => {
      if (op === -1) return room.drop(c, true);
      try {
        if (op === 1) lws.send(td.decode(p));
        else if (op === 2) lws.send(p);
        else if (op === 9) room.sendData(c, wsClientFrame(10, p));
        else if (op === 8) {
          const code = p.length >= 2 ? (p[0] << 8) | p[1] : 1000, reason = p.length > 2 ? td.decode(p.subarray(2)).slice(0, 120) : '';
          room.sendData(c, wsClientFrame(8, p.subarray(0, 2)));
          c.browserGone = true;
          try { lws.close(code === 1005 || code === 1006 || code === 1015 ? 1000 : code, reason); } catch { /* gone */ }
          room.drop(c, true);
        }
      } catch { room.drop(c, true); }
    });
    return c;
  }
}
