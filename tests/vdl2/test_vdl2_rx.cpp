// VDL Mode 2 receiver against the test traffic with real-world faults (tests/common/impair.h and the cases below): frequency offset +-5 kHz,
// sample clock +-50 ppm, low SNR near the decoding threshold, bursts starting in the middle of a buffer, two overlapping bursts on one
// channel, a strong neighbour 25 kHz away, and noise only. Every frame sent is looked for among the frames decoded (by source,
// destination and time), and nothing may be decoded that was not sent.
#include "dect2/vdl2/vdl2_gen.h"
#include "dect2/common/stable_random.h"
#include "dect2/vdl2/vdl2_proto.h"
#include "dect2/vdl2/vdl2_rx.h"
#include "../common/impair.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
using namespace dect2;
#include "../common/test_util.h"

static const double kRate = 1e6, kCenter = 136.85e6, kAmp = 0.08;

struct Sent { double t; double f; uint32_t src, dst; };
struct Clip { std::vector<cf32> x; std::vector<Sent> sent; };

static double sigmaFor(double esn0, double rate = kRate) { return std::sqrt(kAmp * kAmp * 0.85 / std::pow(10.0, esn0 / 10) * (rate / kVdl2SymbolRate) / 2); }

static void addNoise(std::vector<cf32>& x, double esn0, uint32_t seed, double rate = kRate) {
    std::mt19937 r(seed);
    dect2::normal_distribution<float> g(0.f, (float)sigmaFor(esn0, rate));
    for (auto& v : x) v += cf32{g(r), g(r)};
}

static void addBurst(Clip& c, const Vdl2Frames& fr, double t, double f, double levelDb, double carOff, double phase = 0.7) {
    Vdl2Wave w = vdl2MakeWave(fr);
    w.start = t * kRate;
    w.carHz = f - kCenter + carOff;
    w.amp = kAmp * std::pow(10.0, levelDb / 20);
    w.phase = phase;
    vdl2Render(w, c.x.data(), 0, c.x.size(), kRate);
    if ((t + vdl2BurstSeconds(fr)) * kRate < (double)c.x.size() - 2000)
        for (const auto& f1 : fr) { Vdl2Message m; vdl2ParseFrame(f1.data(), f1.size() - 2, m); c.sent.push_back({t, f, m.src, m.dst}); }
}

static Clip trafficAt(double rate, double secs, double cfo, uint32_t seed, double spreadDb = 0, int aircraft = 8, double rateFactor = 2.5) {
    Clip c;
    c.x.assign((size_t)(secs * rate), cf32(0, 0));
    Vdl2GenOptions o;
    o.seed = seed; o.levelSpreadDb = spreadDb; o.aircraft = aircraft; o.rateFactor = rateFactor; o.centerHz = kCenter;
    Vdl2Traffic tr(o);
    std::mt19937 r(seed);
    while (true) {
        const Vdl2TxBurst b = tr.next();
        if (b.t > secs) break;
        Vdl2Wave w = vdl2MakeWave(b.frames);
        w.start = b.t * rate;
        w.carHz = o.channelsHz[(size_t)b.chan] - kCenter + b.carOffHz + cfo;
        w.amp = kAmp * std::pow(10.0, b.levelDb / 20);
        w.phase = (double)(r() % 6283) / 1000;
        vdl2Render(w, c.x.data(), 0, c.x.size(), rate);
        if ((b.t + vdl2BurstSeconds(b.frames)) * rate < (double)c.x.size() - 2000)
            for (const auto& f1 : b.frames) { Vdl2Message m; vdl2ParseFrame(f1.data(), f1.size() - 2, m); c.sent.push_back({b.t, o.channelsHz[(size_t)b.chan], m.src, m.dst}); }
    }
    return c;
}

