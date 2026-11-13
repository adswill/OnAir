// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// DAB / DAB+ audio continuity audit (docs/DAB-AUDIT.md): the generator's clean ensemble goes through a channel model written here, independently of
// the receiver (noise, single frequency network echoes inside and beyond the guard interval, Rayleigh fading with Doppler, deep fades, impulsive
// noise, lost USB blocks, an 8-bit RTL-SDR with tuning error, frequency drift and sample clock error) into the real DabReceiver. The audio it puts
// out is played by a model of the sound card (AudioOut's buffering rules, its own clock), and what the listener would hear is measured: seconds of
// silence per minute and cutouts per minute, next to the per-stage counts (sync losses, FIB CRC, Reed-Solomon, access units, concealment).
// Every condition has a ceiling on cutouts and silence per minute.
#include "dect2/dab/dab.h"
#include "dect2/dab/dab_gen.h"
#include "../common/jobs.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <random>
#include <string>
#include <vector>
using namespace dect2;
using testjobs::jprintf;

static std::atomic<int> fails{0};
#define CHECK(c, ...) do { if (!(c)) { jprintf("FAIL line %d: ", __LINE__); jprintf(__VA_ARGS__); jprintf("\n"); fails++; } } while (0)

static const double kRate = 2.048e6;
static const double kPi = 3.14159265358979323846;

// ---------------------------------------------------------------- the channel (streaming)
struct Tap { int delay = 0; double gainDb = 0; double dopplerHz = 0; };
struct Cond {
    const char* name = "";
    int sub = 1;                       // the service played
    double snrDb = 30;                 // over the 2.048 MHz band, against the mean received power
    std::vector<Tap> taps = {{0, 0, 0}};
    double cfoHz = 0, cfoRampHzPerSec = 0, sroPpm = 0;
    double blankEvery = 0, blankLen = 0, blankAt = -1;   // seconds: the signal gone (noise only) for blankLen every blankEvery, or once at blankAt
    double dropEvery = 0; int dropLen = 0;               // a USB block of dropLen samples lost every dropEvery seconds
    double impEvery = 0, impLen = 0, impDb = 0;          // impulsive noise: bursts of impLen seconds, impDb over the signal, every impEvery
    double q8 = 0;                                        // 8-bit quantisation with this gain (rms 0.2 * q8), 0 = off
    double seconds = 20;
    double cardPpm = 0;                                   // the sound card's clock against the transmitter's
    double maxCutsPerMin = 0, maxSilencePerMin = 0;       // the regression ceilings
};

