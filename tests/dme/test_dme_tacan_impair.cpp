// TACAN through real-world faults: the radial (15 and 135 Hz modulation, MIL-STD-291C reference bursts, X and Y) with a rotation rate off by
// up to 0.5 %, a sample clock off by 100 ppm, pulse timing jitter, a noise sweep, weak and strong beacons, the receiver fed in odd block sizes;
// the Morse ident (1350 pairs/s key-down) sent at the same time as the bursts, as a TACAN beacon does. Simulated pulses only: no recording of a
// TACAN beacon was available.
#include "dect2/dme/dme_rx.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <random>
#include <string>
#include <vector>
using namespace dect2::dme;
#include "../common/test_util.h"

struct Scene {
    double bearing = 90;
    bool y = false;
    double rotScale = 1.0;     // the beacon's rotation rate relative to 15 Hz
    double clockPpm = 0;       // the receiver's sample clock error
    double jitterUs = 0;       // pulse timing jitter (sigma)
    double noise = 0;          // complex noise sigma on the envelope (pulse peak = amp)
    double amp = 1.0;          // the beacon's level
    std::string ident;         // keyed from 0.3 s, dot 0.1 s
    unsigned seed = 1;
};

static const char* morse(char c) {
    static const std::map<char, const char*> m = {{'A', ".-"}, {'C', "-.-."}, {'D', "-.."}, {'E', "."}, {'K', "-.-"}, {'N', "-."}, {'O', "---"}, {'R', ".-."},
                                                  {'S', "..."}, {'T', "-"}, {'U', "..-"}, {'V', "...-"}, {'W', ".--"}, {'X', "-..-"}};
    return m.at(c);
}

static std::vector<float> make(const Scene& s, double seconds, double fs) {
    const double f15 = 15 * s.rotScale, f135 = 135 * s.rotScale;
    const double w = 2 * M_PI * f15, w1 = (s.bearing - 90) * M_PI / 180, w9 = (s.bearing - 90) * 9 * M_PI / 180;
    auto amp = [&](double t) { return (float)(s.amp * 0.5 * (1 + 0.2 * std::cos(w * t - w1) + 0.15 * std::cos(9 * w * t - w9))); };
    struct P { double t; float a; };
    std::vector<P> ps;
    std::mt19937 g(s.seed);
    std::normal_distribution<double> jit(0.0, s.jitterUs * 1e-6);
    const double gap = s.y ? 30e-6 : 12e-6;
    auto pair = [&](double t) { const double j = s.jitterUs > 0 ? jit(g) : 0; ps.push_back({t + j, amp(t)}); ps.push_back({t + j + gap, amp(t)}); };
    for (int c = 0; c < (int)(seconds * f15); c++) {   // main reference bursts
        const double t = c / f15;
        if (!s.y) for (int k = 0; k < 12; k++) { ps.push_back({t + k * 30e-6, amp(t)}); ps.push_back({t + k * 30e-6 + 12e-6, amp(t)}); }
        else for (int k = 0; k < 13; k++) ps.push_back({t + k * 30e-6, amp(t)});
    }
    for (int c = 0; c < (int)(seconds * f135); c++) {  // auxiliary bursts (the ninth is the main one)
        if (c % 9 == 0) continue;
        const double t = c / f135;
        if (!s.y) for (int k = 0; k < 6; k++) { ps.push_back({t + k * 24e-6, amp(t)}); ps.push_back({t + k * 24e-6 + 12e-6, amp(t)}); }
        else for (int k = 0; k < 13; k++) ps.push_back({t + k * 15e-6, amp(t)});
    }
    auto inBurst = [&](double t) { return std::fmod(t, 1.0 / f135) < 700e-6; };
    // the ident key-down periods
    std::vector<std::pair<double, double>> keys;
    double t = 0.3;
    const double dot = 0.1;
    for (size_t li = 0; li < s.ident.size(); li++) {
        const std::string code = morse(s.ident[li]);
        for (size_t j = 0; j < code.size(); j++) {
            const double d = code[j] == '-' ? 3 * dot : dot;
            keys.push_back({t, t + d});
            t += d + (j + 1 < code.size() ? dot : 3 * dot);
        }
    }
    auto keyed = [&](double x) { for (const auto& k : keys) if (x >= k.first && x < k.second) return true; return false; };
    for (const auto& k : keys)
        for (double x = k.first; x < k.second; x += 1.0 / kIdentRatePps) if (!inBurst(x)) pair(x);
    std::exponential_distribution<double> ex(2700);
    for (double x = 0.001; x < seconds; x += std::max(ex(g), 110e-6)) {   // replies and squitter, not in burst slots or while the ident is keyed
        if (inBurst(x) || keyed(x) || keyed(x + 200e-6)) continue;
        pair(x);
    }
    // the receiver's clock: its sample k is at signal time k / (fs (1 + ppm))
    const double fsSig = fs * (1 + s.clockPpm * 1e-6);
    const size_t n = (size_t)(seconds * fs);
    std::vector<float> env(n, 0.f);
    const double width = 3.5e-6, rise = 2.5e-6;
    for (const auto& p : ps) {
        const long a = std::max(0L, (long)((p.t - rise) * fsSig)), b = std::min((long)n, (long)((p.t + width + rise) * fsSig) + 1);
        for (long i = a; i < b; i++) {
            const double ti = (double)i / fsSig;
            double v = 0;
            if (ti >= p.t - rise / 2 && ti < p.t + rise / 2) v = 0.5 - 0.5 * std::cos(M_PI * (ti - (p.t - rise / 2)) / rise);
            else if (ti >= p.t + rise / 2 && ti < p.t + width - rise / 2) v = 1;
            else if (ti >= p.t + width - rise / 2 && ti < p.t + width + rise / 2) v = 0.5 + 0.5 * std::cos(M_PI * (ti - (p.t + width - rise / 2)) / rise);
            env[(size_t)i] = std::max(env[(size_t)i], (float)v * p.a);
        }
    }
    if (s.noise > 0) {
        std::normal_distribution<float> nd(0.f, (float)s.noise);
        for (auto& e : env) { const float re = e + nd(g), im = nd(g); e = std::sqrt(re * re + im * im); }
    }
    return env;
}

