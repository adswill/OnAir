// Sensitivity, offset tolerance, lock time and CPU of the aviation, marine, IoT and drone receivers (docs/SENSITIVITY-3.9-C.md).
// Every mode's own test signal (makeModeSynth, the same as tools/sens/sens_modes.h) goes through the impairment and white noise into a fresh receiver;
// a trial passes when the receiver counted at least half the valid units (CRC / FEC passed) of a clean run, as in sens_sweep.
//   default: a short guard: two trials per mode at the guard SNR (the 3.9 threshold + 2 dB) must pass, and noise alone must give nothing
//   ONAIR_TEST_LONG=1: the full measurement: threshold for 90 % (1 dB steps, 10 trials), the carrier offset and clock error still decoded at
//     threshold + 3 dB, lock time and CPU per second of signal; ONAIR_SENS_MODES=<regex> picks modes
// Simulated signals only: no recording is involved.
#include "sens_modes.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <regex>
#include <thread>

using namespace sens;

namespace {

struct Prep { Sig base; double power = 0; size_t trialN = 0, maxOff = 0; double need = 1; };
struct Res { bool ok = false; double lockS = -1, good = 0; };

Res trial(const Mode& m, const Prep& P, double snrDb, double cfoHz, double sroPpm, bool noiseOnly, uint64_t seed) {
    std::mt19937 rng((uint32_t)(seed ^ (seed >> 32)));
    const size_t off = P.maxOff ? (size_t)(mix64(seed ^ 0x51ed) % P.maxOff) : 0;
    Sig x(P.base.begin() + (long)off, P.base.begin() + (long)std::min(P.base.size(), off + P.trialN + 64));
    if (sroPpm != 0) x = clockError(x, sroPpm);
    if (x.size() > P.trialN) x.resize(P.trialN);
    if (noiseOnly) std::fill(x.begin(), x.end(), cf32(0, 0));
    std::uniform_real_distribution<double> U(0, 2 * M_PI);
    shift(x, cfoHz, m.rate, U(rng));
    addNoise(x, P.power * (m.rate / m.bwHz) / std::pow(10.0, snrDb / 10), rng);
    auto s = m.make(m.rate);
    Res r;
    const size_t chunk = 65536;
    for (size_t o = 0; o < x.size(); o += chunk) {
        s->feed(x.data() + o, std::min(chunk, x.size() - o));
        if (r.lockS < 0) { s->settle(); if (s->poll().good >= 1) r.lockS = (double)std::min(x.size(), o + chunk) / m.rate; }
    }
    s->finish();
    r.good = s->poll().good;
    if (r.lockS < 0 && r.good >= 1) r.lockS = (double)x.size() / m.rate;
    r.ok = r.good >= P.need;
    return r;
}

// fraction of n trials that passed (threads in parallel), median lock time of those that did
double rate(const Mode& m, const Prep& P, double snr, double cfo, double sro, int n, int threads, double* lock = nullptr, uint64_t salt = 0) {
    std::vector<Res> res((size_t)n);
    std::atomic<int> next{0};
    auto work = [&] { for (;;) { const int i = next++; if (i >= n) return; res[(size_t)i] = trial(m, P, snr, cfo, sro, false, mix64(hashStr(m.id) ^ salt ^ (uint64_t)(snr * 16 + 4096) * 131 ^ (uint64_t)i)); } };
    std::vector<std::thread> th;
    for (int k = 0; k < std::max(1, std::min(threads, n)); k++) th.emplace_back(work);
    for (auto& t : th) t.join();
    int ok = 0; std::vector<double> l;
    for (const Res& r : res) if (r.ok) { ok++; l.push_back(r.lockS); }
    if (lock) { std::sort(l.begin(), l.end()); *lock = l.empty() ? -1 : l[l.size() / 2]; }
    return (double)ok / n;
}

bool prepare(const Mode& m, Prep& P) {
    P.trialN = (size_t)(m.secs * m.rate);
    P.maxOff = (size_t)(0.5 * m.secs * m.rate);
    P.base = m.gen(P.trialN + P.maxOff + 64);
    scaleTo(P.base, 0.2);
    P.power = activePower(P.base);
    double g = 0;
    for (int i = 0; i < 2; i++) g += trial(m, P, 60, 0, 0, false, 77 + i).good;
    g /= 2;
    P.need = std::max(1.0, std::floor(0.5 * g));
    return g >= 1;
}

// the guard of the short run: the SNR (dB in the mode's noise bandwidth, see sens_modes.h) where 3.9 decodes reliably, threshold + 2 dB
struct Guard { const char* id; double snrDb; };
const Guard kGuards[] = {   // from the 3.9 calibration (CI runner): threshold + 2 dB
    {"adsb1", 4}, {"acars1", 6}, {"zigbee", 4}, {"zwave", 12}, {"ble", 10}, {"uat", 10}, {"flarm", 6}, {"adsl", 5}, {"sonde", 13},
};

} // namespace

