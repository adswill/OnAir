#include "dect2/common/thread_name.h"
#include "dect2/msgdb/msgdb.h"
#include <sqlite3.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#ifdef _WIN32
#include <windows.h>
#elif defined(__APPLE__)
#include <pthread.h>
#include <pthread/qos.h>
#elif defined(__linux__)
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace dect2::msgdb {

namespace {
using automation::Event;
using automation::Value;

const char* kKept[] = {"adsb", "acars", "vdl2", "hfdl", "pager", "ais", "aprs", "sonde", "lora", "ble", "rid", "dsc", "qso", "mesh", "dab", "epg", "session"};

std::string family(const std::string& type) { return type.substr(0, type.find('.')); }

// types that repeat every few seconds for the same object: stored at most once per kThrottleS per identity
bool chatty(const std::string& t) { return t == "adsb.aircraft" || t == "ais.vessel" || t == "sonde.update" || t == "ble.device" || t == "rid.drone"; }
constexpr double kThrottleS = 60;

std::string fld(const Event& e, const char* k) {
    auto it = e.f.find(k);
    return it == e.f.end() ? std::string() : it->second.text();
}

std::string first(const Event& e, std::initializer_list<const char*> keys) {
    for (const char* k : keys) { std::string s = fld(e, k); if (!s.empty()) return s; }
    return {};
}

std::string lower(std::string s) { for (char& c : s) c = (char)tolower((unsigned char)c); return s; }

std::string jesc(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        switch (c) {
        case '"': o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n"; break;
        case '\r': o += "\\r"; break;
        case '\t': o += "\\t"; break;
        default:
            if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; } else o += (char)c;
        }
    }
    return o;
}

std::string csvq(const std::string& s) {
    if (s.find_first_of(",\"\r\n") == std::string::npos) return s;
    std::string o = "\"";
    for (char c : s) { if (c == '"') o += '"'; o += c; }
    return o + "\"";
}

std::string num(double v, int dec) {
    char b[48];
    snprintf(b, sizeof b, "%.*f", dec, v);
    std::string s = b;
    if (s.find('.') != std::string::npos) { while (!s.empty() && s.back() == '0') s.pop_back(); if (!s.empty() && s.back() == '.') s.pop_back(); }
    return s;
}

std::string isoTime(double t) {
    time_t s = (time_t)std::floor(t);
    struct tm g;
#ifdef _WIN32
    gmtime_s(&g, &s);
#else
    gmtime_r(&s, &g);
#endif
    char b[40];
    snprintf(b, sizeof b, "%04d-%02d-%02dT%02d:%02d:%02dZ", g.tm_year + 1900, g.tm_mon + 1, g.tm_mday, g.tm_hour, g.tm_min, g.tm_sec);
    return b;
}

// "LY1ABC", "LY1ABC-9": letters/digits with at least one digit and one letter, optional -SSID, 3 to 10 characters
bool looksLikeCall(const std::string& s) {
    if (s.size() < 3 || s.size() > 10) return false;
    bool d = false, l = false;
    size_t i = 0;
    for (; i < s.size() && s[i] != '-'; i++) {
        const unsigned char c = (unsigned char)s[i];
        if (isdigit(c)) d = true; else if (isalpha(c)) l = true; else if (c != '/') return false;
    }
    return d && l;
}

// Words of a query: ASCII letters/digits and every byte of a multi-byte UTF-8 character are word characters, anything else separates.
std::vector<Token> tokenizeQuery(const std::string& in) {
    std::vector<Token> out;
    std::string cur;
    auto flush = [&](bool star) {
        if (!cur.empty()) { Token t; t.word = cur; t.prefix = star; out.push_back(t); cur.clear(); }
    };
    for (size_t i = 0; i < in.size(); i++) {
        const unsigned char c = (unsigned char)in[i];
        if (isalnum(c) || c >= 0x80) cur += (char)(c < 0x80 ? tolower(c) : c);
        else if (c == '*' && !cur.empty()) { flush(true); }
        else flush(false);
    }
    flush(false);
    return out;
}

