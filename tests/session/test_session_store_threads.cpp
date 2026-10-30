// The SessionStore's threads (docs/MODE-EXPERIENCE.md, 2.2a and 8.1 test_session_store_threads): one writer thread behind a bounded queue, a drop
// counter that is exact, enqueue that never blocks, decoder threads writing while readers query. Meant to run under ThreadSanitizer too
// (cmake -DDECT2_SANITIZE_THREAD=ON; the TSAN job of ci.yml builds and runs just this program).
#include "dect2/session/session_store.h"
#include "session_test_util.h"
#include "../common/test_util.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <set>
#include <thread>
#include <vector>

using namespace dect2::sess;
using sesstest::mk;

#if defined(__SANITIZE_THREAD__)
#define UNDER_TSAN 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define UNDER_TSAN 1
#endif
#endif
#ifdef UNDER_TSAN
static constexpr int64_t kMaxCallNs = 200LL * 1000 * 1000;   // TSAN slows everything several times
#else
static constexpr int64_t kMaxCallNs = 20LL * 1000 * 1000;
#endif

static StoreConfig quiet() {
    StoreConfig c;
    c.retentionEverySec = 0;
    return c;
}

// the writer is held: enqueue is a push or a counted drop, and never waits for it
static void testEnqueueNeverBlocks() {
    SessionStore st;
    StoreConfig cfg = quiet();
    cfg.queueMax = 256;
    CHECK(st.openAt(":memory:", cfg), "open");
    st.pauseWriter(true);
    std::atomic<int64_t> worst{0};
    std::vector<std::thread> th;
    for (int t = 0; t < 4; t++) th.emplace_back([&, t] {
        for (int i = 0; i < 5000; i++) {
            const auto t0 = std::chrono::steady_clock::now();
            Session s = mk(9, "t" + std::to_string(t) + "/" + std::to_string(i), sesstest::nowMs());
            s.id = st.begin(s);
            st.update(s);
            st.link(s.id, LinkType::Telemetry, "x");
            const int64_t d = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
            int64_t w = worst.load();
            while (d > w && !worst.compare_exchange_weak(w, d)) {}
        }
    });
    for (auto& x : th) x.join();
    // 4 threads x 5000 x (begin + update + link) = 60,000 offers into a queue of 256 with nobody taking from it
    const StoreStats s = st.stats();
    CHECK(s.enqueued == 256, "exactly the queue's size was accepted: %llu", (unsigned long long)s.enqueued);
    CHECK(s.dropped == 60000 - 256, "the drop counter is exact: %llu (want %d)", (unsigned long long)s.dropped, 60000 - 256);
    CHECK(s.queued == 256, "256 waiting");
    CHECK(worst.load() < kMaxCallNs, "no begin + update + link took long: worst %.3f ms", worst.load() / 1e6);
    CHECK(s.maxEnqueueNs < kMaxCallNs, "the store's own timing agrees: %.3f ms", s.maxEnqueueNs / 1e6);
    st.pauseWriter(false);
    st.flush();
    CHECK(st.stats().queued == 0 && st.stats().applied == 256, "the writer takes up what was accepted: applied %llu", (unsigned long long)st.stats().applied);
    st.close();
}

// every item carries the whole session, so a dropped begin is healed by the next update
static void testDroppedBeginHeals() {
    SessionStore st;
    StoreConfig cfg = quiet();
    cfg.queueMax = 16;
    CHECK(st.openAt(":memory:", cfg), "open");
    st.pauseWriter(true);
    std::vector<Session> all;
    for (int i = 0; i < 40; i++) {
        Session s = mk(9, "k" + std::to_string(i), sesstest::nowMs() + i);
        s.id = st.begin(s);
        all.push_back(s);
    }
    CHECK(st.dropped() == 24, "24 begins dropped: %llu", (unsigned long long)st.dropped());
    st.pauseWriter(false);
    st.flush();
    CHECK(st.query([] { Query q; q.mode = 9; return q; }()).total == 16, "only 16 began");
    for (size_t i = 0; i < all.size(); i += 8) {   // updates in small groups so none is dropped again
        for (size_t j = i; j < std::min(all.size(), i + 8); j++) { all[j].eventCount = 7; st.update(all[j]); }
        st.flush();
    }
    CHECK(st.dropped() == 24, "no more drops");
    CHECK(st.query([] { Query q; q.mode = 9; return q; }()).total == 40, "the update brought back all 40 sessions");
    st.close();
}