// Calibration (ONAIR_SENS_CALIBRATE=1; the before / after table of docs/SENSITIVITY-3.9-C.md): thresholds in 1 dB steps with 5 trials (coarse 3 dB
// steps first), lock time, CPU per second of signal. Returns 1 so that ctest --output-on-failure shows the table. ONAIR_SENS_THREADS sets the threads.

int calibrate(const std::vector<Mode>& all, const std::regex& want) {
    const int threads = std::getenv("ONAIR_SENS_THREADS") ? std::atoi(std::getenv("ONAIR_SENS_THREADS")) : (int)std::max(1u, std::thread::hardware_concurrency());
    printf("CALIBRATION %-8s %7s %8s %9s %9s\n", "mode", "thr dB", "lock s", "cpu x rt", "false");
    for (const auto& m0 : all) {
        if (!std::regex_search(m0.id, want)) continue;
        Mode m = m0; m.secs = std::min(m.secs, m.id == "ais1" ? 24.0 : 4.0);   // one ship: a message every few seconds
        Prep P;
        if (!prepare(m, P)) { printf("CALIBRATION %-8s no valid output on the clean test signal\n", m.id.c_str()); continue; }
        double hi = NAN;
        for (double s = m.snrHi; s >= m.snrHi - 42; s -= 3) { if (rate(m, P, s, 0, 0, 5, threads) >= 0.8) hi = s; else break; }
        double thr = NAN;
        if (!std::isnan(hi)) { thr = hi; for (double s = hi - 1; s > hi - 3; s -= 1) { if (rate(m, P, s, 0, 0, 5, threads) >= 0.8) thr = s; else break; } }
        double lock = -1;
        if (!std::isnan(thr)) rate(m, P, thr + 3, 0, 0, 5, threads, &lock);
        const std::clock_t c0 = std::clock();
        trial(m, P, std::isnan(thr) ? m.snrHi : thr + 10, 0, 0, false, 5);
        const double cpu = (double)(std::clock() - c0) / CLOCKS_PER_SEC;
        int falses = 0;
        for (int i = 0; i < 2; i++) falses += trial(m, P, 10, 0, 0, true, 1000 + i).good > 0;
        printf("CALIBRATION %-8s %7.0f %8.2f %9.3f %9d\n", m.id.c_str(), thr, lock, cpu / m.secs, falses);
        fflush(stdout);
    }
    return 1;
}