struct Channel {
    const Cond& c;
    std::mt19937 rng;
    std::normal_distribution<float> g{0.f, 1.f};
    struct Ray { double gain; std::vector<double> f, ph; };
    std::vector<Ray> rays;
    std::vector<cf32> hist;            // the last maxDelay input samples
    int maxDelay = 0;
    uint64_t t = 0;                    // samples out so far (the channel's clock)
    double cfoPh = 0;
    uint64_t nextDrop = 0;
    float sigma = 0, impSigma = 0;
    explicit Channel(const Cond& cc, uint32_t seed) : c(cc), rng(seed) {
        double sum = 0;
        for (const Tap& tp : c.taps) sum += std::pow(10.0, tp.gainDb / 10);
        std::uniform_real_distribution<double> u(0, 2 * kPi);
        for (const Tap& tp : c.taps) {
            Ray r;
            r.gain = std::sqrt(std::pow(10.0, tp.gainDb / 10) / sum);
            const int M = tp.dopplerHz > 0 ? 16 : 1;
            for (int m = 0; m < M; m++) {
                const double a = u(rng);
                r.f.push_back(tp.dopplerHz * std::cos(a));
                r.ph.push_back(tp.dopplerHz > 0 ? u(rng) : 0.0);
            }
            rays.push_back(r);
            maxDelay = std::max(maxDelay, tp.delay);
        }
        hist.assign((size_t)maxDelay, cf32(0, 0));
        const double p = 0.04;   // the generator's mean power over the active symbols
        sigma = (float)std::sqrt(p * std::pow(10.0, -c.snrDb / 10) / 2);
        impSigma = (float)std::sqrt(p * std::pow(10.0, c.impDb / 10) / 2);
        nextDrop = c.dropEvery > 0 ? (uint64_t)(c.dropEvery * kRate) : ~0ull;
    }
    cf32 coef(size_t k, double sec) const {
        const Ray& r = rays[k];
        if (r.f.size() == 1 && r.f[0] == 0) return cf32((float)r.gain, 0);
        std::complex<double> s = 0;
        for (size_t m = 0; m < r.f.size(); m++) s += std::polar(1.0, 2 * kPi * r.f[m] * sec + r.ph[m]);
        return cf32((float)(r.gain * s.real() / std::sqrt((double)r.f.size())), (float)(r.gain * s.imag() / std::sqrt((double)r.f.size())));
    }
    void process(std::vector<cf32>& x) {
        std::vector<cf32> ext(hist);
        ext.insert(ext.end(), x.begin(), x.end());
        std::vector<cf32> y(x.size());
        // fading coefficients change slowly: once per 256 samples
        std::vector<cf32> h(c.taps.size());
        for (size_t i = 0; i < x.size(); i++) {
            const uint64_t ti = t + i;
            const double sec = (double)ti / kRate;
            if ((i & 255) == 0) for (size_t k = 0; k < c.taps.size(); k++) h[k] = coef(k, sec);
            cf32 s(0, 0);
            for (size_t k = 0; k < c.taps.size(); k++) s += h[k] * ext[(size_t)maxDelay + i - (size_t)c.taps[k].delay];
            bool blank = false;
            if (c.blankEvery > 0) { const double ph = std::fmod(sec, c.blankEvery); blank = ph >= c.blankEvery - c.blankLen; }
            if (c.blankAt >= 0 && sec >= c.blankAt && sec < c.blankAt + c.blankLen) blank = true;
            if (blank) s = cf32(0, 0);
            // frequency: tuning error plus a drift (an RTL-SDR's crystal warming up)
            const double f = c.cfoHz + c.cfoRampHzPerSec * sec;
            cfoPh += 2 * kPi * f / kRate;
            if (cfoPh > kPi) cfoPh -= 2 * kPi;
            if (cfoPh < -kPi) cfoPh += 2 * kPi;
            if (f != 0) s *= cf32((float)std::cos(cfoPh), (float)std::sin(cfoPh));
            s += cf32(sigma * g(rng), sigma * g(rng));
            if (c.impEvery > 0 && std::fmod(sec, c.impEvery) < c.impLen) s += cf32(impSigma * g(rng), impSigma * g(rng));
            if (c.q8 > 0) {
                auto q = [&](float v) { v = std::fmax(-1.f, std::fmin(1.f, v * (float)c.q8)); return std::round(v * 127.f) / 127.f; };
                s = cf32(q(s.real()), q(s.imag()));
            }
            y[i] = s;
        }
        if (maxDelay > 0) hist.assign(ext.end() - maxDelay, ext.end());
        t += x.size();
        // lost USB blocks: the samples never reach the receiver
        if (t >= nextDrop && c.dropLen > 0) {
            const size_t at = (size_t)std::min<uint64_t>(y.size(), nextDrop - (t - y.size()));
            y.erase(y.begin() + (long)at, y.begin() + (long)std::min(y.size(), at + (size_t)c.dropLen));
            nextDrop += (uint64_t)(c.dropEvery * kRate);
        }
        x.swap(y);
    }
};