// FTS5 query from user text: every word quoted (so punctuation is safe), a trailing * kept as a prefix match, words ANDed
std::string ftsQuery(const std::string& in) {
    std::string out;
    for (const Token& t : tokenizeQuery(in)) {
        if (!out.empty()) out += ' ';
        out += '"' + t.word + '"';
        if (t.prefix) out += '*';
    }
    return out;
}
}  // namespace

std::vector<Token> tokenize(const std::string& query) { return tokenizeQuery(query); }

bool wantsType(const std::string& type) {
    const std::string f = family(type);
    for (const char* k : kKept) if (f == k) return true;
    return false;
}

bool fromEvent(const Event& ev, Record& r) {
    if (!wantsType(ev.type)) return false;
    // never voice or encrypted content
    const std::string f = family(ev.type);
    if (f == "voice" || f == "audio") return false;
    auto it = ev.f.find("encrypted");
    if (it != ev.f.end() && it->second.truthy()) return false;
    r = Record();
    r.time = ev.time > 0 ? ev.time : automation::wallNow();
    r.type = ev.type;
    r.mode = ev.mode;
    r.freqHz = ev.freqHz;
    r.ident = first(ev, {"tail", "hex", "icao", "mmsi", "from_mmsi", "from", "src", "serial", "id", "mac", "address", "name"});
    r.text = first(ev, {"text", "info", "summary", "detail"});
    if (r.text.empty()) r.text = first(ev, {"name", "callsign", "destination", "kind"});
    auto la = ev.f.find("lat"), lo = ev.f.find("lon");
    if (la != ev.f.end() && lo != ev.f.end() && la->second.t == Value::Num && lo->second.t == Value::Num &&
        std::fabs(la->second.n) <= 90 && std::fabs(lo->second.n) <= 180 && !(la->second.n == 0 && lo->second.n == 0)) {
        r.lat = la->second.n; r.lon = lo->second.n; r.hasPos = true;
    }
    r.source = first(ev, {"file", "source_file", "recording", "iq_file"});
    const bool isSession = f == "session";
    if (isSession) {   // a session summary: the key is the identity, the title the text, the id links to the session; extra keeps ids and counts only
        r.ident = fld(ev, "key");
        r.text = fld(ev, "title");
        r.sessionId = (int64_t)strtoll(fld(ev, "session_id").c_str(), nullptr, 10);
    }
    for (const auto& kv : ev.f) {
        if (isSession && (kv.first == "key" || kv.first == "title")) continue;
        std::string v = kv.second.text();
        for (char& c : v) if (c == '\n' || c == '\r') c = ' ';
        r.extra += kv.first + "=" + v + "\n";
    }
    return true;
}

struct Bind { bool isNum; double d; std::string s; int64_t i = 0; bool isInt = false; };   // isInt: a 64 bit integer (a session id does not fit a double)

struct Db::Impl {
    mutable std::mutex mu;
    sqlite3* db = nullptr;
    std::string path;
    Limits lim;
    std::mutex qmu;
    std::condition_variable qcv;
    std::deque<Event> q;
    std::thread th;
    bool stopReq = false;
    bool running = false;
    std::map<std::string, double> lastSeen;   // type|ident -> time of the stored record (throttle)
    int64_t sincePrune = 0;

    bool exec(const char* sql) { return sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK; }

