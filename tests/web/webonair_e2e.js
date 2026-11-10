#!/usr/bin/env node
// OnAir Web end to end on this computer only: relay (relay/relay.js), directory (the real Worker under `wrangler dev --local`), onair-server with the test signal, guests over WebSocket
// and, when ONAIR_E2E_PLAYWRIGHT points at a playwright-core module, a real headless (muted) Chrome. No request leaves 127.0.0.1 / *.localhost.
//   node tests/web/webonair_e2e.js build/onair-server
//   ONAIR_E2E_RELAY=worker node tests/web/webonair_e2e.js build/onair-server     (the Cloudflare relay Worker under wrangler dev instead)
// Exit 77: wrangler is not installed (cd web/onair-directory && npm install), the test is skipped.
'use strict';
const { spawn, spawnSync } = require('child_process'), fs = require('fs'), net = require('net'), os = require('os'), path = require('path'), crypto = require('crypto');
const ROOT = path.resolve(__dirname, '..', '..');
const SERVER = process.argv[2] ? path.resolve(process.argv[2]) : path.join(ROOT, 'build', 'onair-server');
const WR = path.join(ROOT, 'web', 'onair-directory', 'node_modules', '.bin', 'wrangler');
if (!fs.existsSync(WR)) { console.log('SKIP: wrangler not installed in web/onair-directory (npm install)'); process.exit(77); }
if (!fs.existsSync(SERVER)) { console.log('SKIP: ' + SERVER + ' is not built'); process.exit(77); }
const { createRelay } = require(path.join(ROOT, 'relay', 'relay.js'));
// ONAIR_E2E_RELAY=worker: the relay is the Cloudflare Worker (web/relay-worker) under `wrangler dev --local` instead of relay/relay.js
const WORKER = process.env.ONAIR_E2E_RELAY === 'worker';
const RWDIR = path.join(ROOT, 'web', 'relay-worker'), RWR = path.join(RWDIR, 'node_modules', '.bin', 'wrangler');
if (WORKER && !fs.existsSync(RWR)) { console.log('SKIP: wrangler not installed in web/relay-worker (npm install)'); process.exit(77); }

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
let fails = 0;
const check = (ok, what) => { console.log((ok ? 'PASS: ' : 'FAIL: ') + what); if (!ok) fails++; };
const T = fs.mkdtempSync(path.join(os.tmpdir(), 'onair-e2e-'));

// ---- a small WebSocket client: Host and Origin are chosen, as a browser on the relay's address would send them
class Ws {
  constructor(port, host, origin) {
    this.q = []; this.buf = Buffer.alloc(0); this.up = false; this.closeCode = null; this.bins = { spectrum: 0, audio: 0 };
    this.sock = net.connect(port, '127.0.0.1');
    this.sock.on('connect', () => this.sock.write(`GET /web/v1/ws HTTP/1.1\r\nHost: ${host}\r\nOrigin: ${origin}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ${crypto.randomBytes(16).toString('base64')}\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Protocol: onair-web.v1\r\n\r\n`));
    this.sock.on('data', (d) => this.onData(d));
    this.sock.on('error', () => {}); this.sock.on('close', () => { if (this.closeCode === null) this.closeCode = -1; });
  }
  onData(d) {
    this.buf = Buffer.concat([this.buf, d]);
    if (!this.up) { const e = this.buf.indexOf('\r\n\r\n'); if (e < 0) return; this.head = this.buf.subarray(0, e).toString(); this.up = /^HTTP\/1\.1 101/.test(this.head); this.buf = this.buf.subarray(e + 4); if (!this.up) { this.closeCode = -2; return; } }
    for (;;) {
      const b = this.buf; if (b.length < 2) return;
      const op = b[0] & 15; let len = b[1] & 127, o = 2;
      if (len === 126) { if (b.length < 4) return; len = b.readUInt16BE(2); o = 4; } else if (len === 127) { if (b.length < 10) return; len = Number(b.readBigUInt64BE(2)); o = 10; }
      if (b.length < o + len) return;
      const p = b.subarray(o, o + len); this.buf = b.subarray(o + len);
      if (op === 8) { this.closeCode = p.length >= 2 ? p.readUInt16BE(0) : 1005; this.sock.destroy(); return; }
      if (op === 1) { try { this.q.push(JSON.parse(p.toString())); } catch { /* not JSON */ } }
      if (op === 2) { if (p[0] === 0x10) this.bins.spectrum++; else if (p[0] === 0x20) this.bins.audio++; }
      if (op === 9) this.frame(10, p);
    }
  }
  frame(op, data) { const m = crypto.randomBytes(4), c = Buffer.from(data); for (let i = 0; i < c.length; i++) c[i] ^= m[i & 3]; const n = c.length; const h = n < 126 ? Buffer.from([128 | op, 128 | n]) : Buffer.from([128 | op, 254, n >> 8, n & 255]); this.sock.write(Buffer.concat([h, m, c])); }
  send(o) { this.frame(1, Buffer.from(JSON.stringify(o))); }
  raw(t) { this.frame(1, Buffer.from(t)); }
  close() { this.sock.destroy(); }
  async wait(pred, ms = 6000) { const t = Date.now(); while (Date.now() - t < ms) { const f = this.q.find(pred); if (f) return f; if (this.closeCode !== null && !this.q.find(pred)) return null; await sleep(25); } return null; }
}

