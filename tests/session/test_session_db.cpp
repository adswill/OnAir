// The session database (docs/MODE-EXPERIENCE.md, 2.2): upsert without duplicates, links, notes edited in place, full-text search, paging and
// sorting, the per-mode quota, the size cap, age limits, crash recovery of orphans, a damaged file moved aside, owner-only permissions.
// (The kill-in-the-middle-of-a-write test is test_session_crash.cpp.)
#include "dect2/session/session_db.h"
#include "session_test_util.h"
#include "../common/test_util.h"
#include <cmath>
#include <cctype>
#include <cstring>
#include <fstream>
#ifndef _WIN32
#include <sys/stat.h>
#endif

using namespace dect2::sess;
using sesstest::mk;

static constexpr int64_t kT = 1793000000000LL;   // a fixed wall time

static Session row(uint64_t id, uint16_t mode, const std::string& key, int64_t start, int64_t end = 0, const std::string& title = std::string()) {
    Session s = mk(mode, key, start, title);
    s.id = id;
    s.endWall = end;
    s.durationMs = end ? (uint32_t)(end - start) : 0;
    return s;
}

static void testUpsert() {
    SessionDb db;
    CHECK(db.open(":memory:"), "open");
    Session s = row(makeId(1, 9, 1), 9, "TG 91", kT, 0, "DMR TG 91 Worldwide");
    s.who[0] = "2321001"; s.who[1] = "TG 91"; s.alias[0] = "Jonas"; s.levelDb = -62.5f; s.errRate = 0.03f; s.slot = 1; s.carrierId = 3;
    CHECK(db.upsert(s, kT), "insert");
    CHECK(db.query(Query()).total == 1, "one row");
    // the same id again as an update, then as the end: still one row
    s.eventCount = 5; s.title = "DMR TG 91 Worldwide, 4 s";
    CHECK(db.upsert(s, kT + 1000), "update");
    s.endWall = kT + 4000; s.durationMs = 4000; s.verdict = (uint8_t)Verdict::Clear;
    CHECK(db.upsert(s, kT + 4000), "end");
    Page pg = db.query(Query());
    CHECK(pg.total == 1 && pg.rows.size() == 1, "upsert: no duplicate rows (%lld)", (long long)pg.total);
    const Session& g = pg.rows[0];
    CHECK(g.id == s.id && g.title == "DMR TG 91 Worldwide, 4 s" && g.eventCount == 5 && g.endWall == kT + 4000 && g.durationMs == 4000, "fields replaced");
    CHECK(g.who[0] == "2321001" && g.alias[0] == "Jonas" && g.slot == 1 && g.carrierId == 3 && std::fabs(g.levelDb + 62.5f) < 1e-3 && std::fabs(g.errRate - 0.03f) < 1e-4, "all fields round trip");
    CHECK(!g.hasPosition(), "no position stays none");

    // the star is the user's: an adapter's snapshot never clears it
    CHECK(db.setStarred(s.id, true), "star");
    s.flags = 0;
    CHECK(db.upsert(s, kT + 5000), "snapshot without the star flag");
    Session g2;
    CHECK(db.get(s.id, g2) && (g2.flags & FlagStarred), "the star survives a snapshot");
    CHECK(db.setStarred(s.id, false) && db.get(s.id, g2) && !(g2.flags & FlagStarred), "unstar");

    // links: added once, only for an existing session
    SessionLink l; l.sessionId = s.id; l.type = (uint8_t)LinkType::Audio; l.ref = "/x/y.wav"; l.wall = kT + 4100;
    CHECK(db.addLink(l) && db.addLink(l), "link twice");
    CHECK(db.links(s.id).size() == 1, "the same link once");
    SessionLink bad = l; bad.sessionId = makeId(1, 9, 999);
    db.addLink(bad);
    CHECK(db.links(bad.sessionId).empty() && db.stats().links == 1, "no link for a session that does not exist");

    // notes edited in place
    Note n;
    CHECK(!db.note(s.id, n), "no note yet");
    CHECK(db.setNote(s.id, "heard on the drive", "work,test") && db.note(s.id, n) && n.note == "heard on the drive" && n.tags == "work,test", "note set");
    CHECK(db.setNote(s.id, "edited", "") && db.note(s.id, n) && n.note == "edited" && n.tags.empty(), "note edited in place");
    CHECK(!db.setNote(makeId(1, 9, 4242), "x", "y") || !db.note(makeId(1, 9, 4242), n), "no note for a missing session");
}

