// Retention (docs/MODE-EXPERIENCE.md, 2.7): audio and IQ files by age and by size, only files the sessions point at and only inside the library
// folders, starred sessions kept, sidecars with their file, stale links removed, the trash, and the store's own cleanup of sessions.
#include "dect2/session/retention.h"
#include "dect2/session/session_store.h"
#include "session_test_util.h"
#include "../common/test_util.h"
#include <fstream>

using namespace dect2::sess;
using sesstest::mk;
namespace fs = std::filesystem;

static constexpr int64_t kDay = 86400000LL;

static void touch(const std::string& p, size_t n = 1000) {
    fs::create_directories(fs::path(p).parent_path());
    std::ofstream f(p, std::ios::binary);
    f << std::string(n, 'a');
}
static bool has(const std::string& p) { return fs::exists(fs::path(p)); }

struct Rig {
    sesstest::TempDir td;
    SessionDb db;
    uint64_t seq = 0;
    std::string audio, iq, outside;
    Rig() {
        audio = td.file("audio"); iq = td.file("iq"); outside = td.file("outside");
        fs::create_directories(fs::path(audio)); fs::create_directories(fs::path(iq)); fs::create_directories(fs::path(outside));
        db.open(":memory:");
    }
    // a finished session with one linked file
    uint64_t add(LinkType t, const std::string& path, int64_t wall, bool star = false) {
        Session s = mk(9, "k" + std::to_string(seq), wall);
        s.id = makeId(5, 9, (uint32_t)++seq);
        s.endWall = wall + 1000;
        db.upsert(s, wall);
        SessionLink l; l.sessionId = s.id; l.type = (uint8_t)t; l.ref = path; l.wall = wall;
        db.addLink(l);
        if (star) db.setStarred(s.id, true);
        return s.id;
    }
};

static void testAge() {
    Rig r;
    const int64_t now = 1793000000000LL;
    r.add(LinkType::Audio, r.audio + "/2026-09-18/a.wav", now - 40 * kDay);
    touch(r.audio + "/2026-09-18/a.wav"); touch(r.audio + "/2026-09-18/a.json", 50);
    r.add(LinkType::Audio, r.audio + "/2026-10-08/b.wav", now - 20 * kDay);
    touch(r.audio + "/2026-10-08/b.wav");
    r.add(LinkType::Audio, r.audio + "/2026-10-27/c.wav", now - 1 * kDay);
    touch(r.audio + "/2026-10-27/c.wav");
    r.add(LinkType::Audio, r.audio + "/2026-09-10/star.wav", now - 48 * kDay, true);
    touch(r.audio + "/2026-09-10/star.wav");
    // a link to a file outside the library: ignored, never deleted
    r.add(LinkType::Audio, r.outside + "/old.wav", now - 90 * kDay);
    touch(r.outside + "/old.wav");
    // a link whose file the user already deleted
    r.add(LinkType::Audio, r.audio + "/gone.wav", now - 10 * kDay);
    // a file in the library that no link points at (the user's own recording): never touched
    touch(r.audio + "/2026-09-18/users-own.wav");

    LibraryRule rule; rule.roots = {r.audio}; rule.maxAgeDays = 30;
    LibraryResult res = cleanLibrary(r.db, LinkType::Audio, rule, now);
    CHECK(res.filesDeleted == 1 && res.deleted.size() == 1, "only the 40 day old file went: %d", res.filesDeleted);
    CHECK(!has(r.audio + "/2026-09-18/a.wav") && !has(r.audio + "/2026-09-18/a.json"), "the file and its sidecar are gone");
    CHECK(has(r.audio + "/2026-10-08/b.wav") && has(r.audio + "/2026-10-27/c.wav"), "younger files stay");
    CHECK(has(r.audio + "/2026-09-10/star.wav"), "a starred session's audio stays however old");
    CHECK(has(r.outside + "/old.wav"), "a file outside the library is never deleted");
    CHECK(has(r.audio + "/2026-09-18/users-own.wav"), "a file no link points at is never deleted");
    CHECK(res.staleLinks == 1, "the link to the already deleted file is removed: %d", res.staleLinks);
    CHECK(r.db.libraryFiles((uint8_t)LinkType::Audio).size() == 4, "links left: b, c, star, outside = 4, got %zu", r.db.libraryFiles((uint8_t)LinkType::Audio).size());
    CHECK(has(r.audio), "the library folder itself stays");
    // the emptied day folder: here it still holds the user's own file, so it stays
    CHECK(has(r.audio + "/2026-09-18"), "a folder that is not empty stays");
}

