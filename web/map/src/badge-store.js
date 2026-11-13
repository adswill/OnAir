// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// badge-store.js - the hub's SQLite tables for accounts and badges (Durable Object storage, created with CREATE TABLE IF NOT EXISTS like
// the other tables: no wrangler migration). The logic is in badge-stats.js / badge-service.js; the tests use their memory backends.
//
// bu_users   one row per GitHub account that signed in: uid (GitHub numeric id), login, settings, the stats as JSON
// bu_seen    the unique objects each user was credited for (uid, key), so an object counts once, ever
// bu_tokens  SHA-256 hashes of session tokens (website sessions, and app tokens seen once) with expiry and a revoked flag
// No IP address, home position or GitHub token is stored anywhere.

export class SqlBadgeBackend {
  constructor(sql) {
    this.sql = sql;
    sql.exec(`
      CREATE TABLE IF NOT EXISTS bu_users (
        uid INTEGER PRIMARY KEY,
        login TEXT NOT NULL,
        login_lc TEXT NOT NULL,
        public INTEGER NOT NULL DEFAULT 0,
        badges INTEGER NOT NULL DEFAULT 0,
        hidden INTEGER NOT NULL DEFAULT 0,
        by TEXT NOT NULL DEFAULT '',
        apps_before INTEGER NOT NULL DEFAULT 0,
        json TEXT NOT NULL,
        t INTEGER NOT NULL
      );
      CREATE INDEX IF NOT EXISTS idx_bu_users_login ON bu_users(login_lc);
      CREATE TABLE IF NOT EXISTS bu_seen (
        uid INTEGER NOT NULL,
        k TEXT NOT NULL,
        PRIMARY KEY (uid, k)
      ) WITHOUT ROWID;
      CREATE TABLE IF NOT EXISTS bu_tokens (
        h TEXT PRIMARY KEY,
        uid INTEGER NOT NULL,
        kind TEXT NOT NULL,
        exp INTEGER NOT NULL,
        revoked INTEGER NOT NULL DEFAULT 0
      );
    `);
  }
  row2user(r) {
    let s = null;
    try { s = JSON.parse(r.json); } catch { s = null; }
    return { uid: r.uid, login: r.login, public: !!r.public, badges: !!r.badges, hidden: !!r.hidden, by: r.by || "", appsBefore: r.apps_before || 0, s };
  }
  loadUser(uid) {
    const r = this.sql.exec("SELECT * FROM bu_users WHERE uid = ?", uid).toArray()[0];
    return r ? this.row2user(r) : null;
  }
  saveUser(u) {
    this.sql.exec(
      `INSERT INTO bu_users (uid, login, login_lc, public, badges, hidden, by, apps_before, json, t) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
       ON CONFLICT(uid) DO UPDATE SET login=excluded.login, login_lc=excluded.login_lc, public=excluded.public, badges=excluded.badges,
         hidden=excluded.hidden, by=excluded.by, apps_before=excluded.apps_before, json=excluded.json, t=excluded.t`,
      u.uid, u.login, u.login.toLowerCase(), u.public ? 1 : 0, u.badges ? 1 : 0, u.hidden ? 1 : 0, u.by || "", u.appsBefore || 0, JSON.stringify(u.s), Date.now(),
    );
  }
  deleteUser(uid) {
    this.sql.exec("DELETE FROM bu_users WHERE uid = ?", uid);
    this.sql.exec("DELETE FROM bu_seen WHERE uid = ?", uid);
    this.sql.exec("DELETE FROM bu_tokens WHERE uid = ?", uid);
  }
  loadSeen(uid) { return this.sql.exec("SELECT k FROM bu_seen WHERE uid = ?", uid).toArray().map((r) => r.k); }
  addSeen(uid, keys) { for (const k of keys) this.sql.exec("INSERT OR IGNORE INTO bu_seen (uid, k) VALUES (?, ?)", uid, k); }
  claims() { return this.sql.exec("SELECT by, uid FROM bu_users WHERE by != ''").toArray().map((r) => [r.by, r.uid]); }
  findLogin(lc) { const r = this.sql.exec("SELECT uid FROM bu_users WHERE login_lc = ?", lc).toArray()[0]; return r ? r.uid : 0; }
  boardRows() {
    return this.sql.exec("SELECT * FROM bu_users WHERE public = 1 AND badges = 1 AND hidden = 0").toArray().map((r) => this.row2user(r));
  }
  tokenGet(h) { return this.sql.exec("SELECT uid, kind, exp, revoked FROM bu_tokens WHERE h = ?", h).toArray()[0] || null; }
  tokenPut(h, uid, kind, exp) { this.sql.exec("INSERT OR IGNORE INTO bu_tokens (h, uid, kind, exp) VALUES (?, ?, ?, ?)", h, uid, kind, exp); }
  tokenRevoke(h) { this.sql.exec("UPDATE bu_tokens SET revoked = 1 WHERE h = ?", h); }
  tokenRevokeUser(uid, kind) { this.sql.exec("UPDATE bu_tokens SET revoked = 1 WHERE uid = ? AND kind = ?", uid, kind); }
  tokenCount(uid, kind, nowSec) { const r = this.sql.exec("SELECT COUNT(*) AS n FROM bu_tokens WHERE uid = ? AND kind = ? AND revoked = 0 AND exp >= ?", uid, kind, nowSec).toArray()[0]; return r ? r.n : 0; }
  tokenPrune(nowSec) { this.sql.exec("DELETE FROM bu_tokens WHERE exp < ?", nowSec - 86400); }
}