static void testQueryAndFts() {
    SessionDb db;
    CHECK(db.open(":memory:"), "open");
    db.beginTx();
    for (int i = 0; i < 300; i++) {
        Session s = row(makeId(1, 9, i + 1), 9, "TG " + std::to_string(i % 10), kT + i * 1000LL, i < 295 ? kT + i * 1000LL + 500 + i : 0,
                        i % 50 == 0 ? "Worldwide call" : "Local call " + std::to_string(i));
        s.receiver = i % 2;
        s.who[0] = "radio" + std::to_string(i % 7);
        if (i % 25 == 0) s.verdict = (uint8_t)Verdict::Encrypted;
        db.upsert(s, kT + i);
    }
    for (int i = 0; i < 50; i++) db.upsert(row(makeId(1, 38, i + 1), 38, "RAN " + std::to_string(i), kT + i * 2000LL, kT + i * 2000LL + 100, "NXDN " + std::to_string(i)), kT);
    db.commitTx();

    Query q;
    q.mode = 9; q.limit = 20;
    Page p = db.query(q);
    CHECK(p.total == 300 && p.rows.size() == 20, "mode 9: 300 rows, a page of 20 (%lld, %zu)", (long long)p.total, p.rows.size());
    // active first (5 rows without an end), then newest first
    int actives = 0;
    for (const Session& s : p.rows) if (s.active()) actives++;
    CHECK(actives == 5 && p.rows[0].active() && p.rows[4].active() && !p.rows[5].active(), "active rows are pinned on top (%d)", actives);
    CHECK(p.rows[5].startWall >= p.rows[6].startWall && p.rows[6].startWall >= p.rows[7].startWall, "then newest first");
    q.offset = 5;
    Page p2 = db.query(q);
    CHECK(p2.rows.size() == 20 && !p2.rows[0].active() && p2.rows[0].id == p.rows[5].id, "offset past the active rows continues with the finished ones");
    q.offset = 3;
    Page p3 = db.query(q);
    CHECK(p3.rows.size() == 20 && p3.rows[0].active() && p3.rows[1].active() && !p3.rows[2].active() && p3.rows[2].id == p.rows[5].id, "an offset inside the active rows");
    q.offset = 290;
    CHECK(db.query(q).rows.size() == 10, "the last page holds the rest");
    q.offset = 0;
    q.activeOnly = true;
    CHECK(db.query(q).rows.size() == 5 && db.query(q).total == 5, "active only");
    q.activeOnly = false;
    q.sort = Sort::Oldest;
    CHECK(db.query(q).rows[0].startWall == kT, "oldest first");
    q.sort = Sort::Longest;
    Page pl = db.query(q);
    CHECK(pl.rows[0].durationMs >= pl.rows[1].durationMs && pl.rows[1].durationMs >= pl.rows[2].durationMs, "longest first");
    q.sort = Sort::Title;
    Page pt = db.query(q);
    auto lower = [](std::string x) { for (char& c : x) c = (char)tolower((unsigned char)c); return x; };
    CHECK(lower(pt.rows[0].title) <= lower(pt.rows[1].title) && lower(pt.rows[1].title) <= lower(pt.rows[2].title), "by title");

    // filters
    Query f;
    f.mode = 9; f.receiver = 1;
    CHECK(db.query(f).total == 150, "receiver filter");
    f = Query(); f.verdict = (int)Verdict::Encrypted;
    CHECK(db.query(f).total == 12, "encrypted filter: %lld", (long long)db.query(f).total);
    f = Query(); f.since = kT + 100000; f.until = kT + 110000;
    CHECK(db.query(f).total >= 11, "time window");
    f = Query(); f.mode = -1; f.limit = 1;
    CHECK(db.query(f).total == 350, "all modes");
    CHECK(db.countsByMode().size() == 2 && db.countsByMode()[38] == 50, "counts by mode");

    // full text: words, prefix, several words ANDed, no match, hostile input
    f = Query(); f.text = "worldwide";
    CHECK(db.query(f).total == 6, "FTS word: %lld", (long long)db.query(f).total);
    f.text = "WORLD*";
    CHECK(db.query(f).total == 6, "FTS prefix, any case");
    f.text = "local call 17";
    CHECK(db.query(f).total == 1, "FTS words ANDed");
    f.text = "radio3";
    CHECK(db.query(f).total > 0, "FTS finds a source id");
    f.text = "nothingmatchesthis";
    CHECK(db.query(f).total == 0, "FTS no match");
    f.text = "\" OR 1=1 -- ; DROP TABLE sessions; ) (";
    db.query(f);   // must not crash or throw
    CHECK(db.query(Query()).total == 350, "hostile search text changed nothing");
    // a title edit is found under the new words and not under the old ones
    Session e = row(makeId(1, 9, 1), 9, "TG 0", kT, kT + 500, "Renamed zebra crossing");
    db.upsert(e, kT);
    f = Query(); f.text = "zebra";
    CHECK(db.query(f).total == 1, "FTS follows an update");
    f.text = "Worldwide";
    CHECK(db.query(f).total == 5, "the old title is gone from the index");
}