static void testSize() {
    Rig r;
    const int64_t now = 1793000000000LL;
    for (int i = 0; i < 5; i++) {
        const std::string p = r.audio + "/day/f" + std::to_string(i) + ".wav";
        touch(p, 1000);
        r.add(LinkType::Audio, p, now - (10 - i) * kDay, i == 1);   // f1 is starred
    }
    LibraryRule rule; rule.roots = {r.audio}; rule.maxBytes = 2500;
    LibraryResult res = cleanLibrary(r.db, LinkType::Audio, rule, now);
    // 5000 bytes, cap 2500: the oldest go first (f0, then f1 is starred and stays, f2, f3 ...) until the total fits
    CHECK(!has(r.audio + "/day/f0.wav") && has(r.audio + "/day/f1.wav") && !has(r.audio + "/day/f2.wav"), "oldest unprotected files go first, the starred one stays");
    int left = 0;
    for (int i = 0; i < 5; i++) if (has(r.audio + "/day/f" + std::to_string(i) + ".wav")) left++;
    CHECK(left == 2 && res.filesDeleted == 3 && res.bytesFreed == 3000, "down to the cap: %d left, %d deleted, %lld bytes", left, res.filesDeleted, (long long)res.bytesFreed);
    // all files of a day folder gone: the folder goes too, the root stays
    Rig r2;
    touch(r2.audio + "/d/x.wav");
    r2.add(LinkType::Audio, r2.audio + "/d/x.wav", now - 99 * kDay);
    LibraryRule rule2; rule2.roots = {r2.audio}; rule2.maxAgeDays = 30;
    cleanLibrary(r2.db, LinkType::Audio, rule2, now);
    CHECK(!has(r2.audio + "/d") && has(r2.audio), "an emptied day folder goes, the root stays");
}

static void testIqAndShared() {
    Rig r;
    const int64_t now = 1793000000000LL;
    touch(r.iq + "/cap1.sigmf-data", 5000); touch(r.iq + "/cap1.sigmf-meta", 100);
    touch(r.iq + "/cap2.sigmf-data", 5000); touch(r.iq + "/cap2.sigmf-meta", 100);
    r.add(LinkType::Iq, r.iq + "/cap1.sigmf-data", now - 10 * kDay);
    r.add(LinkType::Iq, r.iq + "/cap2.sigmf-data", now - 2 * kDay);
    LibraryRule rule; rule.roots = {r.iq}; rule.maxAgeDays = 7;   // IQ: 7 days
    cleanLibrary(r.db, LinkType::Iq, rule, now);
    CHECK(!has(r.iq + "/cap1.sigmf-data") && !has(r.iq + "/cap1.sigmf-meta"), "IQ older than 7 days and its SigMF meta went");
    CHECK(has(r.iq + "/cap2.sigmf-data") && has(r.iq + "/cap2.sigmf-meta"), "newer IQ stays");

    // one file linked by two sessions, one of them starred: it stays
    Rig s;
    touch(s.audio + "/shared.wav");
    s.add(LinkType::Audio, s.audio + "/shared.wav", now - 60 * kDay);
    s.add(LinkType::Audio, s.audio + "/shared.wav", now - 59 * kDay, true);
    LibraryRule rule2; rule2.roots = {s.audio}; rule2.maxAgeDays = 30;
    CHECK(cleanLibrary(s.db, LinkType::Audio, rule2, now).filesDeleted == 0 && has(s.audio + "/shared.wav"), "a shared file is kept while any session starred it");
}