async function guest(relayPort, host, opts = {}) {
  const w = new Ws(relayPort, host, 'http://' + host);
  await sleep(100);
  w.send({ type: 'hello', proto: 1, name: opts.name || 'guest', ...(opts.password ? { password: opts.password } : {}), ...(opts.invite ? { invite: opts.invite } : {}) });
  const welcome = await w.wait((m) => m.type === 'welcome');
  return { w, welcome };
}

const conf = (ports, extra) => `device=test\nmode=fm\nfreq_mhz=100\nrate_msps=4\nweb=1\nweb_port=${ports.web}\nweb_name=E2E radio\nweb_max_users=8\nweb_max_per_ip=8\nweb_relay=1\nweb_relay_url=ws://127.0.0.1:${ports.relay}/relay/v1/host\nweb_directory_key=${T}/web_directory.key\nweb_admin_token=e2eadmintoken0123456789abcdef0\nport=${ports.api}\nautostart=1\n${extra || ''}`;
const dirConf = (ports, url, ack) => `web=1\nweb_port=${ports.web}\nweb_name=E2E radio\nweb_max_users=8\nweb_public_url=${url}\nweb_directory=1\nweb_directory_url=http://127.0.0.1:${ports.dir}\nweb_directory_key=${T}/web_directory.key\nweb_directory_loc=54.7,25.3\nweb_directory_loc_decimals=1\nweb_directory_freq=99-101\nweb_directory_radio=Test signal\nweb_directory_desc=local test\nweb_directory_reach=0\nweb_directory_ack=${ack}\n`;
const procs = [];
function startServer(ports, extra) {
  try { fs.unlinkSync(T + '/web_relay_status.json'); } catch { /* first start */ }
  fs.writeFileSync(T + '/server.conf', conf(ports, extra));
  const log = fs.openSync(T + '/server.log', 'a');
  const p = spawn(SERVER, ['--config', T + '/server.conf', '--web', '--web-port', '' + ports.web, '--port', '' + ports.api, '--device', 'test', '--autostart'], { stdio: ['ignore', log, log], env: { ...process.env, DECT2_DATA_DIR: T + '/data' } });
  procs.push(p); return p;
}
const relayStatus = () => { try { return JSON.parse(fs.readFileSync(T + '/web_relay_status.json', 'utf8')); } catch { return {}; } };
async function waitOnline(ms) { const t = Date.now(); while (Date.now() - t < ms) { const s = relayStatus(); if (s.state === 'online' && s.url) return s; await sleep(150); } return relayStatus(); }
async function httpJson(u, headers) { const r = await fetch(u, { headers: headers || {} }); return r.json(); }
async function stopServer(p) { p.kill('SIGTERM'); for (let i = 0; i < 60 && p.exitCode === null && p.signalCode === null; i++) await sleep(100); }