    bool insert(const Record& r) {
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db, "INSERT INTO msgs(t,type,mode,freq,ident,txt,lat,lon,extra,src,sid,fam) VALUES(?,?,?,?,?,?,?,?,?,?,?,?)", -1, &st, nullptr) != SQLITE_OK) return false;
        sqlite3_bind_double(st, 1, r.time);
        sqlite3_bind_text(st, 2, r.type.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 3, r.mode.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(st, 4, r.freqHz);
        sqlite3_bind_text(st, 5, r.ident.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 6, r.text.c_str(), -1, SQLITE_TRANSIENT);
        if (r.hasPos) { sqlite3_bind_double(st, 7, r.lat); sqlite3_bind_double(st, 8, r.lon); }
        else { sqlite3_bind_null(st, 7); sqlite3_bind_null(st, 8); }
        sqlite3_bind_text(st, 9, r.extra.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 10, r.source.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 11, r.sessionId);
        sqlite3_bind_text(st, 12, family(r.type).c_str(), -1, SQLITE_TRANSIENT);
        const bool ok = sqlite3_step(st) == SQLITE_DONE;
        sqlite3_finalize(st);
        return ok;
    }

    // FROM + WHERE for a filter
    void where(const Filter& f, std::string& sql, std::vector<Bind>& b) const {
        sql = " FROM msgs m";
        std::string w;
        auto andw = [&](const std::string& c) { w += w.empty() ? " WHERE " : " AND "; w += c; };
        const std::string fq = ftsQuery(f.text);
        if (!fq.empty() && f.relevance) { sql += " JOIN (SELECT rowid AS rid, bm25(msgs_fts,8.0,3.0,1.0,2.0) AS rk FROM msgs_fts WHERE msgs_fts MATCH ?) r ON r.rid = m.id"; b.push_back({false, 0, fq}); }
        else if (!fq.empty()) { andw("m.id IN (SELECT rowid FROM msgs_fts WHERE msgs_fts MATCH ?)"); b.push_back({false, 0, fq}); }
        if (f.freqLo > 0) { andw("m.freq >= ?"); b.push_back({true, f.freqLo, ""}); }
        if (f.freqHi > 0) { andw("m.freq > 0 AND m.freq <= ?"); b.push_back({true, f.freqHi, ""}); }
        if (!f.source.empty()) { andw("instr(lower(m.src), ?) > 0"); b.push_back({false, 0, lower(f.source)}); }
        if (!f.mode.empty()) { andw("m.mode = ?"); b.push_back({false, 0, f.mode}); }
        if (!f.type.empty()) { andw("m.type = ?"); b.push_back({false, 0, f.type}); }
        if (!f.ident.empty()) { andw("instr(lower(m.ident), ?) > 0"); b.push_back({false, 0, lower(f.ident)}); }
        if (f.since > 0) { andw("m.t >= ?"); b.push_back({true, f.since, ""}); }
        if (f.until > 0) { andw("m.t <= ?"); b.push_back({true, f.until, ""}); }
        if (f.posOnly) andw("m.lat IS NOT NULL");
        if (f.sessionId != 0) { andw("m.sid = ?"); b.push_back({true, 0, "", f.sessionId, true}); }
        sql += w;
    }

    static void bindAll(sqlite3_stmt* st, const std::vector<Bind>& b) {
        for (size_t i = 0; i < b.size(); i++) {
            if (b[i].isInt) sqlite3_bind_int64(st, (int)i + 1, b[i].i);
            else if (b[i].isNum) sqlite3_bind_double(st, (int)i + 1, b[i].d);
            else sqlite3_bind_text(st, (int)i + 1, b[i].s.c_str(), -1, SQLITE_TRANSIENT);
        }
    }

    static std::string col(sqlite3_stmt* st, int i) {
        const unsigned char* t = sqlite3_column_text(st, i);
        return t ? (const char*)t : "";
    }

    std::vector<Record> select(const Filter& f, bool noLimit) const {
        std::vector<Record> out;
        if (!db) return out;
        std::string sql;
        std::vector<Bind> b;
        where(f, sql, b);
        std::string q = "SELECT m.id,m.t,m.type,m.mode,m.freq,m.ident,m.txt,m.lat,m.lon,m.extra,m.src,m.sid" + sql +
                        (!ftsQuery(f.text).empty() && f.relevance ? " ORDER BY r.rk ASC, m.t DESC, m.id DESC" : f.oldestFirst ? " ORDER BY m.t ASC, m.id ASC" : " ORDER BY m.t DESC, m.id DESC");
        if (!noLimit) q += " LIMIT " + std::to_string(std::max(1, std::min(f.limit, 100000))) + " OFFSET " + std::to_string(std::max(0, f.offset));
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db, q.c_str(), -1, &st, nullptr) != SQLITE_OK) return out;
        bindAll(st, b);
        while (sqlite3_step(st) == SQLITE_ROW) {
            Record r;
            r.id = sqlite3_column_int64(st, 0);
            r.time = sqlite3_column_double(st, 1);
            r.type = col(st, 2); r.mode = col(st, 3);
            r.freqHz = sqlite3_column_double(st, 4);
            r.ident = col(st, 5); r.text = col(st, 6);
            if (sqlite3_column_type(st, 7) != SQLITE_NULL) { r.hasPos = true; r.lat = sqlite3_column_double(st, 7); r.lon = sqlite3_column_double(st, 8); }
            r.extra = col(st, 9); r.source = col(st, 10);
            r.sessionId = sqlite3_column_int64(st, 11);
            out.push_back(std::move(r));
        }
        sqlite3_finalize(st);
        return out;
    }

    int64_t scalar(const char* sql) const {
        sqlite3_stmt* st = nullptr;
        int64_t v = 0;
        if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW) v = sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
        return v;
    }

    int64_t fileBytes() const { return scalar("PRAGMA page_count") * scalar("PRAGMA page_size"); }

    int64_t pruneLocked() {
        if (!db) return 0;
        int64_t gone = 0;
        auto del = [&](const std::string& sql) { if (exec(sql.c_str())) gone += sqlite3_changes(db); };
        if (lim.maxAgeDays > 0) del("DELETE FROM msgs WHERE t < " + num(automation::wallNow() - lim.maxAgeDays * 86400, 0));
        if (lim.maxRowsPerFamily > 0) {
            // a chatty family may not push the others out: each keeps at most maxRowsPerFamily rows (the oldest of that family go)
            std::vector<std::pair<std::string, int64_t>> fams;
            sqlite3_stmt* fs = nullptr;
            if (sqlite3_prepare_v2(db, "SELECT fam, count(*) FROM msgs GROUP BY fam", -1, &fs, nullptr) == SQLITE_OK)
                while (sqlite3_step(fs) == SQLITE_ROW) fams.push_back({col(fs, 0), sqlite3_column_int64(fs, 1)});
            sqlite3_finalize(fs);
            for (const auto& fc : fams) {
                if (fc.second <= lim.maxRowsPerFamily) continue;
                std::string esc;
                for (char c : fc.first) { if (c == '\'') esc += '\''; esc += c; }
                del("DELETE FROM msgs WHERE id IN (SELECT id FROM msgs WHERE fam = '" + esc + "' ORDER BY t ASC, id ASC LIMIT " + std::to_string(fc.second - lim.maxRowsPerFamily) + ")");
            }
        }
        if (lim.maxRows > 0) {
            const int64_t n = scalar("SELECT count(*) FROM msgs");
            if (n > lim.maxRows) del("DELETE FROM msgs WHERE id IN (SELECT id FROM msgs ORDER BY t ASC, id ASC LIMIT " + std::to_string(n - lim.maxRows) + ")");
        }
        if (lim.maxBytes > 0) {
            // the file only shrinks through incremental_vacuum; free pages count as free room, so measure the used part
            for (int guard = 0; guard < 40; guard++) {
                exec("PRAGMA incremental_vacuum");
                if (fileBytes() <= lim.maxBytes) break;
                const int64_t n = scalar("SELECT count(*) FROM msgs");
                if (n <= 0) break;
                del("DELETE FROM msgs WHERE id IN (SELECT id FROM msgs ORDER BY t ASC, id ASC LIMIT " + std::to_string(std::max<int64_t>(1, n / 8)) + ")");
            }
        }
        if (gone > 0) exec("PRAGMA incremental_vacuum");
        return gone;
    }

    // indexing is background work: the writer thread runs below the decoders
    static void lowPriority() {
#ifdef _WIN32
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#elif defined(__APPLE__)
        pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
#elif defined(__linux__)
        setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 10);
