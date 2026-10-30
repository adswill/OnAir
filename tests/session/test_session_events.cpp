// The end-of-session summary events and their link to the message database (docs/MODE-EXPERIENCE.md, 2.2 point 4 and 2.2a): one
// session.<kind> event per ended session with ids and counts only, rate limited, kept off the persistence path; msgdb stores them with the
// session id, can filter by it, and has a per-family row quota; an old message database is migrated.
#include "dect2/automation/catalog.h"
#include "dect2/automation/engine.h"
#include "dect2/msgdb/msgdb.h"
#include "dect2/session/session_store.h"
#include "session_test_util.h"
#include "../common/test_util.h"
#include <sqlite3.h>
#include <atomic>
#include <mutex>
#include <set>

using namespace dect2::sess;
namespace au = dect2::automation;
namespace mdb = dect2::msgdb;
using sesstest::mk;

static StoreConfig quiet() {
    StoreConfig c;
    c.retentionEverySec = 0;
    return c;
}

struct Seen {
    std::mutex mu;
    std::vector<au::Event> ev;
    void add(const au::Event& e) { std::lock_guard<std::mutex> lk(mu); ev.push_back(e); }
    size_t n() { std::lock_guard<std::mutex> lk(mu); return ev.size(); }
};

static void testSummaryEvent() {
    au::Engine eng;
    Seen seen;
    eng.setTap([&](const au::Event& e) { seen.add(e); }, au::familyBit(au::FSession));
    SessionStore st;
    st.setEngine(&eng);
    st.setModeName(9, "DMR");
    CHECK(st.openAt(":memory:", quiet()), "open");

    Session s = mk(9, "TG 91/slot1", sesstest::nowMs() - 12000, "DMR TG 91 Worldwide, 12 s");
    s.who[0] = "2321001"; s.who[1] = "TG 91";
    s.alias[0] = "Secret Alias Name"; s.alias[1] = "Worldwide";
    s.freqHz = 438500000; s.eventCount = 40; s.flags |= FlagEmergency;
    s.id = st.begin(s);
    st.flush();
    CHECK(seen.n() == 0, "no event when a session begins or updates, only at its end");
    st.setNote(s.id, "private note about this call", "mytag");
    s.endWall = s.startWall + 12000;
    st.end(s);
    st.flush();
    CHECK(seen.n() == 1, "one event at the end");
    st.end(s);   // a repeated end (a decoder retry) is not announced again
    st.flush();
    CHECK(seen.n() == 1, "a repeated end gives no second event");
    const au::Event e = seen.ev[0];
    CHECK(e.type == "session.call" && e.mode == "DMR" && e.freqHz == 438500000.0, "type, mode, frequency: %s %s", e.type.c_str(), e.mode.c_str());
    CHECK(e.f.count("key") && e.f.at("key").text() == "TG 91/slot1", "ident = the key");
    CHECK(e.f.count("title") && e.f.at("title").text() == "DMR TG 91 Worldwide, 12 s", "text = the title");
    CHECK(e.f.at("session_id").text() == std::to_string(s.id), "the session id as a whole number");
    CHECK(e.f.at("duration_s").n == 12.0 && e.f.at("events").n == 40 && e.f.at("mode_num").n == 9 && e.f.at("emergency").truthy(), "counts and flags");
    // ids and counts only: no alias, tag, note or text anywhere in the event
    static const std::set<std::string> allowed = {"session_id", "key", "title", "kind", "verdict", "mode_num", "receiver", "duration_s", "events", "emergency"};
    for (const auto& kv : e.f) {
        CHECK(allowed.count(kv.first), "unexpected field %s", kv.first.c_str());
        const std::string v = kv.second.text();
        CHECK(v.find("Secret Alias") == std::string::npos && v.find("private note") == std::string::npos && v.find("mytag") == std::string::npos, "no alias, note or tag in %s", kv.first.c_str());
    }
    CHECK(st.stats().eventsEmitted == 1, "counted");

    // the other kinds
    Session t = mk(20, "capcode 1", sesstest::nowMs()); t.kind = (uint8_t)Kind::Thread;
    t.id = st.begin(t); t.endWall = t.startWall + 1000; st.end(t);
    Session k = mk(11, "ICAO 4CA7B1", sesstest::nowMs()); k.kind = (uint8_t)Kind::Track;
    k.id = st.begin(k); k.endWall = k.startWall + 1000; st.end(k);
    st.flush();
    CHECK(seen.n() == 3 && seen.ev[1].type == "session.thread" && seen.ev[2].type == "session.track", "thread and track events");
    // the catalog knows them (the rule editor's drop-downs)
    CHECK(au::findEventInfo("session.call") && au::findEventInfo("session.thread") && au::findEventInfo("session.track"), "in the event catalog");
    st.close();
    eng.setTap(nullptr, 0);
}

