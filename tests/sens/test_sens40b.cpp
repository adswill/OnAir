// Sensitivity of the 4.0 pass B receivers (narrowband and data modes, docs/SENSITIVITY-4.0-B.md): the mode's own test signal through white noise
// (and a carrier offset and a sample-clock error) into a fresh receiver, seeded trials; a trial passes when the receiver counted at least half the valid
// units of a clean run (as sens_sweep). The "off" entries run the 3.9 path of the same receiver (a switch of the receiver), so before and after are
// measured in one binary.
//   default: a short guard (two trials a mode at the guard SNR, noise alone must give nothing)
//   ONAIR_TEST_LONG=1: threshold (1 dB steps, 10 trials), offset and clock error at threshold + 3 dB, false decodes on a long noise run
//   ONAIR_SENS_CALIBRATE=1: the before / after table (1 dB steps, ONAIR_SENS_TRIALS trials, default 10, 80 % criterion)
// Simulated signals only: no recording is involved.
#include "../common/test_long.h"
#include "sens_modes_d.h"
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
    auto work = [&] { for (;;) { const int i = next++; if (i >= n) return; res[(size_t)i] = trial(m, P, snr, cfo, sro, false, mix64(hashStr(m.id.size() > 4 && m.id.compare(m.id.size() - 4, 4, "-off") == 0 ? m.id.substr(0, m.id.size() - 4) : m.id) ^ salt ^ (uint64_t)(snr * 16 + 4096) * 131 ^ (uint64_t)i)); } };
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

