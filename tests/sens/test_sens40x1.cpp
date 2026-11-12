// 4.0 sensitivity pass X1 (docs/SENSITIVITY-4.0-X1.md): information a single-frame decoder throws away. Repeat combining (soft bits of failed frames
// added to the same transmitter's next frames) and long integration over repeated content. Each section runs the receiver twice on the same noise
// (technique on / off), with a carrier error and a clock error, and noise alone (no decode allowed). Simulated signals only.
//
// default: the guard rows (a few seeded trials each, must beat the 3.9 path)
// ONAIR_X1_CURVE=sarsat ONAIR_X1_TRIALS=20 ONAIR_X1_POINTS="2,0,-2,-4": the curve (share identified within 1, 2 and 4 bursts) for both paths
#include "dect2/ndb/ndb_gen.h"
#include "dect2/ndb/ndb_rx.h"
#include "dect2/ndb/ndb_tel.h"
#include "dect2/sarsat/sarsat_gen.h"
#include "dect2/sarsat/sarsat_rx.h"
#include "dect2/sarsat/sarsat_tel.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace dect2;
using cf = std::complex<float>;

namespace {
int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

uint64_t mix64(uint64_t z) { z += 0x9e3779b97f4a7c15ull; z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull; z = (z ^ (z >> 27)) * 0x94d049bb133111ebull; return z ^ (z >> 31); }

// Runs `jobs` independent trials on up to 3 threads (the Mac is shared)
template <class F> void parallel(int jobs, F f) {
    std::atomic<int> next{0};
    auto work = [&]() { for (int j; (j = next++) < jobs;) f(j); };
    std::vector<std::thread> th;
    const int nt = std::min(3, std::max(1, jobs));
    for (int i = 0; i < nt; i++) th.emplace_back(work);
    for (auto& t : th) t.join();
}

std::vector<double> points(const char* env, std::vector<double> def) {
    const char* p = getenv(env);
    if (!p || !*p) return def;
    std::vector<double> v;
    for (const char* q = p; *q;) { char* e; v.push_back(strtod(q, &e)); if (e == q) break; q = e; while (*q == ',' || *q == ' ') q++; }
    return v;
}
int envInt(const char* n, int def) { const char* p = getenv(n); return p && *p ? atoi(p) : def; }

// ---------------------------------------------------------------- COSPAS-SARSAT 406 MHz: repeat combining

struct SarsatResult {
    int firstBurst = 0;           // 1-based burst after which the beacon was first identified (0: never)
    int decodes = 0, wrong = 0;   // decodes of the right beacon, decodes with an identification that was not sent
    int combinedDecodes = 0;
};

// One beacon (test beacon `which`, 0-based) sending `nb` bursts 50 s +- 2.5 s apart into a fresh receiver at 128 kS/s. snrDb: burst power over the
// noise in 3 kHz (the sens_sweep definition). alt >= 0: every second burst is test beacon `alt` instead (two beacons on one channel), noiseOnly: no signal.
SarsatResult sarsatTrial(int path, double snrDb, double cfoHz, double ppm, int nb, uint64_t seed, int which, int alt = -1, bool noiseOnly = false) {
    const double rate = 128000;
    const std::vector<sarsat::Spec> specs = sarsat::testBeacons();
    std::mt19937_64 rng(seed);
    std::uniform_real_distribution<double> U(0, 1);
    const double carrier = -sarsatTuning().tuneOffsetHz + cfoHz;
    std::vector<std::vector<cf>> burst(2);
    std::set<std::string> sent;
    for (int v = 0; v < 2; v++) {
        const int idx = v == 0 ? which : (alt >= 0 ? alt : which);
        burst[(size_t)v] = sarsat::burstSamples(sarsat::encode(specs[(size_t)idx]), rate, carrier, ppm * 1e-4, false);
        sarsat::Beacon b;
        std::vector<uint8_t> bits = sarsat::encode(specs[(size_t)idx]);
        if (sarsat::decode(bits, b)) sent.insert(b.hexId);
    }
    std::vector<int64_t> start;
    double t = 3.0 + 10.0 * U(rng);
    for (int k = 0; k < nb; k++) { start.push_back((int64_t)(t * rate)); t += 50.0 + 5.0 * (U(rng) - 0.5); }
    const int64_t total = start.back() + (int64_t)burst[0].size() + (int64_t)(2.0 * rate);
    const double sigma = std::sqrt((rate / 3000.0) / std::pow(10.0, snrDb / 10.0) / 2.0);
    std::normal_distribution<float> N(0.f, (float)sigma);
    SarsatReceiver rx;
    rx.configure(rate);
    rx.setSignalOffset(-sarsatTuning().tuneOffsetHz);
    // path 0: 3.9; 1: full burst window and 4-block detection; 2: + coherent bits; 3: + repeat combining (the 4.0 receiver);
    // 4: the 4.0 receiver without the repair-cost check (measurement of that check only)
    rx.setRepeatCombining(path >= 3);
    rx.setWeakBurstPath(path >= 1, path >= 2);
    if (path == 4) rx.setRepairCostLimit(1e9);
    const bool verbose = path == 4 && getenv("ONAIR_X1_VERBOSE");
    std::vector<cf> buf(32768);
    std::vector<cf> rot(nb);
    for (int k = 0; k < nb; k++) rot[(size_t)k] = std::polar(1.f, (float)(2 * M_PI * U(rng)));
    SarsatResult r;
    size_t seen = 0;
    for (int64_t o = 0; o < total; o += (int64_t)buf.size()) {
        const size_t n = (size_t)std::min<int64_t>((int64_t)buf.size(), total - o);
        for (size_t i = 0; i < n; i++) buf[i] = cf(N(rng), N(rng));
        if (!noiseOnly)
            for (int k = 0; k < nb; k++) {
                const std::vector<cf>& b = burst[(size_t)(k & 1)];
                const int64_t s = start[(size_t)k], e = s + (int64_t)b.size();
                if (e <= o || s >= o + (int64_t)n) continue;
                for (int64_t i = std::max(s, o); i < std::min(e, o + (int64_t)n); i++) buf[(size_t)(i - o)] += b[(size_t)(i - s)] * rot[(size_t)k];
            }
        rx.feed(buf.data(), n);
        SarsatTelemetry tel;
        if (rx.telemetry(tel, 0) && tel.blocksOk > seen) {
            for (size_t j = 0; j < tel.blocksOk - seen && j < tel.log.size(); j++) {
                const SarsatEntry& e = tel.log[j];
                if (!sent.count(e.b.hexId)) { r.wrong++; if (verbose) printf("    wrong id %s (sent %s), corrected %d, cost %.2f, combined %d\n", e.b.hexId.c_str(), sent.begin()->c_str(), e.b.corrected, e.b.repairCost, e.combined); continue; }
                if (verbose && e.b.corrected) printf("    right id, corrected %d, cost %.2f, combined %d\n", e.b.corrected, e.b.repairCost, e.combined);
                r.decodes++;
                if (e.combined) r.combinedDecodes++;
                if (!r.firstBurst) {   // which burst had just ended
                    int k = 0;
                    while (k < nb && (double)start[(size_t)k] / rate < e.timeSec - 0.05) k++;
                    r.firstBurst = std::max(1, k);
                }
            }
            seen = tel.blocksOk;
        }
    }
    return r;
}

struct Scen { const char* name; double cfo, ppm; };
const Scen kScen[] = {{"awgn", 0, 0}, {"cfo 20 ppm (8 kHz)", 8120, 0}, {"clock 50 ppm", 0, 50}, {"off-centre +30 kHz", 30000, 0}};

void sarsatCurve() {
    const int trials = envInt("ONAIR_X1_TRIALS", 16), nb = 4;
    for (const Scen& sc : kScen) {
        printf("COSPAS-SARSAT, %s: share of %d trials identified within 1 / 2 / 4 bursts (3.9 | window + 4-block detection | + coherent bits | + combining = 4.0 | 4.0 without the repair-cost check), 4 bursts 50 s apart\n", sc.name, trials);
        for (double snr : points("ONAIR_X1_POINTS", {2, 0, -1, -2, -3, -4, -5})) {
            int w[5][3] = {{0}}, wrong[5] = {0, 0, 0, 0, 0}, comb = 0;
            std::mutex mu;
            parallel(5 * trials, [&](int j) {
                const int path = j % 5, i = j / 5;
                const SarsatResult r = sarsatTrial(path, snr, sc.cfo, sc.ppm, nb, mix64(0x5a45ull * 7919 + (uint64_t)i + (uint64_t)((snr + 50) * 100) * 131), i % 5);
                std::lock_guard<std::mutex> lk(mu);
                if (r.firstBurst && r.firstBurst <= 1) w[path][0]++;
                if (r.firstBurst && r.firstBurst <= 2) w[path][1]++;
                if (r.firstBurst) w[path][2]++;
                wrong[path] += r.wrong;
                if (path == 3) comb += r.combinedDecodes;
            });
            printf("  %5.1f dB:", snr);
            for (int p = 0; p < 5; p++) printf(" %3.0f/%3.0f/%3.0f%s", 100.0 * w[p][0] / trials, 100.0 * w[p][1] / trials, 100.0 * w[p][2] / trials, p < 4 ? " |" : "");
            printf("  wrong %d|%d|%d|%d|%d, combined decodes %d\n", wrong[0], wrong[1], wrong[2], wrong[3], wrong[4], comb);
            fflush(stdout);
        }
    }
}

void sarsatGuard() {
    // identified within 2 bursts at -3 dB in 3 kHz, and within 4 bursts at -5 dB (with the carrier error, the clock error and off centre too): the 3.9
    // receiver identifies nothing below -2 dB
    struct G { double snr; int within; const Scen* sc; double need; };
    const G rows[] = {{-3, 2, &kScen[0], 0.6}, {-3, 2, &kScen[1], 0.6}, {-3, 2, &kScen[2], 0.6}, {-3, 2, &kScen[3], 0.6}, {-5, 4, &kScen[0], 0.4}};
    const int trials = envInt("ONAIR_X1_TRIALS", 8);
    for (const G& g : rows) {
        int ok[2] = {0, 0}, wrong = 0;
        std::mutex mu;
        parallel(2 * trials, [&](int j) {
            const int path = j & 1, i = j >> 1;
            const SarsatResult r = sarsatTrial(path == 1 ? 3 : 0, g.snr, g.sc->cfo, g.sc->ppm, g.within, mix64(0x6a11ull + (uint64_t)i * 104729 + (uint64_t)((g.snr + 50) * 10)), i % 5);
            std::lock_guard<std::mutex> lk(mu);
            if (r.firstBurst) ok[path]++;
            wrong += r.wrong;
        });
        printf("sarsat %s %.1f dB within %d bursts: 3.9 path %d / %d, combining %d / %d, wrong %d\n", g.sc->name, g.snr, g.within, ok[0], trials, ok[1], trials, wrong);
        CHECK(ok[1] >= (int)std::ceil(g.need * trials), "sarsat combining %s %.1f dB: %d of %d", g.sc->name, g.snr, ok[1], trials);
        CHECK(ok[1] > ok[0], "sarsat combining not better than the 3.9 path at %.1f dB (%d vs %d)", g.snr, ok[1], ok[0]);
        CHECK(wrong == 0, "sarsat: %d wrong identifications", wrong);
    }
    {   // noise alone: nothing; two beacons alternating on one channel at the repeat interval (their failed bursts get summed): no identification that was not sent
        int dec = 0, wrong = 0, okAlt = 0;
        std::mutex mu;
        const int n = envInt("ONAIR_X1_NOISE", 4);
        parallel(2 * n, [&](int j) {
            const bool alt = j & 1;
            const SarsatResult r = alt ? sarsatTrial(3, -2.0, 0, 0, 4, mix64(0x77ull + (uint64_t)j), 0, 2)
                                       : sarsatTrial(3, -40.0, 0, 0, 4, mix64(0x99ull + (uint64_t)j), 0, -1, true);
            std::lock_guard<std::mutex> lk(mu);
            if (!alt) dec += r.decodes + r.wrong; else { wrong += r.wrong; okAlt += r.decodes > 0; }
        });
        printf("sarsat noise only (%d x 210 s): %d decodes; two beacons alternating at -2 dB: %d wrong identifications (%d of %d trials identified one)\n", n, dec, wrong, okAlt, n);
        CHECK(dec == 0, "sarsat: %d decodes from noise", dec);
        CHECK(wrong == 0, "sarsat: %d wrong identifications with two beacons", wrong);
    }
}

// ---------------------------------------------------------------- NDB: ident folding (long integration over the repeated ident)

struct NdbResult { bool ok = false; int wrong = 0; };

// 'BND' at 7 WPM on a 400 Hz tone (80 % AM), `secs` seconds from a random point of the ident cycle, 48 kS/s. snrDb: the signal's mean power over the
// noise in 300 Hz (the sens_sweep definition). path 0: 3.9 receiver, 1: 4.0 weak-signal path, 2: + ident folding. noiseOnly: no beacon at all.
NdbResult ndbTrial(int path, double snrDb, double cfoHz, double ppm, double secs, uint64_t seed, bool noiseOnly = false) {
    const double rate = 48000;
    NdbGenConfig c;
    c.rate = rate * (1 + ppm * 1e-6);   // a sample clock error: the receiver takes the samples as 48 kS/s
    c.snrDb = 200; c.cfoHz = cfoHz; c.dialOffsetHz = -ndbTuning().tuneOffsetHz; c.ident = "BND"; c.seed = 1;
    NdbGenerator g(c);
    std::mt19937_64 rng(seed);
    std::uniform_real_distribution<double> U(0, 1);
    std::vector<cf32> buf(24000);
    for (int skip = (int)(U(rng) * 7.0 * rate / buf.size()); skip > 0; skip--) g.generate(buf.data(), buf.size());   // random point of the cycle
    const double p0 = (double)c.amp * c.amp * (1 + 0.8 * 0.8 / 2 * 0.45);   // carrier plus the tone sidebands at about 45 % key-down
    const double sigma = std::sqrt(p0 * (rate / 300.0) / std::pow(10.0, snrDb / 10.0) / 2.0);
    std::normal_distribution<float> N(0.f, (float)sigma);
    NdbReceiver rx;
    rx.setSilent(true);
    rx.configure(rate);
    rx.setSignalOffset(-ndbTuning().tuneOffsetHz);
    rx.setWeakSignalPath(path >= 1);
    rx.setIdentFolding(path >= 2);
    const size_t total = (size_t)(secs * rate);
    for (size_t o = 0; o < total; o += buf.size()) {
        const size_t n = std::min(buf.size(), total - o);
        g.generate(buf.data(), n);
        for (size_t i = 0; i < n; i++) buf[i] = (noiseOnly ? cf32(0, 0) : buf[i]) + cf32(N(rng), N(rng));
        rx.feed(buf.data(), n);
    }
    NdbTelemetry t;
    rx.telemetry(t, 0);
    auto isRot = [](const std::string& w) { return w == "BND" || w == "NDB" || w == "DBN"; };
    NdbResult r;
    r.ok = isRot(t.ident);
    for (const auto& h : t.history) if (!isRot(h)) r.wrong++;
    return r;
}

const Scen kNdbScen[] = {{"awgn", 0, 0}, {"cfo 20 ppm (8 Hz)", 8, 0}, {"clock 50 ppm", 0, 50}, {"off-centre 60 Hz", 60, 0}};

void ndbCurve() {
    const int trials = envInt("ONAIR_X1_TRIALS", 12);
    const double secs = envInt("ONAIR_X1_SECS", 60);
    for (const Scen& sc : kNdbScen) {
        printf("NDB 'BND' 7 WPM, %.0f s, %s: share of %d trials with the ident (3.9 | 4.0 weak path | + ident folding)\n", secs, sc.name, trials);
        for (double snr : points("ONAIR_X1_POINTS", {9, 7, 5, 3, 1, -1, -3})) {
            int ok[3] = {0, 0, 0}, wrong[3] = {0, 0, 0};
            std::mutex mu;
            parallel(3 * trials, [&](int j) {
                const int path = j % 3, i = j / 3;
                const NdbResult r = ndbTrial(path, snr, sc.cfo, sc.ppm, secs, mix64(0x9db0ull + (uint64_t)i * 31 + (uint64_t)((snr + 50) * 100) * 7));
                std::lock_guard<std::mutex> lk(mu);
                ok[path] += r.ok; wrong[path] += r.wrong;
            });
            printf("  %5.1f dB: %3.0f | %3.0f | %3.0f %%   wrong idents %d | %d | %d\n", snr, 100.0 * ok[0] / trials, 100.0 * ok[1] / trials, 100.0 * ok[2] / trials, wrong[0], wrong[1], wrong[2]);
            fflush(stdout);
        }
    }
    int dec = 0;
    std::mutex mu;
    parallel(trials, [&](int j) { const NdbResult r = ndbTrial(2, -40, 0, 0, secs * 2, mix64(0x1dbull + (uint64_t)j), true); std::lock_guard<std::mutex> lk(mu); dec += r.ok + r.wrong; });
    printf("NDB noise only, %d x %.0f s with folding: %d idents\n", trials, secs * 2, dec);
}

void ndbGuard() {
    struct G { double snr; const Scen* sc; double need; };
    const G rows[] = {{4, &kNdbScen[0], 0.6}, {4, &kNdbScen[1], 0.6}, {4, &kNdbScen[2], 0.6}, {4, &kNdbScen[3], 0.6}};
    const int trials = envInt("ONAIR_X1_TRIALS", 6);
    for (const G& gr : rows) {
        int ok[2] = {0, 0}, wrong = 0;
        std::mutex mu;
        parallel(2 * trials, [&](int j) {
            const int path = j & 1, i = j >> 1;
            const NdbResult r = ndbTrial(path ? 2 : 1, gr.snr, gr.sc->cfo, gr.sc->ppm, 60, mix64(0x4dbull + (uint64_t)i * 977));
            std::lock_guard<std::mutex> lk(mu);
            ok[path] += r.ok; wrong += r.wrong;
        });
        printf("ndb %s %.0f dB, 60 s: weak path %d / %d, with folding %d / %d, wrong %d\n", gr.sc->name, gr.snr, ok[0], trials, ok[1], trials, wrong);
        CHECK(ok[1] >= (int)std::ceil(gr.need * trials), "ndb folding %s %.0f dB: %d of %d", gr.sc->name, gr.snr, ok[1], trials);
        CHECK(ok[1] > ok[0], "ndb folding not better than the weak path alone (%d vs %d)", ok[1], ok[0]);
        CHECK(wrong == 0, "ndb: %d wrong idents", wrong);
    }
    int dec = 0;
    std::mutex mu;
    const int n = envInt("ONAIR_X1_NOISE", 4);
    parallel(n, [&](int j) { const NdbResult r = ndbTrial(2, -40, 0, 0, 120, mix64(0x2dbull + (uint64_t)j), true); std::lock_guard<std::mutex> lk(mu); dec += r.ok + r.wrong; });
    printf("ndb noise only (%d x 120 s): %d idents\n", n, dec);
    CHECK(dec == 0, "ndb: %d idents from noise", dec);
}

} // namespace

int main(int argc, char** argv) {
#ifndef _WIN32
    setenv("DECT2_NOPACE", "1", 1);
#endif
    const char* curve = getenv("ONAIR_X1_CURVE");
    const std::string only = argc > 1 ? argv[1] : "";
    if (curve && *curve) {
        if (strstr(curve, "sarsat")) sarsatCurve();
        if (strstr(curve, "ndb")) ndbCurve();
        return 0;
    }
    if (only.empty() || only == "sarsat") sarsatGuard();
    if (only.empty() || only == "ndb") ndbGuard();
    if (fails) { printf("%d failures\n", fails); return 1; }
    printf("all passed\n");
    return 0;
}
