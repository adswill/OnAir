// Large-log performance of the session database (docs/MODE-EXPERIENCE.md, 8.3): 100,000 synthetic sessions in one mode (plus 20,000 in others),
// then the times of an indexed page, a deep page, a full-text search and a filtered sort, measured on the reader connection, against the limits
// of the spec: page under 20 ms, FTS under 200 ms, filtered sort under 300 ms. Also the write speed (rows per second) directly and through the
// store, and the UI list asking the store once per generation change. Prints every number. CTest label: perf.
#include "dect2/session/session_store.h"
#include "session_test_util.h"
#include "../common/test_util.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <random>
#include <vector>

using namespace dect2::sess;
using sesstest::mk;
using Clock = std::chrono::steady_clock;

static constexpr int64_t kT = 1793000000000LL;
static const char* kWords[] = {"worldwide", "local", "emergency", "dispatch", "tower", "ground", "approach", "maintenance", "taxi", "weather", "fire", "rescue",
                               "police", "ambulance", "relay", "test", "roll", "call", "net", "bridge"};

static double ms(Clock::time_point a) { return std::chrono::duration<double, std::milli>(Clock::now() - a).count(); }

// the median of `n` runs of f (milliseconds)
template <class F> static double median(int n, F f) {
    std::vector<double> v;
    for (int i = 0; i < n; i++) { const auto a = Clock::now(); f(); v.push_back(ms(a)); }
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

static Session synth(std::mt19937& rng, uint16_t mode, uint32_t seq, int64_t start) {
    Session s;
    s.id = makeId(20000, mode, seq);
    s.mode = mode;
    s.receiver = (uint16_t)(seq % 2);
    s.key = "TG " + std::to_string(seq % 400) + "/slot" + std::to_string(seq % 2 + 1);
    s.title = std::string("DMR ") + kWords[rng() % 20] + " " + kWords[rng() % 20] + " " + std::to_string(seq) + ", " + std::to_string(rng() % 60) + " s";
    s.who[0] = std::to_string(2320000 + rng() % 5000);
    s.who[1] = "TG " + std::to_string(seq % 400);
    s.alias[0] = "Radio " + std::to_string(rng() % 5000);
    s.startWall = start;
    s.durationMs = (uint32_t)(1000 + rng() % 90000);
    s.endWall = start + s.durationMs;
    s.levelDb = -90.f + (float)(rng() % 400) / 10.f;
    s.errRate = (float)(rng() % 100) / 1000.f;
    s.freqHz = 438500000 + (rng() % 40) * 12500;
    s.slot = (int16_t)(seq % 2 + 1);
    s.verdict = (rng() % 10 == 0) ? (uint8_t)Verdict::Encrypted : (uint8_t)Verdict::Clear;
    s.eventCount = (uint32_t)(rng() % 500);
    return s;
}

int main() {
    sesstest::TempDir td;
    std::mt19937 rng(12345);
    SessionDb db;
    CHECK(db.open(td.file("bench.db")), "open");

    // ---- writing straight into the database, one transaction per 5,000 rows
    const int N = 100000, M = 20000;
    auto w0 = Clock::now();
    for (int base = 0; base < N; base += 5000) {
        db.beginTx();
        for (int i = base; i < base + 5000; i++) db.upsert(synth(rng, 9, (uint32_t)i + 1, kT + (int64_t)i * 4000), kT);
        db.commitTx();
    }
    for (int base = 0; base < M; base += 5000) {
        db.beginTx();
        for (int i = base; i < base + 5000; i++) db.upsert(synth(rng, (uint16_t)(i % 4 == 0 ? 38 : i % 4 == 1 ? 50 : i % 4 == 2 ? 26 : 11), (uint32_t)i + 1, kT + (int64_t)i * 5000), kT);
        db.commitTx();
    }
    const double wms = ms(w0);
    printf("bench: wrote %d rows straight into the database in %.0f ms = %.0f rows per second (batches of 5000, indexes and full-text index maintained)\n", N + M, wms, (N + M) / (wms / 1000.0));
    printf("bench: file %.1f MB for %d rows\n", (double)db.stats().bytes / 1048576.0, N + M);

    // ---- reading
    Query page; page.mode = 9; page.limit = 100;                                   // the Items list: the first page of the mode, active rows first
    const double tPage = median(30, [&] { db.query(page); });
    Query deep = page; deep.offset = 50000;                                         // a deep page
    const double tDeep = median(30, [&] { db.query(deep); });
    Query rx = page; rx.receiver = 1;                                               // receiver chip
    const double tRx = median(30, [&] { db.query(rx); });
    Query fts; fts.mode = 9; fts.text = "emergency"; fts.limit = 100;               // full-text search
    const double tFts = median(15, [&] { db.query(fts); });
    Query ftsTwo; ftsTwo.text = "fire rescue"; ftsTwo.limit = 100;                  // two words, all modes
    const double tFts2 = median(15, [&] { db.query(ftsTwo); });
    Query ftsPre; ftsPre.mode = 9; ftsPre.text = "amb*"; ftsPre.limit = 100;        // prefix
    const double tFtsPre = median(15, [&] { db.query(ftsPre); });
    Query filt; filt.mode = 9; filt.verdict = (int)Verdict::Encrypted; filt.sort = Sort::Longest; filt.limit = 100;   // filtered sort
    const double tFilt = median(15, [&] { db.query(filt); });
    Query filt2; filt2.mode = 9; filt2.sort = Sort::Title; filt2.limit = 100;       // a sort by title over the whole mode
    const double tFilt2 = median(7, [&] { db.query(filt2); });
    Query win; win.mode = 9; win.since = kT + 50000LL * 4000; win.until = kT + 51000LL * 4000; win.sort = Sort::Newest; win.limit = 100;   // time window
    const double tWin = median(30, [&] { db.query(win); });
    Query all; all.limit = 100; all.sort = Sort::Newest;                            // all modes
    const double tAll = median(30, [&] { db.query(all); });
    const Page pg = db.query(page);
    printf("bench: page (mode, active first, newest first) %.2f ms  [limit 20]   total %lld rows, %zu on the page\n", tPage, (long long)pg.total, pg.rows.size());
    printf("bench: deep page (offset 50,000) %.2f ms;  receiver filter %.2f ms;  time window %.2f ms;  all modes newest %.2f ms\n", tDeep, tRx, tWin, tAll);
    printf("bench: full-text: one word %.2f ms, two words (all modes) %.2f ms, prefix %.2f ms  [limit 200]\n", tFts, tFts2, tFtsPre);
    printf("bench: filtered sort (encrypted, longest first) %.2f ms;  sort by title over the mode %.2f ms  [limit 300]\n", tFilt, tFilt2);
    CHECK(pg.total == N && pg.rows.size() == 100, "the mode has %d rows", N);
    CHECK(db.query(fts).rows.size() == 100 && db.query(ftsTwo).total > 0 && db.query(filt).total > 5000, "the searches find rows");

    CHECK(tPage < 20.0, "indexed page under 20 ms: %.2f", tPage);
    CHECK(tDeep < 20.0 + 30.0, "a deep page stays fast: %.2f", tDeep);   // offsets scan: allowed a little more
    CHECK(tRx < 40.0 && tWin < 20.0 && tAll < 20.0, "the other indexed pages under 20 ms: %.2f %.2f %.2f", tRx, tWin, tAll);
    CHECK(tFts < 200.0 && tFts2 < 200.0 && tFtsPre < 200.0, "full-text under 200 ms: %.2f %.2f %.2f", tFts, tFts2, tFtsPre);
    CHECK(tFilt < 300.0 && tFilt2 < 300.0, "filtered sort under 300 ms: %.2f %.2f", tFilt, tFilt2);

    // ---- through the store: the writer thread's own speed, and the UI list asking once per generation change
    SessionStore st;
    StoreConfig cfg;
    cfg.retentionEverySec = 0;
    cfg.loadSalt = false;
    cfg.queueMax = 1 << 20;   // the whole batch fits: this measures the writer, not the drop path
    CHECK(st.openAt(td.file("store.db"), cfg), "open store");
    const int K = 100000;
    std::vector<Session> batch;
    batch.reserve(K);
    for (int i = 0; i < K; i++) { Session s = synth(rng, 9, 0, kT + (int64_t)i * 4000); batch.push_back(std::move(s)); }
    const auto e0 = Clock::now();
    for (Session& s : batch) { s.id = st.begin(s); s.endWall = s.startWall + s.durationMs; st.end(s); }
    const double enq = ms(e0);
    st.flush();
    const double total = ms(e0);
    printf("bench: store: %d sessions (begin + end = %d items) enqueued in %.0f ms (%.0f items per second from the decoder side), all written after %.0f ms = %.0f sessions per second written\n",
           K, 2 * K, enq, 2.0 * K / (enq / 1000.0), total, K / (total / 1000.0));
    Query q9; q9.mode = 9;
    CHECK(st.query(q9).total == K && st.dropped() == 0, "all sessions written, none dropped");

    CachedList list(st);
    Query lq; lq.mode = 9; lq.limit = 100;
    list.setQuery(lq);
    sesstest::waitFor([&] { list.page(); return !list.loading(); });
    const uint64_t r0 = list.refreshes();
    for (int i = 0; i < 20000; i++) list.page();
    CHECK(list.refreshes() == r0, "20,000 frames, no change: the list copied no page (refreshes %llu -> %llu)", (unsigned long long)r0, (unsigned long long)list.refreshes());
    Session extra = synth(rng, 9, 0, kT + (int64_t)K * 4000);
    st.begin(extra);
    st.flush();
    sesstest::waitFor([&] { return list.page()->total == K + 1; });
    for (int i = 0; i < 20000; i++) list.page();
    CHECK(list.refreshes() <= r0 + 3, "one session more: at most a few page reads, not one per frame (%llu)", (unsigned long long)(list.refreshes() - r0));
    st.close();
    return testSummary("session_bench");
}