#endif
    }

    void writer() {
        lowPriority();
        for (;;) {
            std::deque<Event> batch;
            {
                std::unique_lock<std::mutex> lk(qmu);
                qcv.wait(lk, [&] { return stopReq || !q.empty(); });   // asleep until an event or stop arrives (both notify)
                batch.swap(q);
                if (batch.empty() && stopReq) return;
            }
            if (batch.empty()) continue;
            std::lock_guard<std::mutex> lk(mu);
            if (!db) continue;
            exec("BEGIN");
            for (const Event& e : batch) {
                Record r;
                if (!fromEvent(e, r)) continue;
                if (chatty(r.type)) {
                    const std::string key = r.type + "|" + r.ident;
                    auto it = lastSeen.find(key);
                    if (it != lastSeen.end() && r.time - it->second < kThrottleS && r.time >= it->second) continue;
                    if (lastSeen.size() > 50000) lastSeen.clear();
                    lastSeen[key] = r.time;
                }
                if (insert(r)) sincePrune++;
            }
            exec("COMMIT");
            if (sincePrune >= 2000) { sincePrune = 0; pruneLocked(); }
        }
    }
};

Db::Db() : p_(new Impl) {}
Db::~Db() { stop(); close(); }

bool Db::open(const std::string& path, std::string* err) {
    close();
    std::lock_guard<std::mutex> lk(p_->mu);
    sqlite3* db = nullptr;
    int rc = sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
    if (rc != SQLITE_OK) {
        if (err) *err = db ? sqlite3_errmsg(db) : "out of memory";
        if (db) sqlite3_close(db);
        return false;
    }
    p_->db = db;
    sqlite3_busy_timeout(db, 3000);
    p_->exec("PRAGMA auto_vacuum=INCREMENTAL");   // only takes effect on a new file
    if (path != ":memory:") { p_->exec("PRAGMA journal_mode=WAL"); p_->exec("PRAGMA synchronous=NORMAL"); }
    const char* schema =
        "CREATE TABLE IF NOT EXISTS msgs(id INTEGER PRIMARY KEY, t REAL NOT NULL, type TEXT NOT NULL, mode TEXT NOT NULL DEFAULT '', freq REAL NOT NULL DEFAULT 0,"
        " ident TEXT NOT NULL DEFAULT '', txt TEXT NOT NULL DEFAULT '', lat REAL, lon REAL, extra TEXT NOT NULL DEFAULT '', src TEXT NOT NULL DEFAULT '',"
        " sid INTEGER NOT NULL DEFAULT 0, fam TEXT NOT NULL DEFAULT '');"
        "CREATE INDEX IF NOT EXISTS msgs_t ON msgs(t);"
        "CREATE INDEX IF NOT EXISTS msgs_mode_t ON msgs(mode, t);"
        "CREATE INDEX IF NOT EXISTS msgs_fam_t ON msgs(fam, t);"
        "CREATE INDEX IF NOT EXISTS msgs_sid ON msgs(sid) WHERE sid <> 0;"
        "CREATE VIRTUAL TABLE IF NOT EXISTS msgs_fts USING fts5(ident, txt, extra, mode, content='msgs', content_rowid='id', tokenize='unicode61 remove_diacritics 2');"
        "CREATE TRIGGER IF NOT EXISTS msgs_ai AFTER INSERT ON msgs BEGIN INSERT INTO msgs_fts(rowid,ident,txt,extra,mode) VALUES(new.id,new.ident,new.txt,new.extra,new.mode); END;"
        "CREATE TRIGGER IF NOT EXISTS msgs_ad AFTER DELETE ON msgs BEGIN INSERT INTO msgs_fts(msgs_fts,rowid,ident,txt,extra,mode) VALUES('delete',old.id,old.ident,old.txt,old.extra,old.mode); END;";
    char* em = nullptr;
    {   // a file from before the source column: add it (the full-text index does not cover it)
        sqlite3_stmt* st = nullptr;
        bool hasTable = false, hasSrc = false, hasSid = false;
        if (sqlite3_prepare_v2(db, "PRAGMA table_info(msgs)", -1, &st, nullptr) == SQLITE_OK) {
            while (sqlite3_step(st) == SQLITE_ROW) { hasTable = true; const unsigned char* n = sqlite3_column_text(st, 1); if (n && std::string((const char*)n) == "src") hasSrc = true; if (n && std::string((const char*)n) == "sid") hasSid = true; }
        }
        sqlite3_finalize(st);
        if (hasTable && !hasSrc) p_->exec("ALTER TABLE msgs ADD COLUMN src TEXT NOT NULL DEFAULT ''");
        if (hasTable && !hasSid) {   // a file from before the session link and the family column: add both, fill the family of the old rows once
            p_->exec("ALTER TABLE msgs ADD COLUMN sid INTEGER NOT NULL DEFAULT 0");
            p_->exec("ALTER TABLE msgs ADD COLUMN fam TEXT NOT NULL DEFAULT ''");
            p_->exec("UPDATE msgs SET fam = substr(type, 1, instr(type || '.', '.') - 1)");
        }
    }
    if (sqlite3_exec(db, schema, nullptr, nullptr, &em) != SQLITE_OK) {
        if (err) *err = em ? em : "schema";
        sqlite3_free(em);
        sqlite3_close(db);
        p_->db = nullptr;
        return false;
    }
    p_->path = path;
    return true;
}

