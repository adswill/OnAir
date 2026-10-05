// ISDB-T receiver end to end: generator -> radio impairments (carrier offset, noise, echo, sample rate) -> receiver -> transport stream.
#include "dect2/exact_resampler.h"
#include "dect2/isdbt_gen.h"
#include "dect2/isdbt_rx.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <random>
#include <vector>

using namespace dect2;
using namespace dect2::isdbt;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

struct Case {
    const char* name; Params p; int frames; double radioRate, cfoHz, snrDb, sroPpm, echoDb; int echoDelay;
};

static Layer L(int seg, int mod, int rate, int ti = 0) { Layer l; l.segments = seg; l.mod = mod; l.rate = rate; l.ti = ti; return l; }

static bool run(const Case& c) {
    Generator gen(c.p, countingSource(5), 1);
    std::vector<cf32> sig, frame;
    for (int f = 0; f < c.frames; f++) { gen.nextFrame(frame); sig.insert(sig.end(), frame.begin(), frame.end()); }
    // echo (in the 8.127 MHz domain)
    if (c.echoDb > 0) {
        std::vector<cf32> e(sig.size());
        const float a = (float)std::pow(10.0, -c.echoDb / 20.0);
        for (size_t i = 0; i < sig.size(); i++) e[i] = sig[i] + (i >= (size_t)c.echoDelay ? a * sig[i - (size_t)c.echoDelay] * cf32(0.6f, 0.8f) : cf32(0, 0));
        sig.swap(e);
    }
    // to the radio's sample rate, with a sample-rate error
    ExactResampler rs;
    rs.configure(kSampleRate, c.radioRate * (1.0 + c.sroPpm * 1e-6));
    std::vector<cf32> radio;
    rs.process(sig.data(), sig.size(), radio);
    // pad with silence of noise before and after so the receiver has to find the signal
    std::mt19937 rng(11);
    std::normal_distribution<float> nd(0.f, 1.f);
    const float sigma = (float)std::sqrt(std::pow(10.0, -c.snrDb / 10.0) / 2.0);
    const size_t lead = (size_t)(0.02 * c.radioRate);
    std::vector<cf32> x(lead + radio.size());
    for (size_t i = 0; i < x.size(); i++) {
        cf32 v = i >= lead ? radio[i - lead] : cf32(0, 0);
        const double ph = 2 * M_PI * c.cfoHz * (double)i / c.radioRate;
        v *= cf32((float)std::cos(ph), (float)std::sin(ph));
        x[i] = v + cf32(nd(rng), nd(rng)) * sigma;
    }
    IsdbtReceiver rx;
    rx.configure(c.radioRate);
    long good[3] = {0, 0, 0}, bad[3] = {0, 0, 0}, gaps[3] = {0, 0, 0};
    long lastCounter[3] = {-1, -1, -1};
    long total = 0;
    // the interleavers need a while after the lock: packets before that are not judged (they are told apart by their PID, which
    // survives in the damaged ones often enough to count them per layer)
    long seen[3] = {0, 0, 0}, skip[3] = {0, 0, 0};
    for (int li = 0; li < 3; li++) if (c.p.layer[li].used()) skip[li] = (long)packetsPerFrame(c.p.mode, c.p.layer[li]) * ((95 * interleavingLength(c.p.mode, c.p.layer[li].ti) + timeInterleaveAdjust(c.p.mode, c.p.layer[li].ti)) / kSymbolsPerFrame + 4);
    rx.setPacketCallback([&](const uint8_t* p, size_t n, double) {
        for (size_t i = 0; i < n; i++) {
            const uint8_t* pk = p + i * 188;
            total++;
            int layer; unsigned counter;
            const bool isOk = !(pk[1] & 0x80) && checkCountingPacket(pk, &layer, &counter);
            if (!isOk) { bad[0]++; continue; }
            if (seen[layer]++ < skip[layer]) { lastCounter[layer] = (long)counter; continue; }
            if (lastCounter[layer] >= 0 && (long)counter != lastCounter[layer] + 1) gaps[layer]++;
            lastCounter[layer] = (long)counter;
            good[layer]++;
        }
    });
    const auto t0 = std::chrono::steady_clock::now();
    for (size_t i = 0; i < x.size();) {
        const size_t k = std::min<size_t>(x.size() - i, 20000 + rng() % 50000);
        rx.feed(x.data() + i, k);
        i += k;
    }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    RxTelemetry t;
    rx.telemetry(t, 0);
    printf("  %s: %.1f s for %.2f s of signal; state %d, tmcc %d, mode %d, GI %d, CFO %.1f Hz (true %.1f), SNR %.1f dB\n", c.name, secs, c.frames * frameSeconds(c.p.mode, c.p.guard),
           t.state, (int)t.isdbt.tmccOk, t.isdbt.mode, t.giIdx, t.cfoHz, c.cfoHz, t.dataSnrDb);
    bool ok = true;
    CHECK(t.isdbt.tmccOk && t.isdbt.mode == c.p.mode, "%s: TMCC locked, mode %d", c.name, t.isdbt.mode);
    CHECK(std::fabs(t.cfoHz - c.cfoHz) < 0.2 * kSampleRate / fftN(c.p.mode) + 50, "%s: CFO estimate %.1f (true %.1f)", c.name, t.cfoHz, c.cfoHz);
    for (int li = 0; li < 3; li++) {
        if (!c.p.layer[li].used()) continue;
        const int per = packetsPerFrame(c.p.mode, c.p.layer[li]);
        const long expect = (long)per * std::max(1, c.frames - 3 * (95 * interleavingLength(c.p.mode, c.p.layer[li].ti) + timeInterleaveAdjust(c.p.mode, c.p.layer[li].ti)) / kSymbolsPerFrame - 12) - 16;
        printf("    layer %c: %ld good, %ld gaps (expect at least %ld)\n", 'A' + li, good[li], gaps[li], expect);
        CHECK(good[li] >= expect, "%s layer %c: %ld good packets (need %ld)", c.name, 'A' + li, good[li], expect);
        CHECK(gaps[li] <= 2, "%s layer %c: %ld gaps in the packet counters", c.name, 'A' + li, gaps[li]);
        if (good[li] < expect) ok = false;
    }
    printf("    %ld bad packets in all (most of them in the first frames)\n", bad[0]);
    (void)total;
    return ok;
}