static void testQuotaSizeAge() {
    SessionDb db;
    CHECK(db.open(":memory:"), "open");
    db.beginTx();
    for (int i = 0; i < 1000; i++) db.upsert(row(makeId(1, 9, i + 1), 9, "k", kT + i * 1000LL, kT + i * 1000LL + 10), kT);
    for (int i = 0; i < 300; i++) db.upsert(row(makeId(1, 38, i + 1), 38, "k", kT + i * 1000LL, kT + i * 1000LL + 10), kT);
    // one old unfinished (active) and one starred session in mode 9: neither may be removed by the quota
    db.upsert(row(makeId(1, 9, 2000), 9, "active", kT - 5000, 0), kT);
    db.upsert(row(makeId(1, 9, 2001), 9, "starred", kT - 4000, kT - 3000), kT);
    db.commitTx();
    db.setStarred(makeId(1, 9, 2001), true);
    DbLimits lim;
    lim.perModeRows = 400; lim.maxBytes = 0; lim.logDays = 0; lim.positionDays = 0;
    PruneResult pr = db.prune(lim, kT + 2000000);
    Query q9; q9.mode = 9;
    Query q38; q38.mode = 38;
    CHECK(db.count(q9) == 400 && db.count(q38) == 300, "per-mode quota: 9 -> %lld, 38 untouched %lld", (long long)db.count(q9), (long long)db.count(q38));
    CHECK(pr.byQuota == 602, "removed %lld", (long long)pr.byQuota);
    Session g;
    CHECK(db.get(makeId(1, 9, 2000), g) && db.get(makeId(1, 9, 2001), g), "active and starred rows survive the quota");
    CHECK(!db.get(makeId(1, 9, 1), g) && db.get(makeId(1, 9, 1000), g), "the oldest finished rows went, the newest stayed");
    Query f; f.text = "starred";
    CHECK(db.query(f).total == 1, "the full-text index follows the deletes");

    // age: finished, not starred, older than logDays
    SessionDb d2;
    CHECK(d2.open(":memory:"), "open 2");
    const int64_t now = kT + 200LL * 86400000;
    d2.upsert(row(makeId(1, 9, 1), 9, "old", kT, kT + 100), kT);                       // 200 days old
    d2.upsert(row(makeId(1, 9, 2), 9, "oldstar", kT, kT + 100), kT);
    d2.setStarred(makeId(1, 9, 2), true);
    d2.upsert(row(makeId(1, 9, 3), 9, "old active", kT, 0), kT);
    d2.upsert(row(makeId(1, 9, 4), 9, "recent", now - 86400000LL, now - 86400000LL + 100), kT);
    SessionLink pos; pos.sessionId = makeId(1, 9, 4); pos.type = (uint8_t)LinkType::Position; pos.ref = "54.1,25.1"; pos.wall = now - 40LL * 86400000;
    SessionLink pos2 = pos; pos2.ref = "54.2,25.2"; pos2.wall = now - 2LL * 86400000;
    SessionLink aud = pos; aud.type = (uint8_t)LinkType::Audio; aud.ref = "/a.wav"; aud.wall = now - 40LL * 86400000;
    d2.addLink(pos); d2.addLink(pos2); d2.addLink(aud);
    DbLimits l2; l2.perModeRows = 0; l2.maxBytes = 0; l2.logDays = 90; l2.positionDays = 30;
    PruneResult p2 = d2.prune(l2, now);
    CHECK(p2.byAge == 1 && p2.positions == 1, "age: %lld sessions, %lld positions", (long long)p2.byAge, (long long)p2.positions);
    CHECK(d2.get(makeId(1, 9, 2), g) && d2.get(makeId(1, 9, 3), g) && d2.get(makeId(1, 9, 4), g) && !d2.get(makeId(1, 9, 1), g), "old finished unstarred went; starred, active, recent stayed");
    CHECK(d2.links(makeId(1, 9, 4)).size() == 2, "the 40 day old position went, the audio link and the 2 day old position stayed");
}

