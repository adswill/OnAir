#include "dect2/session/session_db.h"
#include "dect2/msgdb/msgdb.h"
#include "dect2/session/privacy.h"
#include <sqlite3.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <mutex>

namespace dect2::sess {

namespace fs = std::filesystem;

namespace {

fs::path pathOf(const std::string& s) { return fs::path(reinterpret_cast<const char8_t*>(s.c_str())); }

int64_t nowMsReal() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }

struct Bv {
    bool isInt;
    int64_t i;
    std::string s;
};

const char* kCols = "s.id,s.mode,s.receiver,s.kind,s.key,s.title,s.who0,s.who1,s.alias0,s.alias1,s.start,s.end,s.dur,s.level,s.peak,s.snr,s.err,s.freq,s.carrier,s.slot,s.lat,s.lon,s.alt,s.verdict,s.flags,s.events";

std::string col(sqlite3_stmt* st, int i) {
    const unsigned char* t = sqlite3_column_text(st, i);
    return t ? std::string(reinterpret_cast<const char*>(t), (size_t)sqlite3_column_bytes(st, i)) : std::string();
}

Session decode(sqlite3_stmt* st) {
    Session s;
    s.id = (uint64_t)sqlite3_column_int64(st, 0);
    s.mode = (uint16_t)sqlite3_column_int(st, 1);
    s.receiver = (uint16_t)sqlite3_column_int(st, 2);
    s.kind = (uint8_t)sqlite3_column_int(st, 3);
    s.key = col(st, 4);
    s.title = col(st, 5);
    s.who[0] = col(st, 6); s.who[1] = col(st, 7);
    s.alias[0] = col(st, 8); s.alias[1] = col(st, 9);
    s.startWall = sqlite3_column_int64(st, 10);
    s.endWall = sqlite3_column_int64(st, 11);
    s.durationMs = (uint32_t)sqlite3_column_int64(st, 12);
    s.levelDb = (float)sqlite3_column_double(st, 13);
    s.levelPeakDb = (float)sqlite3_column_double(st, 14);
    s.snrDb = (float)sqlite3_column_double(st, 15);
    s.errRate = (float)sqlite3_column_double(st, 16);
    s.freqHz = (uint64_t)sqlite3_column_int64(st, 17);
    s.carrierId = sqlite3_column_int(st, 18);
    s.slot = (int16_t)sqlite3_column_int(st, 19);
    if (sqlite3_column_type(st, 20) != SQLITE_NULL) s.lat = sqlite3_column_double(st, 20); else s.lat = kNaN;
    if (sqlite3_column_type(st, 21) != SQLITE_NULL) s.lon = sqlite3_column_double(st, 21); else s.lon = kNaN;
    if (sqlite3_column_type(st, 22) != SQLITE_NULL) s.altM = (float)sqlite3_column_double(st, 22); else s.altM = (float)kNaN;
    s.verdict = (uint8_t)sqlite3_column_int(st, 23);
    s.flags = (uint16_t)sqlite3_column_int(st, 24);
    s.eventCount = (uint32_t)sqlite3_column_int64(st, 25);
    return s;
}

void bindAll(sqlite3_stmt* st, const std::vector<Bv>& b) {
    for (size_t i = 0; i < b.size(); i++) {
        if (b[i].isInt) sqlite3_bind_int64(st, (int)i + 1, b[i].i);
        else sqlite3_bind_text(st, (int)i + 1, b[i].s.c_str(), (int)b[i].s.size(), SQLITE_TRANSIENT);
    }
}

// the FTS5 query for user text: every word quoted, a trailing * kept as a prefix match, words ANDed
std::string ftsQuery(const std::string& in) {
    std::string out;
    for (const msgdb::Token& t : msgdb::tokenize(in)) {
        if (!out.empty()) out += ' ';
        out += '"' + t.word + '"';
        if (t.prefix) out += '*';
    }
    return out;
}

const char* kSchema =
    "CREATE TABLE IF NOT EXISTS sessions("
    " id INTEGER PRIMARY KEY, mode INTEGER NOT NULL, receiver INTEGER NOT NULL DEFAULT 0, kind INTEGER NOT NULL DEFAULT 1,"
    " key TEXT NOT NULL DEFAULT '', title TEXT NOT NULL DEFAULT '', who0 TEXT NOT NULL DEFAULT '', who1 TEXT NOT NULL DEFAULT '',"
    " alias0 TEXT NOT NULL DEFAULT '', alias1 TEXT NOT NULL DEFAULT '', start INTEGER NOT NULL, end INTEGER NOT NULL DEFAULT 0,"
    " dur INTEGER NOT NULL DEFAULT 0, level REAL NOT NULL DEFAULT 0, peak REAL NOT NULL DEFAULT 0, snr REAL NOT NULL DEFAULT 0,"
    " err REAL NOT NULL DEFAULT -1, freq INTEGER NOT NULL DEFAULT 0, carrier INTEGER NOT NULL DEFAULT -1, slot INTEGER NOT NULL DEFAULT -1,"
    " lat REAL, lon REAL, alt REAL, verdict INTEGER NOT NULL DEFAULT 0, flags INTEGER NOT NULL DEFAULT 0, events INTEGER NOT NULL DEFAULT 0,"
    " upd INTEGER NOT NULL DEFAULT 0);"
    "CREATE INDEX IF NOT EXISTS sessions_mode_start ON sessions(mode, start);"
    "CREATE INDEX IF NOT EXISTS sessions_mode_rx_start ON sessions(mode, receiver, start);"
    "CREATE INDEX IF NOT EXISTS sessions_mode_key ON sessions(mode, key);"
    "CREATE INDEX IF NOT EXISTS sessions_start ON sessions(start);"
    "CREATE INDEX IF NOT EXISTS sessions_active ON sessions(mode, start) WHERE end = 0;"
    "CREATE TABLE IF NOT EXISTS links(id INTEGER PRIMARY KEY, session_id INTEGER NOT NULL REFERENCES sessions(id) ON DELETE CASCADE,"
    " type INTEGER NOT NULL, ref TEXT NOT NULL, wall INTEGER NOT NULL DEFAULT 0);"
    "CREATE INDEX IF NOT EXISTS links_session ON links(session_id);"
    "CREATE INDEX IF NOT EXISTS links_type_wall ON links(type, wall);"
    "CREATE INDEX IF NOT EXISTS links_ref ON links(ref);"
    "CREATE TABLE IF NOT EXISTS notes(session_id INTEGER PRIMARY KEY REFERENCES sessions(id) ON DELETE CASCADE, note TEXT NOT NULL DEFAULT '', tags TEXT NOT NULL DEFAULT '');"
    "CREATE TABLE IF NOT EXISTS meta(k TEXT PRIMARY KEY, v TEXT NOT NULL);"
    "CREATE VIRTUAL TABLE IF NOT EXISTS sessions_fts USING fts5(title, who0, who1, alias0, alias1, key, content='sessions', content_rowid='id', tokenize='unicode61 remove_diacritics 2');"
    "CREATE TRIGGER IF NOT EXISTS sessions_ai AFTER INSERT ON sessions BEGIN"
    " INSERT INTO sessions_fts(rowid,title,who0,who1,alias0,alias1,key) VALUES(new.id,new.title,new.who0,new.who1,new.alias0,new.alias1,new.key); END;"
    "CREATE TRIGGER IF NOT EXISTS sessions_ad AFTER DELETE ON sessions BEGIN"
    " INSERT INTO sessions_fts(sessions_fts,rowid,title,who0,who1,alias0,alias1,key) VALUES('delete',old.id,old.title,old.who0,old.who1,old.alias0,old.alias1,old.key); END;"
    "CREATE TRIGGER IF NOT EXISTS sessions_au AFTER UPDATE ON sessions"
    " WHEN old.title IS NOT new.title OR old.who0 IS NOT new.who0 OR old.who1 IS NOT new.who1 OR old.alias0 IS NOT new.alias0 OR old.alias1 IS NOT new.alias1 OR old.key IS NOT new.key BEGIN"
    " INSERT INTO sessions_fts(sessions_fts,rowid,title,who0,who1,alias0,alias1,key) VALUES('delete',old.id,old.title,old.who0,old.who1,old.alias0,old.alias1,old.key);"
    " INSERT INTO sessions_fts(rowid,title,who0,who1,alias0,alias1,key) VALUES(new.id,new.title,new.who0,new.who1,new.alias0,new.alias1,new.key); END;";

const char* kUpsert =
    "INSERT INTO sessions(id,mode,receiver,kind,key,title,who0,who1,alias0,alias1,start,end,dur,level,peak,snr,err,freq,carrier,slot,lat,lon,alt,verdict,flags,events,upd)"
    " VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,?18,?19,?20,?21,?22,?23,?24,?25,?26,?27)"
    " ON CONFLICT(id) DO UPDATE SET mode=excluded.mode,receiver=excluded.receiver,kind=excluded.kind,key=excluded.key,title=excluded.title,"
    " who0=excluded.who0,who1=excluded.who1,alias0=excluded.alias0,alias1=excluded.alias1,start=excluded.start,end=excluded.end,dur=excluded.dur,"
    " level=excluded.level,peak=excluded.peak,snr=excluded.snr,err=excluded.err,freq=excluded.freq,carrier=excluded.carrier,slot=excluded.slot,"
    " lat=excluded.lat,lon=excluded.lon,alt=excluded.alt,verdict=excluded.verdict,"
    " flags=(excluded.flags & ~16) | (sessions.flags & 16)," // the star is the user's: an adapter's snapshot never clears it
    " events=excluded.events,upd=excluded.upd";

}  // namespace