int main(int argc, char** argv) {
    const bool quick = argc > 1 && !strcmp(argv[1], "quick");
    std::vector<Case> cases;
    {
        Params p; p.mode = 1; p.guard = kGi8; p.layer[0] = L(13, k64Qam, kR34);
        cases.push_back({"mode 1, 13 seg 64QAM 3/4, 10 Msps, CFO 7.3 kHz", p, 16, 10e6, 7300, 28, 0, 0, 0});
    }
    {
        Params p; p.mode = 2; p.guard = kGi16; p.partial = true; p.layer[0] = L(1, kQpsk, kR12, 2); p.layer[1] = L(12, k16Qam, kR23, 1);
        cases.push_back({"mode 2, partial + 12 seg, 8 Msps, CFO -5.1 kHz", p, 22, 8e6, -5100, 25, 0, 0, 0});
    }
    if (!quick) {
        {
            Params p; p.mode = 3; p.guard = kGi8; p.layer[0] = L(13, k64Qam, kR23);
            cases.push_back({"mode 3, 13 seg 64QAM 2/3, 10 Msps, CFO 3.3 kHz, 20 ppm", p, 10, 10e6, 3300, 28, 20, 0, 0});
        }
        {
            Params p; p.mode = 1; p.guard = kGi4; p.partial = true; p.layer[0] = L(1, kQpsk, kR12, 1); p.layer[1] = L(4, kDqpsk, kR12, 1); p.layer[2] = L(8, k64Qam, kR78, 1);
            cases.push_back({"mode 1, partial + DQPSK + 64QAM, 12.288 Msps", p, 30, 12.288e6, 9100, 30, 0, 0, 0});
        }
        {
            Params p; p.mode = 2; p.guard = kGi4; p.layer[0] = L(13, k16Qam, kR34);
            cases.push_back({"mode 2, 13 seg 16QAM 3/4, echo -6 dB at 40 samples, 22 dB", p, 12, 10e6, -2000, 22, 0, 6, 40});
        }
    }
    for (auto& c : cases) run(c);
    printf(fails ? "isdbt rx: FAILED\n" : "isdbt rx: ok\n");
    return fails ? 1 : 0;
}
