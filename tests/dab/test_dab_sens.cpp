// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// DAB / DAB+ decoding sensitivity: the threshold SNR (the lowest SNR, in 0.5 dB steps, at which at least 90 % of the FIB CRCs pass and at least
// 90 % of the DAB+ access units decode) under white noise, TU6- and RA4-like multipath with Doppler, carrier offsets of +-5 kHz and sample clock
// errors of +-50 ppm; and the time to lock from a cold start (sync found, first FIB decoded, the service list known, first access unit decoded),
// median and 90th percentile over random start points. The generator (dab_gen.cpp) and the channel model here are independent of the receiver.
// Usage: test_dab_sens [thr|lock] [condition substring]. DAB_SENS_SECONDS changes the seconds per SNR point (default 6), DAB_SENS_STEP the
// step (0.5 dB), DAB_SENS_TOP the first SNR (14 dB).
#include "dect2/dab/dab.h"
#include "dect2/dab/dab_gen.h"
#include "../common/jobs.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>
using namespace dect2;
using testjobs::jprintf;

static std::atomic<int> fails{0};
#define CHECK(c, ...) do { if (!(c)) { jprintf("FAIL line %d: ", __LINE__); jprintf(__VA_ARGS__); jprintf("\n"); fails++; } } while (0)

static const double kRate = 2.048e6;
static const double kPi = 3.14159265358979323846;

struct Tap { int delay; double gainDb; };
struct Cond {
    const char* name;
    std::vector<Tap> taps;
    double dopplerHz = 0, cfoHz = 0, sroPpm = 0;
    double maxThr = 99;       // ceiling on the measured threshold (dB)
};

// White noise over 2.048 MHz against the mean signal power, and Rayleigh fading per tap (sum of 16 sinusoids, Jakes-like)
struct Channel {
    const Cond& c;
    std::mt19937 rng;
    std::normal_distribution<float> g{0.f, 1.f};
    std::vector<double> amp;
    std::vector<std::vector<double>> f, ph;
    std::vector<cf32> hist;
    int maxDelay = 0;
    uint64_t t = 0;
    float sigma;
    Channel(const Cond& cc, double snrDb, uint32_t seed) : c(cc), rng(seed) {
        double sum = 0;
        for (const Tap& tp : c.taps) sum += std::pow(10.0, tp.gainDb / 10);
        std::uniform_real_distribution<double> u(0, 2 * kPi);
        for (const Tap& tp : c.taps) {
            amp.push_back(std::sqrt(std::pow(10.0, tp.gainDb / 10) / sum));
            std::vector<double> ff, pp;
            const int M = c.dopplerHz > 0 ? 16 : 1;
            for (int m = 0; m < M; m++) { ff.push_back(c.dopplerHz * std::cos(u(rng))); pp.push_back(c.dopplerHz > 0 ? u(rng) : 0.0); }
            f.push_back(ff); ph.push_back(pp);
            maxDelay = std::max(maxDelay, tp.delay);
        }
        hist.assign((size_t)maxDelay, cf32(0, 0));
        sigma = (float)std::sqrt(0.04 * std::pow(10.0, -snrDb / 10) / 2);   // 0.04: the generator's mean power
    }
    void process(std::vector<cf32>& x) {
        std::vector<cf32> ext(hist);
        ext.insert(ext.end(), x.begin(), x.end());
        std::vector<cf32> h(c.taps.size());
        for (size_t i = 0; i < x.size(); i++) {
            if ((i & 127) == 0) {
                const double sec = (double)(t + i) / kRate;
                for (size_t k = 0; k < c.taps.size(); k++) {
                    std::complex<double> s = 0;
                    for (size_t m = 0; m < f[k].size(); m++) s += std::polar(1.0, 2 * kPi * f[k][m] * sec + ph[k][m]);
                    s *= amp[k] / std::sqrt((double)f[k].size());
                    h[k] = cf32((float)s.real(), (float)s.imag());
                }
            }
            cf32 s(0, 0);
            for (size_t k = 0; k < c.taps.size(); k++) s += h[k] * ext[(size_t)maxDelay + i - (size_t)c.taps[k].delay];
            x[i] = s + cf32(sigma * g(rng), sigma * g(rng));
        }
        if (maxDelay > 0) hist.assign(ext.end() - maxDelay, ext.end());
        t += x.size();
    }
};