int main() {
    const bool full = std::getenv("ONAIR_TEST_LONG") != nullptr;
    const char* sel = std::getenv("ONAIR_SENS_MODES");
    const std::regex want(sel ? sel : "^(adsb1|ais1|ais|acars1|vdl2|sonde|dsc|uat|flarm|adsl|rid|dji|vdes|zigbee|zwave|ble|wmbus|loraraw|mesh)$");
    const int threads = full ? std::max(1u, std::thread::hardware_concurrency() / 2) : 1;
    int fails = 0;
    const auto all = allModes();
    if (std::getenv("ONAIR_SENS_CALIBRATE")) return calibrate(all, want);
    if (!full) {
        for (const Guard& g : kGuards) {
            const Mode* m = nullptr;
            for (const auto& x : all) if (x.id == g.id) m = &x;
            if (!m) { printf("%-8s not in the harness\n", g.id); fails++; continue; }
            Mode mm = *m; mm.secs = std::min(mm.secs, 3.0);
            Prep P;
            if (!prepare(mm, P)) { printf("%-8s FAIL: no valid output on the clean test signal\n", g.id); fails++; continue; }
            const double ok = rate(mm, P, g.snrDb, 0, 0, 2, 1);
            const Res fl = trial(mm, P, 10, 0, 0, true, 999);
            printf("%-8s %4.1f dB: %3.0f %% of 2 trials, noise only: %.0f units\n", g.id, g.snrDb, 100 * ok, fl.good);
            if (ok < 1 || fl.good > 0) { printf("FAIL: %s\n", g.id); fails++; }
        }
        printf(fails ? "%d FAILED\n" : "all passed\n", fails);
        return fails ? 1 : 0;
    }
    printf("%-8s %7s %8s %10s %9s %9s %8s\n", "mode", "thr dB", "lock s", "cfo ok Hz", "sro ppm", "cpu x rt", "false");
    for (const auto& m : all) {
        if (!std::regex_search(m.id, want)) continue;
        Prep P;
        if (!prepare(m, P)) { printf("%-8s no valid output on the clean test signal\n", m.id.c_str()); fails++; continue; }
        const int n = 10;
        double thr = NAN;
        for (double s = m.snrHi; s >= m.snrHi - 40; s -= 1) {
            if (rate(m, P, s, 0, 0, n, threads) >= 0.9) thr = s; else if (!std::isnan(thr)) break;
        }
        if (std::isnan(thr)) { printf("%-8s no threshold above %.0f dB\n", m.id.c_str(), m.snrHi - 40); continue; }
        double lock = -1;
        rate(m, P, thr + 3, 0, 0, n, threads, &lock);
        double cfoOk = 0;   // the largest offset in the series that still passes at threshold + 3 dB
        const double lim = 0.45 * m.rate - 0.5 * m.bwHz;
        for (double f : {0.05, 0.1, 0.2, 0.3, 0.5, 0.75, 1.0}) {
            const double hz = f * m.bwHz;
            if (hz > lim) break;
            if (rate(m, P, thr + 3, hz, 0, 6, threads, nullptr, 1) >= 0.83 && rate(m, P, thr + 3, -hz, 0, 6, threads, nullptr, 2) >= 0.83) cfoOk = hz; else break;
        }
        double sroOk = 0;
        for (double p : {20.0, 50.0, 100.0, 200.0}) { if (rate(m, P, thr + 3, 0, p, 6, threads, nullptr, 3) >= 0.83) sroOk = p; else break; }
        // CPU: one receiver, one thread, the clean signal 10 dB above the threshold
        const Mode& mm = m;
        const auto t0 = std::chrono::steady_clock::now();
        const std::clock_t c0 = std::clock();
        trial(mm, P, thr + 10, 0, 0, false, 5);
        const double cpu = (double)(std::clock() - c0) / CLOCKS_PER_SEC, wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        int falses = 0;
        for (int i = 0; i < 4; i++) falses += trial(m, P, 10, 0, 0, true, 1000 + i).good > 0;
        printf("%-8s %7.0f %8.2f %10.0f %9.0f %9.3f %8d   (wall %.2f s for %.1f s)\n", m.id.c_str(), thr, lock, cfoOk, sroOk, cpu / m.secs, falses, wall, m.secs);
        fflush(stdout);
    }
    return fails ? 1 : 0;
}