struct SessionDb::Impl {
    mutable std::mutex wmu, rmu;
    sqlite3* w = nullptr;
    sqlite3* r = nullptr;
    bool memory = false;
    bool tx = false;
    std::string path;
    sqlite3_stmt* upsertSt = nullptr;
    sqlite3_stmt* linkSt = nullptr;

    // the connection and lock for reading: its own, or the writer's for a :memory: file
    sqlite3* rdb() const { return memory ? w : r; }
    std::mutex& rlock() const { return memory ? wmu : rmu; }

    bool exec(sqlite3* db, const char* sql) { return sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK; }

    int64_t scalar(sqlite3* db, const std::string& sql, const std::vector<Bv>& b = {}) const {
        sqlite3_stmt* st = nullptr;
        int64_t v = 0;
        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) == SQLITE_OK) {
            bindAll(st, b);
            if (sqlite3_step(st) == SQLITE_ROW) v = sqlite3_column_int64(st, 0);
        }
        sqlite3_finalize(st);
        return v;
    }

    void finalizeStatements() {
        if (upsertSt) { sqlite3_finalize(upsertSt); upsertSt = nullptr; }
        if (linkSt) { sqlite3_finalize(linkSt); linkSt = nullptr; }
    }

    void closeAll() {
        finalizeStatements();
        if (r) { sqlite3_close(r); r = nullptr; }
        if (w) { sqlite3_close(w); w = nullptr; }
        tx = false;
    }

    // FROM + WHERE of a query
    void where(const Query& q, std::string& w, std::vector<Bv>& b) const {
        auto andw = [&](const std::string& c) { w += w.empty() ? " WHERE " : " AND "; w += c; };
        if (q.mode >= 0) { andw("s.mode = ?"); b.push_back({true, q.mode, ""}); }
        if (q.receiver >= 0) { andw("s.receiver = ?"); b.push_back({true, q.receiver, ""}); }
        if (q.kind >= 0) { andw("s.kind = ?"); b.push_back({true, q.kind, ""}); }
        if (q.verdict >= 0) { andw("s.verdict = ?"); b.push_back({true, q.verdict, ""}); }
        if (q.activeOnly) andw("s.end = 0");
        if (q.starredOnly) andw("(s.flags & 16) <> 0");
        if (q.withAudio) andw("EXISTS (SELECT 1 FROM links l WHERE l.session_id = s.id AND l.type = 1)");
        if (q.since > 0) { andw("s.start >= ?"); b.push_back({true, q.since, ""}); }
        if (q.until > 0) { andw("s.start <= ?"); b.push_back({true, q.until, ""}); }
        const std::string fq = ftsQuery(q.text);
        if (!fq.empty()) { andw("s.id IN (SELECT rowid FROM sessions_fts WHERE sessions_fts MATCH ?)"); b.push_back({false, 0, fq}); }
    }

    std::vector<Session> select(sqlite3* db, const std::string& w, std::vector<Bv> b, const std::string& order, size_t limit, size_t offset) const {
        std::vector<Session> out;
        const std::string sql = std::string("SELECT ") + kCols + " FROM sessions s" + w + " ORDER BY " + order + " LIMIT " + std::to_string(limit) + " OFFSET " + std::to_string(offset);
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) return out;
        bindAll(st, b);
        out.reserve(std::min<size_t>(limit, 1000));
        while (sqlite3_step(st) == SQLITE_ROW) out.push_back(decode(st));
        sqlite3_finalize(st);
        return out;
    }

    bool openFile(const std::string& p, std::string* err, bool* created, bool ownerOnly) {
        const bool existed = p == ":memory:" ? false : fs::exists(pathOf(p));
        *created = !existed;
        memory = p == ":memory:";
        if (!memory && ownerOnly) {
            const fs::path parent = pathOf(p).parent_path();
            if (!parent.empty()) { const auto u8 = parent.u8string(); makeOwnerOnlyDir(std::string(reinterpret_cast<const char*>(u8.c_str()))); }
        }
        int rc = sqlite3_open_v2(p.c_str(), &w, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
        if (rc != SQLITE_OK) {
            if (err) *err = w ? sqlite3_errmsg(w) : "out of memory";
            if (w) { sqlite3_close(w); w = nullptr; }
            return false;
        }
        sqlite3_busy_timeout(w, 3000);
        exec(w, "PRAGMA auto_vacuum=INCREMENTAL");   // only takes effect on a new file
        if (!memory) { exec(w, "PRAGMA journal_mode=WAL"); exec(w, "PRAGMA synchronous=NORMAL"); }
        exec(w, "PRAGMA foreign_keys=ON");
        char* em = nullptr;
        rc = sqlite3_exec(w, kSchema, nullptr, nullptr, &em);
        if (rc != SQLITE_OK) {
            if (err) *err = em ? em : "schema";
            sqlite3_free(em);
            sqlite3_close(w);
            w = nullptr;
            return false;
        }
        // a first read of an index: a file whose pages are badly damaged fails here
        if (sqlite3_exec(w, "SELECT id FROM sessions ORDER BY start DESC LIMIT 1", nullptr, nullptr, nullptr) != SQLITE_OK) {
            if (err) *err = sqlite3_errmsg(w);
            sqlite3_close(w);
            w = nullptr;
            return false;
        }
        if (!memory) {
            if (ownerOnly) {
                restrictToOwner(p, false);
                restrictToOwner(p + "-wal", false);
                restrictToOwner(p + "-shm", false);
            }
            if (sqlite3_open_v2(p.c_str(), &r, SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK) {
                if (err) *err = r ? sqlite3_errmsg(r) : "out of memory";
                if (r) { sqlite3_close(r); r = nullptr; }
                sqlite3_close(w);
                w = nullptr;
                return false;
            }
            sqlite3_busy_timeout(r, 3000);
            exec(r, "PRAGMA query_only=ON");
        }
        path = p;
        return true;
    }
};

SessionDb::SessionDb() : p_(new Impl) {}
SessionDb::~SessionDb() { close(); }

bool SessionDb::open(const std::string& path, std::string* err, OpenInfo* info, bool ownerOnly) {
    close();
    OpenInfo local;
    OpenInfo& oi = info ? *info : local;
    oi = OpenInfo();
    std::lock_guard<std::mutex> lk(p_->wmu);
    std::lock_guard<std::mutex> lk2(p_->rmu);
    bool created = false;
    if (p_->openFile(path, err, &created, ownerOnly)) { oi.created = created; return true; }
    p_->closeAll();
    if (path == ":memory:") return false;
    // damaged beyond repair: keep the old file for the user, make a new one
    std::error_code ec;
    if (!fs::exists(pathOf(path), ec)) return false;
    const std::string moved = path + ".corrupt-" + std::to_string(nowMsReal());
    fs::rename(pathOf(path), pathOf(moved), ec);
    if (ec) return false;
    fs::remove(pathOf(path + "-wal"), ec);
    fs::remove(pathOf(path + "-shm"), ec);
    if (!p_->openFile(path, err, &created, ownerOnly)) { p_->closeAll(); return false; }
    oi.created = true;
    oi.quarantined = true;
    oi.quarantinedTo = moved;
    return true;
}

void SessionDb::close() {
    std::lock_guard<std::mutex> lk(p_->wmu);
    std::lock_guard<std::mutex> lk2(p_->rmu);
    if (p_->w && p_->tx) { sqlite3_exec(p_->w, "COMMIT", nullptr, nullptr, nullptr); }
    p_->closeAll();
    p_->path.clear();
}

bool SessionDb::isOpen() const { std::lock_guard<std::mutex> lk(p_->wmu); return p_->w != nullptr; }
std::string SessionDb::path() const { std::lock_guard<std::mutex> lk(p_->wmu); return p_->path; }

// ------------------------------------------------------------------ writing

bool SessionDb::beginTx() {
    std::lock_guard<std::mutex> lk(p_->wmu);
    if (!p_->w || p_->tx) return false;
    p_->tx = sqlite3_exec(p_->w, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) == SQLITE_OK;
    return p_->tx;
}

bool SessionDb::commitTx() {
    std::lock_guard<std::mutex> lk(p_->wmu);
    if (!p_->w || !p_->tx) return false;
    p_->tx = false;
    return sqlite3_exec(p_->w, "COMMIT", nullptr, nullptr, nullptr) == SQLITE_OK;
}

bool SessionDb::inTx() const { std::lock_guard<std::mutex> lk(p_->wmu); return p_->tx; }

bool SessionDb::upsert(const Session& s, int64_t updMs) {
    std::lock_guard<std::mutex> lk(p_->wmu);
    if (!p_->w) return false;
    if (!p_->upsertSt && sqlite3_prepare_v2(p_->w, kUpsert, -1, &p_->upsertSt, nullptr) != SQLITE_OK) { p_->upsertSt = nullptr; return false; }
    sqlite3_stmt* st = p_->upsertSt;
    sqlite3_reset(st);
    auto text = [&](int i, const std::string& v) { sqlite3_bind_text(st, i, v.c_str(), (int)v.size(), SQLITE_TRANSIENT); };
    sqlite3_bind_int64(st, 1, (int64_t)s.id);
    sqlite3_bind_int(st, 2, s.mode);
    sqlite3_bind_int(st, 3, s.receiver);
    sqlite3_bind_int(st, 4, s.kind);
    text(5, s.key); text(6, s.title);
    text(7, s.who[0]); text(8, s.who[1]); text(9, s.alias[0]); text(10, s.alias[1]);
    sqlite3_bind_int64(st, 11, s.startWall);
    sqlite3_bind_int64(st, 12, s.endWall);
    sqlite3_bind_int64(st, 13, s.durationMs);
    sqlite3_bind_double(st, 14, std::isfinite(s.levelDb) ? s.levelDb : 0.0);
    sqlite3_bind_double(st, 15, std::isfinite(s.levelPeakDb) ? s.levelPeakDb : 0.0);
    sqlite3_bind_double(st, 16, std::isfinite(s.snrDb) ? s.snrDb : 0.0);
    sqlite3_bind_double(st, 17, std::isfinite(s.errRate) ? s.errRate : -1.0);
    sqlite3_bind_int64(st, 18, (int64_t)s.freqHz);
    sqlite3_bind_int(st, 19, s.carrierId);
    sqlite3_bind_int(st, 20, s.slot);
    if (std::isfinite(s.lat) && std::isfinite(s.lon)) { sqlite3_bind_double(st, 21, s.lat); sqlite3_bind_double(st, 22, s.lon); }
    else { sqlite3_bind_null(st, 21); sqlite3_bind_null(st, 22); }
    if (std::isfinite(s.altM)) sqlite3_bind_double(st, 23, s.altM); else sqlite3_bind_null(st, 23);
    sqlite3_bind_int(st, 24, s.verdict);
    sqlite3_bind_int(st, 25, s.flags);
    sqlite3_bind_int64(st, 26, s.eventCount);
    sqlite3_bind_int64(st, 27, updMs);
    const bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_reset(st);
    return ok;
}

bool SessionDb::addLink(const SessionLink& l) {
    std::lock_guard<std::mutex> lk(p_->wmu);
    if (!p_->w) return false;
    if (!p_->linkSt && sqlite3_prepare_v2(p_->w, "INSERT INTO links(session_id,type,ref,wall) SELECT ?1,?2,?3,?4 WHERE EXISTS (SELECT 1 FROM sessions WHERE id = ?1)"
                                                    " AND NOT EXISTS (SELECT 1 FROM links WHERE session_id = ?1 AND type = ?2 AND ref = ?3 AND wall = ?4)", -1, &p_->linkSt, nullptr) != SQLITE_OK) { p_->linkSt = nullptr; return false; }
    sqlite3_stmt* st = p_->linkSt;
    sqlite3_reset(st);
    sqlite3_bind_int64(st, 1, (int64_t)l.sessionId);
    sqlite3_bind_int(st, 2, l.type);
    sqlite3_bind_text(st, 3, l.ref.c_str(), (int)l.ref.size(), SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 4, l.wall);
    const bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_reset(st);
    return ok;
}

bool SessionDb::setNote(uint64_t id, const std::string& note, const std::string& tags) {
    std::lock_guard<std::mutex> lk(p_->wmu);
    if (!p_->w) return false;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(p_->w, "INSERT INTO notes(session_id,note,tags) SELECT ?1,?2,?3 WHERE EXISTS (SELECT 1 FROM sessions WHERE id = ?1)"
                                  " ON CONFLICT(session_id) DO UPDATE SET note=excluded.note, tags=excluded.tags", -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int64(st, 1, (int64_t)id);
    sqlite3_bind_text(st, 2, note.c_str(), (int)note.size(), SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, tags.c_str(), (int)tags.size(), SQLITE_TRANSIENT);
    const bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}

bool SessionDb::setStarred(uint64_t id, bool on) {
    std::lock_guard<std::mutex> lk(p_->wmu);
    if (!p_->w) return false;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(p_->w, on ? "UPDATE sessions SET flags = flags | 16 WHERE id = ?" : "UPDATE sessions SET flags = flags & ~16 WHERE id = ?", -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int64(st, 1, (int64_t)id);
    const bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}

bool SessionDb::dropContentLinks(uint64_t id) {
    std::lock_guard<std::mutex> lk(p_->wmu);
    if (!p_->w) return false;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(p_->w, "DELETE FROM links WHERE session_id = ? AND type IN (1, 3, 5, 6)", -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int64(st, 1, (int64_t)id);
    const bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}

bool SessionDb::removeLinksByRef(const std::string& ref) {
    std::lock_guard<std::mutex> lk(p_->wmu);
    if (!p_->w) return false;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(p_->w, "DELETE FROM links WHERE ref = ?", -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(st, 1, ref.c_str(), (int)ref.size(), SQLITE_TRANSIENT);
    const bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}

bool SessionDb::setMeta(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lk(p_->wmu);
    if (!p_->w) return false;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(p_->w, "INSERT INTO meta(k,v) VALUES(?1,?2) ON CONFLICT(k) DO UPDATE SET v=excluded.v", -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(st, 1, key.c_str(), (int)key.size(), SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, value.c_str(), (int)value.size(), SQLITE_TRANSIENT);
    const bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}

std::string SessionDb::meta(const std::string& key) const {
    std::lock_guard<std::mutex> lk(p_->rlock());
    sqlite3* db = p_->rdb();
    if (!db) return {};
    sqlite3_stmt* st = nullptr;
    std::string v;
    if (sqlite3_prepare_v2(db, "SELECT v FROM meta WHERE k = ?", -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, key.c_str(), (int)key.size(), SQLITE_TRANSIENT);
        if (sqlite3_step(st) == SQLITE_ROW) v = col(st, 0);
    }
    sqlite3_finalize(st);
    return v;
}

int SessionDb::recoverOrphans(int64_t nowMs, int64_t startedMs, int64_t staleMs, bool force) {
    if (!force) {
        const std::string hb = meta("heartbeat");
        if (!hb.empty()) {
            const int64_t last = atoll(hb.c_str());
            if (nowMs - last < staleMs && last <= nowMs) return -1;   // another instance may still be running
        }
    }
    std::lock_guard<std::mutex> lk(p_->wmu);
    if (!p_->w) return 0;
    const bool own = !p_->tx;
    if (own) sqlite3_exec(p_->w, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr);
    sqlite3_stmt* st = nullptr;
    int n = 0;
    // end = the last time anything was written to the row (never before its start)
    if (sqlite3_prepare_v2(p_->w, "UPDATE sessions SET end = max(upd, start + 1), dur = max(upd, start + 1) - start, flags = flags | 64 WHERE end = 0 AND upd < ?", -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, startedMs);
        if (sqlite3_step(st) == SQLITE_DONE) n = sqlite3_changes(p_->w);
    }
    sqlite3_finalize(st);
    if (own) sqlite3_exec(p_->w, "COMMIT", nullptr, nullptr, nullptr);
    return n;
}

int SessionDb::closeActive(int64_t nowMs, int64_t sinceMs) {
    std::lock_guard<std::mutex> lk(p_->wmu);
    if (!p_->w) return 0;
    const bool own = !p_->tx;
    if (own) sqlite3_exec(p_->w, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr);
    sqlite3_stmt* st = nullptr;
    int n = 0;
    if (sqlite3_prepare_v2(p_->w, "UPDATE sessions SET end = max(?1, start + 1), dur = max(?1, start + 1) - start WHERE end = 0 AND upd >= ?2", -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, nowMs);
        sqlite3_bind_int64(st, 2, sinceMs);
        if (sqlite3_step(st) == SQLITE_DONE) n = sqlite3_changes(p_->w);
    }
    sqlite3_finalize(st);
    if (own) sqlite3_exec(p_->w, "COMMIT", nullptr, nullptr, nullptr);
    return n;
}

PruneResult SessionDb::prune(const DbLimits& lim, int64_t nowMs) {
    PruneResult pr;
    std::lock_guard<std::mutex> lk(p_->wmu);
    sqlite3* db = p_->w;
    if (!db || p_->tx) return pr;
    auto del = [&](const std::string& sql, const std::vector<Bv>& b) -> int64_t {
        sqlite3_stmt* st = nullptr;
        int64_t n = 0;
        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) == SQLITE_OK) {
            bindAll(st, b);
            if (sqlite3_step(st) == SQLITE_DONE) n = sqlite3_changes(db);
        }
        sqlite3_finalize(st);
        return n;
    };
    sqlite3_exec(db, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr);
    if (lim.logDays > 0)
        pr.byAge = del("DELETE FROM sessions WHERE end > 0 AND (flags & 16) = 0 AND start < ?", {{true, nowMs - (int64_t)lim.logDays * 86400000ll, ""}});
    if (lim.positionDays > 0)
        pr.positions = del("DELETE FROM links WHERE type = 4 AND wall < ?", {{true, nowMs - (int64_t)lim.positionDays * 86400000ll, ""}});
    if (lim.perModeRows > 0) {
        std::vector<std::pair<int, int64_t>> modes;
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db, "SELECT mode, count(*) FROM sessions GROUP BY mode", -1, &st, nullptr) == SQLITE_OK)
            while (sqlite3_step(st) == SQLITE_ROW) modes.push_back({sqlite3_column_int(st, 0), sqlite3_column_int64(st, 1)});
        sqlite3_finalize(st);
        for (const auto& m : modes) {
            if (m.second <= lim.perModeRows) continue;
            pr.byQuota += del("DELETE FROM sessions WHERE id IN (SELECT id FROM sessions WHERE mode = ?1 AND end > 0 AND (flags & 16) = 0 ORDER BY start ASC, id ASC LIMIT ?2)",
                              {{true, m.first, ""}, {true, m.second - lim.perModeRows, ""}});
        }
    }
    sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr);
    if (lim.maxBytes > 0) {
        // the file only shrinks through incremental_vacuum; free pages count as free room, so measure the used part
        auto used = [&] { return (p_->scalar(db, "PRAGMA page_count") - p_->scalar(db, "PRAGMA freelist_count")) * p_->scalar(db, "PRAGMA page_size"); };
        for (int guard = 0; guard < 40 && used() > lim.maxBytes; guard++) {
            const int64_t n = p_->scalar(db, "SELECT count(*) FROM sessions WHERE end > 0 AND (flags & 16) = 0");
            if (n <= 0) break;
            sqlite3_exec(db, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr);
            pr.bySize += del("DELETE FROM sessions WHERE id IN (SELECT id FROM sessions WHERE end > 0 AND (flags & 16) = 0 ORDER BY start ASC, id ASC LIMIT ?)", {{true, std::max<int64_t>(1, n / 8), ""}});
            sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr);
            sqlite3_exec(db, "PRAGMA incremental_vacuum", nullptr, nullptr, nullptr);
        }
    }
    if (pr.byAge + pr.byQuota + pr.bySize + pr.positions > 0) sqlite3_exec(db, "PRAGMA incremental_vacuum", nullptr, nullptr, nullptr);
    return pr;
}

