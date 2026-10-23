// Whole-span TV for DVB-S / S2 (docs/WHOLESPAN-TV-DESIGN.md 2.1): narrow transponders of different symbol rates side by side in one IF capture,
// one receiver each. Every transponder must be found, get its own receiver at its own rate, lock, and show its own named service in the merged
// list with its signal; the selected one's packets reach the sink and switching moves them; "selected only" runs one receiver; the cap and the CPU
// budget hold. Cases: an RTL-SDR (2.4 Msps, 8 bit, three SCPC carriers), 8 Msps with three carriers, 20 Msps with a big and two small ones.
#include "dect2/engine/spantv/spantv.h"
#include "dect2/t2/t2rx.h"
#include "spantv_testkit.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
using namespace dect2;
using namespace spantvtest;
#include "../common/test_util.h"

struct Result { std::vector<TvChannelRow> rows; std::vector<MultiService> svc; int running = 0; double wall = 0; std::map<int, int> sinkPk; };

static Result runCase(const char* title, const std::vector<SatSpec>& sats, double rate, double secs, bool quantise, int maxCh = 3, bool decodeAll = true,
                      double budget = 0, bool switchHalf = false) {
    printf("%s\n", title);
    SatCapture cap(sats, rate, 2, quantise);
    SpanTv tv;
    SpanTvOptions o; o.stdMode = 8; o.spanRate = rate; o.maxChannels = maxCh; o.decodeAll = decodeAll; o.cpuBudgetCores = budget; o.blocking = true;
    CHECK(tv.configure(o), "configure");
    Result r;
    std::mutex mu;
    tv.setSink([&](int slot, const uint8_t*, size_t n, double, const TsSnapshot&) { std::lock_guard<std::mutex> lk(mu); r.sinkPk[slot] += (int)n; });
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<cf32> blk;
    RxTelemetry t;
    const size_t total = (size_t)(rate * secs);
    bool switched = false;
    for (size_t done = 0; done < total; done += 1 << 15) {
        cap.pull(1 << 15, blk);
        tv.feed(blk.data(), blk.size());
        tv.poll(t);
        if (switchHalf && !switched && done > total / 2) {   // play the highest transponder from now on
            const auto rows = tv.channels();
            if (!rows.empty()) { tv.setSelected(rows.back().slot); switched = true; }
        }
    }
    tv.flush();
    for (int i = 0; i < 100; i++) { tv.poll(t); r.svc = tv.services(); if (r.svc.size() >= sats.size() || !decodeAll) break; std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
    r.rows = tv.channels();
    r.running = (int)tv.runningSlots().size();
    r.wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    for (const auto& row : r.rows)
        printf("  signal %d %+8.1f kHz  bw %7.1f kHz  Rs %7.1f kBd  roll-off %.2f %s%s%s %s  snr %4.1f dB  %d services  CPU %.0f %%  %s\n", row.slot, row.offsetHz / 1e3, row.bwHz / 1e3,
               row.rsHz / 1e3, row.rollOff, row.running ? "running" : "idle", row.locked ? " locked" : "", row.noLock ? " no-lock" : "", row.selected ? "[playing]" : "", row.snrDb, row.services,
               row.cpuPct, row.params.c_str());
    for (const auto& s : r.svc) printf("  service %d \"%s\" on signal %d (%+.1f kHz, %.1f kBd)\n", s.svc.id, s.svc.name.c_str(), s.slot, s.offsetHz / 1e3, s.rsHz / 1e3);
    for (const auto& kv : r.sinkPk) printf("  sink: %d packets from signal %d\n", kv.second, kv.first);
    printf("  %d receivers, %.1f s of signal in %.1f s, pool %.2f cores\n", r.running, secs, r.wall, tv.poolCores());
    tv.reset();
    return r;
}

static const TvChannelRow* rowNear(const Result& r, double hz, double tol) {
    for (const auto& row : r.rows) if (std::fabs(row.offsetHz - hz) < tol) return &row;
    return nullptr;
}
static bool hasService(const Result& r, const std::string& name, double hz, double tol) {
    for (const auto& s : r.svc) if (s.svc.name == name && std::fabs(s.offsetHz - hz) < tol) return true;
    return false;
}

static void checkAll(const char* title, const Result& r, const std::vector<SatSpec>& sats) {
    CHECK(r.running == (int)sats.size(), "%s: %d receivers, want %zu", title, r.running, sats.size());
    for (const SatSpec& s : sats) {
        const TvChannelRow* row = rowNear(r, s.offsetHz, 0.1 * s.rs);
        CHECK(row != nullptr, "%s: no signal at %+.0f kHz", title, s.offsetHz / 1e3);
        if (!row) continue;
        CHECK(row->locked, "%s: %+.0f kHz not locked", title, s.offsetHz / 1e3);
        CHECK(std::fabs(row->rsHz / s.rs - 1) < 0.01, "%s: %+.0f kHz Rs %.1f kBd, want %.1f", title, s.offsetHz / 1e3, row->rsHz / 1e3, s.rs / 1e3);
        CHECK(hasService(r, s.name, s.offsetHz, 0.1 * s.rs), "%s: service \"%s\" missing", title, s.name.c_str());
    }
}

int main() {
    const bool quick = getenv("SPANTV_QUICK") != nullptr;
    {   // RTL-SDR: three SCPC carriers in 2.4 Msps, 8-bit samples, a 2 dB IF tilt
        std::vector<SatSpec> s = {{2, dvbs::kQpsk, 5, 500e3, 0.35, -720e3, 14, 101, "Scpc One", false, false, 1},
                                  {1, dvbs::kQpsk, 0, 333e3, 0.35, 0, 14, 102, "Scpc Two", false, false, 2},
                                  {2, dvbs::k8psk, 4, 400e3, 0.20, 700e3, 16, 103, "Scpc Three", false, false, 3}};
        Result r = runCase("RTL-SDR 2.4 Msps: three SCPC carriers (S2 QPSK 500 kBd, DVB-S 333 kBd, S2 8PSK 400 kBd), playing switches half way", s, 2.4e6, quick ? 5 : 8, true, 3, true, 0, true);
        checkAll("2.4 Msps", r, s);
        int sources = 0;
        for (auto& kv : r.sinkPk) sources += kv.second > 0;
        CHECK(sources == 2, "the sink got packets from %d signals (before and after the switch)", sources);
    }
    {   // 8 Msps: 2 / 1 / 1.5 MBd
        std::vector<SatSpec> s = {{2, dvbs::kQpsk, 6, 2e6, 0.25, -2.2e6, 13, 201, "Mid One", false, false, 4},
                                  {1, dvbs::kQpsk, 2, 1e6, 0.35, 0, 13, 202, "Mid Two", false, false, 5},
                                  {2, dvbs::k8psk, 5, 1.5e6, 0.20, 2.3e6, 16, 203, "Mid Three", false, false, 6}};
        Result r = runCase("8 Msps: three carriers (S2 QPSK 2 MBd, DVB-S 1 MBd, S2 8PSK 1.5 MBd)", s, 8e6, quick ? 3 : 5, false);
        checkAll("8 Msps", r, s);
    }
    if (!quick) {   // 20 Msps: 5 / 2 / 3 MBd
        std::vector<SatSpec> s = {{2, dvbs::kQpsk, 5, 5e6, 0.35, -5.5e6, 13, 301, "Wide One", false, false, 7},
                                  {1, dvbs::kQpsk, 1, 2e6, 0.35, 0, 12, 302, "Wide Two", false, false, 8},
                                  {2, dvbs::k8psk, 5, 3e6, 0.25, 5e6, 16, 303, "Wide Three", false, false, 9}};
        Result r = runCase("20 Msps: three carriers (S2 QPSK 5 MBd, DVB-S 2 MBd, S2 8PSK 3 MBd)", s, 20e6, 3, false);
        checkAll("20 Msps", r, s);
    }
    {   // the cap: three carriers, two receivers (the strongest two)
        std::vector<SatSpec> s = {{2, dvbs::kQpsk, 5, 500e3, 0.35, -720e3, 12, 101, "A", false, false, 1},
                                  {2, dvbs::kQpsk, 5, 333e3, 0.35, 0, 18, 102, "B", false, false, 2},
                                  {2, dvbs::kQpsk, 5, 400e3, 0.35, 700e3, 15, 103, "C", false, false, 3}};
        Result r = runCase("cap of 2 on three carriers", s, 2.4e6, 3, false, 2);
        CHECK(r.running == 2, "cap 2: %d receivers", r.running);
        int seen = 0;
        for (auto& row : r.rows) seen += row.occupied;
        CHECK(seen == 3, "all three carriers listed (%d)", seen);
        // selected only: one receiver
        Result q = runCase("selected only", s, 2.4e6, 3, false, 3, false);
        CHECK(q.running == 1, "selected only: %d receivers", q.running);
        // a CPU budget that holds about one small receiver beside the selected one
        Result b = runCase("CPU budget 0.12 cores", s, 2.4e6, 3, false, 3, true, 0.12);
        CHECK(b.running == 2, "budget: %d receivers", b.running);
    }
    return testSummary("spantv_dvbs");
}