struct Point { double fib = 0, au = 0; uint64_t rsFail = 0; };

// Counts from `warm` seconds on (the lock and the 16 CIFs of interleaving are not counted)
static Point runPoint(const Cond& c, double snr, double seconds, uint32_t seed) {
    dabgen::TxConfig tc;
    tc.utcSeconds = 1700000000;
    SynthConfig sc;
    sc.snrDb = 200; sc.cfoHz = c.cfoHz; sc.sroPpm = c.sroPpm;
    auto syn = makeDabSynth(tc, sc, kRate);
    Channel ch(c, snr, seed);
    DabReceiver rx;
    rx.configure(kRate);
    rx.audio().setSilent(true);
    rx.select(1);
    const size_t blk = 32768;
    const double warm = 1.5;
    std::vector<cf32> x;
    uint64_t f0 = 0, fb0 = 0, a0 = 0, ab0 = 0;
    bool marked = false;
    for (size_t done = 0; done < (size_t)((seconds + warm) * kRate); done += blk) {
        x.resize(blk);
        syn->generate(x.data(), blk);
        ch.process(x);
        rx.feed(x.data(), x.size());
        if (!marked && (double)done / kRate >= warm) {
            DabTelemetry t; rx.telemetry(t, 0);
            const DabAudioStats a = rx.audio().stats();
            f0 = t.fibOk; fb0 = t.fibBad; a0 = a.auOk; ab0 = a.auBad; marked = true;
        }
    }
    DabTelemetry t; rx.telemetry(t, 0);
    const DabAudioStats a = rx.audio().stats();
    Point p;
    // FIBs: 12 per frame expected, a frame not decoded at all counts as 12 failed
    const double fibExp = seconds / 0.096 * 12;
    p.fib = std::min(100.0, 100.0 * (double)(t.fibOk - f0) / fibExp);
    // access units: DAB+ at 48 kHz, 6 per 120 ms superframe expected
    const double auExp = seconds / 0.12 * 6;
    p.au = std::min(100.0, 100.0 * (double)(a.auOk - a0) / auExp);
    (void)fb0; (void)ab0;
    p.rsFail = a.rsUncorrectable;
    return p;
}

struct Thr { double fic = 99, msc = 99; std::string log; };

static Thr threshold(const Cond& c, double seconds) {
    Thr r;
    bool ficDone = false, mscDone = false;
    char b[96];
    double step = 0.5, top = 14;
    if (const char* e = std::getenv("DAB_SENS_STEP")) step = std::atof(e);
    if (const char* e = std::getenv("DAB_SENS_TOP")) top = std::atof(e);
    for (double snr = top; snr >= -2 && !(ficDone && mscDone); snr -= step) {
        const Point p = runPoint(c, snr, seconds, 7000 + (uint32_t)(snr * 100 + 1000));
        if (p.au < 100) snprintf(b, sizeof b, " %.2f:%.0f/%.0f(RS %llu bad)", snr, p.fib, p.au, (unsigned long long)p.rsFail);
        else snprintf(b, sizeof b, " %.2f:%.0f/%.0f", snr, p.fib, p.au);
        r.log += b;
        if (!ficDone) { if (p.fib >= 90) r.fic = snr; else ficDone = true; }
        if (!mscDone) { if (p.au >= 90) r.msc = snr; else mscDone = true; }
    }
    return r;
}

// Time to lock from a cold start at a random point of the transmission
struct Lock { double sync = -1, fic = -1, list = -1, audio = -1; };