// ---------------------------------------------------------------- the listener: AudioOut's buffering rules on the sound card's clock
struct Listener {
    int threshold = DabAudio::kStartFrames, resume = 38400;   // AudioOut: playback starts at the threshold, after an underrun at 0.8 s
    std::deque<float> q;              // left channel only (the level is all that is measured)
    bool playing = false, started = false;
    uint64_t silentBlocks = 0, blocks = 0, cuts = 0, underruns = 0;
    bool lastSilent = false, heard = false;   // heard: the first audible block was played (the measure starts there)
    double blk = 0; int blkN = 0;
    double pend = 0;
    void push(const float* st, int n) { for (int i = 0; i < n; i++) q.push_back(st[2 * i]); }
    void sample(float v) {   // one sample played (silence: 0); 10 ms blocks below -50 dBFS are silent
        blk += (double)v * v;
        if (++blkN < 480) return;
        const bool silent = blk / 480 < 1e-5;
        if (!silent) heard = true;
        if (!heard) { blk = 0; blkN = 0; return; }
        blocks++;
        if (silent) { silentBlocks++; if (!lastSilent) cuts++; if (std::getenv("DAB_AUDIT_TRACE")) jprintf("silent block %llu (%.2f s), queued %zu, playing %d\n", (unsigned long long)blocks, blocks * 0.01, q.size(), (int)playing); }
        lastSilent = silent;
        blk = 0; blkN = 0;
    }
    void consume(double frames) {
        pend += frames;
        int n = (int)pend;
        pend -= n;
        if (!playing) {
            if ((int)q.size() >= threshold) { playing = true; started = true; threshold = resume; }
            else { if (started) for (int i = 0; i < n; i++) sample(0); return; }
        }
        const int take = std::min<int>(n, (int)q.size());
        for (int i = 0; i < take; i++) { sample(q.front()); q.pop_front(); }
        if (take < n) { underruns++; playing = false; for (int i = take; i < n; i++) sample(0); }
    }
};

struct Result { double fib = 0, auBadPct = 0, silencePerMin = 0, cutsPerMin = 0; uint64_t syncLoss = 0, sfBad = 0, rsCorr = 0, rsFail = 0, underruns = 0, concealed = 0; };

static Result runCond(const Cond& c, uint32_t seed) {
    dabgen::TxConfig tc;
    tc.utcSeconds = 1700000000;
    SynthConfig sc;
    sc.snrDb = 200; sc.sroPpm = c.sroPpm;
    auto syn = makeDabSynth(tc, sc, kRate);
    Channel ch(c, seed);
    DabReceiver rx;
    rx.configure(kRate);
    rx.audio().setSilent(true);
    rx.select(c.sub);
    Listener L;
    rx.audio().setPcmTap([&](const float* s, int n) { L.push(s, n); });
    rx.audio().setFillProbe([&] { return (int)L.q.size(); });
    const size_t blk = 32768;
    const size_t total = (size_t)(c.seconds * kRate);
    std::vector<cf32> x;
    uint64_t losses = 0;
    int lastState = 0;
    bool everLocked = false;
    for (size_t done = 0; done < total; done += blk) {
        x.resize(blk);
        syn->generate(x.data(), blk);
        ch.process(x);
        rx.feed(x.data(), x.size());
        DabTelemetry t;
        if (rx.telemetry(t, 0)) {
            if (lastState == 2 && t.state != 2) losses++;   // (also counted by the receiver: t.syncLosses)
            if (t.state == 2) everLocked = true;
            lastState = t.state;
        }
        L.consume((double)blk / kRate * 48000.0 * (1.0 + c.cardPpm * 1e-6));
    }
    (void)everLocked;
    DabTelemetry t;
    rx.telemetry(t, 0);
    const DabAudioStats a = rx.audio().stats();
    Result r;
    r.fib = (t.fibOk + t.fibBad) ? 100.0 * (double)t.fibOk / (double)(t.fibOk + t.fibBad) : 0;
    r.auBadPct = (a.auOk + a.auBad) ? 100.0 * (double)a.auBad / (double)(a.auOk + a.auBad) : 0;
    r.syncLoss = losses; r.sfBad = a.superframesBad; r.rsCorr = a.rsCorrected; r.rsFail = a.rsUncorrectable; r.underruns = L.underruns; r.concealed = a.concealedFrames;
    const double minutes = (double)L.blocks * 0.01 / 60.0;
    if (minutes > 0) { r.silencePerMin = (double)L.silentBlocks * 0.01 / minutes; r.cutsPerMin = (double)L.cuts / minutes; }
    if (!L.started) { r.silencePerMin = 60; r.cutsPerMin = 99; }
    return r;
}

