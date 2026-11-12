// SDR input health meter: level, peak, clipping, DC offset and the blind IQ mismatch read against what the generator put in (noise plus a few signals), at three sample rates, with the verdicts for
// low, good, high and overloaded input. Simulated signals only.
#include "x40_gen3.h"
#include <cstdlib>
using namespace x40gen; using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

// wideband noise of power pw per sample, a few tones, then the radio's faults: DC offset, IQ gain / phase mismatch, clipping at +-clip
struct Radio {
    double fs, pw, dcI, dcQ, gainDb, phaseDeg, clip; Rng rng{3}; double p1 = 0, p2 = 0;
    std::vector<cf32> operator()(double, size_t n) {
        std::vector<cf32> x(n); const float sg = (float)std::sqrt(pw / 2); const double g = std::pow(10.0, gainDb / 20), ph = phaseDeg * kPi / 180;
        for (size_t i = 0; i < n; i++) {
            p1 += 2 * kPi * 123456.0 / fs; p2 += 2 * kPi * (-300000.0) / fs;
            cf32 s(sg * rng.g(), sg * rng.g()); s += (float)std::sqrt(pw) * 0.3f * (cf32((float)std::cos(p1), (float)std::sin(p1)) + cf32((float)std::cos(p2), (float)std::sin(p2)));
            double I = s.real(), Q = s.imag();
            const double Qm = g * (Q * std::cos(ph) + I * std::sin(ph));
            I += dcI; double Qo = Qm + dcQ;
            I = std::max(-clip, std::min(clip, I)); Qo = std::max(-clip, std::min(clip, Qo));
            x[i] = cf32((float)I, (float)Qo);
        }
        return x;
    }
};
static double num(const X40Telemetry& t, const char* key, const char* what) { for (auto& f : t.facts) if (f.first == key) { const char* p = strstr(f.second.c_str(), what); return p ? atof(p + strlen(what)) : atof(f.second.c_str()); } return -999; }

int main() {
    for (double fs : {250000.0, 1e6, 2.4e6}) {
        // 1. a clean radio at -26 dBFS with DC +0.5 % / -0.3 %, gain mismatch 0.8 dB and phase error 2.5 degrees
        Radio r{fs, 0.0025, 0.005, -0.003, 0.8, 2.5, 1.0};
        auto t = runChunks(x40::InputHealth, fs, 3.0, std::ref(r), 0, 145e6, 1.0);
        const double rms = num(t, "level (RMS)", ""), g = num(t, "IQ gain mismatch (Q against I)", ""), ph = num(t, "IQ phase error", ""), dcI = num(t, "DC offset", "I ");
        CHECK(std::fabs(rms - (-26.0 + 10 * std::log10(1 + 0.18))) < 1.5, "fs %.0f: RMS %.1f dBFS", fs, rms);       // the two tones add 0.18 of the noise power
        CHECK(std::fabs(g - 0.8) < 0.15, "fs %.0f: gain mismatch %.2f want 0.8", fs, g);
        CHECK(std::fabs(ph - 2.5) < 0.4, "fs %.0f: phase %.2f want 2.5", fs, ph);
        CHECK(std::fabs(dcI - 0.5) < 0.05, "fs %.0f: DC I %.3f want 0.5", fs, dcI);
        CHECK(factStr(t, "verdict").rfind("level good", 0) == 0, "verdict %s", factStr(t, "verdict").c_str());
        CHECK(num(t, "clipping", "") < 5, "clipping %.1f ppm", num(t, "clipping", ""));
    }
    // 2. too low, high and overloaded
    { Radio r{1e6, 1e-6, 0, 0, 0, 0, 1.0}; auto t = runChunks(x40::InputHealth, 1e6, 3.0, std::ref(r), 0, 145e6, 1.0); CHECK(factStr(t, "verdict").find("too low") != std::string::npos, "low: %s", factStr(t, "verdict").c_str()); }
    { Radio r{1e6, 0.2, 0, 0, 0, 0, 1.0}; auto t = runChunks(x40::InputHealth, 1e6, 3.0, std::ref(r), 0, 145e6, 1.0); printf("  RMS -7 dBFS: %s, clipping %.0f ppm\n", factStr(t, "verdict").c_str(), num(t, "clipping", "")); CHECK(factStr(t, "verdict").find("overload") != std::string::npos || factStr(t, "verdict").find("high") != std::string::npos, "high: %s", factStr(t, "verdict").c_str()); }
    // clipping at +-0.9 of a signal of -12 dBFS RMS: 0.9 = 4.6 sigma-ish... use a lower clip level for a measurable fraction
    printf("clipping: clip level / RMS (dB) -> ppm read, true ppm\n");
    for (double clipLvl : {0.5, 0.35, 0.25}) {
        Radio r{1e6, 0.0316, 0, 0, 0, 0, clipLvl};     // -15 dBFS noise; the limiter is at the generator, so the clip counter must be on full scale: scale up after the limit
        auto gen = [&](double t0, size_t n) { auto x = r(t0, n); for (auto& v : x) v *= (float)(1.0 / clipLvl); return x; };
        auto t = runChunks(x40::InputHealth, 1e6, 3.0, gen, 0, 145e6, 1.0);
        // true: share of samples whose I or Q exceeded the limit: from the generator by counting
        Radio r2{1e6, 0.0316, 0, 0, 0, 0, 9.0}; auto y = r2(0, 1000000); size_t c = 0; for (auto& v : y) if (std::fabs(v.real()) >= clipLvl || std::fabs(v.imag()) >= clipLvl) c++;
        const double want = 1e6 * (double)c / 1e6, got = num(t, "clipping", "");
        printf("  limit %.2f: %.0f ppm read, %.0f true (%s)\n", clipLvl, got, want, factStr(t, "verdict").c_str());
        CHECK(std::fabs(got - want) < 0.15 * want + 50, "clip %.2f: %.0f vs %.0f", clipLvl, got, want);
        if (want > 500) CHECK(factStr(t, "verdict").find("overload") != std::string::npos, "overload not flagged at %.0f ppm", want);
    }
    printf(fails ? "x40 inputhealth: %d FAILED\n" : "x40 inputhealth: ok\n", fails);
    return fails ? 1 : 0;
}