int64_t SessionDb::deleteMode(uint16_t mode, std::vector<std::string>* refs) {
    std::lock_guard<std::mutex> lk(p_->wmu);
    sqlite3* db = p_->w;
    if (!db || p_->tx) return 0;
    if (refs) {
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db, "SELECT ref FROM links WHERE session_id IN (SELECT id FROM sessions WHERE mode = ?)", -1, &st, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(st, 1, mode);
            while (sqlite3_step(st) == SQLITE_ROW) refs->push_back(col(st, 0));
        }
        sqlite3_finalize(st);
    }
    int64_t n = 0;
    sqlite3_stmt* st = nullptr;
    sqlite3_exec(db, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr);
    if (sqlite3_prepare_v2(db, "DELETE FROM sessions WHERE mode = ?", -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(st, 1, mode);
        if (sqlite3_step(st) == SQLITE_DONE) n = sqlite3_changes(db);
    }
    sqlite3_finalize(st);
    sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr);
    sqlite3_exec(db, "PRAGMA incremental_vacuum", nullptr, nullptr, nullptr);
    return n;
}

int64_t SessionDb::deleteAll(std::vector<std::string>* refs) {
    std::lock_guard<std::mutex> lk(p_->wmu);
    sqlite3* db = p_->w;
    if (!db || p_->tx) return 0;
    if (refs) {
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db, "SELECT ref FROM links", -1, &st, nullptr) == SQLITE_OK)
            while (sqlite3_step(st) == SQLITE_ROW) refs->push_back(col(st, 0));
        sqlite3_finalize(st);
    }
    const int64_t n = p_->scalar(db, "SELECT count(*) FROM sessions");
    sqlite3_exec(db, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr);
    sqlite3_exec(db, "DELETE FROM sessions", nullptr, nullptr, nullptr);
    sqlite3_exec(db, "DELETE FROM links", nullptr, nullptr, nullptr);
    sqlite3_exec(db, "DELETE FROM notes", nullptr, nullptr, nullptr);
    sqlite3_exec(db, "INSERT INTO sessions_fts(sessions_fts) VALUES('rebuild')", nullptr, nullptr, nullptr);
    sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr);
    sqlite3_exec(db, "PRAGMA incremental_vacuum", nullptr, nullptr, nullptr);
    sqlite3_exec(db, "PRAGMA wal_checkpoint(TRUNCATE)", nullptr, nullptr, nullptr);
    return n;
}

