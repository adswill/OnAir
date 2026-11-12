// 4.0 sensitivity part 4 (docs/SENSITIVITY-4.0-A4.md): sonde symbols from tone energies (iMet-54, MRZ, iMS-100), table and guard. Each row is the mode's own
// test signal through white noise (and a carrier / clock error) into a fresh receiver, seeded trials: the fraction of the clean run's units decoded must
// reach `need` (the 3.9 receiver is below it), and noise alone must give nothing. Simulated signals only.
#include "sens_modes_d.h"
#include <cstdio>
#include <cstring>
using namespace sens;

namespace {
int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

struct Case { Mode m; Sig base; double power = 0; size_t trialN = 0, maxOff = 0; double ref = 1; };

double trial(const Case& C, double snrDb, double cfoHz, double sroPpm, int noiseKind, uint64_t seed) {   // noiseKind 0 signal, 1 noise only, 2 noise only with a slow level drift
    std::mt19937 rng((uint32_t)(seed ^ (seed >> 32)));
    const size_t off = C.maxOff ? (size_t)(mix64(seed ^ 0x51ed) % C.maxOff) : 0;
    Sig x(C.base.begin() + (long)off, C.base.begin() + (long)std::min(C.base.size(), off + C.trialN + 64));
    if (sroPpm != 0) x = clockError(x, sroPpm);
    if (x.size() > C.trialN) x.resize(C.trialN);
    if (noiseKind) std::fill(x.begin(), x.end(), cf32(0, 0));
    std::uniform_real_distribution<double> U(0, 2 * M_PI);
    shift(x, cfoHz, C.m.rate, U(rng));
    addNoise(x, C.power * (C.m.rate / C.m.bwHz) / std::pow(10.0, snrDb / 10), rng);
    if (noiseKind == 2) for (size_t i = 0; i < x.size(); i++) x[i] *= (float)(1.0 + 0.03 * std::sin(2 * M_PI * (double)i / (C.m.rate * 7.0)));   // +-0.26 dB in 7 s
    auto s = C.m.make(C.m.rate);
    for (size_t o = 0; o < x.size(); o += 65536) s->feed(x.data() + o, std::min<size_t>(65536, x.size() - o));
    s->finish();
    return s->poll().good;
}

Case makeCase(const char* id, double secs = 0, int sondeMask = 0) {
    Case C;
    auto all = allModes();
    for (auto& m : extraModesD()) all.push_back(std::move(m));
    for (const auto& m : all) if (m.id == id) C.m = m;
    if (secs > 0) C.m.secs = secs;
    if (sondeMask) C.m.gen = [sondeMask](size_t n) { SynthConfig c; c.modeOpt[0] = sondeMask; c.modeOpt[1] = 1; return clean(15, c, 8e6, n); };
    C.trialN = (size_t)(C.m.secs * C.m.rate);
    C.maxOff = (size_t)(0.5 * C.m.secs * C.m.rate);
    C.base = C.m.gen(C.trialN + C.maxOff + 64);
    scaleTo(C.base, 0.2);
    C.power = activePower(C.base);
    C.ref = std::max(1.0, trial(C, 60, 0, 0, 0, 77));
    return C;
}
double frac(const Case& C, double snr, double cfo, double sro, int n) {
    double s = 0;
    for (int i = 0; i < n; i++) s += std::min(1.0, trial(C, snr, cfo, sro, 0, mix64(0x40a4ull * 1000 + (uint64_t)((snr + 50) * 10) + (uint64_t)i * 7919)) / C.ref);
    return s / n;
}
}
int main() {
#ifndef _WIN32
    setenv("DECT2_NOPACE", "1", 1);
#endif
    setvbuf(stdout, nullptr, _IONBF, 0);
    struct R { const char* name; int mask; double secs; double snrs[6]; };
    const R rs[] = {
        {"iMet-54", 128, 12.0, {19, 16, 13, 11, 9, 7}},
        {"MRZ", 256, 24.0, {14, 12, 10, 8, 6, 4}},
        {"iMS-100", 512, 24.0, {10, 8, 6, 4, 2, 0}},
    };
    const double cfos[] = {0};
    for (int phase = 0; phase < 3; phase++) {
    setenv("A4_VAR", phase == 0 ? "0" : phase == 1 ? "1" : "2", 1);
    printf("=== A4_VAR %d (0 before, 1 tone + mean threshold, 2 mean threshold only)\n", phase);
    for (const R& r : rs) {
        const Case S = makeCase("sonde", r.secs, r.mask);
        for (double cfo : cfos) {
            printf("%-8s cfo %5.0f ref %.1f:", r.name, cfo, S.ref);
            for (double snr : r.snrs) printf("  %.0fdB %3.0f%%", snr, 100 * frac(S, snr, cfo, 0, 4));
            printf("\n");
        }
        double falses = 0;
        for (int i = 0; i < 3; i++) falses += trial(S, 10, 0, 0, 1, 1000 + (uint64_t)i);
        printf("%-8s noise only: %.0f units\n", r.name, falses);
        CHECK(falses == 0, "%s noise-only %.0f", r.name, falses);
    }
    }
    printf(fails ? "FAILED\n" : "OK\n");
    return fails ? 1 : 0;
}