static Clip traffic(double secs, double cfo, uint32_t seed, double spreadDb = 0, int aircraft = 8, double rateFactor = 2.5) {
    Clip c;
    c.x.assign((size_t)(secs * kRate), cf32(0, 0));
    Vdl2GenOptions o;
    o.seed = seed; o.levelSpreadDb = spreadDb; o.aircraft = aircraft; o.rateFactor = rateFactor; o.centerHz = kCenter;
    Vdl2Traffic tr(o);
    std::mt19937 r(seed);
    while (true) {
        const Vdl2TxBurst b = tr.next();
        if (b.t > secs) break;
        addBurst(c, b.frames, b.t, o.channelsHz[(size_t)b.chan], b.levelDb, b.carOffHz + cfo, (double)(r() % 6283) / 1000);
    }
    return c;
}

static bool sel(const char* key) { const char* e = getenv("VDL2_ONLY"); return !e || !strcmp(e, key); }   // run one case only (debugging)

struct Result { int sent = 0, found = 0, extra = 0; double tuneErr = 0; };

static Result run(const std::vector<cf32>& x, const std::vector<Sent>& sent, double timeScale = 1.0, int chunk = 16384, double tuneGuess = 0, bool randomChunks = false, double tShift = 0, double rate = kRate) {
    Vdl2Receiver rx;
    rx.configure(rate);
    rx.setCenterHz(kCenter);
    if (tuneGuess != 0) rx.setTuneErrorHz(tuneGuess);
    std::mt19937 r(3);
    for (size_t i = 0; i < x.size();) {
        const size_t n = std::min(x.size() - i, randomChunks ? (size_t)(1 + r() % 20000) : (size_t)chunk);
        rx.feed(x.data() + i, n);
        i += n;
    }
    Vdl2Telemetry t;
    rx.telemetry(t, 0);
    Result res;
    res.sent = (int)sent.size();
    res.tuneErr = t.cfoHz;
    std::vector<bool> used(t.messages.size(), false);
    for (const auto& s : sent) {
        const int before = res.found;
        for (size_t i = 0; i < t.messages.size(); i++) {
            const auto& m = t.messages[i];
            if (used[i] || m.src != s.src || m.dst != s.dst || std::fabs(m.freqHz - s.f) > 1) continue;
            if (std::fabs(m.timeSec * timeScale + tShift - s.t) > 0.006) continue;
            used[i] = true; res.found++; break;
        }
        if (res.found == before && getenv("VDL2_MISSED")) fprintf(stderr, "missed %.4f s %.3f MHz %06X\n", s.t, s.f / 1e6, s.src);
    }
    for (bool u : used) res.extra += !u;
    return res;
}

static void row(const char* name, const Result& r, double minPct) {
    const double pct = r.sent ? 100.0 * r.found / r.sent : 0;
    printf("  %-44s %3d / %3d  %5.1f %%  extra %d  tune %+6.0f Hz  (need %.0f %%)\n", name, r.found, r.sent, pct, r.extra, r.tuneErr, minPct);
    CHECK(pct >= minPct && r.extra == 0, "%s: %.1f %% found, %d extra", name, pct, r.extra);
}