void Db::close() {
    stop();
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->db) { sqlite3_close(p_->db); p_->db = nullptr; }
    p_->path.clear();
}

bool Db::isOpen() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->db != nullptr; }
std::string Db::path() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->path; }

bool Db::add(const Record& r) {
    if (r.type.rfind("voice", 0) == 0 || r.type.rfind("audio", 0) == 0) return false;
    if (lower(r.extra).find("encrypted=true") != std::string::npos) return false;
    std::lock_guard<std::mutex> lk(p_->mu);
    return p_->db && p_->insert(r);
}

void Db::enqueue(const Event& ev) {
    if (!wantsType(ev.type)) return;
    std::lock_guard<std::mutex> lk(p_->qmu);
    if (p_->q.size() >= 20000) return;
    p_->q.push_back(ev);
    p_->qcv.notify_one();
}

void Db::start() {
    if (p_->th.joinable()) return;
    p_->stopReq = false;
    p_->th = dect2::namedThread("onair-msgdb", [this] { p_->writer(); });
}

void Db::stop() {
    if (!p_->th.joinable()) return;
    { std::lock_guard<std::mutex> lk(p_->qmu); p_->stopReq = true; }
    p_->qcv.notify_all();
    p_->th.join();
}

void Db::setLimits(const Limits& l) { std::lock_guard<std::mutex> lk(p_->mu); p_->lim = l; }
Limits Db::limits() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->lim; }
int64_t Db::prune() { std::lock_guard<std::mutex> lk(p_->mu); return p_->pruneLocked(); }

