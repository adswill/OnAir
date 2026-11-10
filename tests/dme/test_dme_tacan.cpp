// TACAN bearing: rotation at 15 and 135 Hz, the reference bursts of MIL-STD-291C (X and Y), random replies, noise. Simulated pulses only.
#include "dect2/dme/dme_rx.h"
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>
using namespace dect2::dme;
#include "../common/test_util.h"
#undef CHECK
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static std::vector<float> scene(double bearing, bool y, double seconds, double fs, double noise, unsigned seed) {
    const double w = 2 * M_PI * 15, w1 = (bearing - 90) * M_PI / 180, w9 = (bearing - 90) * 9 * M_PI / 180;
    struct P { double t; float a; };
    std::vector<P> ps;
    auto amp = [&](double t) { return (float)(0.5 * (1 + 0.2 * std::cos(w * t - w1) + 0.15 * std::cos(9 * w * t - w9))); };
    std::mt19937 g(seed);
    const double gap = y ? 30e-6 : 12e-6;
    const double mainN = 15, aux = 135;
    for (int c = 0; c < (int)(seconds * mainN); c++) {
        const double t = c / mainN;
        if (!y) for (int k = 0; k < 12; k++) { ps.push_back({t + k * 30e-6, amp(t)}); ps.push_back({t + k * 30e-6 + 12e-6, amp(t)}); }
        else for (int k = 0; k < 13; k++) ps.push_back({t + k * 30e-6, amp(t)});
    }
    for (int c = 0; c < (int)(seconds * aux); c++) {
        if (c % 9 == 0) continue;
        const double t = c / aux;
        if (!y) for (int k = 0; k < 6; k++) { ps.push_back({t + k * 24e-6, amp(t)}); ps.push_back({t + k * 24e-6 + 12e-6, amp(t)}); }
        else for (int k = 0; k < 13; k++) ps.push_back({t + k * 15e-6, amp(t)});
    }
    std::exponential_distribution<double> ex(2700);
    for (double t = 0.001; t < seconds; t += std::max(ex(g), 110e-6)) {
        double phase = std::fmod(t, 1.0 / aux);   // leave the burst slots alone
        if (phase < 700e-6) continue;
        ps.push_back({t, amp(t)}); ps.push_back({t + gap, amp(t)});
    }
    const size_t n = (size_t)(seconds * fs);
    std::vector<float> env(n, 0.f);
    const double width = 3.5e-6, rise = 2.5e-6;
    for (const auto& p : ps) {
        const size_t a = (size_t)std::max(0.0, (p.t - rise) * fs), b = std::min(n, (size_t)((p.t + width + rise) * fs));
        for (size_t i = a; i < b; i++) {
            const double t = (double)i / fs;
            double v = 0;
            if (t >= p.t - rise / 2 && t < p.t + rise / 2) v = 0.5 - 0.5 * std::cos(M_PI * (t - (p.t - rise / 2)) / rise);
            else if (t >= p.t + rise / 2 && t < p.t + width - rise / 2) v = 1;
            else if (t >= p.t + width - rise / 2 && t < p.t + width + rise / 2) v = 0.5 + 0.5 * std::cos(M_PI * (t - (p.t + width - rise / 2)) / rise);
            env[i] = std::max(env[i], (float)v * p.a);
        }
    }
    if (noise > 0) { std::normal_distribution<float> nd(0.f, (float)noise); for (auto& e : env) { const float re = e + nd(g), im = nd(g); e = std::sqrt(re * re + im * im); } }
    return env;
}

static double diff(double a, double b) { double d = std::fmod(a - b + 540, 360) - 180; return d; }

int main() {
    const double fs = 4e6;
    for (bool y : {false, true}) for (double bearing : {12.0, 95.0, 187.0, 268.5, 359.0}) for (double noise : {0.0, 0.03}) {
        auto e = scene(bearing, y, 4.0, fs, noise, 5u);
        Receiver r;
        r.configure(fs);
        r.feed(e.data(), e.size());
        const auto& t = r.telemetry();
        printf("%s bearing %.1f noise %.2f: valid %d, %.2f, depth %.2f %.2f\n", y ? "Y" : "X", bearing, noise, (int)t.tacanValid, t.tacanRadialDeg, (double)t.tacanDepth15, (double)t.tacanDepth135);
        CHECK(t.tacanValid);
        CHECK(std::fabs(diff(t.tacanRadialDeg, bearing)) < 1.5);
    }
    // a plain DME, no bursts: no bearing
    { std::mt19937 g(1); std::exponential_distribution<double> ex(2700); std::vector<float> env((size_t)(fs * 2), 0.f);
      for (double t = 0.001; t < 1.99; t += std::max(ex(g), 110e-6)) for (double dt : {0.0, 12e-6}) { const size_t a = (size_t)((t + dt) * fs), b = a + (size_t)(3.5e-6 * fs); for (size_t i = a; i < b; i++) env[i] = 1.f; }
      Receiver r; r.configure(fs); r.feed(env.data(), env.size()); CHECK(!r.telemetry().tacanValid); }
    return testSummary("dme_tacan");
}