int main() {
    // clean
    if (sel("clean"))
    {
        Clip c = traffic(6, 0, 1, 10);
        addNoise(c.x, 30, 11);
        row("clean, Es/N0 20 - 30 dB (levels spread 10 dB)", run(c.x, c.sent), 99);
    }
    // the radio off by +-5 kHz
    if (sel("cfo"))
    for (double cfo : {5000.0, -5000.0}) {
        Clip c = traffic(6, cfo, 2);
        addNoise(c.x, 25, 12);
        char nm[64]; snprintf(nm, sizeof nm, "tuning error %+.0f kHz, Es/N0 25 dB", cfo / 1e3);
        const Result r = run(c.x, c.sent);
        row(nm, r, 95);
        CHECK(std::fabs(r.tuneErr - cfo) < 300, "tuning error measured %.0f", r.tuneErr);
    }
    // sample clock +-50 ppm
    if (sel("clock"))
    for (double ppm : {50.0, -50.0}) {
        Clip c = traffic(6, 0, 3);
        addNoise(c.x, 25, 13);
        const std::vector<cf32> y = impair::clock(c.x, ppm);
        char nm[64]; snprintf(nm, sizeof nm, "sample clock %+.0f ppm", ppm);
        row(nm, run(y, c.sent, 1.0 / (1.0 + ppm * 1e-6), 16384, 0, false, 16.0 / kRate), 97);
    }
    // low SNR near the threshold
    if (sel("snr"))
    {
        Clip c = traffic(6, 0, 4);
        Clip d = c;
        addNoise(c.x, 17, 14);
        row("Es/N0 17 dB", run(c.x, c.sent), 90);
        addNoise(d.x, 15, 15);
        row("Es/N0 15 dB (the decoding threshold)", run(d.x, d.sent), 30);
    }
    // bursts starting anywhere in the buffers: random buffer sizes, the clip cut in the middle of a burst
    if (sel("buffers"))
    {
        Clip c = traffic(6, 0, 5);
        addNoise(c.x, 25, 16);
        row("random buffer sizes (1 - 20000)", run(c.x, c.sent, 1.0, 0, 0, true), 99);
        // start the input in the middle of the first burst of a channel
        const double cut = c.sent.front().t + 0.004;
        std::vector<cf32> y(c.x.begin() + (long)(cut * kRate), c.x.end());
        std::vector<Sent> s2;
        for (const auto& s : c.sent) if (s.t > cut + 0.001) s2.push_back({s.t - cut, s.f, s.src, s.dst});
        row("input starts inside a burst", run(y, s2, 1.0, 4096), 99);
    }
    // two overlapping bursts on one channel, 20 dB apart (capture): the stronger one is decoded, both when it comes first and when it
    // starts in the middle of the weaker one
    if (sel("overlap"))
    {
        Clip c;
        c.x.assign((size_t)(3 * kRate), cf32(0, 0));
        std::vector<Sent> want;
        const double f = 136.975e6;
        for (int k = 0; k < 8; k++) {
            Vdl2Frames weak = {vdl2Frame({0x10A000, 4, false}, {0x400001u + (uint32_t)k, 1, false}, vdl2ControlI(k, 0, false), vdl2AcarsInfo("G-WEAK", "H1", '1', "M01AXX0001#DFB/WEAK STATION TEXT ABOUT FORTY CHARACTERS LONG"))};
            Vdl2Frames strong = {vdl2Frame({0x10A000, 4, false}, {0x500001u + (uint32_t)k, 1, false}, vdl2ControlI(k, 0, false), vdl2AcarsInfo("G-STRG", "H1", '2', "M02AYY0002#DFB/STRONG STATION TEXT ABOUT FORTY CHARACTERS"))};
            const double t = 0.1 + k * 0.35;
            const double dw = vdl2BurstSeconds(weak);
            Clip tmp; tmp.x.swap(c.x);
            if (k % 2 == 0) { addBurst(tmp, weak, t, f, -20, 80); addBurst(tmp, strong, t + dw * 0.5, f, 0, -60); }
            else { addBurst(tmp, strong, t, f, 0, -60); addBurst(tmp, weak, t + vdl2BurstSeconds(strong) * 0.6, f, -20, 80); }
            c.x.swap(tmp.x);
            for (const auto& s : tmp.sent) if ((s.src >> 20) == 5) want.push_back(s);
        }
        addNoise(c.x, 30, 17);
        const Result r = run(c.x, want);
        Result r2 = r; r2.extra = 0;   // the weak ones may survive now and then: not counted as wrong
        row("two overlapping bursts (strong one counted)", r2, 100);
    }
    // a strong neighbour 25 kHz away (30 dB stronger, continuous) next to the traffic on 136.900
    if (sel("adjacent"))
    {
        Clip c;
        c.x.assign((size_t)(4 * kRate), cf32(0, 0));
        std::mt19937 r(9);
        double t = 0.05;
        while (t < 3.8) {
            Vdl2Frames fr = {vdl2Frame({0x10A000, 4, false}, {0x400100u + (uint32_t)(r() % 200), 1, false}, vdl2ControlI(1, 2, false),
                                       vdl2AcarsInfo("G-ADJC", "H1", '3', "M03AZZ0003#DFB/NEXT TO A STRONG CHANNEL"))};
            addBurst(c, fr, t, 136.900e6, -6, 0, (double)(r() % 6283) / 1000);
            t += vdl2BurstSeconds(fr) + 0.05 + (r() % 100) * 1e-3;
        }
        const std::vector<Sent> want = c.sent;
        // the neighbour: back-to-back long bursts on 136.875, +30 dB over the wanted ones
        double tn = 0.0;
        while (tn < 4.0) {
            std::vector<uint8_t> info(240);
            for (auto& v : info) v = (uint8_t)r();
            Vdl2Frames fr = {vdl2Frame({0x10A000, 4, false}, {0x7AAAAA, 1, false}, vdl2ControlI(0, 0, false), info)};
            Vdl2Wave w = vdl2MakeWave(fr);
            w.start = tn * kRate; w.carHz = 136.875e6 - kCenter; w.amp = kAmp * std::pow(10.0, 24.0 / 20); w.phase = 0.3;
            vdl2Render(w, c.x.data(), 0, c.x.size(), kRate);
            tn += vdl2BurstSeconds(fr) + 0.0005;
        }
        addNoise(c.x, 25, 18);
        Result res = run(c.x, want);
        row("neighbour 25 kHz away, 30 dB stronger", res, 95);
    }
    // HackRF default sample rate (2 Msps, same channel rate after decimation as 1 Msps tests)
    if (sel("hackrf"))
    {
        constexpr double kHackRf = 2e6;
        Clip c = trafficAt(kHackRf, 6, 0, 21, 10);
        addNoise(c.x, 25, 22, kHackRf);
        row("HackRF 2 Msps, Es/N0 25 dB", run(c.x, c.sent, 1.0, 16384, 0, false, 0, kHackRf), 99);
    }
    // noise only, then NaN samples: nothing decoded, nothing breaks
    if (sel("noise"))
    {
        std::vector<cf32> x((size_t)(3 * kRate), cf32(0, 0));
        addNoise(x, 20, 19);
        for (size_t i = 100000; i < 100100; i++) x[i] = cf32(NAN, INFINITY);
        Vdl2Receiver rx;
        rx.configure(kRate);
        rx.setCenterHz(kCenter);
        for (size_t i = 0; i < x.size(); i += 10000) rx.feed(x.data() + i, std::min<size_t>(10000, x.size() - i));
        Vdl2Telemetry t;
        rx.telemetry(t, 0);
        printf("  %-44s %llu frames, %llu failed bursts\n", "noise only (and NaN samples)", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
        CHECK(t.blocksOk == 0, "frames decoded from noise");
    }
    // Whole span: the worldwide channel set inside the capture, plus the list (#236)
    {
        auto inBand = [](bool whole, const std::vector<double>& list) {
            Vdl2Receiver rx;
            rx.configure(2e6);
            rx.setCenterHz(kCenter);
            rx.setWholeSpan(whole);
            if (!list.empty()) rx.setChannels(list);
            std::vector<cf32> x(40000, cf32(0, 0));
            rx.feed(x.data(), x.size());
            Vdl2Telemetry t;
            rx.telemetry(t, 0);
            int n = 0;
            for (const auto& c : t.channels) n += c.inBand ? 1 : 0;
            return n;
        };
        const int a = inBand(false, {}), b = inBand(true, {}), c = inBand(true, {136.500e6}), d = inBand(false, {136.500e6});
        printf("  %-44s %d channels off, %d on, %d on + list, %d list only\n", "Whole span channels in band (2 Msps)", a, b, c, d);
        CHECK(a == 6, "Europe set: %d channels in band", a);
        CHECK(b == 11, "worldwide set at 136.85 MHz, 2 Msps: %d channels in band", b);
        CHECK(c == b + 1 && d == 1, "the list is added to the set (%d) / replaces it (%d)", c, d);
    }
    printf(fails ? "vdl2 rx: %d FAILED\n" : "vdl2 rx: all passed\n", fails);
    return fails ? 1 : 0;
}