void SessionDb::compact() {
    std::lock_guard<std::mutex> lk(p_->wmu);
    if (!p_->w || p_->tx) return;
    sqlite3_exec(p_->w, "PRAGMA incremental_vacuum", nullptr, nullptr, nullptr);
    sqlite3_exec(p_->w, "PRAGMA wal_checkpoint(PASSIVE)", nullptr, nullptr, nullptr);
}

// ------------------------------------------------------------------ reading

bool SessionDb::get(uint64_t id, Session& out) const {
    std::lock_guard<std::mutex> lk(p_->rlock());
    sqlite3* db = p_->rdb();
    if (!db) return false;
    sqlite3_stmt* st = nullptr;
    bool ok = false;
    if (sqlite3_prepare_v2(db, (std::string("SELECT ") + kCols + " FROM sessions s WHERE s.id = ?").c_str(), -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (int64_t)id);
        if (sqlite3_step(st) == SQLITE_ROW) { out = decode(st); ok = true; }
    }
    sqlite3_finalize(st);
    return ok;
}

int64_t SessionDb::count(const Query& q) const {
    std::lock_guard<std::mutex> lk(p_->rlock());
    sqlite3* db = p_->rdb();
    if (!db) return 0;
    std::string w;
    std::vector<Bv> b;
    p_->where(q, w, b);
    return p_->scalar(db, "SELECT count(*) FROM sessions s" + w, b);
}