void Db::clear() {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (!p_->db) return;
    p_->exec("DELETE FROM msgs");
    p_->exec("INSERT INTO msgs_fts(msgs_fts) VALUES('rebuild')");
    p_->exec("PRAGMA incremental_vacuum");
}

void Db::rebuildIndex() {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (!p_->db) return;
    p_->exec("INSERT INTO msgs_fts(msgs_fts) VALUES('rebuild')");
    p_->exec("INSERT INTO msgs_fts(msgs_fts) VALUES('optimize')");
}

std::vector<Record> Db::query(const Filter& f) const { std::lock_guard<std::mutex> lk(p_->mu); return p_->select(f, false); }

int64_t Db::count(const Filter& f) const {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (!p_->db) return 0;
    std::string sql;
    std::vector<Bind> b;
    p_->where(f, sql, b);
    sqlite3_stmt* st = nullptr;
    int64_t n = 0;
    if (sqlite3_prepare_v2(p_->db, ("SELECT count(*)" + sql).c_str(), -1, &st, nullptr) == SQLITE_OK) {
        Impl::bindAll(st, b);
        if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int64(st, 0);
    }
    sqlite3_finalize(st);
    return n;
}

Stats Db::stats() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    Stats s;
    if (!p_->db) return s;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(p_->db, "SELECT count(*), coalesce(min(t),0), coalesce(max(t),0) FROM msgs", -1, &st, nullptr) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW) {
        s.rows = sqlite3_column_int64(st, 0); s.oldest = sqlite3_column_double(st, 1); s.newest = sqlite3_column_double(st, 2);
    }
    sqlite3_finalize(st);
    s.bytes = p_->fileBytes();
    return s;
}