static Lock lockOnce(const Cond& c, double snr, uint32_t seed) {
    dabgen::TxConfig tc;
    tc.utcSeconds = 1700000000;
    SynthConfig sc;
    sc.snrDb = 200; sc.cfoHz = c.cfoHz; sc.sroPpm = c.sroPpm;
    auto syn = makeDabSynth(tc, sc, kRate);
    std::mt19937 rng(seed);
    {   // a random start: 0.5 .. 3.5 s into the transmission
        std::vector<cf32> skip(32768);
        const size_t n = (size_t)(std::uniform_real_distribution<double>(0.5, 3.5)(rng) * kRate);
        for (size_t d = 0; d < n; d += skip.size()) { skip.resize(std::min(skip.size(), n - d)); syn->generate(skip.data(), skip.size()); }
    }
    Channel ch(c, snr, seed + 1);
    DabReceiver rx;
    rx.configure(kRate);
    rx.audio().setSilent(true);
    rx.select(1);
    Lock L;
    const size_t blk = 8192;   // 4 ms: the resolution of the times
    std::vector<cf32> x;
    for (size_t done = 0; done < (size_t)(6 * kRate); done += blk) {
        x.resize(blk);
        syn->generate(x.data(), blk);
        ch.process(x);
        rx.feed(x.data(), x.size());
        const double now = (double)(done + blk) / kRate;
        DabTelemetry t; rx.telemetry(t, 0);
        if (L.sync < 0 && t.state == 2) L.sync = now;
        if (L.fic < 0 && t.fibOk > 0) L.fic = now;
        if (L.list < 0 && t.ensemble) L.list = now;
        if (L.audio < 0 && rx.audio().stats().auOk > 0) { L.audio = now; break; }
    }
    return L;
}

static void stats(std::vector<double> v, double& med, double& p90, int& missing) {
    missing = 0;
    for (double& x : v) if (x < 0) { x = 99; missing++; }
    std::sort(v.begin(), v.end());
    med = v[v.size() / 2];
    p90 = v[std::min(v.size() - 1, (size_t)std::ceil(0.9 * (double)v.size()) - 1)];
}

