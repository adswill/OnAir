-- OnAir plugin marketplace (D1). Apply once: wrangler d1 execute onair-plugins --remote --file=schema.sql
-- What is stored about people: the GitHub user id and login name of uploaders and reporters, salted hashes of install ids (likes, one
-- download count per install and day) and short-lived salted hashes of addresses (rate limits, deleted when their window is over).

CREATE TABLE IF NOT EXISTS plugins (
  id            TEXT PRIMARY KEY,              -- the manifest id (first come, first served; owned by the uploader)
  title         TEXT NOT NULL,
  description   TEXT NOT NULL,
  repo          TEXT NOT NULL DEFAULT '',      -- optional https:// link
  tags          TEXT NOT NULL DEFAULT ',',     -- ",sdr,ft8," so a tag filter is one LIKE
  kind          TEXT NOT NULL,
  platforms     TEXT NOT NULL,                 -- ",macos,linux,windows,"
  permissions   TEXT NOT NULL DEFAULT '',      -- "network,record"
  license       TEXT NOT NULL,
  author        TEXT NOT NULL,                 -- the manifest's author field
  version       TEXT NOT NULL,                 -- the current version
  owner_id      INTEGER NOT NULL,              -- GitHub user id
  owner_login   TEXT NOT NULL,                 -- GitHub login at the last upload
  likes         INTEGER NOT NULL DEFAULT 0,
  downloads     INTEGER NOT NULL DEFAULT 0,
  created_at    INTEGER NOT NULL,
  updated_at    INTEGER NOT NULL,
  status        TEXT NOT NULL DEFAULT 'live',  -- live | pulled
  pulled_reason TEXT NOT NULL DEFAULT ''
);
CREATE INDEX IF NOT EXISTS plugins_likes ON plugins(status, likes DESC);
CREATE INDEX IF NOT EXISTS plugins_downloads ON plugins(status, downloads DESC);
CREATE INDEX IF NOT EXISTS plugins_newest ON plugins(status, updated_at DESC);
CREATE INDEX IF NOT EXISTS plugins_owner ON plugins(owner_id);

CREATE TABLE IF NOT EXISTS versions (
  plugin_id   TEXT NOT NULL,
  version     TEXT NOT NULL,
  r2_key      TEXT NOT NULL,                   -- p/<id>/<version>/<sha256>.onairplugin
  size        INTEGER NOT NULL,
  sha256      TEXT NOT NULL,
  min_onair   TEXT NOT NULL DEFAULT '',
  platforms   TEXT NOT NULL,
  permissions TEXT NOT NULL DEFAULT '',
  created_at  INTEGER NOT NULL,
  PRIMARY KEY (plugin_id, version)
);

CREATE TABLE IF NOT EXISTS likes (
  plugin_id TEXT NOT NULL,
  voter     TEXT NOT NULL,                     -- salted hash of the install id
  at        INTEGER NOT NULL,
  PRIMARY KEY (plugin_id, voter)
);

CREATE TABLE IF NOT EXISTS dl_seen (           -- one download count per install (or address) per plugin per day
  plugin_id TEXT NOT NULL,
  voter     TEXT NOT NULL,
  day       INTEGER NOT NULL,
  PRIMARY KEY (plugin_id, voter, day)
);

CREATE TABLE IF NOT EXISTS reports (
  n              INTEGER PRIMARY KEY AUTOINCREMENT,
  plugin_id      TEXT NOT NULL,
  version        TEXT NOT NULL DEFAULT '',
  reporter_id    INTEGER NOT NULL,
  reporter_login TEXT NOT NULL,
  reason         TEXT NOT NULL,                -- malware | broken | licence | spam | other
  text           TEXT NOT NULL DEFAULT '',
  at             INTEGER NOT NULL,
  status         TEXT NOT NULL DEFAULT 'open'  -- open | closed
);
CREATE INDEX IF NOT EXISTS reports_open ON reports(status, n DESC);

CREATE TABLE IF NOT EXISTS bans (
  login     TEXT PRIMARY KEY,                  -- lower case
  github_id INTEGER NOT NULL DEFAULT 0,        -- 0 when the login never uploaded or reported
  reason    TEXT NOT NULL DEFAULT '',
  at        INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS rl (                -- rate-limit windows: k = "<what>:<salted hash>", deleted after exp
  k   TEXT PRIMARY KEY,
  win INTEGER NOT NULL,
  n   INTEGER NOT NULL,
  exp INTEGER NOT NULL
);
