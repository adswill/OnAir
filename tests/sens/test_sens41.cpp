// 4.1 sensitivity (docs/SENSITIVITY-4.1.md): the gaps 4.0 left open. Each section measures the new path against the 4.0 one in the same run (same seeds, same
// signals: the receivers' own switches turn the 4.1 change off), prints the curve, and fails when the new path is not better at its guard point or noise alone
// decodes anything. Simulated signals only. ONAIR_SENS41=<names> picks sections (default all), ONAIR_SENS41_TRIALS sets the trials a point.
#include "sens_modes_d.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
using namespace sens;

namespace {
int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

bool want(const char* name) {
    const char* e = getenv("ONAIR_SENS41");
    if (!e || !*e) return true;
    return strstr(e, name) != nullptr;
}
int trialsDefault(int n) { const char* e = getenv("ONAIR_SENS41_TRIALS"); return e && atoi(e) > 0 ? atoi(e) : n; }

struct Case { Mode m; Sig base; double power = 0; size_t trialN = 0, maxOff = 0; double ref = 1; };

double trial(const Case& C, double snrDb, double cfoHz, double sroPpm, bool noiseOnly, uint64_t seed) {
    std::mt19937 rng((uint32_t)(seed ^ (seed >> 32)));
    const size_t off = C.maxOff ? (size_t)(mix64(seed ^ 0x51ed) % C.maxOff) : 0;
    Sig x(C.base.begin() + (long)off, C.base.begin() + (long)std::min(C.base.size(), off + C.trialN + 64));
    if (sroPpm != 0) x = clockError(x, sroPpm);
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

Mode findMode(const char* id) {
    auto all = allModes();
    for (auto& m : extraModesD()) all.push_back(std::move(m));
    for (const auto& m : all) if (m.id == id) return m;
    printf("no mode %s\n", id);
    return Mode{};
}
void prepare(Case& C) {
    C.trialN = (size_t)(C.m.secs * C.m.rate);
    C.maxOff = (size_t)(0.5 * C.m.secs * C.m.rate);
    C.base = C.m.gen(C.trialN + C.maxOff + 64);
    scaleTo(C.base, 0.2);
    C.power = activePower(C.base);
    C.ref = std::max(1.0, trial(C, 60, 0, 0, false, 77));
}
double frac(const Case& C, double snr, double cfo, double sro, int n, uint64_t salt) {
    double s = 0;
    for (int i = 0; i < n; i++) s += std::min(1.0, trial(C, snr, cfo, sro, false, mix64(salt * 1000 + (uint64_t)((snr + 50) * 10) + (uint64_t)i * 7919)) / C.ref);
    return s / n;
}
double falses(const Case& C, int n, uint64_t salt) {
    double f = 0;
    for (int i = 0; i < n; i++) f += trial(C, 10, 0, 0, true, mix64(salt + (uint64_t)i));
    return f;
}
// one curve: the 4.0 path (`oldC`) and the 4.1 path (`newC`) at every point, both printed; returns the sum of (new - old) over the points
double curve(const char* name, const Case& oldC, const Case& newC, const std::vector<double>& snrs, double cfo, double sro, int n, uint64_t salt) {
    double gain = 0;
    printf("%-14s carrier %+6.0f Hz clock %3.0f ppm (ref %.0f / %.0f units):", name, cfo, sro, oldC.ref, newC.ref);
    for (double snr : snrs) {
        const double a = frac(oldC, snr, cfo, sro, n, salt), b = frac(newC, snr, cfo, sro, n, salt);
        printf("  %g dB %.0f->%.0f %%", snr, 100 * a, 100 * b);
        gain += b - a;
    }
    printf("\n");
    return gain;
}

// ------------------------------------------------------------------------------------------- 1. iMet-54 (and MRZ): soft symbols
Case sondeCase(int mask, double secs, bool soft) {
    Case C;
    C.m = findMode("sonde");
    C.m.secs = secs;
    C.m.gen = [mask](size_t n) { SynthConfig c; c.modeOpt[0] = mask; c.modeOpt[1] = 1; return clean(15, c, 8e6, n); };
    C.m.make = factory<SondeReceiver, SondeTelemetry>([soft](SondeReceiver& rx, double r) {
        rx.setSynchronous(true); rx.setSignalOffset(0); rx.configure(r); rx.setCenterMhz(403); rx.setSoftDecisions(soft); },
        blocks<SondeTelemetry>, false, [](SondeReceiver& rx) { rx.flush(); });
    prepare(C);
    return C;
}
void sectionSonde() {
    const int n = trialsDefault(4);
    {   // RS41 for the comparison (no change)
        const Case r = sondeCase(1, 12.0, true);
        printf("RS41 reference:");
        for (double snr : {9.0, 7.0, 5.0, 3.0}) printf("  %g dB %.0f %%", snr, 100 * frac(r, snr, 0, 0, n, 0x41a1));
        printf("\n");
    }
    const Case io = sondeCase(128, 12.0, false), in = sondeCase(128, 12.0, true);
    CHECK(in.ref >= 3, "iMet-54 clean reference %.1f", in.ref);
    curve("iMet-54", io, in, {13, 11, 9, 7, 5}, 0, 0, n, 0x41a1);
    curve("iMet-54", io, in, {11, 9, 7}, 8000, 0, n, 0x41a2);
    curve("iMet-54", io, in, {11, 9, 7}, 0, 50, n, 0x41a3);
    const double g = frac(in, 9, 0, 0, n, 0x41a4);
    printf("iMet-54 guard 9 dB: %.0f %%\n", 100 * g);
    CHECK(g >= 0.6, "iMet-54 9 dB soft %.2f", g);
    const Case mo = sondeCase(256, 24.0, false), mn = sondeCase(256, 24.0, true);
    CHECK(mn.ref >= 3, "MRZ clean reference %.1f", mn.ref);
    curve("MRZ", mo, mn, {10, 8, 6, 4, 2}, 0, 0, n, 0x41b1);
    curve("MRZ", mo, mn, {8, 6, 4}, 8000, 0, n, 0x41b2);
    curve("MRZ", mo, mn, {8, 6, 4}, 0, 50, n, 0x41b3);
    const double gm = frac(mn, 6, 0, 0, n, 0x41b4);
    printf("MRZ guard 6 dB: %.0f %%\n", 100 * gm);
    CHECK(gm >= 0.6, "MRZ 6 dB soft %.2f", gm);
    for (const Case* c : {&in, &mn}) { const double f = falses(*c, 4, 0x41f0); printf("sonde noise-only units: %.0f\n", f); CHECK(f == 0, "sonde noise-only %.0f", f); }
}
} // namespace

int main() {
#ifndef _WIN32
    setenv("DECT2_NOPACE", "1", 1);
#endif
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (want("sonde")) sectionSonde();
    printf(fails ? "FAILED\n" : "OK\n");
    return fails ? 1 : 0;
}