static void testSizeCapFile() {
    sesstest::TempDir td;
    SessionDb db;
    std::string err;
    CHECK(db.open(td.file("sessions.db"), &err), "open file %s", err.c_str());
    db.beginTx();
    for (int i = 0; i < 20000; i++) {
        Session s = row(makeId(1, 9, i + 1), 9, "k" + std::to_string(i), kT + i * 1000LL, kT + i * 1000LL + 10, "a fairly long title to make the rows bigger " + std::to_string(i));
        s.who[0] = "source identity " + std::to_string(i);
        db.upsert(s, kT);
    }
    db.commitTx();
    const int64_t before = db.stats().bytes;
    DbLimits lim;
    lim.perModeRows = 0; lim.logDays = 0; lim.positionDays = 0; lim.maxBytes = before / 2;
    PruneResult pr = db.prune(lim, kT);
    const int64_t after = db.stats().bytes;
    Query q;
    CHECK(pr.bySize > 0 && db.count(q) < 20000 && db.count(q) > 1000, "size cap removed the oldest (%lld rows left)", (long long)db.count(q));
    CHECK(after <= before / 2 + 65536, "the file shrank: %lld -> %lld (cap %lld)", (long long)before, (long long)after, (long long)before / 2);
    Session g;
    CHECK(db.get(makeId(1, 9, 20000), g) && !db.get(makeId(1, 9, 1), g), "the newest stayed, the oldest went");
    CHECK(db.integrityOk(), "integrity");
}

static void testOrphansAndRecovery() {
    sesstest::TempDir td;
    const std::string path = td.file("sessions.db");
    {
        SessionDb db;
        CHECK(db.open(path), "open");
        db.beginTx();
        db.upsert(row(makeId(1, 9, 1), 9, "done", kT, kT + 1000), kT + 1000);
        db.upsert(row(makeId(1, 9, 2), 9, "running", kT + 2000, 0), kT + 7000);       // last written 5 s after it began
        db.upsert(row(makeId(1, 38, 3), 38, "running too", kT + 3000, 0), kT + 3500);
        db.commitTx();
        db.setMeta("heartbeat", std::to_string(kT + 7000));
    }   // the "crash": the sessions are still active in the file
    {
        SessionDb db;
        OpenInfo oi;
        CHECK(db.open(path, nullptr, &oi), "reopen");
        CHECK(!oi.created && !oi.quarantined, "an existing healthy file");
        const int64_t startedMs = kT + 60000;
        // the heartbeat is fresh: another program instance may still be writing: nothing is touched
        CHECK(db.recoverOrphans(kT + 8000, startedMs, 5000) == -1, "fresh heartbeat: wait");
        Session g;
        db.get(makeId(1, 9, 2), g);
        CHECK(g.active(), "still active while waiting");
        // stale heartbeat: the orphans are closed at the time of their last update
        const int n = db.recoverOrphans(kT + 60000, startedMs, 5000);
        CHECK(n == 2, "two orphans closed, got %d", n);
        CHECK(db.get(makeId(1, 9, 2), g) && !g.active() && g.endWall == kT + 7000 && g.durationMs == 5000 && (g.flags & FlagOrphanClosed), "orphan closed at its last update: end %lld dur %u flags %u", (long long)g.endWall, g.durationMs, g.flags);
        CHECK(db.get(makeId(1, 38, 3), g) && g.endWall == kT + 3500 && (g.flags & FlagOrphanClosed), "the other orphan too");
        CHECK(db.get(makeId(1, 9, 1), g) && g.endWall == kT + 1000 && !(g.flags & FlagOrphanClosed), "a finished session is not an orphan");
        // sessions written by this run are not orphans
        db.upsert(row(makeId(1, 9, 3), 9, "this run", kT + 61000, 0), kT + 61000);
        CHECK(db.recoverOrphans(kT + 120000, startedMs, 0, true) == 0, "this run's sessions are not closed");
        // a clean shutdown ends this run's sessions without the orphan flag
        CHECK(db.closeActive(kT + 70000, startedMs) == 1 && db.get(makeId(1, 9, 3), g) && g.endWall == kT + 70000 && !(g.flags & FlagOrphanClosed), "clean close");
    }
}

static void testCorruptFile() {
    sesstest::TempDir td;
    const std::string path = td.file("sessions.db");
    {
        SessionDb db;
        CHECK(db.open(path), "open");
        db.beginTx();
        for (int i = 0; i < 2000; i++) db.upsert(row(makeId(1, 9, i + 1), 9, "k" + std::to_string(i), kT + i, kT + i + 5), kT);
        db.commitTx();
        db.compact();
    }
    {   // a damaged file: garbage written over the first page header (the file is not a database any more)
        std::fstream f(path, std::ios::in | std::ios::out | std::ios::binary);
        std::string junk(4096, 'x');
        f.write(junk.data(), (std::streamsize)junk.size());
    }
    SessionDb db;
    OpenInfo oi;
    std::string err;
    CHECK(db.open(path, &err, &oi), "a damaged file still opens (a new one is made): %s", err.c_str());
    CHECK(oi.quarantined && !oi.quarantinedTo.empty() && sesstest::fs::exists(sesstest::fs::path(oi.quarantinedTo)), "the damaged file is kept aside");
    CHECK(db.count(Query()) == 0 && db.upsert(row(makeId(1, 9, 1), 9, "new", kT, 0), kT) && db.count(Query()) == 1, "the new file works");
}