Page SessionDb::query(const Query& q) const {
    const auto t0 = std::chrono::steady_clock::now();
    Page pg;
    std::lock_guard<std::mutex> lk(p_->rlock());
    sqlite3* db = p_->rdb();
    if (!db) return pg;
    std::string w;
    std::vector<Bv> b;
    p_->where(q, w, b);
    const size_t limit = std::max<size_t>(1, std::min<size_t>(q.limit, 5000));
    pg.total = p_->scalar(db, "SELECT count(*) FROM sessions s" + w, b);
    switch (q.sort) {
    case Sort::ActiveThenNewest: {
        // the active rows are few (a partial index), so they are taken first and the finished ones follow, newest first, from the index on (mode, start)
        std::string wa = w + (w.empty() ? " WHERE s.end = 0" : " AND s.end = 0");
        std::string wf = w + (w.empty() ? " WHERE s.end > 0" : " AND s.end > 0");
        const int64_t nActive = q.activeOnly ? pg.total : p_->scalar(db, "SELECT count(*) FROM sessions s" + wa, b);
        if ((int64_t)q.offset < nActive) {
            pg.rows = p_->select(db, wa, b, "s.start DESC, s.id DESC", limit, q.offset);
            if (pg.rows.size() < limit && !q.activeOnly) {
                auto more = p_->select(db, wf, b, "s.start DESC, s.id DESC", limit - pg.rows.size(), 0);
                pg.rows.insert(pg.rows.end(), std::make_move_iterator(more.begin()), std::make_move_iterator(more.end()));
            }
        } else if (!q.activeOnly) {
            pg.rows = p_->select(db, wf, b, "s.start DESC, s.id DESC", limit, q.offset - (size_t)nActive);
        }
        break;
    }
    case Sort::Newest: pg.rows = p_->select(db, w, b, "s.start DESC, s.id DESC", limit, q.offset); break;
    case Sort::Oldest: pg.rows = p_->select(db, w, b, "s.start ASC, s.id ASC", limit, q.offset); break;
    case Sort::Longest: pg.rows = p_->select(db, w, b, "s.dur DESC, s.start DESC, s.id DESC", limit, q.offset); break;
    case Sort::Title: pg.rows = p_->select(db, w, b, "s.title COLLATE NOCASE ASC, s.start DESC, s.id DESC", limit, q.offset); break;
    }
    pg.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return pg;
}