static void testNotOnTheBusForPersistence() {
    // nobody wants session events: nothing is built or submitted, and the database is written all the same
    au::Engine eng;
    SessionStore st;
    st.setEngine(&eng);
    CHECK(st.openAt(":memory:", quiet()), "open");
    for (int i = 0; i < 100; i++) {
        Session s = mk(9, "k" + std::to_string(i), sesstest::nowMs());
        s.id = st.begin(s);
        s.endWall = s.startWall + 100;
        st.end(s);
    }
    st.flush();
    CHECK(st.stats().eventsEmitted == 0 && eng.dropped() == 0, "no event without a listener");
    Query q; q.mode = 9;
    CHECK(st.query(q).total == 100, "persistence did not need the bus");
    st.close();
}

static void testRateLimit() {
    au::Engine eng;
    Seen seen;
    eng.setTap([&](const au::Event& e) { seen.add(e); }, au::familyBit(au::FSession));
    SessionStore st;
    st.setEngine(&eng);
    st.setEventsPerSecond(50);
    CHECK(st.openAt(":memory:", quiet()), "open");
    for (int i = 0; i < 1000; i++) {   // 1000 sessions of one mode ending within a moment
        Session s = mk(9, "k" + std::to_string(i), sesstest::nowMs());
        s.id = st.begin(s);
        s.endWall = s.startWall + 100;
        st.end(s);
    }
    st.flush();
    const StoreStats ss = st.stats();
    CHECK(ss.eventsEmitted >= 45 && ss.eventsEmitted <= 120, "about 50 per second per mode: %llu", (unsigned long long)ss.eventsEmitted);
    CHECK(ss.eventsEmitted + ss.eventsThrottled == 1000, "the rest is counted as throttled: %llu + %llu", (unsigned long long)ss.eventsEmitted, (unsigned long long)ss.eventsThrottled);
    // another mode has its own budget
    const size_t before = seen.n();
    Session o = mk(38, "other", sesstest::nowMs());
    o.id = st.begin(o); o.endWall = o.startWall + 100; st.end(o);
    st.flush();
    CHECK(seen.n() == before + 1, "another mode is not throttled by the first");
    // the database has every session whatever the events did
    Query q; q.mode = 9;
    CHECK(st.query(q).total == 1000, "all 1000 are stored");
    st.close();
    eng.setTap(nullptr, 0);
}

static void testMsgdbLink() {
    au::Engine eng;
    mdb::Db db;
    CHECK(db.open(":memory:"), "msgdb open");
    db.start();
    eng.setTap([&](const au::Event& e) { db.enqueue(e); }, au::familyBit(au::FSession));
    SessionStore st;
    st.setEngine(&eng);
    st.setModeName(9, "DMR");
    CHECK(st.openAt(":memory:", quiet()), "open");
    std::vector<uint64_t> ids;
    for (int i = 0; i < 5; i++) {
        Session s = mk(9, "TG " + std::to_string(90 + i), sesstest::nowMs() - 5000, "DMR TG " + std::to_string(90 + i) + ", 5 s");
        s.alias[0] = "Hidden Name " + std::to_string(i);
        s.id = st.begin(s);
        s.endWall = s.startWall + 5000;
        st.end(s);
        ids.push_back(s.id);
    }
    st.flush();
    db.stop();
    mdb::Filter f;
    f.sessionId = (int64_t)ids[2];
    auto rows = db.query(f);
    CHECK(rows.size() == 1 && rows[0].sessionId == (int64_t)ids[2], "msgdb finds the record by session id, 63 bit ids intact: %zu", rows.size());
    CHECK(rows[0].type == "session.call" && rows[0].mode == "DMR" && rows[0].ident == "TG 92" && rows[0].text == "DMR TG 92, 5 s", "ident = key, text = title");
    CHECK(rows[0].extra.find("Hidden Name") == std::string::npos && rows[0].extra.find("key=") == std::string::npos && rows[0].extra.find("title=") == std::string::npos, "extra has ids and counts only:\n%s", rows[0].extra.c_str());
    CHECK(rows[0].extra.find("session_id=") != std::string::npos && rows[0].extra.find("duration_s=5") != std::string::npos, "extra keeps the ids and counts");
    mdb::Filter all;
    all.type = "session.call";
    CHECK(db.count(all) == 5, "five records");
    // a text search finds the title
    mdb::Filter t;
    t.text = "TG 93";
    CHECK(db.query(t).size() >= 1, "full text finds the title");

    // voice content is still refused; a record of the voice family or an encrypted flag never gets in
    au::Event v("voice.call");
    v.at(1, "DMR", 1);
    mdb::Record r;
    CHECK(!mdb::fromEvent(v, r), "voice events are refused");
    mdb::Record vr; vr.type = "voice.call";
    CHECK(!db.add(vr), "add() refuses voice");
    mdb::Record er; er.type = "acars.message"; er.extra = "encrypted=true\n";
    CHECK(!db.add(er), "add() refuses encrypted");
    // an encrypted session is metadata only and is stored as such
    Session enc = mk(9, "TG 4000", sesstest::nowMs()); enc.verdict = (uint8_t)Verdict::Encrypted;
    au::Event ee("session.call");
    ee.at(sesstest::nowMs() / 1000.0, "DMR", 1).str("key", "TG 4000").str("title", "DMR TG 4000").str("session_id", "123").str("verdict", "encrypted");
    CHECK(mdb::fromEvent(ee, r) && r.sessionId == 123, "an encrypted session's summary has no 'encrypted' field and is kept (metadata)");
    st.close();
    eng.setTap(nullptr, 0);
}