static void testRootsAndTraversal() {
    sesstest::TempDir td;
    const std::string root = td.file("lib");
    fs::create_directories(fs::path(root + "/sub"));
    touch(root + "/sub/a.wav"); touch(td.file("secret.txt"));
    CHECK(insideRoots(root + "/sub/a.wav", {root}), "inside");
    CHECK(!insideRoots(root + "/../secret.txt", {root}), "a .. path out of the library is outside");
    CHECK(!insideRoots(td.file("secret.txt"), {root}) && !insideRoots(root, {root}), "outside; the root itself is not a file inside");
    CHECK(!insideRoots(root + "/sub/a.wav", {}) && !insideRoots(root + "/sub/a.wav", {""}), "no roots, nothing is inside");
    CHECK(!insideRoots(td.file("lib2/a.wav"), {root}), "a sibling folder with the same prefix is outside");
#ifndef _WIN32
    std::error_code ec;
    fs::create_symlink(fs::path(td.file("secret.txt")), fs::path(root + "/link.wav"), ec);
    if (!ec) CHECK(!insideRoots(root + "/link.wav", {root}), "a symbolic link out of the library is outside");
#endif
}

static void testTrash() {
    sesstest::TempDir td;
    const std::string trash = td.file("trash"), f = td.file("lib/call.wav");
    touch(f); touch(td.file("lib/call.json"), 10);
    const int64_t now = 1793000000000LL;
    CHECK(moveToTrash(f, trash, now), "moved");
    CHECK(!has(f) && !has(td.file("lib/call.json")), "file and sidecar left the library");
    CHECK(has(trash + "/2026-10-26/call.wav") || fs::exists(fs::path(trash)), "they are in a dated trash folder");
    int files = 0;
    for (const auto& e : fs::recursive_directory_iterator(fs::path(trash))) if (e.is_regular_file()) files++;
    CHECK(files == 2, "two files in the trash: %d", files);
    CHECK(!moveToTrash(td.file("lib/missing.wav"), trash, now) && !moveToTrash(f, "", now), "nothing to move, or no trash folder: refused");
    // not purged while young; purged after trashDays
    CHECK(purgeTrash(trash, 7, sesstest::nowMs()) == 0, "files that were just trashed stay");
    for (const auto& e : fs::recursive_directory_iterator(fs::path(trash)))
        if (e.is_regular_file()) fs::last_write_time(e.path(), fs::file_time_type::clock::now() - std::chrono::hours(24 * 8));
    CHECK(purgeTrash(trash, 7, sesstest::nowMs()) == 2, "after 8 days they are removed for good");
    int left = 0;
    for (const auto& e : fs::recursive_directory_iterator(fs::path(trash))) { (void)e; left++; }
    CHECK(left == 0, "and the emptied day folder with them");
}

