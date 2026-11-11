// 4.0 sensitivity part 2 (docs/SENSITIVITY-4.0-A2.md): EDACS and Motorola Type II control channels with a narrower channel filter plus
// the FmFront carrier follow-up (power-spectrum centroid). The receivers' own test signals through tools/sens, noise in the mode's
// bandwidth. Checks the fraction of units decoded at SNRs where the 3.9 receiver gets about a third or less, with a carrier error, and that
// noise alone gives nothing. Simulated signals only.
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
    const Case ed = makeCase("edacs"), sn = makeCase("smartnet");
    CHECK(ed.ref >= 2 && sn.ref >= 2, "clean reference EDACS %.1f SmartNet %.1f", ed.ref, sn.ref);
    // 3.9 receiver: EDACS 12 dB 75 % (0 Hz), 33 % (-6 kHz); SmartNet 4 dB 72 % (0 Hz), 0 % (+3.1 kHz), 6 dB 39 % (+3.1 kHz)
    struct R { const Case* c; const char* n; double snr, cfo, need; } rows[] = {
        {&ed, "EDACS", 12, 0, 0.9}, {&ed, "EDACS", 12, -6000, 0.7}, {&ed, "EDACS", 12, 3100, 0.8},
        {&sn, "SmartNet", 4, 0, 0.85}, {&sn, "SmartNet", 6, 3100, 0.8}, {&sn, "SmartNet", 4, 3100, 0.5},
    };
    for (const auto& r : rows) {
        const double f = frac(*r.c, r.snr, r.cfo, 6);
        printf("%s %.0f dB, carrier error %.0f Hz: %.0f %% (need %.0f %%)\n", r.n, r.snr, r.cfo, 100 * f, 100 * r.need);
        CHECK(f >= r.need, "%s %.0f dB %.0f Hz: %.2f", r.n, r.snr, r.cfo, f);
    }
    double falses = 0;
    for (int i = 0; i < 2; i++) falses += trial(ed, 10, 0, true, 1000 + (uint64_t)i) + trial(sn, 10, 0, true, 2000 + (uint64_t)i);
    printf("noise only: %.0f units\n", falses);
    CHECK(falses == 0, "noise-only runs gave %.0f units", falses);
    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