std::vector<SessionLink> SessionDb::links(uint64_t id) const {
    std::lock_guard<std::mutex> lk(p_->rlock());
    sqlite3* db = p_->rdb();
    std::vector<SessionLink> out;
    if (!db) return out;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT type, ref, wall FROM links WHERE session_id = ? ORDER BY wall, id", -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (int64_t)id);
        while (sqlite3_step(st) == SQLITE_ROW) { SessionLink l; l.sessionId = id; l.type = (uint8_t)sqlite3_column_int(st, 0); l.ref = col(st, 1); l.wall = sqlite3_column_int64(st, 2); out.push_back(std::move(l)); }
    }
    sqlite3_finalize(st);
    return out;
}

std::vector<SessionLink> SessionDb::linksOfType(uint8_t type, int32_t mode) const {
    std::lock_guard<std::mutex> lk(p_->rlock());
    sqlite3* db = p_->rdb();
    std::vector<SessionLink> out;
    if (!db) return out;
    sqlite3_stmt* st = nullptr;
    const char* sql = mode >= 0 ? "SELECT l.session_id, l.type, l.ref, l.wall FROM links l JOIN sessions s ON s.id = l.session_id WHERE l.type = ?1 AND s.mode = ?2"
                                : "SELECT l.session_id, l.type, l.ref, l.wall FROM links l WHERE l.type = ?1";
    if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(st, 1, type);
        if (mode >= 0) sqlite3_bind_int(st, 2, mode);
        while (sqlite3_step(st) == SQLITE_ROW) { SessionLink l; l.sessionId = (uint64_t)sqlite3_column_int64(st, 0); l.type = (uint8_t)sqlite3_column_int(st, 1); l.ref = col(st, 2); l.wall = sqlite3_column_int64(st, 3); out.push_back(std::move(l)); }
    }
    sqlite3_finalize(st);
    return out;
}

