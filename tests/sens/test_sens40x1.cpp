// 4.0 sensitivity pass X1 (docs/SENSITIVITY-4.0-X1.md): information a single-frame decoder throws away. Repeat combining (soft bits of failed frames
// added to the same transmitter's next frames) and long integration over repeated content. Each section runs the receiver twice on the same noise
// (technique on / off), with a carrier error and a clock error, and noise alone (no decode allowed). Simulated signals only.
//
// default: the guard rows (a few seeded trials each, must beat the 3.9 path)
// ONAIR_X1_CURVE=sarsat ONAIR_X1_TRIALS=20 ONAIR_X1_POINTS="2,0,-2,-4": the curve (share identified within 1, 2 and 4 bursts) for both paths
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
    // path 0: 3.9; 1: full burst window; 2: + coherent bits; 3: + repeat combining (the 4.0 receiver)
    rx.setRepeatCombining(path >= 3);
    rx.setWeakBurstPath(path >= 1, path >= 2);
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
                if (!sent.count(e.b.hexId)) { r.wrong++; if (getenv("ONAIR_X1_VERBOSE")) printf("    wrong id %s (sent %s), corrected %d, cost %.2f, combined %d\n", e.b.hexId.c_str(), sent.begin()->c_str(), e.b.corrected, e.b.repairCost, e.combined); continue; }
                if (getenv("ONAIR_X1_VERBOSE") && e.b.corrected) printf("    right id, corrected %d, cost %.2f, combined %d\n", e.b.corrected, e.b.repairCost, e.combined);
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
        printf("COSPAS-SARSAT, %s: share of %d trials identified within 1 / 2 / 4 bursts (3.9 | full burst window | + coherent bits | + combining), 4 bursts 50 s apart\n", sc.name, trials);
        for (double snr : points("ONAIR_X1_POINTS", {2, 0, -1, -2, -3, -4, -5})) {
            int w[4][3] = {{0}}, wrong[4] = {0, 0, 0, 0}, comb = 0;
            std::mutex mu;
            parallel(4 * trials, [&](int j) {
                const int path = j % 4, i = j / 4;
                const SarsatResult r = sarsatTrial(path, snr, sc.cfo, sc.ppm, nb, mix64(0x5a45ull * 7919 + (uint64_t)i + (uint64_t)((snr + 50) * 100) * 131), i % 5);
                std::lock_guard<std::mutex> lk(mu);
                if (r.firstBurst && r.firstBurst <= 1) w[path][0]++;
                if (r.firstBurst && r.firstBurst <= 2) w[path][1]++;
                if (r.firstBurst) w[path][2]++;
                wrong[path] += r.wrong;
                if (path == 3) comb += r.combinedDecodes;
            });
            printf("  %5.1f dB:", snr);
            for (int p = 0; p < 4; p++) printf(" %3.0f/%3.0f/%3.0f%s", 100.0 * w[p][0] / trials, 100.0 * w[p][1] / trials, 100.0 * w[p][2] / trials, p < 3 ? " |" : "");
            printf("  wrong %d|%d|%d|%d, comb %d\n", wrong[0], wrong[1], wrong[2], wrong[3], comb);
            fflush(stdout);
        }
    }
}

void sarsatGuard() {
    // identified within 2 bursts at -1.5 dB in 3 kHz, and within 4 bursts at -3.5 dB: the 3.9 path (best single burst) is clearly below
    struct G { double snr; int within; const Scen* sc; double need; };
    const G rows[] = {{-1.5, 2, &kScen[0], 0.75}, {-1.5, 2, &kScen[1], 0.75}, {-1.5, 2, &kScen[2], 0.75}, {-3.5, 4, &kScen[0], 0.6}};
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

} // namespace

int main(int argc, char** argv) {
#ifndef _WIN32
    setenv("DECT2_NOPACE", "1", 1);
#endif
    const char* curve = getenv("ONAIR_X1_CURVE");
    const std::string only = argc > 1 ? argv[1] : "";
    if (curve && *curve) {
        if (!strcmp(curve, "sarsat")) sarsatCurve();
        return 0;
    }
    if (only.empty() || only == "sarsat") sarsatGuard();
    if (fails) { printf("%d failures\n", fails); return 1; }
    printf("all passed\n");
    return 0;
}
