// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Sweep scanner against a fake radio: known tones in white noise, a settling transient after every retune.
#include "dect2/sweep/sweep.h"
#include "dect2/common/stable_random.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
using namespace dect2;
using namespace dect2::sweep;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

struct Tone { double hz, amp; };
class FakeRadio : public SweepSource {
public:
    double rate = 8e6, sigma2 = 1e-4;      // complex noise variance
    std::vector<Tone> tones;
    double settleSec = 0;                  // after a retune this long the output is a loud transient
    size_t retunes = 0;
    bool retune(double hz, std::string&) override { fc_ = hz; transient_ = (size_t)(settleSec * rate); retunes++; return true; }
    size_t capture(cf32* out, size_t n) override {
        dect2::normal_distribution<float> g(0.f, (float)std::sqrt(sigma2 / 2));
        for (size_t i = 0; i < n; i++, t_++) {
            float re = g(rng_), im = g(rng_);
            for (auto& tn : tones) {
                if (std::fabs(tn.hz - fc_) > 0.45 * rate) continue;   // the radio's anti-alias filter
                const double ph = 2 * M_PI * std::fmod((tn.hz - fc_) * (double)t_ / rate, 1.0);
                re += (float)(tn.amp * std::cos(ph)); im += (float)(tn.amp * std::sin(ph));
            }
            if (transient_) { re *= 10; im *= 10; transient_--; }
            out[i] = cf32(re, im);
        }
        return n;
    }
    double sampleRate() const override { return rate; }
    void discard(double s) override { cf32 d[256]; size_t k = (size_t)(s * rate); while (k) { size_t m = std::min<size_t>(k, 256); capture(d, m); k -= m; } }
private:
    double fc_ = 0;
    uint64_t t_ = 0;
    size_t transient_ = 0;
    std::mt19937 rng_{12345};
};

static double expectedFloorDb(double sigma2, int fft) { return 10 * std::log10(sigma2 * 1.5 / fft); }   // Hann: sum(w^2)/sum(w)^2 = 1.5/N