(async () => {
  const ports = { relay: await freePort(), web: await freePort(), api: await freePort(), dir: await freePort() };
  const relayOpt = { domain: 'relay.localhost', mainHost: '127.0.0.1', scheme: 'http', publicPort: '' + ports.relay };
  const startRelay = async () => {
    if (!WORKER) { const r = createRelay(relayOpt); await r.listen(ports.relay, '127.0.0.1'); return r; }
    // the Worker's own config without routes (with a route, wrangler dev replaces the Host header), state kept across restarts
    const cfg = path.join(RWDIR, '.wrangler-test.jsonc');
    fs.writeFileSync(cfg, fs.readFileSync(path.join(RWDIR, 'wrangler.jsonc'), 'utf8').replace(/ {2}"routes": \[[\s\S]*?\],\n/, ''));
    const lg = fs.openSync(T + '/relay-worker.log', 'a');
    const p = spawn(RWR, ['dev', '-c', cfg, '--local', '--persist-to', T + '/rw', '--port', '' + ports.relay, '--ip', '127.0.0.1', '--show-interactive-dev-session=false',
      '--var', 'RELAY_DEV_LOCAL:1', '--var', 'RELAY_MAIN_HOST:127.0.0.1', '--var', 'RELAY_LABEL_SUFFIX:.relay.localhost', '--var', 'RELAY_SCHEME:http',
      '--var', 'RELAY_PUBLIC_PORT:' + ports.relay, '--var', 'HOST_ATTEMPTS_PER_MIN:30'],
    { cwd: RWDIR, env: { ...process.env, CI: '1', WRANGLER_SEND_METRICS: 'false' }, stdio: ['ignore', lg, lg], detached: true });
    let ok = false;
    for (let i = 0; i < 240 && !ok; i++) { await sleep(250); try { ok = (await fetch(`http://127.0.0.1:${ports.relay}/relay/v1/status`)).ok; } catch { /* not yet */ } }
    check(ok, 'wrangler dev serves the relay Worker');
    return { close: () => { try { process.kill(-p.pid, 'SIGTERM'); } catch { try { p.kill('SIGTERM'); } catch { /* gone */ } } try { fs.unlinkSync(cfg); } catch { /* gone */ } } };
  };
  let relay = await startRelay();
  // ---- the directory: the real Worker, local D1
  const persist = T + '/wr';
  const dirEnv = { ...process.env, CI: '1', WRANGLER_SEND_METRICS: 'false' };
  const dx = spawnSync(WR, ['d1', 'execute', 'onair-directory', '--local', '--persist-to', persist, '--file', 'schema.sql', '--yes'], { cwd: path.join(ROOT, 'web', 'onair-directory'), env: dirEnv, encoding: 'utf8', timeout: 120000 });
  check(dx.status === 0, 'directory schema applied to the local D1' + (dx.status ? ': ' + (dx.stderr || dx.stdout).slice(-300) : ''));
  const dirLog = fs.openSync(T + '/dir.log', 'a');
  const dir = spawn(WR, ['dev', '--local', '--persist-to', persist, '--port', '' + ports.dir, '--ip', '127.0.0.1', '--var', 'ONAIR_DEV_LOCAL:1', '--show-interactive-dev-session=false'], { cwd: path.join(ROOT, 'web', 'onair-directory'), env: dirEnv, stdio: ['ignore', dirLog, dirLog], detached: true });
  let up = false;
  for (let i = 0; i < 200 && !up; i++) { await sleep(250); try { const r = await fetch(`http://127.0.0.1:${ports.dir}/api/v1/health`); up = r.ok; } catch { /* not yet */ } }
  check(up, 'wrangler dev serves the directory');
  try {
    let sv = startServer(ports);
    let s = await waitOnline(20000);
    check(s.state === 'online' && !!s.url, 'server is online on the relay: ' + (s.url || s.message));
    const url = s.url, host = new URL(url).host;
    // ---- listing: preview, ack, heartbeat, appears in the public list
    fs.writeFileSync(T + '/dir.conf', dirConf(ports, url, ''));
    const pv = spawnSync(SERVER, ['--config', T + '/dir.conf', '--directory-preview'], { encoding: 'utf8', timeout: 30000, env: { ...process.env, DECT2_DATA_DIR: T + '/data' } });
    const ack = /^ack=(\S+)/.exec(pv.stdout || '');
    check(!!ack, 'directory preview gives a confirmation code');
    fs.writeFileSync(T + '/dir.conf', dirConf(ports, url, ack ? ack[1] : ''));
    const dirRun = spawn(SERVER, ['--config', T + '/dir.conf', '--directory-run'], { stdio: 'ignore', env: { ...process.env, DECT2_DATA_DIR: T + '/data' } }); procs.push(dirRun);
    let listed = null;
    for (let i = 0; i < 60 && !listed; i++) { await sleep(500); try { const j = await httpJson(`http://127.0.0.1:${ports.dir}/api/v1/servers`); listed = (j.servers || []).find((x) => x.url === url); } catch { /* retry */ } }
    check(!!listed, 'the listing appears in the directory with the relay address');
    check(!!listed && listed.location === '54.7,25.3' && listed.lat === 54.7, 'the listed location is the coarse one that was sent');
    // ---- a stranger comes in through the listing's address (the relay), two at once
    const a = await guest(ports.relay, host, { name: 'stranger A' }), b = await guest(ports.relay, host, { name: 'stranger B' });
    check(!!a.welcome && !!b.welcome, 'two guests through the relay get a welcome');
    for (const g of [a, b]) { g.w.send({ type: 'tune', freq_hz: 100500000, mode: 'nfm' }); g.w.send({ type: 'audio', on: true, codec: 'adpcm', rate: 12000 }); g.w.send({ type: 'spectrum', on: true, bins: 512, fps: 10 }); }
    await sleep(2500);
    check(a.w.bins.spectrum > 3 && b.w.bins.spectrum > 3, `both guests get waterfall rows (${a.w.bins.spectrum}, ${b.w.bins.spectrum})`);
    check(a.w.bins.audio > 3 && b.w.bins.audio > 3, `both guests get audio frames (${a.w.bins.audio}, ${b.w.bins.audio})`);
    a.w.send({ type: 'tune', freq_hz: 100700000, mode: 'am' });
    const stA = await a.w.wait((m) => m.type === 'state' && m.mode === 'am' && Math.round(m.freq_hz) === 100700000);
    check(!!stA, 'mode and frequency change apply to guest A');
    check(!(b.w.q.find((m) => m.type === 'state' && m.mode === 'am')), 'guest B is not affected by guest A (own receiver)');
    // ---- safety: what guests can not do
    for (const t of ['tx', 'transmit', 'ptt', 'arm', 'config', 'admin', 'import_key', 'record', 'save', 'upload', 'exec', 'shell']) a.w.send({ type: t, on: true });
    a.w.raw('{not json'); a.w.send({ type: 'tune', freq_hz: 1e15 }); a.w.send({ type: 'tune', freq_hz: 'x' });
    await sleep(800);
    const errs = a.w.q.filter((m) => m.type === 'error');
    check(errs.filter((e) => e.code === 'forbidden').length === 12, 'transmit, keys, files, settings are refused (' + errs.length + ' errors)');
    check(errs.some((e) => e.code === 'bad_json') && errs.some((e) => e.code === 'out_of_range'), 'bad messages are refused');
    a.w.close(); b.w.close();
    // ---- web paths and traversal
    const info = JSON.parse(await rawBody(ports.relay, host, '/web/v1/info'));
    check(info.proto === 1 && info.name === 'E2E radio', '/web/v1/info through the relay');
    for (const p of ['/../../../../etc/passwd', '/%2e%2e/%2e%2e/etc/passwd', '/web/v1/admin/stats']) { const r = await rawGet(ports.relay, host, p); check(r === 404 || r === 403 || r === 401, 'refused: ' + p + ' (' + r + ')'); }
    // ---- rate limit
    const c = await guest(ports.relay, host, { name: 'flood' });
    for (let i = 0; i < 120; i++) c.w.send({ type: 'ping', t: i });
    const rl = await c.w.wait((m) => m.type === 'error' && m.code === 'rate_limited', 3000);
    check(!!rl || c.w.closeCode === 4008, 'a flood is rate limited');
    c.w.close();
    // ---- the network drops: relay gone and back; the server reconnects with the same address; the listing still points there
    const g1 = await guest(ports.relay, host, { name: 'before drop' });
    check(!!g1.welcome, 'guest connected before the drop');
    relay.close(); await sleep(800);
    check(g1.w.closeCode !== null, 'the guest is cut off when the relay goes away');
    if (WORKER) await sleep(1500);   // the old wrangler gives the port back
    relay = await startRelay();
    s = await waitOnline(70000);
    check(s.state === 'online' && s.url === url, 'the server reconnects to the relay by itself, same address');
    const g2 = await guest(ports.relay, host, { name: 'after drop' });
    check(!!g2.welcome, 'a guest connects again after the reconnect'); g2.w.close();
    // ---- wrong password, right password
    await stopServer(sv);
    sv = startServer(ports, 'web_auth=password\nweb_password_hash=' + makeHash('correct-horse-9') + '\n');
    s = await waitOnline(20000);
    const bad = await guest(ports.relay, host, { name: 'bad', password: 'not-the-password' });
    check(!bad.welcome && bad.w.closeCode === 4001, 'wrong password: refused with 4001');
    const good = await guest(ports.relay, host, { name: 'good', password: 'correct-horse-9' });
    check(!!good.welcome && good.welcome.class === 'member', 'right password: welcome as member');
    // ---- the control slot
    await stopServer(sv);
    sv = startServer(ports, 'web_control=slot\nweb_control_slot_min=1\n');
    s = await waitOnline(20000);
    const p = await guest(ports.relay, host, { name: 'p' }), q = await guest(ports.relay, host, { name: 'q' });
    check(!!p.welcome && p.welcome.control && p.welcome.control.mode === 'slot' && !p.welcome.control.can_tune, 'control slot: guests start without tuning rights');
    p.w.send({ type: 'tune', freq_hz: 100500000, mode: 'am' });
    check(!!(await p.w.wait((m) => m.type === 'error' && m.code === 'not_allowed', 2000)), 'control slot: tune without the slot is refused');
    p.w.send({ type: 'control', take: true });
    check(!!(await p.w.wait((m) => m.type === 'control' && m.holder === true)), 'control slot: the first guest takes it');
    await sleep(300);
    q.w.send({ type: 'control', take: true });
    check(!!(await q.w.wait((m) => m.type === 'error' && m.code === 'busy', 2000)), 'control slot: the second guest is told it is busy');
    p.w.send({ type: 'tune', freq_hz: 100500000, mode: 'am' });
    check(!!(await p.w.wait((m) => m.type === 'state' && m.mode === 'am', 2000)), 'control slot: the holder can tune');
    p.w.send({ type: 'control', take: false });
    check(!!(await q.w.wait((m) => m.type === 'control' && m.busy === false && m.can_tune === false, 3000)), 'control slot: released and free again');
    p.w.close(); q.w.close();
    await stopServer(sv);
    // ---- a real browser, when playwright-core is available (ONAIR_E2E_PLAYWRIGHT=/path/to/node_modules/playwright-core)
    if (process.env.ONAIR_E2E_PLAYWRIGHT) {
      sv = startServer(ports, 'web_auth=password\nweb_password_hash=' + makeHash('correct-horse-9') + '\nweb_root=' + path.join(ROOT, 'web', 'onair-web') + '\n');
      s = await waitOnline(20000);
      const { chromium } = require(process.env.ONAIR_E2E_PLAYWRIGHT);
      const browser = await chromium.launch({ channel: 'chrome', headless: true, args: ['--mute-audio', '--autoplay-policy=no-user-gesture-required'] });
      const page = await (await browser.newContext()).newPage();
      const errors = []; page.on('pageerror', (e) => errors.push(e.message));
      await page.goto(url, { waitUntil: 'load', timeout: 20000 });
      await page.waitForSelector('#loginPw', { state: 'visible', timeout: 15000 });
      await page.fill('#loginPw', 'wrong-pw-here'); await page.click('#loginGo'); await sleep(1200);
      const again = await page.evaluate(() => !document.getElementById('login').hidden);
      check(again, 'browser: wrong password brings the login back');
      await page.fill('#loginPw', 'correct-horse-9'); await page.click('#loginGo');
      { // the page's CSP has no unsafe-eval, which page.waitForFunction needs: poll with page.evaluate
        const t0 = Date.now(); let shown = false;
        while (!shown && Date.now() - t0 < 15000) { shown = await page.evaluate(() => !!document.getElementById('app') && !document.getElementById('app').hidden); if (!shown) await sleep(150); }
        check(shown, 'browser: the right password opens the receiver');
      }
      const ok = await page.evaluate(() => { const n = document.getElementById('noticeOk'); if (n && n.offsetParent) n.click(); return document.getElementById('connText').textContent; });
      await sleep(2500);
      const colours = await page.evaluate(() => { const c = document.getElementById('wf'), x = c.getContext('2d'), d = x.getImageData(0, 0, Math.min(c.width, 400), Math.min(c.height, 200)).data; const s = new Set(); for (let i = 0; i < d.length; i += 388) s.add(d[i] + ',' + d[i + 1]); return s.size; });
      check(colours > 4, 'browser: the waterfall draws (' + colours + ' colours, connection ' + ok + ')');
      await page.click('#audioBtn'); await sleep(1500);
      check((await page.evaluate(() => document.getElementById('audioBtn').getAttribute('aria-pressed'))) === 'true', 'browser: audio starts');
      check(errors.length === 0, 'browser: no page errors ' + errors.join(' | ').slice(0, 200));
      await browser.close();
      await stopServer(sv);
    } else console.log('(set ONAIR_E2E_PLAYWRIGHT to also drive a real headless Chrome)');
  } finally {
    for (const p of procs) { try { p.kill('SIGTERM'); } catch { /* gone */ } }
    relay.close();
    try { process.kill(-dir.pid, 'SIGTERM'); } catch { try { dir.kill('SIGTERM'); } catch { /* gone */ } }
    try { fs.rmSync(T, { recursive: true, force: true }); } catch { /* best effort */ }
  }
  console.log(fails ? `${fails} FAILED` : 'ALL PASSED');
  await sleep(200);
  process.exit(fails ? 1 : 0);
})().catch((e) => { console.error(e); process.exit(2); });

function rawBody(port, host, p) { return new Promise((res) => { const s = net.connect(port, '127.0.0.1', () => s.write(`GET ${p} HTTP/1.1\r\nHost: ${host}\r\nConnection: close\r\n\r\n`)); let d = ''; s.on('data', (b) => { d += b.toString(); }); s.on('end', () => res(d.slice(d.indexOf('\r\n\r\n') + 4))); s.on('error', () => res('{}')); }); }
function rawGet(port, host, p) {
  return new Promise((res) => { const s = net.connect(port, '127.0.0.1', () => s.write(`GET ${p} HTTP/1.1\r\nHost: ${host}\r\nConnection: close\r\n\r\n`)); let d = ''; s.on('data', (b) => { d += b.toString(); }); s.on('end', () => res(parseInt(d.split(' ')[1], 10) || 0)); s.on('error', () => res(0)); s.setTimeout(5000, () => { s.destroy(); res(0); }); });
}
// the same PBKDF2 the server uses (web_policy.cpp: hashPassword)
function makeHash(pw) { const salt = crypto.randomBytes(16); const iters = 60000; return `pbkdf2$${iters}$${salt.toString('hex')}$${crypto.pbkdf2Sync(pw, salt, iters, 32, 'sha256').toString('hex')}`; }