// Service switching: from "OnAir Tones 1" (sub-channel 1) to "OnAir Melody" (3) and back while locked: the seconds of input until the new service's
// first audio comes out (the de-interleaver's 16 CIFs, then the superframe), and the receiver must not lose the sync for it
static void switching() {
    dabgen::TxConfig tc;
    tc.utcSeconds = 1700000000;
    SynthConfig sc;
    sc.snrDb = 20;
    auto syn = makeDabSynth(tc, sc, kRate);
    DabReceiver rx;
    rx.configure(kRate);
    rx.audio().setSilent(true);
    rx.select(1);
    double now = 0, firstAfter = -1, switchedAt = -1;
    int target = 1;
    rx.audio().setPcmTap([&](const float*, int) { if (switchedAt >= 0 && firstAfter < 0) firstAfter = now; });
    std::vector<cf32> x(32768);
    double worst = 0;
    int switches = 0;
    for (int b = 0; b < (int)(16 * kRate / 32768); b++) {
        now = (double)b * 32768 / kRate;
        if ((b == 250 || b == 500) && rx.audio().stats().pcmFrames > 0) {   // 4 s and 8 s
            target = target == 1 ? 3 : 1;
            rx.select(target);
            switchedAt = now; firstAfter = -1; switches++;
        }
        syn->generate(x.data(), x.size());
        rx.feed(x.data(), x.size());
        if (switchedAt >= 0 && firstAfter >= 0) { worst = std::max(worst, firstAfter - switchedAt); switchedAt = -1; }
    }
    DabTelemetry t;
    rx.telemetry(t, 0);
    jprintf("service switching: %d switches, the new service's audio after %.2f s at most, sync lost %llu times\n", switches, worst, (unsigned long long)t.syncLosses);
    CHECK(switches == 2 && worst > 0 && worst < 1.0 && t.syncLosses == 0 && switchedAt < 0, "switching: %d switches, %.2f s, %llu sync losses", switches, worst, (unsigned long long)t.syncLosses);
}