static double diff(double a, double b) { return std::fmod(a - b + 540, 360) - 180; }

static Telemetry runRx(const std::vector<float>& e, double fs, size_t block) {
    Receiver r;
    r.configure(fs);
    for (size_t i = 0; i < e.size(); i += block) r.feed(e.data() + i, std::min(block, e.size() - i));
    return r.telemetry();
}

int main() {
    const double fs = 4e6;
    int n = 0, ok = 0, hard = 0, hardOk = 0;   // hard: noise 0.1 (pulse SNR about 14 dB), where a few misses are allowed
    // the radial through rotation rate, clock, jitter and noise, X and Y
    for (bool y : {false, true})
        for (double rot : {0.995, 1.0, 1.005})
            for (double ppm : {0.0, 100.0})
                for (double noise : {0.0, 0.05, 0.1}) {
                    Scene s; s.y = y; s.rotScale = rot; s.clockPpm = ppm; s.noise = noise; s.jitterUs = 0.1;
                    s.bearing = 37.0 + 61 * n; s.bearing = std::fmod(s.bearing, 360.0); s.seed = 10u + (unsigned)n;
                    const auto e = make(s, 4.0, fs);
                    const Telemetry t = runRx(e, fs, 65536);
                    const double err = std::fabs(diff(t.tacanRadialDeg, s.bearing));
                    n++;
                    if (t.tacanValid && err < 2.0) ok++;
                    if (noise >= 0.1) { hard++; if (t.tacanValid && err < 2.0) hardOk++; else printf("  (noise 0.10 miss: %s rot %.3f ppm %.0f bearing %.1f valid %d)\n", y ? "Y" : "X", rot, ppm, s.bearing, (int)t.tacanValid); continue; }
                    CHECK(t.tacanValid && err < 2.0, "%s rot %.3f ppm %.0f noise %.2f bearing %.1f: valid %d radial %.2f (error %.2f)", y ? "Y" : "X", rot, ppm, noise,
                          s.bearing, (int)t.tacanValid, t.tacanRadialDeg, err);
                }
    printf("radial: %d of %d within 2 degrees (noise 0.10: %d of %d)\n", ok, n, hardOk, hard);
    CHECK(hardOk * 10 >= hard * 8, "noise 0.10: only %d of %d radials", hardOk, hard);
    // weak and strong beacons (the receiver's threshold is peak-relative), odd block sizes (state across calls)
    for (double a : {0.05, 1.0, 8.0})
        for (size_t block : {(size_t)997, (size_t)65536, (size_t)1000003}) {
            Scene s; s.amp = a; s.bearing = 222.5; s.noise = 0.02 * a; s.seed = 3;
            const auto e = make(s, 4.0, fs);
            const Telemetry t = runRx(e, fs, block);
            CHECK(t.tacanValid && std::fabs(diff(t.tacanRadialDeg, 222.5)) < 2.0, "amp %.2f block %zu: valid %d radial %.2f", a, block, (int)t.tacanValid, t.tacanRadialDeg);
        }
    // other sample rates
    for (double r : {2e6, 8e6}) {
        Scene s; s.bearing = 300; s.noise = 0.03;
        const auto e = make(s, 4.0, r);
        const Telemetry t = runRx(e, r, 65536);
        CHECK(t.tacanValid && std::fabs(diff(t.tacanRadialDeg, 300)) < 2.0, "fs %.0f: valid %d radial %.2f", r, (int)t.tacanValid, t.tacanRadialDeg);
    }
    // the ident while the bursts go on: the radial and the letters together, X and Y
    for (bool y : {false, true})
        for (double noise : {0.0, 0.05}) {
            Scene s; s.y = y; s.bearing = 145; s.noise = noise; s.ident = "TAC"; s.seed = 7;
            const auto e = make(s, 5.0, fs);
            const Telemetry t = runRx(e, fs, 65536);
            CHECK(t.tacanValid && std::fabs(diff(t.tacanRadialDeg, 145)) < 2.0, "%s ident scene noise %.2f: valid %d radial %.2f", y ? "Y" : "X", noise, (int)t.tacanValid, t.tacanRadialDeg);
            CHECK(t.ident.find("TAC") != std::string::npos, "%s noise %.2f: ident read '%s'", y ? "Y" : "X", noise, t.ident.c_str());
            CHECK(t.channel == (y ? Channel::Y : Channel::X), "%s: channel %s", y ? "Y" : "X", channelName(t.channel));
        }
    return testSummary("dme_tacan_impair");
}
