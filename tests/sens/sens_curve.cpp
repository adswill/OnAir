// Smooth sensitivity curve of one mode: mean fraction of the clean-signal valid units decoded at each SNR (several seeded trials), with
// optional carrier error and clock error, plus a noise-only false-decode count. A finer view than the 90 % threshold of sens_sweep: use it to
// see a gain of a fraction of a dB. Usage: sens_curve <mode> <snrFrom> <snrTo> <step> <trials> [cfoHz] [sroPpm] [offFrac]
// Simulated signals only (the mode's own generator through tools/sens/sens_modes.h).
#include "sens_modes.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>
using namespace sens;
namespace {
struct Prep { Sig base; double power = 0; size_t trialN = 0, maxOff = 0; double ref = 1; };
double trial(const Mode& m, const Prep& P, double snrDb, double cfoHz, double sroPpm, bool noiseOnly, uint64_t seed) {
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
    const size_t chunk = 65536;
    for (size_t o = 0; o < x.size(); o += chunk) s->feed(x.data() + o, std::min(chunk, x.size() - o));
    s->finish();
    return s->poll().good;
}
}
int main(int argc, char** argv) {
#ifndef _WIN32
    setenv("DECT2_NOPACE", "1", 1);
#endif
    if (argc < 6) { printf("usage: sens_curve mode from to step trials [cfoHz] [sroPpm]\n"); return 2; }
    const double lo = atof(argv[2]), hi = atof(argv[3]), st = atof(argv[4]); const int n = atoi(argv[5]);
    const double cfo = argc > 6 ? atof(argv[6]) : 0, sro = argc > 7 ? atof(argv[7]) : 0;
    const auto all = allModes();
    const Mode* mp = nullptr;
    for (const auto& m : all) if (m.id == argv[1]) mp = &m;
    if (!mp) { printf("no mode %s\n", argv[1]); return 2; }
    Mode mcopy = *mp;
    if (getenv("CURVE_OPT0") && mcopy.id == "sonde") { const int o0 = atoi(getenv("CURVE_OPT0")); mcopy.gen = [o0](size_t n) { SynthConfig c; c.modeOpt[0] = o0; c.modeOpt[1] = 1; return clean(15, c, 8e6, n); }; }
    const Mode& m = mcopy;
    Prep P;
    P.trialN = (size_t)(m.secs * m.rate);
    P.maxOff = (size_t)(0.5 * m.secs * m.rate);
    P.base = m.gen(P.trialN + P.maxOff + 64);
    scaleTo(P.base, 0.2);
    P.power = activePower(P.base);
    double g = 0;
    for (int i = 0; i < 3; i++) g += trial(m, P, 60, 0, 0, false, 77 + i);
    P.ref = std::max(1.0, g / 3);
    printf("%s: clean units per trial %.1f, noise bw %.0f Hz, cfo %.0f Hz, sro %.0f ppm\n", m.id.c_str(), P.ref, m.bwHz, cfo, sro);
    double falses = 0;
    for (int i = 0; i < 4; i++) falses += trial(m, P, 10, 0, 0, true, 1000 + i);
    const int nth = std::max(1, std::min((int)std::thread::hardware_concurrency() / 3, n));
    for (double s = hi; s >= lo - 1e-9; s -= st) {
        std::vector<double> r((size_t)n);
        std::atomic<int> nx{0};
        std::vector<std::thread> th;
        for (int k = 0; k < nth; k++) th.emplace_back([&] { for (;;) { int i = nx++; if (i >= n) return; r[(size_t)i] = trial(m, P, s, cfo, sro, false, mix64(hashStr(m.id) ^ (uint64_t)(s * 16 + 4096) * 131 ^ (uint64_t)i * 7919)); } });
        for (auto& t : th) t.join();
        double sum = 0; for (double v : r) sum += std::min(1.0, v / P.ref);
        printf("%6.1f dB  %5.1f %%\n", s, 100 * sum / n);
        fflush(stdout);
    }
    printf("noise-only units in 4 runs: %.0f\n", falses);
}
