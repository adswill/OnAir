// Crash in the middle of a write (docs/MODE-EXPERIENCE.md, 2.2 crash recovery and 8.1 test_session_db "kill mid-write, reopen, orphans closed").
// The test runs itself as a child process that writes sessions and then dies without closing anything (_Exit in the middle of an open
// transaction, and in the middle of the store's queue), and checks what the parent finds when it reopens the file:
//  - everything that was committed is there, nothing of the open transaction is, the file passes the integrity check;
//  - the sessions that were still active are closed with the orphanClosed flag at their last update time;
//  - the store starts its id counters above what the file holds, so new sessions never collide with old ones.
#include "dect2/session/session_store.h"
#include "session_test_util.h"
#include "../common/test_util.h"
#include <cstdlib>
#include <cstring>
#include <set>

using namespace dect2::sess;
using sesstest::mk;

static constexpr int64_t kT = 1793000000000LL;

static int runChild(const std::string& exe, const std::string& args) {
    std::string cmd = "\"" + exe + "\" " + args;
#ifdef _WIN32
    cmd = "\"" + cmd + "\"";
#endif
    return std::system(cmd.c_str());
}

// ---- the children: they never return normally
static int childDb(const std::string& path) {
    SessionDb db;
    if (!db.open(path)) return 2;
    db.beginTx();
    for (int i = 0; i < 100; i++) {
        Session s = mk(9, "committed " + std::to_string(i), kT + i * 1000LL, "committed");
        s.id = makeId(1, 9, i + 1);
        if (i % 2) { s.endWall = s.startWall + 500; s.durationMs = 500; }   // half are still active
        db.upsert(s, kT + i * 1000LL + 700);
    }
    db.commitTx();
    db.beginTx();   // the write that is cut off: 5000 rows, no commit
    for (int i = 0; i < 5000; i++) {
        Session s = mk(9, "lost " + std::to_string(i), kT + 200000 + i, "lost " + std::string(200, 'x'));
        s.id = makeId(1, 9, 1000 + i);
        db.upsert(s, kT + 200000 + i);
    }
    std::_Exit(137);   // like a kill: no destructor, no close, no commit
}

static int childStore(const std::string& path) {
    SessionStore st;
    StoreConfig cfg;
    cfg.retentionEverySec = 0;
    if (!st.openAt(path, cfg)) return 2;
    std::vector<Session> mine;
    for (int i = 0; i < 300; i++) {
        Session s = mk(9, "flushed " + std::to_string(i), sesstest::nowMs() - 100000 + i, "flushed");
        s.id = st.begin(s);
        s.eventCount = 3;
        st.update(s);
        mine.push_back(s);
    }
    for (int i = 0; i < 100; i++) { mine[i].endWall = mine[i].startWall + 2000; st.end(mine[i]); }
    st.flush();
    for (int i = 0; i < 200; i++) st.begin(mk(9, "in flight " + std::to_string(i), sesstest::nowMs(), "in flight"));
    std::_Exit(137);   // the writer thread may be in the middle of its transaction
}

static void testDbCrash(const std::string& exe) {
    sesstest::TempDir td;
    const std::string path = td.file("sessions.db");
    runChild(exe, "--child-db \"" + path + "\"");
    SessionDb db;
    OpenInfo oi;
    CHECK(db.open(path, nullptr, &oi) && !oi.quarantined, "the file opens after the crash");
    CHECK(db.integrityOk(), "integrity check passes");
    Query q;
    q.limit = 1;
    CHECK(db.count(q) == 100, "exactly the committed rows are there: %lld", (long long)db.count(q));
    CHECK(db.count([] { Query x; x.text = "lost"; return x; }()) == 0, "nothing of the open transaction");
    // 50 of them are active; the heartbeat is absent (the child never wrote one), so recovery runs
    const int64_t startedMs = kT + 10000000;
    const int n = db.recoverOrphans(startedMs + 1, startedMs, 5000);
    CHECK(n == 50, "50 orphans closed, got %d", n);
    Session g;
    CHECK(db.get(makeId(1, 9, 1), g) && !g.active() && (g.flags & FlagOrphanClosed) && g.endWall == kT + 700, "orphan: end = last update (%lld)", (long long)g.endWall);
    CHECK(db.get(makeId(1, 9, 2), g) && g.endWall == kT + 1500 && !(g.flags & FlagOrphanClosed), "a finished session is as it was");
    Query act; act.activeOnly = true;
    CHECK(db.count(act) == 0, "nothing is active after the recovery");
    // the file is still writable and the id space is intact
    CHECK(db.upsert([] { Session s = mk(9, "after", kT + 20000000); s.id = makeId(1, 9, 5000); return s; }(), kT) && db.count(q) == 101, "writable after the crash");
}

static void testStoreCrash(const std::string& exe) {
    sesstest::TempDir td;
    const std::string path = td.file("sessions.db");
    runChild(exe, "--child-store \"" + path + "\"");
    SessionStore st;
    StoreConfig cfg;
    cfg.retentionEverySec = 0;
    cfg.orphanStaleMs = 0;   // the dead program's heartbeat is fresh, but we know it is gone
    std::string err;
    CHECK(st.openAt(path, cfg, &err), "the store opens after the crash: %s", err.c_str());
    CHECK(st.openInfo().quarantined == false, "no quarantine");
    st.flush();
    Page all;
    {
        Query q; q.limit = 5000; q.sort = Sort::Oldest;
        all = st.query(q);
    }
    CHECK(all.total >= 300 && all.total <= 500, "the flushed sessions and perhaps some of the in-flight ones: %lld", (long long)all.total);
    int orphans = 0, ended = 0, active = 0, flushed = 0;
    std::set<uint64_t> ids;
    for (const Session& s : all.rows) {
        CHECK(ids.insert(s.id).second, "unique ids");
        if (s.active()) active++;
        else if (s.flags & FlagOrphanClosed) orphans++;
        else ended++;
        if (s.title == "flushed") flushed++;
    }
    CHECK(active == 0, "no session is left active, %d are", active);
    CHECK(flushed == 300 && ended == 100 && orphans >= 200, "300 flushed (100 ended cleanly, 200 orphans): %d / %d / %d", flushed, ended, orphans);
    for (const Session& s : all.rows) if (s.flags & FlagOrphanClosed) { CHECK(s.endWall >= s.startWall && s.durationMs == (uint32_t)(s.endWall - s.startWall), "orphan end after its start"); break; }
    // new sessions get ids above everything in the file
    uint64_t maxOld = 0;
    for (const Session& s : all.rows) maxOld = std::max(maxOld, s.id);
    Session ns = mk(9, "new", sesstest::nowMs());
    const uint64_t id = st.begin(ns);
    CHECK(id > maxOld && !ids.count(id), "a new id is above the old ones");
    st.close();
}

int main(int argc, char** argv) {
    if (argc >= 3 && !strcmp(argv[1], "--child-db")) return childDb(argv[2]);
    if (argc >= 3 && !strcmp(argv[1], "--child-store")) return childStore(argv[2]);
    const std::string exe = argv[0];
    testDbCrash(exe);
    testStoreCrash(exe);
    return testSummary("session_crash");
}