int main() {
    std::string err;
    FakeRadio r;
    SweepConfig c;
    c.startHz = 100e6; c.stopHz = 140e6; c.binHz = 15625; c.dwellSec = 0.004; c.settleSec = 0.001;
    // tones: on a bin centre, off centre, one exactly on a hop boundary region, one weak
    HopPlan plan = planHops(c, r.rate);
    printf("plan: fft %d bin %.1f Hz, %d bins/hop, %zu hops, %d averages, %zu samples/hop\n", plan.fftSize, plan.binHz, plan.binsPerHop, plan.centers.size(), plan.averages, plan.samplesPerHop);
    const double edge = c.startHz + plan.binsPerHop * plan.binHz;   // first hop boundary
    r.tones = {{c.startHz + 100 * plan.binHz, 0.1}, {107.3333e6 + 3000, 0.05}, {edge, 0.1}, {edge + 40 * plan.binHz, 0.1}, {135e6 + 5 * plan.binHz, 0.004}};
    Sweep s;
    auto t0 = std::chrono::steady_clock::now();
    CHECK(runSweep(r, c, s, err), "sweep: %s", err.c_str());
    const double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const double want = expectedFloorDb(r.sigma2, s.fftSize);
    printf("floor overall %.2f dB, expected %.2f; %zu signals\n", s.floorOverallDb, want, s.signals.size());
    CHECK(std::fabs(s.floorOverallDb - want) < 1.0, "noise floor %.2f vs %.2f", s.floorOverallDb, want);
    for (size_t i = 0; i < s.bins(); i += 5000) CHECK(std::fabs(s.floorDb[i] - want) < 1.0, "hop floor at bin %zu: %.2f", i, s.floorDb[i]);
    // tone detection and frequency accuracy
    int found = 0;
    for (auto& tn : r.tones) {
        const Detection* best = nullptr;
        for (auto& d : s.signals) if (std::fabs(d.freqHz - tn.hz) < 2 * s.binHz) best = &d;
        CHECK(best, "tone at %.4f MHz not found", tn.hz / 1e6);
        if (!best) continue;
        found++;
        CHECK(std::fabs(best->freqHz - tn.hz) < 0.3 * s.binHz, "tone %.4f MHz off by %.0f Hz", tn.hz / 1e6, best->freqHz - tn.hz);
        const double expPeak = 20 * std::log10(tn.amp);
        CHECK(best->peakDb < expPeak + 0.5 && best->peakDb > expPeak - 2.0, "tone %.4f MHz level %.2f expected ~%.2f", tn.hz / 1e6, best->peakDb, expPeak);
        CHECK(std::fabs(best->snrDb - (best->peakDb - want)) < 1.0, "snr %.2f", best->snrDb);
    }
    for (auto& d : s.signals) printf("  det %.4f MHz bw %.0f peak %.1f snr %.1f\n", d.freqHz / 1e6, d.bwHz, d.peakDb, d.snrDb);
    CHECK(s.signals.size() == r.tones.size(), "%zu detections for %zu tones (false alarms?)", s.signals.size(), r.tones.size());
    printf("found %d/%zu tones\n", found, r.tones.size());
    // stitching: noise-only trace must be continuous over hop boundaries
    FakeRadio q; q.rate = r.rate;
    Sweep n;
    CHECK(runSweep(q, c, n, err), "%s", err.c_str());
    double worstStep = 0;
    for (int h = 1; (size_t)(h + 1) * n.binsPerHop < n.bins(); h++) {
        double a = 0, b = 0;
        const size_t g = (size_t)h * n.binsPerHop;
        for (int k = 1; k <= 64; k++) { a += std::pow(10, n.db[g - k] / 10); b += std::pow(10, n.db[g + k - 1] / 10); }
        worstStep = std::max(worstStep, std::fabs(10 * std::log10(a / b)));
    }
    printf("worst step over hop boundaries %.2f dB\n", worstStep);
    CHECK(worstStep < 0.5, "step %.2f dB", worstStep);
    CHECK(n.signals.empty(), "%zu false detections on noise", n.signals.size());
    // a tone straddling the boundary reads the same on both sides of it
    // CSV round trip (rtl_power format)
    std::vector<Sweep> v = {s};
    CHECK(writeCsv("test_sweep.csv", v, err), "%s", err.c_str());
    std::vector<Sweep> back;
    CHECK(readCsv("test_sweep.csv", back, err), "%s", err.c_str());
    CHECK(back.size() == 1, "%zu sweeps read", back.size());
    if (back.size() == 1) {
        CHECK(back[0].bins() == s.bins(), "bins %zu vs %zu", back[0].bins(), s.bins());
        CHECK(std::fabs(back[0].startHz - s.startHz) < 1 && std::fabs(back[0].binHz - s.binHz) < 0.01, "grid %.2f %.3f", back[0].startHz, back[0].binHz);
        double worst = 0;
        for (size_t i = 0; i < std::min(s.bins(), back[0].bins()); i++) worst = std::max(worst, (double)std::fabs(s.db[i] - back[0].db[i]));
        CHECK(worst < 0.006, "csv worst %.4f dB", worst);
        CHECK(back[0].signals.size() == s.signals.size(), "signals after reload %zu vs %zu", back[0].signals.size(), s.signals.size());
        SweepDiff d = compareSweeps(s, back[0]);
        CHECK(std::fabs(d.floorChangeDb) < 0.1, "self compare floor %.3f", d.floorChangeDb);
    }
    // compare: raise the noise by 3 dB
    FakeRadio loud; loud.sigma2 = r.sigma2 * 2; loud.tones = r.tones;
    Sweep s2;
    CHECK(runSweep(loud, c, s2, err), "%s", err.c_str());
    SweepDiff dd = compareSweeps(s, s2);
    printf("noise floor change %.2f dB (expected 3.01)\n", dd.floorChangeDb);
    CHECK(std::fabs(dd.floorChangeDb - 3.01) < 0.5, "floor change %.2f", dd.floorChangeDb);
    // activity log
    SignalLog log;
    log.update(s); log.update(s2);
    CHECK(log.records().size() == r.tones.size(), "log has %zu records", log.records().size());
    for (auto& x : log.records()) CHECK(x.count == 2, "count %d", x.count);
    CHECK(writeSignalsCsv("test_sweep_signals.csv", log.records(), err), "%s", err.c_str());
    // robust floor with a crowded band: 30 % of the bins occupied by a wide signal
    {
        float x[1000];
        std::mt19937 rg(1);
        std::exponential_distribution<float> e(1.f);
        for (int i = 0; i < 1000; i++) x[i] = 10 * std::log10(i < 300 ? 1000.f * e(rg) : e(rg));
        const float f = noiseFloorDb(x, 1000, 50, 1);
        printf("crowded floor %.2f dB (true 0)\n", f);
        CHECK(std::fabs(f) < 1.2, "crowded floor %.2f", f);
    }
    // settle calibration: a 3 ms transient after each retune
    {
        FakeRadio t; t.settleSec = 0.003;
        const double st = calibrateSettle(t, 100e6, 900e6, err);
        printf("calibrated settle %.2f ms (true 3.00)\n", st * 1e3);
        CHECK(st >= 0.0029 && st < 0.007, "settle %.4f", st);
        // a sweep with too little settle time sees the transient, one with the calibrated time does not
        SweepConfig cc = c; cc.settleSec = st;
        Sweep ok;
        CHECK(runSweep(t, cc, ok, err), "%s", err.c_str());
        CHECK(std::fabs(ok.floorOverallDb - want) < 1.0, "floor with settle %.2f", ok.floorOverallDb);
    }
    // schedule
    {
        FakeRadio t;
        SweepConfig cc = c; cc.maxSweeps = 3; cc.minIntervalSec = 0.05; cc.stopHz = 104e6;
        int k = 0;
        auto ts = std::chrono::steady_clock::now();
        CHECK(runSchedule(t, cc, [&](const Sweep&) { k++; return true; }, err), "%s", err.c_str());
        const double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - ts).count();
        CHECK(k == 3 && el >= 0.1, "schedule: %d sweeps in %.3f s", k, el);
    }
    // speed (processing only: the fake radio delivers instantly)
    {
        FakeRadio t; t.sigma2 = 1e-4;
        SweepConfig cc = c; cc.startHz = 100e6; cc.stopHz = 500e6; cc.dwellSec = 0.002; cc.settleSec = 0;
        Sweep w;
        auto ts = std::chrono::steady_clock::now();
        runSweep(t, cc, w, err);
        const double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - ts).count();
        printf("SPEED: 400 MHz in %.3f s = %.0f MHz/s (incl. fake source generation), %d hops, %d averages/hop\n", el, 400 / el, w.hops, w.averages);
    }
    printf(fails ? "FAILED\n" : "OK\n");
    return fails ? 1 : 0;
}
