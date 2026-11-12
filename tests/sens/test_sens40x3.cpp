// Sensitivity pass X3 (docs/SENSITIVITY-4.0-X3.md): receivers in realistic channels. The mode's own test signal through the sens_sweep channel
// chain (static echo, TU6 Rayleigh / Rician fading with Jakes Doppler, phase noise, carrier and clock error) and white noise into a fresh receiver,
// seeded trials; a trial passes when the receiver counted at least half the valid units of a clean run (as sens_sweep). Every point is run twice
// with the same seeds: the new receiver path and the 3.9 path of the same receiver (an A/B switch of the receiver, set through the environment),
// so before and after come from one binary on one machine. Process CPU time of each run is printed next to it.
//   default: a short guard (the improved scenario at a fixed SNR must pass, noise alone must give nothing)
//   ONAIR_SENS_CALIBRATE=1: the before / after table. X3_MODES (regex, default isdbt), X3_SCEN (regex, default awgn|echo|tu6|rician|phasenoise|combined),
//   X3_TRIALS (8), X3_SNR "hi:lo:step" (per mode default), X3_SECS (trial length, default the mode's)
// Simulated signals only: no recording is involved.
#include "sens_modes_d.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <regex>
#include <thread>
#ifndef _WIN32
#include <sys/resource.h>
#endif

using namespace sens;

namespace {

struct Scen { std::string name; double cfoHz = 0, sroPpm = 0, pnHz = 0; std::string path; bool noiseOnly = false; };

std::vector<Scen> scenarios(const Mode& m) {
    const double lim = 0.45 * m.rate - 0.5 * m.bwHz;
    const double ppm1 = std::min(2e-6 * m.mhz * 1e6, lim);
    std::vector<Scen> s;
    s.push_back({"awgn", 0, 0, 0, ""});
    s.push_back({"echo", 0, 0, 0, "echo"});
    s.push_back({"tu6", 0, 0, 0, "tu6"});
    s.push_back({"rician", 0, 0, 0, "rician"});
    s.push_back({"phasenoise", 0, 0, m.pnHz, ""});
    s.push_back({"combined", ppm1, 20, m.pnHz, "echo"});
    return s;
}

struct Prep { Sig base; double power = 0; size_t trialN = 0, maxOff = 0; double need = 1; };

double trial(const Mode& m, const Prep& P, const Scen& sc, double snrDb, uint64_t seed) {
    std::mt19937 rng((uint32_t)(seed ^ (seed >> 32)));
    const size_t off = P.maxOff ? (size_t)(mix64(seed ^ 0x51ed) % P.maxOff) : 0;
    const size_t pad = (size_t)(m.rate * 0.01) + 64;
    Sig x(P.base.begin() + (long)off, P.base.begin() + (long)std::min(P.base.size(), off + P.trialN + pad));
    if (sc.sroPpm != 0) x = clockError(x, sc.sroPpm);
    if (!sc.path.empty()) x = multipath(x, profileTaps(sc.path, m.echoUs), m.rate, m.fdHz, rng);
    if (x.size() > P.trialN) x.resize(P.trialN);
    std::uniform_real_distribution<double> U(0, 2 * M_PI);
    if (sc.noiseOnly) std::fill(x.begin(), x.end(), cf32(0, 0));
    shift(x, sc.cfoHz, m.rate, U(rng));
    phaseNoise(x, sc.pnHz, m.rate, rng);
    addNoise(x, P.power * (m.rate / m.bwHz) / std::pow(10.0, snrDb / 10), rng);
    auto s = m.make(m.rate);
    const size_t chunk = 65536;
    for (size_t o = 0; o < x.size(); o += chunk) s->feed(x.data() + o, std::min(chunk, x.size() - o));
    s->finish();
    return s->poll().good;
}

double cpuSeconds() {
#ifndef _WIN32
    rusage u{};
    getrusage(RUSAGE_SELF, &u);
    return (double)u.ru_utime.tv_sec + 1e-6 * (double)u.ru_utime.tv_usec + (double)u.ru_stime.tv_sec + 1e-6 * (double)u.ru_stime.tv_usec;
#else
    return 0;
#endif
}

struct Pt { double pass = 0, frac = 0, cpu = 0; };

// n seeded trials in parallel: fraction that passed, mean fraction of the clean units, process CPU seconds of the run
Pt run(const Mode& m, const Prep& P, const Scen& sc, double snr, int n, int threads) {
    std::vector<double> g((size_t)n);
    std::atomic<int> next{0};
    const double c0 = cpuSeconds();
    auto work = [&] { for (;;) { const int i = next++; if (i >= n) return; g[(size_t)i] = trial(m, P, sc, snr, mix64(hashStr(m.id) ^ hashStr(sc.name) * 31 ^ (uint64_t)(snr * 16 + 4096) * 131 ^ (uint64_t)i)); } };
    std::vector<std::thread> th;
    for (int k = 0; k < std::max(1, std::min(threads, n)); k++) th.emplace_back(work);
    for (auto& t : th) t.join();
    Pt r;
    for (double v : g) { if (v >= P.need) r.pass += 1; r.frac += std::min(1.0, v / std::max(1.0, 2 * P.need)); }
    r.pass /= n; r.frac /= n; r.cpu = cpuSeconds() - c0;
    return r;
}

// the A/B switch of each receiver (its 3.9 path)
const char* offSwitch(const std::string& id) {
    if (id == "isdbt") return "DECT2_ISDBT_NOTI";
    if (id == "atsc") return "ATSC_NOSEG";
    if (id == "atsc3") return "DECT2_ATSC3_NOCSI";
    return nullptr;
}
void setOff(const char* sw, bool off) {
    if (!sw) return;
#ifndef _WIN32
    if (off) setenv(sw, "1", 1); else unsetenv(sw);
#else
    _putenv_s(sw, off ? "1" : "");
#endif
}

Prep prepare(const Mode& m, double secs, int threads) {
    Prep P;
    P.trialN = (size_t)(secs * m.rate);
    P.maxOff = (size_t)(0.5 * secs * m.rate);
    P.base = m.gen(P.trialN + P.maxOff + (size_t)(m.rate * 0.01) + 64);
    scaleTo(P.base, 0.2);
    P.power = activePower(P.base);
    Scen clean{"awgn"};
    (void)threads;
    double sum = 0;
    for (int i = 0; i < 3; i++) sum += trial(m, P, clean, 60, 77 + (uint64_t)i);
    P.need = std::max(1.0, 0.5 * sum / 3);
    return P;
}

const Mode* findMode(const std::vector<Mode>& all, const std::string& id) {
    for (const auto& m : all) if (m.id == id) return &m;
    return nullptr;
}

} // namespace

