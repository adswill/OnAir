// 4.0 radiosonde sensitivity (docs/SENSITIVITY-4.0-A.md): RS41 goes through a second, tighter channel filter (4.5 kHz half width).
// The sondes are the receiver's own test signal (sonde_gen), one RS41 alone, noise added at the stated SNR in 10 kHz (the generator's definition).
// Checks: the RS41 decodes at 7 dB with no carrier error, with +8 kHz (20 ppm at 403 MHz) and -3 kHz; DFM and M10 still decode as before
// at 20 dB; noise alone gives no frame. The 3.9 receiver decodes the RS41 case at 7 dB in none of the frames, so this fails on it.
// Simulated signals only.
#include "../../tools/sens/sens_modes.h"
#include <cstdio>
using namespace sens;

namespace {
int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

struct Case { Mode m; Sig base; double power = 0; size_t trialN = 0, maxOff = 0; double ref = 1; };

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

Case makeCase(int typeMask) {
    Case C;
    for (const auto& m : allModes()) if (m.id == "sonde") C.m = m;
    C.m.gen = [typeMask](size_t n) { SynthConfig c; c.modeOpt[0] = typeMask; c.modeOpt[1] = 1; return clean(15, c, 8e6, n); };
    C.trialN = (size_t)(C.m.secs * C.m.rate);
    C.maxOff = (size_t)(0.5 * C.m.secs * C.m.rate);
    C.base = C.m.gen(C.trialN + C.maxOff + 64);
    scaleTo(C.base, 0.2);
    C.power = activePower(C.base);
    C.ref = std::max(1.0, trial(C, 60, 0, false, 77));
    return C;
}

int passes(const Case& C, double snr, double cfo, int n) {
    int ok = 0;
    for (int i = 0; i < n; i++) ok += trial(C, snr, cfo, false, mix64(0x40ull * 1000 + (uint64_t)(snr * 10) + (uint64_t)i * 7919)) >= 0.5 * C.ref;
    return ok;
}
} // namespace

int main() {
#ifndef _WIN32
    setenv("DECT2_NOPACE", "1", 1);
#endif
    const Case rs41 = makeCase(1), dfm = makeCase(2), m10 = makeCase(4);
    CHECK(rs41.ref >= 2, "RS41 clean reference %.1f", rs41.ref);
    for (double cfo : {0.0, 8000.0, -3000.0}) {
        const int ok = passes(rs41, 7.0, cfo, 3);
        printf("RS41 7 dB, carrier error %.0f Hz: %d of 3\n", cfo, ok);
        CHECK(ok == 3, "RS41 at 7 dB, %.0f Hz: %d of 3", cfo, ok);
    }
    const int okd = passes(dfm, 20.0, 0, 3), okm = passes(m10, 20.0, 0, 3);
    printf("DFM 20 dB %d of 3, M10 20 dB %d of 3\n", okd, okm);
    CHECK(okd >= 2, "DFM at 20 dB: %d of 3", okd);
    CHECK(okm >= 2, "M10 at 20 dB: %d of 3", okm);
    double falses = 0;
    for (int i = 0; i < 2; i++) falses += trial(rs41, 10, 0, true, 1000 + (uint64_t)i);
    printf("noise only: %.0f units\n", falses);
    CHECK(falses == 0, "noise-only run gave %.0f units", falses);
    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
