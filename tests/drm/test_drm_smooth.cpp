// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// DRM receiver: how smooth the sound is. Every case runs the synthetic signal through an impairment (noise, the fading channels of Annex B.1, flat
// Rayleigh fading at several speeds, deep holes in the signal, impulse noise, offsets) into the receiver and listens to the 48 kHz sound as a sound
// device would play it (testkit.h, listen()). Each line prints the health of every stage (lock, FAC, SDC, multiplex frames, audio frames) and what the
// listener gets: dropouts per minute, seconds of dropout (silence or underrun) per minute, seconds of concealment per minute.
// The checks are the regression thresholds of docs/DRM-AUDIT.md; ONAIR_DRM_AUDIT=1 runs the whole survey of that document instead (several minutes).
#include "../data/drm/testkit.h"
#include "../common/jobs.h"
#include <atomic>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <string>
using namespace drmtest;
using testjobs::jprintf;
static std::atomic<int> fails{0};
static testjobs::Jobs jobs;
#define CHECK(c, ...) do { if (!(c)) { jprintf("FAIL: " __VA_ARGS__); jprintf("\n"); fails++; } } while (0)

static constexpr double kRate = 48000;

struct Case {
    std::string name;
    Sig sig;
    Fade fade;                 // kind 0: the noise of the synthetic source (sig.snrDb)
    double snrDb = 30;         // with a fade: the mean SNR after it
    double secs = 60;
    std::vector<std::pair<double, double>> zeros;
    double rate = kRate;
    int special = 0;           // 1 impulse noise (1 ms bursts every 0.7 s, 14 dB above the signal), 2 a reconfiguration at 30 s (64-QAM -> 16-QAM)
};

struct Out { Result r; Listen l; double conceal = 0; };

// a reconfiguration: two transmitters in step (the same frame timing from the start), the second one's signal from `at` seconds on (a whole number
// of super frames)
static Gen switchAt(Gen a, Gen b, double rate, double at) {
    auto n = std::make_shared<uint64_t>(0);
    auto tmp = std::make_shared<std::vector<cf32>>();
    const uint64_t cut = (uint64_t)std::llround(at * rate);
    return [a, b, n, tmp, cut](cf32* o, size_t k) {
        tmp->resize(k);
        a(o, k); b(tmp->data(), k);
        for (size_t i = 0; i < k; i++) if (*n + i >= cut) o[i] = (*tmp)[i];
        *n += k;
    };
}

static Out runCase(const Case& c) {
    Sig s = c.sig;
    Gen g;
    const double rate = c.rate;
    const double bw = s.mode == 5 ? 100000.0 : 1000.0 * (s.occ == 0 ? 10.0 : (const double[]){4.5, 5, 9, 10, 18, 20}[s.occ - 1]);
    if (c.fade.kind > 0) {
        s.snrDb = 200;
        g = withFadeNoise(synthGen(s, rate), rate, c.fade, c.snrDb, bw);
    } else {
        s.snrDb = c.snrDb;
        g = synthGen(s, rate);
    }
    if (c.special == 1) g = withImpulses(g, (size_t)(0.7 * rate), (size_t)(0.001 * rate), 0.2 * 5.0, 3);
    if (c.special == 2) { Sig s2 = s; s2.qam16 = 1 - s.qam16; g = switchAt(g, synthGen(s2, rate), rate, 30.0); }
    RunOpt o;
    o.secs = c.secs;
    o.chunk = 2400;            // 50 ms, like a radio's blocks
    o.audio = true;
    o.zeros = c.zeros;
    if (c.special == 3) o.cuts = {{20.0, 0.040}};
    Out out;
    o.chunk = (size_t)(0.05 * rate);
    out.r = runRx(g, rate, o);
    ListenOpt lo;
    if (getenv("ONAIR_DRM_OLD_DEVICE")) lo.startFrames = 12000;   // the sound device as the receiver set it up before the audit (start at 250 ms)
    out.l = listen(out.r, lo);
    out.conceal = out.l.perMin(out.r.tel.audioConcealedSecs + out.r.tel.audioMutedSecs);
    return out;
}