int main() {
#ifndef _WIN32
    setenv("DECT2_NOPACE", "1", 1);
#endif
    auto all = allModes();
    for (auto& m : extraModesD()) all.push_back(std::move(m));
    const int threads = std::max(1, (int)std::thread::hardware_concurrency());
    if (getenv("ONAIR_SENS_CALIBRATE")) {
        const std::regex modeRe(getenv("X3_MODES") ? getenv("X3_MODES") : "isdbt");
        const std::regex scenRe(getenv("X3_SCEN") ? getenv("X3_SCEN") : "awgn|echo|tu6|rician|phasenoise|combined");
        const int n = getenv("X3_TRIALS") ? atoi(getenv("X3_TRIALS")) : 8;
        for (const auto& m0 : all) {
            if (!std::regex_match(m0.id, modeRe)) continue;
            Mode m = m0;
            if (getenv("X3_SECS")) m.secs = atof(getenv("X3_SECS"));
            const char* sw = offSwitch(m.id);
            double hi = m.snrHi, lo = m.snrHi - 20, st = 2;
            if (getenv("X3_SNR")) sscanf(getenv("X3_SNR"), "%lf:%lf:%lf", &hi, &lo, &st);
            setOff(sw, false);
            const int thr = std::max(1, std::min(threads, m.maxJobs));
            const Prep P = prepare(m, m.secs, thr);
            printf("== %s (%s): %.1f s trials, need %.1f units, %d trials a point, switch %s\n", m.id.c_str(), m.name.c_str(), m.secs, P.need, n, sw ? sw : "-");
            for (const auto& sc : scenarios(m)) {
                if (!std::regex_match(sc.name, scenRe)) continue;
                printf("-- %s\n   SNR    new pass/frac/cpu      old pass/frac/cpu\n", sc.name.c_str());
                for (double s = hi; s >= lo - 1e-9; s -= st) {
                    setOff(sw, false);
                    const Pt a = run(m, P, sc, s, n, thr);
                    Pt b;
                    if (sw) { setOff(sw, true); b = run(m, P, sc, s, n, thr); setOff(sw, false); }
                    printf("  %5.1f   %4.0f%% %4.0f%% %6.1fs     %4.0f%% %4.0f%% %6.1fs\n", s, 100 * a.pass, 100 * a.frac, a.cpu, 100 * b.pass, 100 * b.frac, b.cpu);
                    fflush(stdout);
                    if (a.pass == 0 && b.pass == 0) break;
                }
            }
            // false decodes: noise only, long runs
            Scen no{"noise-only"}; no.noiseOnly = true;
            Prep Pn = P; Pn.need = 0.5;   // any unit counts
            setOff(sw, false);
            const Pt fa = run(m, Pn, no, 10, 2 * n, thr);
            Pt fb;
            if (sw) { setOff(sw, true); fb = run(m, Pn, no, 10, 2 * n, thr); setOff(sw, false); }
            printf("-- noise only, %d runs: new %.0f%% of runs with a unit, old %.0f%%\n", 2 * n, 100 * fa.pass, 100 * fb.pass);
        }
        return 0;
    }
    // ---- guard: ISDB-T 64-QAM 3/4 in TU6 fading (10 Hz Doppler) at 26 dB, where the 3.9 receiver (newest pilot per grid point) did not decode;
    // AWGN at 20 dB; noise alone gives no packet
    int fails = 0;
    const Mode* mi = findMode(all, "isdbt");
    if (!mi) { printf("no isdbt mode\n"); return 1; }
    const int thr = std::max(1, std::min(threads, mi->maxJobs));
    const Prep P = prepare(*mi, mi->secs, thr);
    const Scen tu6{"tu6", 0, 0, 0, "tu6"}, awgn{"awgn"};
    const Pt t = run(*mi, P, tu6, 26, 4, thr);
    const Pt a = run(*mi, P, awgn, 20, 2, thr);
    Scen no{"noise-only"}; no.noiseOnly = true;
    Prep Pn = P; Pn.need = 0.5;
    const Pt f = run(*mi, Pn, no, 10, 4, thr);
    printf("isdbt: tu6 26 dB pass %.0f%% (units %.0f%%), awgn 20 dB pass %.0f%%, noise only %.0f%% of runs with a packet\n", 100 * t.pass, 100 * t.frac, 100 * a.pass, 100 * f.pass);
    if (t.pass < 0.74) { printf("FAIL: isdbt tu6 26 dB\n"); fails++; }
    if (a.pass < 0.99) { printf("FAIL: isdbt awgn 20 dB\n"); fails++; }
    if (f.pass > 0) { printf("FAIL: isdbt decoded noise\n"); fails++; }
    printf(fails ? "FAILED\n" : "PASSED\n");
    return fails ? 1 : 0;
}