namespace s40 {
using namespace sens;
std::vector<Mode> modes40b() {
    std::vector<Mode> v;
    auto pager = [&](const char* id, const char* name, int kind, bool tone) {
        const ModeTuning* t = modeTuning(25);
        Mode m; m.id = id; m.name = name; m.family = "narrowband-data"; m.stdMode = 25; m.rate = t ? t->sampleRate : 1.2e6; m.secs = 8.0; m.bwHz = 25e3; m.mhz = 466;
        m.maxJobs = 4; m.snrHi = 25;
        m.gen = [kind, r = m.rate](size_t n) { SynthConfig c; c.modeOpt[0] = kind; return clean(25, c, r, n); };
        m.make = factory<PagerReceiver, PagerTelemetry>([tone](PagerReceiver& rx, double r) { prepGeneric(rx, r, 25); rx.setToneDetection(tone); }, blocks<PagerTelemetry>);
        v.push_back(m);
    };
    pager("pocsag", "POCSAG (4.0 receiver)", 1, true);
    pager("pocsag-off", "POCSAG (3.9 receiver)", 1, false);
    pager("flex", "FLEX (4.0 receiver)", 2, true);
    pager("flex-off", "FLEX (3.9 receiver)", 2, false);
    // satellite-data style entries: the mode's own test signal at its tuning rate; `on` switches the 4.0 path of the receiver
    auto satdata = [&](const char* id, const char* name, int std_, double secs, double bw, double snrHi, auto prep) {
        const ModeTuning* mt = modeTuning(std_);
        Mode m; m.id = id; m.name = name; m.family = "satdata"; m.stdMode = std_; m.rate = mt->sampleRate; m.secs = secs; m.bwHz = bw; m.mhz = mt->defMhz;
        m.snrHi = snrHi; m.maxJobs = 4;
        const double rate = mt->sampleRate;
        m.gen = [rate, std_](size_t n) { return clean(std_, SynthConfig(), rate, n); };
        prep(m);
        v.push_back(m);
    };
    for (int on = 1; on >= 0; on--)
        satdata(on ? "zwave" : "zwave-off", on ? "Z-Wave (4.0 receiver)" : "Z-Wave (3.9 receiver)", 100, 4.0, 100e3, 25, [on](Mode& m) {
            m.make = factory<ZwaveReceiver, ZwaveTelemetry>([on](ZwaveReceiver& rx, double r) { prepGeneric(rx, r, 100); rx.setToneDetection(on != 0); }, blocks<ZwaveTelemetry>);
        });
    for (int on = 1; on >= 0; on--)
        satdata(on ? "argos" : "argos-off", on ? "Argos (4.0 receiver)" : "Argos (3.9 receiver)", 154, 6.0, 5e3, 25, [on](Mode& m) {
            m.make = factory<ArgosReceiver, ArgosTelemetry>([on](ArgosReceiver& rx, double r) { prepGeneric(rx, r, 154); rx.setWeakSignalPath(on != 0); }, blocks<ArgosTelemetry>);
        });
    for (int on = 1; on >= 0; on--)
        satdata(on ? "ndb" : "ndb-off", on ? "NDB (4.0 receiver)" : "NDB (3.9 receiver)", 31, 40.0, 300, 25, [on](Mode& m) {
            m.make = factory<NdbReceiver, NdbTelemetry>([on](NdbReceiver& rx, double r) { prepGeneric(rx, r, 31); rx.setWeakSignalPath(on != 0); },
                [](const NdbTelemetry& t) { const bool ok = t.ident.size() == 3 && std::string("BNDBND").find(t.ident) != std::string::npos; return Metric{ok ? (double)t.blocksOk : 0.0, 0.0}; });   // only the generator's ident (any rotation of it) counts
        });
    for (int on = 1; on >= 0; on--)
        satdata(on ? "dsc" : "dsc-off", on ? "Marine DSC VHF ch 70 (4.0 receiver)" : "Marine DSC VHF ch 70 (3.9 receiver)", 17, 25.0, 25e3, 25, [on](Mode& m) {
            m.mhz = 156.525;
            m.gen = [r = m.rate](size_t n) { SynthConfig c; c.modeOpt[0] = 4; return clean(17, c, r, n); };
            m.make = factory<MarineReceiver, MarineTelemetry>([on](MarineReceiver& rx, double r) { prepGeneric(rx, r, 17); rx.setVhfNarrowChannel(on != 0); }, blocks<MarineTelemetry>);
        });
    for (int on = 1; on >= 0; on--) {   // radiosondes: an RS41 on the centre, a DFM-17 and an M10 elsewhere (as sens_modes.h)
        Mode m; m.id = on ? "sonde" : "sonde-off"; m.name = on ? "Radiosonde (4.0 receiver)" : "Radiosonde (3.9 receiver)"; m.family = "narrowband-data"; m.stdMode = 15;
        m.rate = 8e6; m.secs = 5.0; m.bwHz = 10e3; m.mhz = 403; m.snrHi = 25; m.maxJobs = 4;
        m.gen = [](size_t n) { return clean(15, SynthConfig(), 8e6, n); };
        m.make = factory<SondeReceiver, SondeTelemetry>([on](SondeReceiver& rx, double r) { rx.setToneDetection(on != 0); rx.setSynchronous(true); rx.setSignalOffset(0); rx.configure(r); rx.setCenterMhz(403); },
                                                        blocks<SondeTelemetry>, false, [](SondeReceiver& rx) { rx.flush(); });
        v.push_back(m);
    }
    return v;
}

// the guard of the short run: the SNR (dB in the mode's noise bandwidth) a few dB above the 4.0 threshold
struct Guard { const char* id; double snrDb; };
const Guard kGuards[] = {
    {"pocsag", -1}, {"flex", 5}, {"zwave", 14}, {"argos", 0}, {"ndb", 14}, {"dsc", 8},
};
}
using namespace s40;

} // namespace

// Calibration (ONAIR_SENS_CALIBRATE=1; the before / after table of docs/SENSITIVITY-3.9-C.md): thresholds in 1 dB steps with 5 trials (coarse 3 dB
// steps first), lock time, CPU per second of signal. Returns 1 so that ctest --output-on-failure shows the table. ONAIR_SENS_THREADS sets the threads.