int main(int argc, char** argv) {
    const std::string what = argc > 1 ? argv[1] : "all";
    const char* only = argc > 2 ? argv[2] : nullptr;
    double secs = 6;
    if (const char* e = std::getenv("DAB_SENS_SECONDS")) secs = std::atof(e);
    // TU6 (COST 207 typical urban) and RA4 (rural area) delay profiles at 2.048 MHz (0.49 us per sample)
    const std::vector<Tap> tu6 = {{0, -3}, {0, 0}, {1, -2}, {3, -6}, {5, -8}, {10, -10}};
    const std::vector<Tap> ra4 = {{0, 0}, {0, -2}, {1, -10}, {1, -20}};
    std::vector<Cond> cs = {
        {"AWGN", {{0, 0}}, 0, 0, 0, 3.5},
        {"AWGN, +5 kHz", {{0, 0}}, 0, 5000, 0, 3.5},
        {"AWGN, -5 kHz", {{0, 0}}, 0, -5000, 0, 3.5},
        {"AWGN, +50 ppm", {{0, 0}}, 0, 0, 50, 3.5},
        {"AWGN, -50 ppm", {{0, 0}}, 0, 0, -50, 3.5},
        {"TU6, Doppler 10 Hz", tu6, 10, 0, 0, 8.5},
        {"TU6, Doppler 60 Hz", tu6, 60, 0, 0, 11},
        {"RA4, Doppler 30 Hz", ra4, 30, 0, 0, 9},
        {"SFN 2 paths 0 dB, 300 samples", {{0, 0}, {300, 0}}, 0, 0, 0, 5},
    };
    // lock-only conditions at 10 dB: offsets beyond the threshold tests, fading, an echo (3.2 quality pass)
    std::vector<Cond> lc = {cs[0], cs[1], cs[2], cs[3], cs[5],
                            {"AWGN, +20 kHz", {{0, 0}}, 0, 20000, 0, 99}, {"AWGN, -20 kHz", {{0, 0}}, 0, -20000, 0, 99},
                            {"AWGN, +100 kHz", {{0, 0}}, 0, 100000, 0, 99}, cs[8]};
    std::vector<std::vector<Lock>> lockC(lc.size(), std::vector<Lock>(10));
    testjobs::Jobs jobs;
    std::vector<Thr> thr(cs.size());
    const bool doThr = what == "all" || what == "thr", doLock = what == "all" || what == "lock";
    if (doThr)
        for (size_t i = 0; i < cs.size(); i++) {
            if (only && std::string(cs[i].name).find(only) == std::string::npos) continue;
            jobs.add([&, i] { thr[i] = threshold(cs[i], secs); });
        }
    const double lockSnr[] = {4, 6, 10, 20};
    const int trials = 15;
    std::vector<std::vector<Lock>> locks(4, std::vector<Lock>(trials));
    if (doLock)
        for (int s = 0; s < 4; s++)
            for (int k = 0; k < trials; k++) jobs.add([&, s, k] { locks[(size_t)s][(size_t)k] = lockOnce(cs[0], lockSnr[s], 100 + (uint32_t)(s * 1000 + k)); });
    if (doLock && !only)
        for (size_t i = 0; i < lc.size(); i++)
            for (int k = 0; k < 10; k++) jobs.add([&, i, k] { lockC[i][(size_t)k] = lockOnce(lc[i], 10, 500 + (uint32_t)(i * 100 + k)); });
    jobs.run(4);
    if (doLock && !only) {
        jprintf("time to lock at 10 dB by condition (s, 10 starts): median / p90 [missing]   sync, first audio\n");
        for (size_t i = 0; i < lc.size(); i++) {
            std::vector<double> a, b;
            for (const Lock& l : lockC[i]) { a.push_back(l.sync); b.push_back(l.audio); }
            double m[2], p[2]; int miss[2];
            stats(a, m[0], p[0], miss[0]); stats(b, m[1], p[1], miss[1]);
            jprintf("%-24s %5.2f / %5.2f [%d]   %5.2f / %5.2f [%d]\n", lc[i].name, m[0], p[0], miss[0], m[1], p[1], miss[1]);
        }
    }
    if (doThr) {
        jprintf("%-34s %9s %9s   SNR:FIB%%/AU%%\n", "condition", "FIC thr", "DAB+ thr");
        for (size_t i = 0; i < cs.size(); i++) {
            if (only && std::string(cs[i].name).find(only) == std::string::npos) continue;
            jprintf("%-34s %7.1f dB %7.1f dB  %s\n", cs[i].name, thr[i].fic, thr[i].msc, thr[i].log.c_str());
            CHECK(thr[i].msc <= cs[i].maxThr, "%s: DAB+ threshold %.1f dB (max %.1f)", cs[i].name, thr[i].msc, cs[i].maxThr);
        }
    }
    if (doLock) {
        jprintf("time to lock (s, cold start, AWGN, %d starts): median / p90 [missing]\n", trials);
        jprintf("%8s %16s %16s %16s %16s\n", "SNR", "sync", "first FIB", "service list", "first audio");
        for (int s = 0; s < 4; s++) {
            std::vector<double> v[4];
            for (const Lock& l : locks[(size_t)s]) { v[0].push_back(l.sync); v[1].push_back(l.fic); v[2].push_back(l.list); v[3].push_back(l.audio); }
            double m[4], p[4]; int miss[4];
            for (int j = 0; j < 4; j++) stats(v[j], m[j], p[j], miss[j]);
            jprintf("%6.0f dB   %5.2f / %5.2f [%d]  %5.2f / %5.2f [%d]  %5.2f / %5.2f [%d]  %5.2f / %5.2f [%d]\n", lockSnr[s], m[0], p[0], miss[0], m[1], p[1], miss[1], m[2], p[2], miss[2], m[3], p[3], miss[3]);
        }
    }
    if (fails) { jprintf("%d failed\n", fails.load()); return 1; }
    jprintf("all passed\n");
    return 0;
}