static void testTruncatedWal() {
#ifndef _WIN32
    sesstest::TempDir td;
    const std::string path = td.file("sessions.db");
    // write rows without a clean close and cut the write-ahead log in the middle of a frame: SQLite keeps the committed prefix
    {
        SessionDb* db = new SessionDb;
        CHECK(db->open(path), "open");
        for (int c = 0; c < 5; c++) {   // five commits: cutting the log loses the last, not all
            db->beginTx();
            for (int i = c * 100; i < c * 100 + 100; i++) db->upsert(row(makeId(1, 9, i + 1), 9, "k" + std::to_string(i), kT + i, kT + i + 5), kT);
            db->commitTx();
        }
        // keep the handle open (as after a crash): copy the files while it is
        sesstest::fs::copy_file(path, td.file("copy.db"), sesstest::fs::copy_options::overwrite_existing);
        if (sesstest::fs::exists(path + "-wal")) {
            sesstest::fs::copy_file(path + "-wal", td.file("copy.db-wal"), sesstest::fs::copy_options::overwrite_existing);
            const auto sz = sesstest::fs::file_size(td.file("copy.db-wal"));
            if (sz > 5000) sesstest::fs::resize_file(td.file("copy.db-wal"), sz - 1234);   // mid-frame
        }
        delete db;
    }
    SessionDb db;
    OpenInfo oi;
    CHECK(db.open(td.file("copy.db"), nullptr, &oi), "a file with a cut log opens");
    CHECK(!oi.quarantined && db.integrityOk(), "no damage reported for a cut log, integrity ok");
    const int64_t n = db.count(Query());
    CHECK(n >= 100 && n < 500, "a committed prefix is there (%lld rows), the cut commit is not", (long long)n);
#endif
}

static void testPermissions() {
#ifndef _WIN32
    sesstest::TempDir td;
    const std::string dir = td.file("sessions");
    SessionDb db;
    CHECK(db.open(dir + "/sessions.db"), "open in a new folder");
    db.upsert(row(makeId(1, 9, 1), 9, "k", kT, kT), kT);
    struct stat st;
    CHECK(stat(dir.c_str(), &st) == 0 && (st.st_mode & 077) == 0, "the folder is owner only (%o)", (unsigned)(st.st_mode & 0777));
    CHECK(stat((dir + "/sessions.db").c_str(), &st) == 0 && (st.st_mode & 077) == 0, "the database is owner only (%o)", (unsigned)(st.st_mode & 0777));
    if (stat((dir + "/sessions.db-wal").c_str(), &st) == 0) CHECK((st.st_mode & 077) == 0, "the log is owner only (%o)", (unsigned)(st.st_mode & 0777));
#endif
}

static void testMaxSequences() {
    SessionDb db;
    CHECK(db.open(":memory:"), "open");
    db.upsert(row(makeId(5, 9, 17), 9, "a", kT, kT), kT);
    db.upsert(row(makeId(5, 9, 3), 9, "b", kT, kT), kT);
    db.upsert(row(makeId(5, 38, 8), 38, "c", kT, kT), kT);
    db.upsert(row(makeId(6, 9, 2), 9, "d", kT, kT), kT);
    auto m = db.maxSequences();
    CHECK(m.size() == 3, "three (day, mode) groups, got %zu", m.size());
    bool a = false, b = false, c = false;
    for (const auto& e : m) {
        if (e.first.first == 5 && e.first.second == 9 && e.second == 17) a = true;
        if (e.first.first == 5 && e.first.second == 38 && e.second == 8) b = true;
        if (e.first.first == 6 && e.first.second == 9 && e.second == 2) c = true;
    }
    CHECK(a && b && c, "highest sequence of each group");
}

int main() {
    testUpsert();
    testQueryAndFts();
    testQuotaSizeAge();
    testSizeCapFile();
    testOrphansAndRecovery();
    testCorruptFile();
    testTruncatedWal();
    testPermissions();
    testMaxSequences();
    return testSummary("session_db");
}