std::vector<LibFile> SessionDb::libraryFiles(uint8_t linkType) const {
    std::lock_guard<std::mutex> lk(p_->rlock());
    sqlite3* db = p_->rdb();
    std::vector<LibFile> out;
    if (!db) return out;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT l.session_id, l.ref, l.wall, (s.flags & 16) <> 0 FROM links l JOIN sessions s ON s.id = l.session_id WHERE l.type = ? ORDER BY l.wall", -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(st, 1, linkType);
        while (sqlite3_step(st) == SQLITE_ROW) {
            LibFile f;
            f.sessionId = (uint64_t)sqlite3_column_int64(st, 0);
            f.ref = col(st, 1);
            f.wall = sqlite3_column_int64(st, 2);
            f.starred = sqlite3_column_int(st, 3) != 0;
            out.push_back(std::move(f));
        }
    }
    sqlite3_finalize(st);
    return out;
}

bool SessionDb::note(uint64_t id, Note& out) const {
    std::lock_guard<std::mutex> lk(p_->rlock());
    sqlite3* db = p_->rdb();
    if (!db) return false;
    sqlite3_stmt* st = nullptr;
    bool ok = false;
    if (sqlite3_prepare_v2(db, "SELECT note, tags FROM notes WHERE session_id = ?", -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (int64_t)id);
        if (sqlite3_step(st) == SQLITE_ROW) { out.note = col(st, 0); out.tags = col(st, 1); ok = true; }
    }
    sqlite3_finalize(st);
    return ok;
}