std::vector<std::string> Db::modes() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    std::vector<std::string> o;
    if (!p_->db) return o;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(p_->db, "SELECT DISTINCT mode FROM msgs WHERE mode<>'' ORDER BY mode", -1, &st, nullptr) == SQLITE_OK)
        while (sqlite3_step(st) == SQLITE_ROW) o.push_back(Impl::col(st, 0));
    sqlite3_finalize(st);
    return o;
}

std::string Db::exportAs(const Filter& f, const std::string& format) const {
    std::vector<Record> rows;
    { std::lock_guard<std::mutex> lk(p_->mu); rows = p_->select(f, true); }
    const std::string fmt = lower(format);
    std::string o;
    if (fmt == "json") {
        o = "[";
        bool firstRow = true;
        for (const Record& r : rows) {
            o += firstRow ? "\n" : ",\n";
            firstRow = false;
            o += "{\"time\":\"" + isoTime(r.time) + "\",\"type\":\"" + jesc(r.type) + "\",\"mode\":\"" + jesc(r.mode) + "\",\"freq_hz\":" + num(r.freqHz, 0) +
                 ",\"ident\":\"" + jesc(r.ident) + "\",\"text\":\"" + jesc(r.text) + "\"";
            if (r.hasPos) o += ",\"lat\":" + num(r.lat, 6) + ",\"lon\":" + num(r.lon, 6);
            o += ",\"fields\":{";
            bool fk = true;
            size_t i = 0;
            while (i < r.extra.size()) {
                size_t e = r.extra.find('\n', i);
                if (e == std::string::npos) e = r.extra.size();
                const std::string line = r.extra.substr(i, e - i);
                const size_t eq = line.find('=');
                if (eq != std::string::npos) {
                    if (!fk) o += ",";
                    fk = false;
                    o += "\"" + jesc(line.substr(0, eq)) + "\":\"" + jesc(line.substr(eq + 1)) + "\"";
                }
                i = e + 1;
            }
            o += "}}";
        }
        o += rows.empty() ? "]\n" : "\n]\n";
    } else if (fmt == "adif") {
        auto tag = [&](const char* n, const std::string& v) { if (!v.empty()) o += "<" + std::string(n) + ":" + std::to_string(v.size()) + ">" + v + " "; };
        o = "OnAir message database export\n<ADIF_VER:5>3.1.4 <PROGRAMID:5>OnAir <EOH>\n";
        for (const Record& r : rows) {
            if (r.type.rfind("aprs", 0) != 0 || !looksLikeCall(r.ident)) continue;   // amateur call signs only: APRS stations
            time_t s = (time_t)std::floor(r.time);
            struct tm g;
#ifdef _WIN32
            gmtime_s(&g, &s);
#else
            gmtime_r(&s, &g);
#endif
            char d[16], t[16];
            snprintf(d, sizeof d, "%04d%02d%02d", g.tm_year + 1900, g.tm_mon + 1, g.tm_mday);
            snprintf(t, sizeof t, "%02d%02d%02d", g.tm_hour, g.tm_min, g.tm_sec);
            tag("CALL", r.ident);
            tag("QSO_DATE", d);
            tag("TIME_ON", t);
            if (r.freqHz > 0) tag("FREQ", num(r.freqHz / 1e6, 6));
            tag("MODE", r.mode);
            if (!r.text.empty()) tag("COMMENT", r.text.substr(0, 200));
            o += "<EOR>\n";
        }
    } else {
        o = "time,type,mode,freq_hz,ident,text,lat,lon\r\n";
        for (const Record& r : rows) {
            o += isoTime(r.time) + "," + csvq(r.type) + "," + csvq(r.mode) + "," + num(r.freqHz, 0) + "," + csvq(r.ident) + "," + csvq(r.text) + "," +
                 (r.hasPos ? num(r.lat, 6) : "") + "," + (r.hasPos ? num(r.lon, 6) : "") + "\r\n";
        }
    }
    return o;
}

Db& global() {
    static Db* d = new Db;   // never destroyed: the writer thread may still run at exit
    return *d;
}

}  // namespace dect2::msgdb
