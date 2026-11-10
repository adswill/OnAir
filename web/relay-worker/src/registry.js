// The one global Durable Object: which room holds which server id / address, the opt-in public list, ban lists,
// and the host-link attempt limits. It sees ids, names and addresses only, never listener traffic.
import { DurableObject } from 'cloudflare:workers';
import { Attempts } from './lib.js';

export class Registry extends DurableObject {
  constructor(ctx, env) {
    super(ctx, env);
    this.sql = ctx.storage.sql;
    this.sql.exec(`CREATE TABLE IF NOT EXISTS hosts (id TEXT PRIMARY KEY, label TEXT UNIQUE, room TEXT, name TEXT, url TEXT, listed INTEGER, since INTEGER, listeners INTEGER, seen INTEGER);
      CREATE TABLE IF NOT EXISTS bans (kind TEXT, val TEXT, PRIMARY KEY (kind, val));`);
    this.attempts = new Attempts(Number(env.HOST_ATTEMPTS_PER_MIN) || 6);
    this.maxHosts = Number(env.MAX_HOSTS) || 2000;
    for (const [kind, list] of [['id', env.BAN_IDS], ['ip', env.BAN_IPS]]) for (const v of String(list || '').split(',').filter(Boolean)) this.sql.exec('INSERT OR IGNORE INTO bans VALUES (?, ?)', kind, v.trim());
  }
  banned(kind, val) { return this.sql.exec('SELECT 1 FROM bans WHERE kind = ? AND val = ?', kind, val).toArray().length > 0; }

  // Before a host link is accepted: banned address or too many attempts from it.
  gate(ip) {
    if (this.banned('ip', ip)) return { ok: false, status: 403, message: 'banned' };
    if (this.attempts.tooMany('ip:' + ip, Date.now())) return { ok: false, status: 429, message: 'slow down' };
    return { ok: true };
  }

  // After a valid hello: take the address. One live link per id: the room that held it before is told to close.
  async claim(h) {
    if (this.banned('id', h.id)) return { ok: false, code: 'banned', message: 'this server is banned' };
    if (this.attempts.tooMany('id:' + h.id, Date.now())) return { ok: false, code: 'busy', message: 'too many connection attempts, try again soon' };
    const old = this.sql.exec('SELECT room FROM hosts WHERE id = ?', h.id).toArray()[0];
    if (!old) {
      this.sweep();
      const n = this.sql.exec('SELECT COUNT(*) AS n FROM hosts').one().n;
      if (n >= this.maxHosts) return { ok: false, code: 'busy', message: 'the relay is full' };
    }
    const now = Math.floor(Date.now() / 1000);
    this.sql.exec('DELETE FROM hosts WHERE label = ? AND id != ?', h.label, h.id);   // a 10-character label collision: the newest wins
    this.sql.exec('INSERT OR REPLACE INTO hosts VALUES (?, ?, ?, ?, ?, ?, ?, 0, ?)', h.id, h.label, h.room, h.name, h.url, h.listed ? 1 : 0, now, now);
    if (old && old.room !== h.room) this.evict(old.room, h.id);
    return { ok: true };
  }
  evict(room, id) { try { this.ctx.waitUntil(this.env.ROOM.get(this.env.ROOM.idFromString(room)).evict(id).catch(() => {})); } catch { /* bad id */ } }

  release(id, room) { this.sql.exec('DELETE FROM hosts WHERE id = ? AND room = ?', id, room); }
  touch(id, room, listeners) {
    const now = Math.floor(Date.now() / 1000);
    if (listeners == null) this.sql.exec('UPDATE hosts SET seen = ? WHERE id = ? AND room = ?', now, id, room);
    else this.sql.exec('UPDATE hosts SET seen = ?, listeners = ? WHERE id = ? AND room = ?', now, listeners, id, room);
  }
  // A room that has not reported for 2 hours (its object was reset without a close) is dropped.
  sweep() { this.sql.exec('DELETE FROM hosts WHERE seen < ?', Math.floor(Date.now() / 1000) - 7200); }

  // A browser request for an address: the room that holds it now, and the address bans (the Worker caches both briefly).
  lookup(label) { const r = this.sql.exec('SELECT room FROM hosts WHERE label = ?', label).toArray()[0]; return r ? r.room : null; }
  ipBans() { return this.sql.exec("SELECT val FROM bans WHERE kind = 'ip'").toArray().map((r) => r.val); }

  list() {
    this.sweep();
    return this.sql.exec('SELECT id, name, url, listeners, since FROM hosts WHERE listed = 1 ORDER BY listeners DESC, name ASC LIMIT 500').toArray();
  }
  stats() { return { hosts: this.sql.exec('SELECT COUNT(*) AS n FROM hosts').one().n }; }

  ban(kind, val, on) {
    if (kind !== 'id' && kind !== 'ip') return false;
    if (on) this.sql.exec('INSERT OR IGNORE INTO bans VALUES (?, ?)', kind, val); else this.sql.exec('DELETE FROM bans WHERE kind = ? AND val = ?', kind, val);
    if (on && kind === 'id') { const r = this.sql.exec('SELECT room FROM hosts WHERE id = ?', val).toArray()[0]; if (r) { this.evict(r.room, val); this.sql.exec('DELETE FROM hosts WHERE id = ?', val); } }
    return true;
  }
}
