// Beacon propagation monitor: a steady beacon, a fading one (sporadic-E style), an aurora-smeared one, with offset, drift and noise; no beacon gives no class.
#include "x40_gen.h"
using namespace x40gen; using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

// per-input-sample noise so that the beacon (amplitude 1) stands snrBin dB over the mean noise in a 1.17 Hz bin of the 2048 point analysis
static double noisePower(double fs, double snrBin) { return 1.0 * 510.0 * 2.0 / std::pow(10.0, snrBin / 10) * fs / 2202.0; }
// the class the monitor ends on, from the last "Path change" row
static std::string lastClass(const X40Telemetry& t) { for (auto& r : t.rows) if (r.kind == "Path change") return r.id; return "none"; }

// multiplicative fading (bandwidth about fadeHz) and a Doppler spread (random-phase tones within +-spreadHz)
static std::vector<cf32> beacon(double fs, double sec, double off, double fadeHz, double fadeDepthDb, double spreadHz, unsigned seed) {
    Rng r(seed); std::vector<cf32> x((size_t)(fs * sec));
    std::vector<double> ph(24), fr(24), am(24); for (int k = 0; k < 24; k++) { ph[k] = r.g() * 3.14; fr[k] = spreadHz * (k - 12) / 12.0; am[k] = 1; }
    const double a0 = r.g(), a1 = r.g(); double f1 = 0, f2 = 0;
    double p = 0;
    for (size_t i = 0; i < x.size(); i++) {
        const double t = i / fs;
        double ph2 = 0; if (spreadHz > 0) for (int k = 0; k < 24; k++) ph2 += 0.35 * std::sin(2 * kPi * fr[k] * t + ph[k]);
        double g = 1;
        if (fadeHz > 0) { f1 = a0 * 0 + std::sin(2 * kPi * fadeHz * t + a0) + 0.6 * std::sin(2 * kPi * fadeHz * 1.7 * t + a1); g = std::pow(10.0, fadeDepthDb / 20 * 0.5 * f1 / 1.6); }
        (void)f2;
        p += 2 * kPi * off / fs;
        x[i] = (float)g * cf32((float)std::cos(p + ph2), (float)std::sin(p + ph2));
    }
    return x;
}

int main() {
    const double fs = 250000;
    for (double off : {0.0, 180.0, -420.0}) for (double snr : {35.0, 30.0}) {
        auto x = beacon(fs, 40, off, 0, 0, 0, 3); addNoise(x, noisePower(fs, snr), 1);
        auto t = run(x40::Beacon, x, fs); CHECK(lastClass(t) == "steady", "steady off %.0f snr %.0f: '%s'", off, snr, lastClass(t).c_str());
        CHECK(std::fabs(t.cfoHz - off) < 3, "line offset %.1f want %.0f", t.cfoHz, off);
        x = beacon(fs, 60, off, 0.12, 14, 0, 4); addNoise(x, noisePower(fs, snr), 2);
        t = run(x40::Beacon, x, fs); CHECK(lastClass(t) == "fading", "fading off %.0f snr %.0f: '%s'", off, snr, lastClass(t).c_str());
        x = beacon(fs, 40, off, 0, 0, 120, 5); addNoise(x, noisePower(fs, snr), 3);
        t = run(x40::Beacon, x, fs); CHECK(lastClass(t) == "aurora-like", "aurora off %.0f snr %.0f: '%s'", off, snr, lastClass(t).c_str());
    }
    // the weak end, for the record: at 25 dB in a bin a smeared line drops under the detector
    { auto x = beacon(fs, 40, 0, 0, 0, 120, 5); addNoise(x, noisePower(fs, 25), 3); printf("aurora-smeared beacon at 25 dB reads '%s'\n", lastClass(run(x40::Beacon, x, fs)).c_str()); }
    // lower levels, for the record: steady / fading / aurora-smeared beacon
    for (double snr : {20.0, 15.0, 12.0}) { const char* nm[3] = {"steady", "fading", "aurora"}; std::string r;
      for (int k = 0; k < 3; k++) { auto x = beacon(fs, 60, 90, k == 1 ? 0.12 : 0, k == 1 ? 14 : 0, k == 2 ? 120 : 0, 12 + k); addNoise(x, noisePower(fs, snr), 6 + k); r += std::string(nm[k]) + " -> " + lastClass(run(x40::Beacon, x, fs)) + "; "; }
      printf("%.0f dB in a bin: %s\n", snr, r.c_str()); }
    // a drifting line stays steady
    { auto x = beacon(fs, 60, 0, 0, 0, 0, 6); mix(x, 100, 4.0, fs); addNoise(x, noisePower(fs, 30), 4);
      auto t = run(x40::Beacon, x, fs); CHECK(lastClass(t) == "steady", "drifting line: '%s'", lastClass(t).c_str()); }
    // false classes: noise alone
    { std::vector<cf32> z((size_t)(fs * 300)); addNoise(z, 1.0, 9); auto t = run(x40::Beacon, z, fs); CHECK(t.rows.empty(), "noise only: %zu rows", t.rows.size()); }
    // a beacon that goes away and comes back: the classes follow
    { auto a = beacon(fs, 30, 0, 0, 0, 0, 7); auto b = std::vector<cf32>((size_t)(fs * 30)); auto c = beacon(fs, 30, 0, 0, 0, 0, 8);
      a.insert(a.end(), b.begin(), b.end()); a.insert(a.end(), c.begin(), c.end()); addNoise(a, noisePower(fs, 30), 5);
      auto t = run(x40::Beacon, a, fs); CHECK(countKind(t, "Path change") >= 3, "gap: %d changes", countKind(t, "Path change")); }
    printf(fails ? "x40 beacon: %d FAILED\n" : "x40 beacon: ok\n", fails);
    return fails ? 1 : 0;
}