int calibrate(const std::vector<Mode>& all, const std::regex& want) {
    const int threads = std::getenv("ONAIR_SENS_THREADS") ? std::atoi(std::getenv("ONAIR_SENS_THREADS")) : (int)std::max(1u, std::thread::hardware_concurrency());
    const int ntr = std::getenv("ONAIR_SENS_TRIALS") ? std::atoi(std::getenv("ONAIR_SENS_TRIALS")) : 10;
    printf("CALIBRATION %-8s %7s %8s %9s %9s\n", "mode", "thr dB", "lock s", "cpu x rt", "false");
    for (const auto& m0 : all) {
        if (!std::regex_search(m0.id, want)) continue;
        Mode m = m0; m.secs = std::min(m.secs, m.id == "ais1" ? 24.0 : 4.0);   // one ship: a message every few seconds
        Prep P;
        if (!prepare(m, P)) { printf("CALIBRATION %-8s no valid output on the clean test signal\n", m.id.c_str()); continue; }
        double hi = NAN;
        for (double s = m.snrHi; s >= m.snrHi - 42; s -= 3) { if (rate(m, P, s, 0, 0, ntr, threads) >= 0.8) hi = s; else break; }
        double thr = NAN;
        if (!std::isnan(hi)) { thr = hi; for (double s = hi - 1; s > hi - 3; s -= 1) { if (rate(m, P, s, 0, 0, ntr, threads) >= 0.8) thr = s; else break; } }
        double lock = -1;
        if (!std::isnan(thr)) rate(m, P, thr + 3, 0, 0, ntr, threads, &lock);
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

// ONAIR_SENS_POINTS="-3,-4,-5" (ONAIR_SENS_TRIALS trials each): the pass rate at those SNRs for AWGN, a 20 ppm carrier error (capped at a quarter of the
// channel), a 50 ppm sample clock and an off-centre carrier (20 % of the channel), then noise alone.
int points(const std::vector<Mode>& all, const std::regex& want, const char* list) {
    const int threads = std::max(1u, std::thread::hardware_concurrency() / 2);
    const int n = std::getenv("ONAIR_SENS_TRIALS") ? std::atoi(std::getenv("ONAIR_SENS_TRIALS")) : 20;
    std::vector<double> snr;
    for (const char* q = list; *q;) { snr.push_back(std::atof(q)); while (*q && *q != ',') q++; if (*q == ',') q++; }
    for (const auto& m : all) {
        if (!std::regex_search(m.id, want)) continue;
        Prep P;
        if (!prepare(m, P)) { printf("POINTS %-10s no valid output on the clean test signal\n", m.id.c_str()); continue; }
        const double cfo = std::min(20.0 * m.mhz, 0.25 * m.bwHz), off = 0.2 * m.bwHz;
        const struct { const char* n; double c, s; } sc[] = {{"awgn", 0, 0}, {"cfo20ppm", cfo, 0}, {"sro50ppm", 0, 50}, {"offcentre", off, 0}};
        for (const auto& c : sc) {
            printf("POINTS %-10s %-9s", m.id.c_str(), c.n);
            for (double s : snr) printf("  %.0f:%2.0f/%d", s, rate(m, P, s, c.c, c.s, n, threads) * n, n);
            printf("\n");
            fflush(stdout);
        }
        int falses = 0;
        for (int i = 0; i < n; i++) falses += trial(m, P, 10, 0, 0, true, 1000 + i).good > 0;
        printf("POINTS %-10s noise only: %d of %d trials gave a unit\n", m.id.c_str(), falses, n);
    }
    return 1;
}

int main() {
    const bool full = testLong();
    const char* sel = std::getenv("ONAIR_SENS_MODES");
    const std::regex want(sel ? sel : "^(pocsag|pocsag-off|flex|flex-off|zwave|zwave-off|sonde|sonde-off|argos|argos-off|ndb|ndb-off|dsc|dsc-off)$");
    const int threads = full ? std::max(1u, std::thread::hardware_concurrency() / 2) : 1;
    int fails = 0;
    const auto all = modes40b();
    if (std::getenv("ONAIR_SENS_POINTS")) return points(all, want, std::getenv("ONAIR_SENS_POINTS"));
    if (std::getenv("ONAIR_SENS_CALIBRATE")) return calibrate(all, want);
    if (!full) {
        for (const Guard& g : kGuards) {
            const Mode* m = nullptr;
            for (const auto& x : all) if (x.id == g.id) m = &x;
            if (!m) { printf("%-8s not in the harness\n", g.id); fails++; continue; }
            Mode mm = *m;   // the whole signal: FLEX needs about 4 s to find its first frame
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
        for (int i = 0; i < (std::getenv("ONAIR_SENS_NOISE") ? std::atoi(std::getenv("ONAIR_SENS_NOISE")) : 8); i++) falses += trial(m, P, 10, 0, 0, true, 1000 + i).good > 0;
        printf("%-8s %7.0f %8.2f %10.0f %9.0f %9.3f %8d   (wall %.2f s for %.1f s)\n", m.id.c_str(), thr, lock, cfoOk, sroOk, cpu / m.secs, falses, wall, m.secs);
        fflush(stdout);
    }
    return fails ? 1 : 0;
}