int main(int argc, char** argv) {
    double secs = 20;
    if (const char* e = std::getenv("DAB_AUDIT_SECONDS")) secs = std::atof(e);
    const char* only = argc > 1 ? argv[1] : nullptr;
    std::vector<Cond> cs;
    auto add = [&](Cond c) { if (c.seconds == 20) c.seconds = secs; cs.push_back(c); };
    // name, then the ceilings (cutouts per minute, seconds of silence per minute) the receiver must stay under
    auto C = [&](const char* name, double maxCuts, double maxSil) { Cond c; c.name = name; c.maxCutsPerMin = maxCuts; c.maxSilencePerMin = maxSil; return c; };
    { Cond c = C("clean, 20 dB", 0, 0); c.snrDb = 20; add(c); }
    { Cond c = C("AWGN 6 dB", 0, 0); c.snrDb = 6; add(c); }
    { Cond c = C("AWGN 5 dB", 3.2, 0.5); c.snrDb = 5; add(c); }
    { Cond c = C("AWGN 4 dB", 3.2, 0.5); c.snrDb = 4; add(c); }
    { Cond c = C("AWGN 4.5 dB, sample clock +100 ppm", 3.2, 0.5); c.snrDb = 4.5; c.sroPpm = 100; add(c); }
    { Cond c = C("AWGN 4.5 dB, sample clock -100 ppm", 3.2, 0.5); c.snrDb = 4.5; c.sroPpm = -100; add(c); }
    { Cond c = C("AWGN 4.5 dB", 3.2, 0.5); c.snrDb = 4.5; add(c); }
    { Cond c = C("AWGN 20 dB, sample clock +100 ppm", 0, 0); c.snrDb = 20; c.sroPpm = 100; add(c); }
    { Cond c = C("SFN echo 0 dB at 195 us (in guard)", 0, 0); c.snrDb = 18; c.taps = {{0, 0, 0}, {400, 0, 0}}; add(c); }
    { Cond c = C("SFN pre-echo -3 dB, main 146 us later", 0, 0); c.snrDb = 12; c.taps = {{0, -3, 0}, {300, 0, 0}}; add(c); }
    { Cond c = C("SFN echo -3 dB at 342 us (beyond guard)", 0, 0); c.snrDb = 18; c.taps = {{0, 0, 0}, {700, -3, 0}}; add(c); }
    { Cond c = C("mobile, 3 paths, Doppler 20 Hz, 10 dB", 3.2, 0.5); c.snrDb = 10; c.taps = {{0, 0, 20}, {60, -3, 20}, {200, -6, 20}}; add(c); }
    { Cond c = C("mobile, 3 paths, Doppler 60 Hz, 18 dB", 3.2, 0.5); c.snrDb = 18; c.taps = {{0, 0, 60}, {60, -3, 60}, {200, -6, 60}}; add(c); }
    { Cond c = C("mobile, 3 paths, Doppler 100 Hz, 25 dB", 30, 20); c.snrDb = 25; c.taps = {{0, 0, 100}, {60, -3, 100}, {200, -6, 100}}; add(c); }
    { Cond c = C("flat Rayleigh, Doppler 2 Hz, 12 dB", 10, 3); c.snrDb = 12; c.taps = {{0, 0, 2}}; add(c); }
    { Cond c = C("deep fade 250 ms every 4 s", 16, 3.5); c.snrDb = 18; c.blankEvery = 4; c.blankLen = 0.25; add(c); }
    { Cond c = C("deep fade 100 ms every 2 s", 3.2, 0.5); c.snrDb = 18; c.blankEvery = 2; c.blankLen = 0.1; add(c); }
    { Cond c = C("signal gone 1.5 s once (tunnel)", 3.2, 6); c.snrDb = 18; c.blankAt = 6; c.blankLen = 1.5; add(c); }
    { Cond c = C("USB block of 8192 lost every 2 s", 0, 0); c.snrDb = 20; c.dropEvery = 2; c.dropLen = 8192; add(c); }
    { Cond c = C("USB block of 131072 lost every 3 s", 10, 1.5); c.snrDb = 20; c.dropEvery = 3; c.dropLen = 131072; add(c); }
    { Cond c = C("impulsive noise 100 us +15 dB at 100 Hz", 0, 0); c.snrDb = 18; c.impEvery = 0.01; c.impLen = 100e-6; c.impDb = 15; add(c); }
    { Cond c = C("RTL-SDR: 8 bit, +25 kHz, 80 Hz/s drift, 60 ppm", 0, 0); c.snrDb = 12; c.q8 = 2.5; c.cfoHz = 25000; c.cfoRampHzPerSec = 80; c.sroPpm = 60; add(c); }
    { Cond c = C("MP2 service, Doppler 20 Hz, 10 dB", 3.2, 0.5); c.sub = 4; c.snrDb = 10; c.taps = {{0, 0, 20}, {60, -3, 20}, {200, -6, 20}}; add(c); }
    { Cond c = C("MP2 service, deep fade 250 ms every 4 s", 10, 1.5); c.sub = 4; c.snrDb = 18; c.blankEvery = 4; c.blankLen = 0.25; add(c); }
    std::vector<Result> res(cs.size());
    testjobs::Jobs jobs;
    for (size_t i = 0; i < cs.size(); i++) {
        if (only && std::string(cs[i].name).find(only) == std::string::npos) continue;
        jobs.add([&, i] { res[i] = runCond(cs[i], 1000 + (uint32_t)i); });
    }
    if (!only) jobs.add(switching);
    jobs.run(4);
    jprintf("%-48s %7s %7s %5s %6s %7s %7s %6s %8s %9s %8s\n", "condition", "FIB %", "AU bad%", "lost", "SF bad", "RS fix", "RS fail", "concl", "underrun", "silence/m", "cuts/m");
    for (size_t i = 0; i < cs.size(); i++) {
        if (only && std::string(cs[i].name).find(only) == std::string::npos) continue;
        const Result& r = res[i];
        jprintf("%-48s %7.2f %7.2f %5llu %6llu %7llu %7llu %6llu %8llu %8.2fs %8.2f\n", cs[i].name, r.fib, r.auBadPct, (unsigned long long)r.syncLoss, (unsigned long long)r.sfBad,
                (unsigned long long)r.rsCorr, (unsigned long long)r.rsFail, (unsigned long long)r.concealed, (unsigned long long)r.underruns, r.silencePerMin, r.cutsPerMin);
        CHECK(r.cutsPerMin <= cs[i].maxCutsPerMin + 1e-9 && r.silencePerMin <= cs[i].maxSilencePerMin + 1e-9, "%s: %.2f cutouts/min (max %.2f), %.2f s silence/min (max %.2f)", cs[i].name,
              r.cutsPerMin, cs[i].maxCutsPerMin, r.silencePerMin, cs[i].maxSilencePerMin);
    }
    if (fails) { jprintf("%d failed\n", fails.load()); return 1; }
    jprintf("all passed\n");
    return 0;
}