DbStats SessionDb::stats() const {
    std::lock_guard<std::mutex> lk(p_->rlock());
    sqlite3* db = p_->rdb();
    DbStats s;
    if (!db) return s;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT count(*), coalesce(min(start),0), coalesce(max(start),0) FROM sessions", -1, &st, nullptr) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW) {
        s.rows = sqlite3_column_int64(st, 0); s.oldest = sqlite3_column_int64(st, 1); s.newest = sqlite3_column_int64(st, 2);
    }
    sqlite3_finalize(st);
    s.links = p_->scalar(db, "SELECT count(*) FROM links");
    s.bytes = p_->scalar(db, "PRAGMA page_count") * p_->scalar(db, "PRAGMA page_size");
    return s;
}

std::map<uint16_t, int64_t> SessionDb::countsByMode() const {
    std::lock_guard<std::mutex> lk(p_->rlock());
    sqlite3* db = p_->rdb();
    std::map<uint16_t, int64_t> out;
    if (!db) return out;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT mode, count(*) FROM sessions GROUP BY mode", -1, &st, nullptr) == SQLITE_OK)
        while (sqlite3_step(st) == SQLITE_ROW) out[(uint16_t)sqlite3_column_int(st, 0)] = sqlite3_column_int64(st, 1);
    sqlite3_finalize(st);
    return out;
}

std::vector<std::pair<std::pair<uint32_t, uint16_t>, uint32_t>> SessionDb::maxSequences() const {
    std::lock_guard<std::mutex> lk(p_->rlock());
    sqlite3* db = p_->rdb();
    std::vector<std::pair<std::pair<uint32_t, uint16_t>, uint32_t>> out;
    if (!db) return out;
    // one indexed probe per (day, mode) group: the highest id below the group found last
    int64_t bound = INT64_MAX;
    for (int guard = 0; guard < 200000; guard++) {
        sqlite3_stmt* st = nullptr;
        int64_t id = 0;
        bool have = false;
        if (sqlite3_prepare_v2(db, "SELECT max(id) FROM sessions WHERE id < ?", -1, &st, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(st, 1, bound);
            if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_type(st, 0) != SQLITE_NULL) { id = sqlite3_column_int64(st, 0); have = true; }
        }
        sqlite3_finalize(st);
        if (!have) break;
        out.push_back({{idDay((uint64_t)id), idMode((uint64_t)id)}, idSeq((uint64_t)id)});
        bound = (int64_t)(((uint64_t)id >> 32) << 32);   // the first id of this group: everything below belongs to the previous one
    }
    return out;
}

bool SessionDb::integrityOk() const {
    std::lock_guard<std::mutex> lk(p_->rlock());
    sqlite3* db = p_->rdb();
    if (!db) return false;
    sqlite3_stmt* st = nullptr;
    bool ok = false;
    if (sqlite3_prepare_v2(db, "PRAGMA quick_check", -1, &st, nullptr) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW) ok = col(st, 0) == "ok";
    sqlite3_finalize(st);
    return ok;
}

}  // namespace dect2::sess