static void testFamilyQuota() {
    mdb::Db db;
    CHECK(db.open(":memory:"), "open");
    const double t0 = 1793000000;
    for (int i = 0; i < 1000; i++) {
        mdb::Record r; r.type = "acars.message"; r.mode = "ACARS"; r.time = t0 + i; r.ident = "A" + std::to_string(i % 20); r.text = "msg " + std::to_string(i);
        db.add(r);
    }
    for (int i = 0; i < 30; i++) {
        mdb::Record r; r.type = "session.call"; r.mode = "DMR"; r.time = t0 + i; r.ident = "TG " + std::to_string(i); r.text = "call"; r.sessionId = 1000 + i;
        db.add(r);
    }
    for (int i = 0; i < 40; i++) {
        mdb::Record r; r.type = "pager.message"; r.mode = "POCSAG"; r.time = t0 + i; r.ident = "1234"; r.text = "page";
        db.add(r);
    }
    mdb::Limits l;
    l.maxRows = 0; l.maxBytes = 0; l.maxAgeDays = 0;
    l.maxRowsPerFamily = 100;
    db.setLimits(l);
    db.prune();
    mdb::Filter fa; fa.type = "acars.message";
    mdb::Filter fs; fs.type = "session.call";
    mdb::Filter fp; fp.type = "pager.message";
    CHECK(db.count(fa) == 100 && db.count(fs) == 30 && db.count(fp) == 40, "a chatty family is cut to its quota, the others are untouched: %lld / %lld / %lld", (long long)db.count(fa), (long long)db.count(fs), (long long)db.count(fp));
    auto rows = db.query(fa);
    CHECK(rows.size() == 100 && rows.back().text == "msg 900", "the oldest of that family went (kept msg 900..999, oldest kept %s)", rows.back().text.c_str());
}

static void testMigration() {
    sesstest::TempDir td;
    const std::string path = td.file("msg.db");
    {   // a message database as the program wrote it before the session link: no sid and fam columns
        sqlite3* d = nullptr;
        CHECK(sqlite3_open(path.c_str(), &d) == SQLITE_OK, "raw open");
        sqlite3_exec(d,
            "CREATE TABLE msgs(id INTEGER PRIMARY KEY, t REAL NOT NULL, type TEXT NOT NULL, mode TEXT NOT NULL DEFAULT '', freq REAL NOT NULL DEFAULT 0,"
            " ident TEXT NOT NULL DEFAULT '', txt TEXT NOT NULL DEFAULT '', lat REAL, lon REAL, extra TEXT NOT NULL DEFAULT '', src TEXT NOT NULL DEFAULT '');"
            "INSERT INTO msgs(t,type,mode,ident,txt) VALUES(1793000000,'acars.message','ACARS','A6-EDA','old message');"
            "INSERT INTO msgs(t,type,mode,ident,txt) VALUES(1793000001,'pager.message','POCSAG','1234','old page');",
            nullptr, nullptr, nullptr);
        sqlite3_close(d);
    }
    mdb::Db db;
    std::string err;
    CHECK(db.open(path, &err), "the old file opens: %s", err.c_str());
    CHECK(db.count(mdb::Filter()) == 2, "old rows kept");
    db.rebuildIndex();   // the old file had no text index for these rows: the application rebuilds after an upgrade
    mdb::Record r; r.type = "session.call"; r.mode = "DMR"; r.time = 1793000002; r.sessionId = 5000000000000000001LL;
    CHECK(db.add(r), "a new record with a session id");
    mdb::Filter f; f.sessionId = 5000000000000000001LL;
    CHECK(db.query(f).size() == 1, "63 bit session id round trips");
    mdb::Limits l; l.maxRows = 0; l.maxBytes = 0; l.maxRowsPerFamily = 1;
    db.setLimits(l);
    mdb::Record a; a.type = "acars.message"; a.mode = "ACARS"; a.time = 1793000003; a.text = "newer";
    db.add(a);
    db.prune();
    mdb::Filter fa; fa.type = "acars.message";
    auto rows = db.query(fa);
    CHECK(rows.size() == 1 && rows[0].text == "newer", "the family of the old rows was filled in, so the quota sees them");
}

int main() {
    testSummaryEvent();
    testNotOnTheBusForPersistence();
    testRateLimit();
    testMsgdbLink();
    testFamilyQuota();
    testMigration();
    return testSummary("session_events");
}