// decoder threads write, readers query, a user thread stars and changes settings: no data race (TSAN), no lost session, no stuck flush
static void testConcurrent() {
    SessionStore st;
    StoreConfig cfg = quiet();
    CHECK(st.openAt(":memory:", cfg), "open");
    std::atomic<bool> stop{false};
    std::atomic<int> began{0}, ended{0};
    std::vector<std::thread> producers, readers;
    const int nProd = 6, perProd = 1500;
    for (int t = 0; t < nProd; t++) producers.emplace_back([&, t] {
        const uint16_t mode = (uint16_t)(t % 3 == 0 ? 9 : t % 3 == 1 ? 38 : 26);
        for (int i = 0; i < perProd; i++) {
            Session s = mk(mode, "p" + std::to_string(t) + "/" + std::to_string(i), sesstest::nowMs(), "call " + std::to_string(i), (uint16_t)(t % 2));
            s.id = st.begin(s);
            began++;
            s.eventCount = 3; s.levelDb = -70.f; s.snrDb = 12.f;
            st.update(s);
            if (i % 3 == 0) st.link(s.id, LinkType::Telemetry, "lvl=" + std::to_string(i));
            if (i % 2 == 0) { s.endWall = s.startWall + 1500; st.end(s); ended++; }
            if (i % 200 == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));   // the queue is 8192: producers stay below what the writer takes
            while (st.stats().queued > 3000) std::this_thread::sleep_for(std::chrono::milliseconds(2));   // a slow or busy machine: wait for the writer
        }
    });
    for (int r = 0; r < 3; r++) readers.emplace_back([&, r] {
        CachedList list(st);
        Query q; q.mode = 9; q.limit = 50;
        list.setQuery(q);
        while (!stop) {
            auto p = list.page();
            (void)p->rows.size();
            auto w = st.window(38, r % 2, 100);
            (void)w.size();
            (void)st.gen(9); (void)st.genAll();
            Query qq; qq.mode = 26; qq.text = "call"; qq.limit = 20;
            st.query(qq);
            st.stats();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
    std::thread user([&] {
        int i = 0;
        while (!stop) {
            st.setLogging(9, (i++ % 7) != 0);   // switched off now and then: the window still works
            auto w = st.window(9, -1, 10);
            if (!w.empty() && st.stats().queued < 3000) { st.setStarred(w[0].id, i % 2); st.setNote(w[0].id, "note " + std::to_string(i), "t"); }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        st.setLogging(9, true);
    });
    for (auto& p : producers) p.join();
    st.flush();
    stop = true;
    for (auto& r : readers) r.join();
    user.join();
    st.flush();
    const StoreStats s = st.stats();
    CHECK(s.dropped == 0, "nothing dropped at this rate: %llu", (unsigned long long)s.dropped);
    CHECK(began == nProd * perProd, "all began");
    // windows hold the newest 200 per mode and receiver; the database holds what was logged (mode 9 was off for a while)
    for (uint16_t m : {(uint16_t)38, (uint16_t)26}) {
        Query q; q.mode = m;
        CHECK(st.query(q).total == 2 * perProd, "mode %u: every session is in the database (%lld)", m, (long long)st.query(q).total);
    }
    Query q9; q9.mode = 9;
    CHECK(st.query(q9).total <= 2 * perProd && st.query(q9).total > 0, "mode 9: partly logged (it was off at times): %lld", (long long)st.query(q9).total);
    CHECK(st.window(9).size() == 200, "the window of mode 9 is full (capped by the default max of 200)");
    CHECK(st.window(9, 0, 1000).size() == 200 && st.window(9, 1, 1000).size() == 200, "200 per receiver");
    Query qa; qa.mode = 38; qa.limit = 5000;
    int active = 0;
    for (const Session& x : st.query(qa).rows) if (x.active()) active++;
    CHECK(active == perProd, "half of the sessions of mode 38 are still active (never ended): %d", active);
    st.close();
}

// the generation counter, and a list that asks again only when it changed
static void testGenerationAndCachedList() {
    SessionStore st;
    CHECK(st.openAt(":memory:", quiet()), "open");
    const uint64_t g0 = st.gen(9);
    const uint64_t other0 = st.gen(10);   // a mode in another bucket
    Session s = mk(9, "a", sesstest::nowMs());
    s.id = st.begin(s);
    st.flush();
    CHECK(st.gen(9) != g0, "gen changed after a session began");
    CHECK(st.gen(10) == other0, "the generation of another mode did not");

    CachedList list(st);
    Query q; q.mode = 9; q.limit = 20;
    list.setQuery(q);
    CHECK(list.loading(), "loading before the first page");
    CHECK(sesstest::waitFor([&] { list.page(); return !list.loading(); }), "the first page arrives");
    CHECK(list.page()->rows.size() == 1, "one row");
    const uint64_t r0 = list.refreshes();
    for (int i = 0; i < 2000; i++) { list.page(); if (i % 100 == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
    CHECK(list.refreshes() == r0, "2000 frames with nothing changed ask the store 0 more times (%llu -> %llu)", (unsigned long long)r0, (unsigned long long)list.refreshes());
    // a change: one more page read (the generation moves when the session is written to the window and again when it is committed)
    Session s2 = mk(9, "b", sesstest::nowMs() + 1);
    st.begin(s2);
    st.flush();
    CHECK(sesstest::waitFor([&] { return list.page()->rows.size() == 2; }), "the new row shows up");
    const uint64_t r1 = list.refreshes();
    CHECK(r1 > r0 && r1 <= r0 + 3, "a change costs a page or two, not one per frame: %llu -> %llu", (unsigned long long)r0, (unsigned long long)r1);
    for (int i = 0; i < 1000; i++) list.page();
    CHECK(list.refreshes() == r1, "and then it is quiet again");
    // a different query invalidates the page
    q.text = "zzz";
    list.setQuery(q);
    CHECK(sesstest::waitFor([&] { return list.page()->rows.empty() && !list.loading(); }), "the new query is applied");
    // a list that is destroyed while its page is on the way does not crash
    {
        CachedList tmp(st);
        tmp.setQuery(Query());
        tmp.page();
    }
    st.close();
}

// the hot window: newest first, trimmed, per receiver; stars survive snapshots
static void testWindow() {
    SessionStore st;
    StoreConfig cfg = quiet();
    cfg.hotWindow = 50;
    CHECK(st.openAt(":memory:", cfg), "open");
    const int64_t t0 = sesstest::nowMs();
    uint64_t first = 0;
    for (int i = 0; i < 80; i++) {
        Session s = mk(9, "k" + std::to_string(i), t0 + i * 10LL, "t", (uint16_t)(i % 2));
        const uint64_t id = st.begin(s);
        if (i == 0) first = id;
    }
    st.flush();
    CHECK(st.window(9, 0).size() == 40 && st.window(9, 1).size() == 40, "50 per mode and receiver is the cap; 40 each fit: %zu / %zu", st.window(9, 0).size(), st.window(9, 1).size());
    auto w = st.window(9, -1, 200);
    CHECK(w.size() == 80, "both receivers together %zu", w.size());
    bool sorted = true;
    for (size_t i = 1; i < w.size(); i++) if (w[i - 1].startWall < w[i].startWall) sorted = false;
    CHECK(sorted, "newest first");
    CHECK(st.window(9, -1, 10).size() == 10, "max honoured");
    // star and snapshot
    st.setStarred(w.back().id, true);
    st.flush();
    Session snap = w.back();
    snap.flags = 0;
    snap.eventCount = 9;
    st.update(snap);
    st.flush();
    bool starred = false;
    for (const Session& x : st.window(9, -1, 200)) if (x.id == snap.id) starred = (x.flags & FlagStarred) && x.eventCount == 9;
    CHECK(starred, "the star survives a snapshot");
    (void)first;
    st.close();
}

int main() {
    testEnqueueNeverBlocks();
    testDroppedBeginHeals();
    testWindow();
    testGenerationAndCachedList();
    testConcurrent();
    return testSummary("session_store_threads");
}
