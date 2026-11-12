// 4.0 sensitivity part 3 (docs/SENSITIVITY-4.0-A3.md): guard for the burst and packet detection gates that were loosened. Each row is the mode's own
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
    for (int i = 0; i < n; i++) s += std::min(1.0, trial(C, snr, cfo, sro, 0, mix64(0x40a3ull * 1000 + (uint64_t)((snr + 50) * 10) + (uint64_t)i * 7919)) / C.ref);
    return s / n;
}
struct Row { const char* id; double snr, cfo, sro, need; int n; };
struct SondeRow { const char* name; int mask; double snr, cfo, need; int n; };
} // namespace

int main(int argc, char** argv) {
#ifndef _WIN32
    setenv("DECT2_NOPACE", "1", 1);
#endif
    const Row rows[] = {
        {"halow", 6.0, 0, 0, 0.45, 4},          // 3.9: 20 %
        {"halow", 6.0, 18000, 0, 0.45, 4},
        {"halow", 6.0, 0, 50, 0.45, 4},
        {"pocsag", -4.5, 0, 0, 0.6, 6},         // 3.9: 29 %
        {"pocsag", -4.5, 0, 50, 0.6, 6},
        {"pocsag", 0.0, 3100, 0, 0.65, 4},      // the second path must not cost the carrier error case (3.9: 62 %)
        {"flex", 8.0, 0, 0, 0.55, 6},            // FLEX at the usual level: 67 % with and without the second path
        {"flex", 0.0, 0, 0, 0.4, 6},            // 3.9: 21 %. The slow FLEX speeds (1600 / 3200 two level) are decoded by the second path too
        {"zigbee", 1.5, 0, 0, 0.78, 6},         // 3.9: 38 %
        {"zigbee", 1.5, 48000, 0, 0.78, 6},     // 20 ppm at 2.4 GHz (3.9: 36 %)
        {"zigbee", 1.5, 0, 50, 0.65, 6},
        {"tetrapol", 7.5, 0, 0, 0.4, 6},        // 3.9: 37 %
        {"tetrapol", 7.5, 3100, 0, 0.25, 6},    // 3.9: 17 %
        {"tetrapol", 7.5, 8000, 0, 0.2, 6},     // 3.9: 14 %
        {"globalstar", -4.5, 0, 0, 0.9, 4},     // 3.9: nothing below -2 dB
        {"globalstar", -4.5, 18000, 50, 0.9, 4},
    };
    const char* only = argc > 1 ? argv[1] : nullptr;
    const char* last = "";
    Case C;
    for (const Row& r : rows) {
        if (only && std::strcmp(only, r.id) != 0) continue;
        if (std::strcmp(last, r.id) != 0) { C = makeCase(r.id); last = r.id; CHECK(C.ref >= 1, "%s clean reference %.1f", r.id, C.ref); }
        const double f = frac(C, r.snr, r.cfo, r.sro, r.n);
        printf("%s %.1f dB, carrier %.0f Hz, clock %.0f ppm: %.0f %% (need %.0f %%)\n", r.id, r.snr, r.cfo, r.sro, 100 * f, 100 * r.need);
        CHECK(f >= r.need, "%s %.1f dB %.0f Hz %.0f ppm: %.2f", r.id, r.snr, r.cfo, r.sro, f);
    }
    // MRZ and iMS-100 sondes, 24 s trials (one frame a second): 3.9 had the MRZ serial reset at every pause (7 times fewer frames) and both on the 10 kHz path
    if (!only || std::strcmp(only, "sonde") == 0) {
        const SondeRow srows[] = {
            {"MRZ", 256, 14.0, 0, 0.6, 4},          // 3.9: 0.5 frames of 7 a trial
            {"MRZ", 256, 14.0, 8000, 0.6, 4},
            {"iMS-100", 512, 6.0, 0, 0.5, 4},       // 3.9: 0 % at 6 dB
            {"iMS-100", 512, 6.0, 8000, 0.5, 4},
        };
        for (const SondeRow& r : srows) {
            const Case S = makeCase("sonde", 24.0, r.mask);
            CHECK(S.ref >= 3, "%s clean reference %.1f", r.name, S.ref);
            const double f = frac(S, r.snr, r.cfo, 0, r.n);
            printf("%s %.0f dB, carrier %.0f Hz: %.0f %% (need %.0f %%) of %.1f frames\n", r.name, r.snr, r.cfo, 100 * f, 100 * r.need, S.ref);
            CHECK(f >= r.need, "%s %.0f dB %.0f Hz: %.2f", r.name, r.snr, r.cfo, f);
        }
        const Case N = makeCase("sonde", 24.0, 256 | 512);
        double falses = 0;
        for (int i = 0; i < 3; i++) falses += trial(N, 10, 0, 0, 1, 1000 + (uint64_t)i);
        printf("sonde noise only: %.0f units\n", falses);
        CHECK(falses == 0, "sonde noise-only runs gave %.0f units", falses);
    }
    const char* noiseIds[] = {"halow", "globalstar", "pocsag", "zigbee", "tetrapol"};
    for (const char* id : noiseIds) {
        if (only && std::strcmp(only, id) != 0) continue;
        const Case N = makeCase(id, std::strcmp(id, "globalstar") == 0 ? 20.0 : 0);
        double falses = 0;
        for (int i = 0; i < 3; i++) falses += trial(N, 10, 0, 0, 1 + (std::strcmp(id, "globalstar") == 0 ? 1 : 0), 1000 + (uint64_t)i);
        printf("%s noise only: %.0f units\n", id, falses);
        CHECK(falses == 0, "%s noise-only runs gave %.0f units", id, falses);
    }
    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