static void testStoreRetention() {
    sesstest::TempDir td;
    const int64_t now = sesstest::nowMs();
    StoreConfig cfg;
    cfg.retentionEverySec = 0;
    cfg.audioRoots = {td.file("audio")};
    cfg.retention.audioDays = 30;
    cfg.retention.logDays = 90;
    cfg.limits.perModeRows = 50;
    SessionStore st;
    CHECK(st.openAt(td.file("sessions/sessions.db"), cfg), "open");
    uint64_t oldId = 0, starId = 0, recentId = 0;
    for (int i = 0; i < 100; i++) {
        Session s = mk(9, "k" + std::to_string(i), now - (200 - i) * kDay);   // 200 .. 101 days old: all beyond 90 days
        s.endWall = s.startWall + 1000;
        const uint64_t id = st.begin(s);
        s.id = id; st.end(s);
        if (i == 0) oldId = id;
        if (i == 1) starId = id;
    }
    for (int i = 0; i < 80; i++) {   // recent: 80 within the last days, over the quota of 50
        Session s = mk(9, "r" + std::to_string(i), now - (80 - i) * 3600000LL);
        s.endWall = s.startWall + 1000;
        s.id = st.begin(s); st.end(s);
        if (i == 79) recentId = s.id;
    }
    st.flush();
    st.setStarred(starId, true);
    // audio: one old, one new
    touch(td.file("audio/old.wav")); touch(td.file("audio/new.wav"));
    st.link(recentId, LinkType::Audio, td.file("audio/new.wav"), now - 3600000);
    st.link(oldId, LinkType::Audio, td.file("audio/old.wav"), now - 45 * kDay);
    st.flush();
    const int deleted = st.runRetention();
    Query q; q.mode = 9; q.limit = 500;
    const Page pg = st.query(q);
    CHECK(deleted == 1 && !has(td.file("audio/old.wav")) && has(td.file("audio/new.wav")), "the 45 day old recording went, the new one stayed (%d)", deleted);
    // sessions: the 100 old ones are beyond 90 days (only the starred one stays); then the quota of 50 for the mode (a starred session is never removed by it)
    bool hasStar = false, hasOld = false;
    for (const Session& s : pg.rows) { if (s.id == starId) hasStar = true; if (s.id == oldId) hasOld = true; }
    CHECK(hasStar && !hasOld, "the starred old session stays, the unstarred old one went");
    CHECK(pg.total == 50, "the quota of 50 holds (the starred one among them, so 49 recent): %lld", (long long)pg.total);
    CHECK(st.window(9).size() > 0, "the window is kept in memory");
    st.close();

    // settings persist: reopen and see the retention, the logging switch and the notice
    SessionStore st2;
    CHECK(st2.openAt(td.file("sessions/sessions.db"), StoreConfig()), "reopen");
    CHECK(st2.retention().audioDays == 30 && st2.retention().logDays == 90, "retention loaded from the settings file");
    st2.setLogging(26, false);
    st2.setLoggingDefault(true);
    st2.acknowledgeFirstRunNotice();
    st2.close();
    SessionStore st3;
    CHECK(st3.openAt(td.file("sessions/sessions.db"), StoreConfig()), "reopen again");
    CHECK(!st3.logging(26) && st3.logging(9) && st3.loggingLabel() == "Logging: ON" && !st3.firstRunNoticeNeeded(), "logging per mode and the acknowledged notice are remembered");
    st3.close();
}

static void testDefaults() {
    SessionStore st;
    CHECK(st.firstRunNoticeNeeded(), "the first-run notice is due until acknowledged");
    CHECK(st.loggingDefault() && st.logging(9) && st.loggingLabel() == "Logging: ON", "session metadata logging is ON by default");
    const StoreConfig c;
    CHECK(c.retention.audioDays == 30 && c.retention.audioMaxBytes == (5ll << 30) && c.retention.logDays == 90 && c.retention.iqDays == 7 && c.retention.iqMaxBytes == (10ll << 30) &&
          c.retention.positionDays == 30 && c.retention.trashDays == 7, "retention defaults: audio 30 days / 5 GB, logs 90 days, IQ 7 days / 10 GB, positions 30 days, trash 7 days");
    CHECK(c.queueMax == 8192 && c.commitMs == 250 && c.commitItems == 256 && c.hotWindow == 200 && c.eventsPerSecPerMode == 50 && c.limits.perModeRows == 200000 && c.limits.maxBytes == (2ll << 30), "queue, commit, window, event and quota defaults");
}

int main() {
    testAge();
    testSize();
    testIqAndShared();
    testRootsAndTraversal();
    testTrash();
    testStoreRetention();
    testDefaults();
    return testSummary("session_retention");
}