static void line(const Case& c, const Out& o) {
    const DrmTelemetry& t = o.r.tel;
    const double aud = (double)(t.blocksOk + t.blocksBad);
    jprintf("%-30s lock %5.1f s lost %2llu  FAC %4llu/%-3llu SDC %3llu/%-3llu MSC %3llu/%-3llu  audio %5llu/%-4llu (%4.1f%% bad)  SNR %4.1f Dop %4.2f fades %3llu resync %llu | dropouts %5.2f/min  %5.2f s/min (underrun %5.2f s silent %5.2f s)  conceal %5.2f s/min  %4.1fx\n",
            c.name.c_str(), o.r.lockSecs, (unsigned long long)o.r.lostLocks, (unsigned long long)t.facOk, (unsigned long long)t.facBad, (unsigned long long)t.sdcOk,
            (unsigned long long)t.sdcBad, (unsigned long long)t.mscFramesOk, (unsigned long long)t.mscFramesBad, (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad,
            aud > 0 ? 100.0 * (double)t.blocksBad / aud : 0.0, t.snrDb, t.dopplerHz, (unsigned long long)t.fades, (unsigned long long)t.resyncs, o.l.dropoutsPerMin(), o.l.dropoutSecsPerMin(), o.l.underrunSecs, o.l.silentSecs, o.conceal, o.r.rt());
}

static Sig sig(int mode, int qam16, int occ = 0, int channel = 0) { Sig s; s.mode = mode; s.qam16 = qam16; s.occ = occ; s.channel = channel; return s; }
static Fade rayleigh(double fd, uint32_t seed = 7) { Fade f; f.kind = 1; f.dopplerHz = fd; f.seed = seed; return f; }
static Fade holes(double len, double period = 15, double depth = 40) { Fade f; f.kind = 2; f.holeSecs = len; f.periodSecs = period; f.depthDb = depth; return f; }

static std::vector<Case> survey() {
    std::vector<Case> v;
    char b[80];
    for (double snr : {14.0, 15.0, 16.0, 18.0, 22.0}) { snprintf(b, sizeof b, "B 64-QAM AWGN %.0f dB", snr); v.push_back({b, sig(2, 0), {}, snr}); }
    for (double snr : {8.0, 9.0, 10.0, 12.0, 18.0}) { snprintf(b, sizeof b, "B 16-QAM AWGN %.0f dB", snr); v.push_back({b, sig(2, 1), {}, snr}); }
    v.push_back({"A 16-QAM AWGN 12 dB", sig(1, 1), {}, 12});
    v.push_back({"A 16-QAM 4.5 kHz AWGN 14 dB", sig(1, 1, 1), {}, 14});
    v.push_back({"B 64-QAM 20 kHz AWGN 22 dB", sig(2, 0, 6), {}, 22});
    v.push_back({"C 16-QAM AWGN 14 dB", sig(3, 1), {}, 14});
    v.push_back({"D 16-QAM AWGN 14 dB", sig(4, 1), {}, 14});
    for (int ch = 2; ch <= 6; ch++) for (double snr : {20.0, 26.0}) { snprintf(b, sizeof b, "B 16-QAM channel %d %.0f dB", ch, snr); v.push_back({b, sig(2, 1, 0, ch), {}, snr}); }
    v.push_back({"D 16-QAM channel 6 20 dB", sig(4, 1, 0, 6), {}, 20});
    v.push_back({"A 64-QAM channel 2 26 dB", sig(1, 0, 0, 2), {}, 26});
    for (double fd : {0.05, 0.2, 0.5, 2.0}) { snprintf(b, sizeof b, "B 16-QAM Rayleigh %.2f Hz 22 dB", fd); v.push_back({b, sig(2, 1), rayleigh(fd), 22, 90}); }
    v.push_back({"B 64-QAM Rayleigh 0.5 Hz 28 dB", sig(2, 0), rayleigh(0.5), 28, 90});
    for (double len : {0.1, 0.3, 0.8, 2.0, 5.0}) { snprintf(b, sizeof b, "B 16-QAM hole %.1f s / 15 s", len); v.push_back({b, sig(2, 1), holes(len), 25, 90}); }
    { Case c{"B 16-QAM 2 s zeros at 20 s", sig(2, 1), {}, 25, 60}; c.zeros = {{20.0, 2.0}}; v.push_back(c); }
    { Case c{"B 16-QAM CFO +1 kHz SRO 60 ppm", sig(2, 1), {}, 22, 60}; c.sig.cfoHz = 1000; c.sig.sroPpm = 60; v.push_back(c); }
    { Case c{"B 16-QAM SRO -150 ppm", sig(2, 1), {}, 22, 60}; c.sig.sroPpm = -150; v.push_back(c); }
    { Case c{"B 16-QAM impulse noise 22 dB", sig(2, 1), {}, 22, 60}; c.special = 1; v.push_back(c); }
    { Case c{"B 64->16-QAM reconfiguration at 30 s", sig(2, 0), {}, 26, 60}; c.special = 2; v.push_back(c); }
    { Case c{"B 16-QAM 40 ms lost at 20 s", sig(2, 1), {}, 25, 60}; c.zeros = {}; c.special = 3; v.push_back(c); }
    { Case c{"E 16-QAM AWGN 12 dB (192 kHz)", sig(5, 0), {}, 12, 40}; c.rate = 192000; v.push_back(c); }
    { Case c{"E 16-QAM hole 0.5 s / 10 s (192 kHz)", sig(5, 0), holes(0.5, 10), 18, 40}; c.rate = 192000; v.push_back(c); }
    return v;
}

int main() {
    if (const char* one = getenv("ONAIR_DRM_CASE")) {   // the survey cases whose name contains this text, one after the other
        for (const Case& c : survey()) if (c.name.find(one) != std::string::npos) {
            const Out o = runCase(c);
            line(c, o);
            if (getenv("ONAIR_DRM_LOG")) fputs(o.r.log.c_str(), stdout);
        }
        return 0;
    }
    const char* e = getenv("ONAIR_DRM_AUDIT");
    if (e && *e == '1') {
        for (const Case& c : survey()) jobs.add([c] { line(c, runCase(c)); });
        jobs.run();
        return 0;
    }
    // The regression thresholds: made-up sound (concealed or muted) and dropouts (silence or underrun) in seconds per minute, locks lost. Set at
    // about 1.5 times what the receiver measured when they were written (docs/DRM-AUDIT.md has both, and the numbers before the audit).
    // sro: the clock offset the receiver must report within 10 ppm (in fading channels the delay wanders; that must not read as a clock offset)
    struct Reg { const char* name; double maxConceal, maxDropout; int maxLost = 0; int minResync = 0; double sro = 1e9; };
    const Reg regs[] = {
        {"B 64-QAM AWGN 18 dB", 0.1, 0.05},
        {"B 64-QAM AWGN 16 dB", 0.5, 0.3},
        {"B 16-QAM AWGN 10 dB", 0.3, 0.2},
        {"B 16-QAM AWGN 9 dB", 2.0, 1.0},
        {"C 16-QAM AWGN 14 dB", 0.1, 0.05},
        {"D 16-QAM AWGN 14 dB", 0.1, 0.05},
        {"B 16-QAM channel 3 20 dB", 0.3, 0.2},
        {"B 16-QAM channel 4 20 dB", 0.3, 0.2},
        {"B 16-QAM channel 5 20 dB", 2.0, 1.0},
        {"B 16-QAM channel 5 26 dB", 0.3, 0.2, 0, 0, 0.0},
        {"B 16-QAM channel 6 26 dB", 0.3, 0.2, 0, 0, 0.0},
        {"D 16-QAM channel 6 20 dB", 0.3, 0.2},
        {"B 16-QAM Rayleigh 0.20 Hz 22 dB", 0.3, 0.2},
        {"B 16-QAM Rayleigh 2.00 Hz 22 dB", 1.0, 0.5},
        {"B 16-QAM hole 0.3 s / 15 s", 0.5, 0.3},
        {"B 16-QAM hole 2.0 s / 15 s", 14.0, 10.0},
        {"B 16-QAM hole 5.0 s / 15 s", 26.0, 24.0},
        {"B 16-QAM 2 s zeros at 20 s", 4.0, 3.0},
        {"B 16-QAM 40 ms lost at 20 s", 4.5, 4.0, 0, 1},
        {"B 64->16-QAM reconfiguration at 30 s", 4.5, 4.0},
        {"B 16-QAM SRO -150 ppm", 0.1, 0.05, 0, 0, -150.0},
        {"B 16-QAM impulse noise 22 dB", 0.3, 0.2},
        {"E 16-QAM AWGN 12 dB (192 kHz)", 0.1, 0.05},
        {"E 16-QAM hole 0.5 s / 10 s (192 kHz)", 4.5, 2.0},
    };
    const std::vector<Case> all = survey();
    for (const Reg& g : regs) {
        const Case* c = nullptr;
        for (const Case& k : all) if (k.name == g.name) c = &k;
        if (!c) { jprintf("FAIL: no case '%s'\n", g.name); fails++; continue; }
        jobs.add([g, c] {
            const Out o = runCase(*c);
            line(*c, o);
            CHECK(o.r.lockSecs > 0 && o.r.lockSecs < 6.5, "%s: lock after %.1f s", g.name, o.r.lockSecs);
            CHECK(o.conceal <= g.maxConceal, "%s: %.2f s/min made up (at most %.2f)", g.name, o.conceal, g.maxConceal);
            CHECK(o.l.dropoutSecsPerMin() <= g.maxDropout, "%s: %.2f s/min of dropouts (at most %.2f)", g.name, o.l.dropoutSecsPerMin(), g.maxDropout);
            CHECK(o.l.underrunSecs == 0, "%s: the sound device ran empty for %.2f s", g.name, o.l.underrunSecs);
            CHECK((int)o.r.lostLocks <= g.maxLost, "%s: lost the signal %llu times", g.name, (unsigned long long)o.r.lostLocks);
            if (g.sro < 1e8) CHECK(std::fabs(o.r.tel.sroPpm - g.sro) < 10.0, "%s: clock offset %.1f ppm reported (%.0f sent)", g.name, o.r.tel.sroPpm, g.sro);
            CHECK((int)o.r.tel.resyncs >= g.minResync, "%s: re-synchronised %llu times (expected %d)", g.name, (unsigned long long)o.r.tel.resyncs, g.minResync);
        });
    }
    jobs.run();
    if (fails) { jprintf("%d check(s) failed\n", fails.load()); return 1; }
    jprintf("OK\n");
    return 0;
}
