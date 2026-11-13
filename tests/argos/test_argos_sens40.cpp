// 4.0 sensitivity part 2 (docs/SENSITIVITY-4.0-A2.md): Argos PTT uplink bursts, burst extent from the coherent 1 ms power and the message length chosen by where the
// phase steps stop. The receiver's own test signal through tools/sens, noise in 5 kHz. The 3.9 receiver decodes nothing below 10 dB; this checks the
// fraction of units decoded at 2 dB and 0 dB (also with a carrier error) and that noise alone gives nothing. Simulated signals only.
#include "../../tools/sens/sens_modes_d.h"
#include <cstdio>
using namespace sens;

namespace {
int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

struct Case { Mode m; std::string id; Sig base; double power = 0; size_t trialN = 0, maxOff = 0; double ref = 1; };

double trial(const Case& C, double snrDb, double cfoHz, bool noiseOnly, uint64_t seed) {
    std::mt19937 rng((uint32_t)(seed ^ (seed >> 32)));
    const size_t off = C.maxOff ? (size_t)(mix64(seed ^ 0x51ed) % C.maxOff) : 0;
    Sig x(C.base.begin() + (long)off, C.base.begin() + (long)std::min(C.base.size(), off + C.trialN + 64));
    if (x.size() > C.trialN) x.resize(C.trialN);
    if (noiseOnly) std::fill(x.begin(), x.end(), cf32(0, 0));
    std::uniform_real_distribution<double> U(0, 2 * M_PI);
    shift(x, cfoHz, C.m.rate, U(rng));
    addNoise(x, C.power * (C.m.rate / C.m.bwHz) / std::pow(10.0, snrDb / 10), rng);
    auto s = C.m.make(C.m.rate);
    for (size_t o = 0; o < x.size(); o += 65536) s->feed(x.data() + o, std::min<size_t>(65536, x.size() - o));
    s->finish();
    return s->poll().good;
}

Case makeCase(const char* id) {
    Case C;
    auto all = allModes();
    for (auto& m : extraModesD()) all.push_back(std::move(m));
    for (const auto& m : all) if (m.id == id) C.m = m;
    C.trialN = (size_t)(C.m.secs * C.m.rate);
    C.maxOff = (size_t)(0.5 * C.m.secs * C.m.rate);
    C.base = C.m.gen(C.trialN + C.maxOff + 64);
    scaleTo(C.base, 0.2);
    C.power = activePower(C.base);
    C.ref = std::max(1.0, trial(C, 60, 0, false, 77));
    return C;
}
double frac(const Case& C, double snr, double cfo, int n) {
    double s = 0;
    for (int i = 0; i < n; i++) s += std::min(1.0, trial(C, snr, cfo, false, mix64(0x40a2ull * 1000 + (uint64_t)(snr * 10) + (uint64_t)i * 7919)) / C.ref);
    return s / n;
}
} // namespace

int main() {
#ifndef _WIN32
    setenv("DECT2_NOPACE", "1", 1);
#endif
    const Case ar = makeCase("argos");
    CHECK(ar.ref >= 1, "clean reference Argos %.1f", ar.ref);
    struct R { double snr, cfo, need; } rows[] = {{4, 0, 0.85}, {2, 0, 0.8}, {2, 3100, 0.7}, {0, 0, 0.7}, {0, 3100, 0.6}};   // 3.9: 0 % at every one of them
    for (const auto& r : rows) {
        const double f = frac(ar, r.snr, r.cfo, 8);
        printf("Argos %.0f dB, carrier error %.0f Hz: %.0f %% (need %.0f %%)\n", r.snr, r.cfo, 100 * f, 100 * r.need);
        CHECK(f >= r.need, "Argos %.0f dB %.0f Hz: %.2f", r.snr, r.cfo, f);
    }
    double falses = 0;
    for (int i = 0; i < 3; i++) falses += trial(ar, 10, 0, true, 1000 + (uint64_t)i);
    printf("noise only: %.0f units\n", falses);
    CHECK(falses == 0, "noise-only runs gave %.0f units", falses);
    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
