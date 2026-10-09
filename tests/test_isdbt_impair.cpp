// ISDB-T receiver against real-world faults (REAL_WORLD_CHECKLIST.md): the generator's signal is damaged with tests/impair.h, which shares
// nothing with the receiver, and must still decode.
#include "dect2/exact_resampler.h"
#include "dect2/isdbt_gen.h"
#include "dect2/isdbt_rx.h"
#include "impair.h"
#include "jobs.h"
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <vector>

using namespace dect2;
using namespace dect2::isdbt;
using testjobs::jprintf;
static std::atomic<int> fails{0};

static Layer L(int seg, int mod, int rate, int ti = 0) { Layer l; l.segments = seg; l.mod = mod; l.rate = rate; l.ti = ti; return l; }

static std::vector<cf32> frames(const Params& p, int n, unsigned seed = 5) {
    Generator gen(p, countingSource(seed), 1);
    std::vector<cf32> sig, f;
    for (int i = 0; i < n; i++) { gen.nextFrame(f); sig.insert(sig.end(), f.begin(), f.end()); }
    return sig;
}
static std::vector<cf32> toRate(const std::vector<cf32>& x, double rate) {
    if (rate == kSampleRate) return x;
    ExactResampler rs;
    rs.configure(kSampleRate, rate);
    std::vector<cf32> y;
    rs.process(x.data(), x.size(), y);
    return y;
}

struct Result { long good[3] = {0, 0, 0}; long goodAfter = 0; RxTelemetry t; };

// counts the intact counting packets per layer; goodAfter: those that arrive after `afterSample` input samples were fed
static Result receive(const std::vector<cf32>& x, double rate, size_t afterSample = 0) {
    IsdbtReceiver rx;
    rx.configure(rate);
    Result r;
    size_t fed = 0;
    rx.setPacketCallback([&](const uint8_t* p, size_t n, double) {
        for (size_t i = 0; i < n; i++) {
            int layer; unsigned counter;
            if ((p[i * 188 + 1] & 0x80) || !checkCountingPacket(p + i * 188, &layer, &counter)) continue;
            r.good[layer]++;
            if (fed > afterSample) r.goodAfter++;
        }
    });
    for (size_t i = 0; i < x.size();) {
        const size_t k = std::min<size_t>(x.size() - i, 37000);
        rx.feed(x.data() + i, k);
        i += k; fed = i;
    }
    rx.telemetry(r.t, 0);
    return r;
}

static void check(bool c, const char* name, const char* what) { if (!c) { jprintf("FAIL: %s: %s\n", name, what); fails++; } }

// at least `share` of the packets of every used layer over the frames after the interleavers have filled
static void expectLayers(const char* name, const Params& p, int nFrames, int lostFrames, const Result& r, double share = 0.9) {
    jprintf("  %s: tmcc %d mode %d, CFO %.0f Hz, clock %.1f ppm, good A %ld B %ld C %ld\n", name, (int)r.t.isdbt.tmccOk, r.t.isdbt.mode, r.t.cfoHz, r.t.sroPpm, r.good[0], r.good[1], r.good[2]);
    for (int li = 0; li < 3; li++) {
        if (!p.layer[li].used()) continue;
        const long expect = (long)(share * packetsPerFrame(p.mode, p.layer[li]) * std::max(0, nFrames - lostFrames));
        char msg[96]; snprintf(msg, sizeof msg, "layer %c: %ld good packets, need %ld", 'A' + li, r.good[li], expect);
        check(r.good[li] >= expect, name, msg);
    }
}

int main() {
    testjobs::Jobs jobs;
    // development: ISDBT_CASE=n runs only the n-th case
    int caseNo = 0;
    auto add = [&](std::function<void()> f) { const char* o = getenv("ISDBT_CASE"); if (!o || atoi(o) == caseNo) jobs.add(std::move(f)); caseNo++; };
    // a sample clock 80 ppm fast at the native rate (a file written at 512/63 Msps, or a radio set to it): the clock must be corrected there too
    add([] {
        Params p; p.mode = 3; p.guard = kGi8; p.layer[0] = L(13, k64Qam, kR34);
        auto x = impair::clock(frames(p, 14), 80);
        impair::noise(x, 30);
        Result r = receive(x, kSampleRate);
        expectLayers("native rate, +80 ppm clock", p, 14, 6, r);
        check(std::fabs(r.t.sroPpm - 80) < 15, "native rate, +80 ppm clock", "clock estimate");
    });
    // a slow clock (-100 ppm) with the shortest guard interval of mode 3: the symbols drift late by 0.84 samples per symbol, into the next one
    add([] {
        Params p; p.mode = 3; p.guard = kGi32; p.layer[0] = L(13, k64Qam, kR23);
        auto x = impair::clock(toRate(frames(p, 14), 10e6), -100);
        impair::noise(x, 30);
        Result r = receive(x, 10e6);
        expectLayers("mode 3 GI 1/32, -100 ppm clock", p, 14, 6, r);
        check(std::fabs(r.t.sroPpm + 100) < 15, "mode 3 GI 1/32, -100 ppm clock", "clock estimate");
    });
    // the largest UHF tuning error (50 ppm of 860 MHz)
    add([] {
        Params p; p.mode = 1; p.guard = kGi16; p.layer[0] = L(13, k16Qam, kR23);
        auto x = toRate(frames(p, 20), 10e6);
        impair::shift(x, -43000, 10e6);
        impair::noise(x, 28);
        Result r = receive(x, 10e6);
        expectLayers("mode 1, -43 kHz tuning error", p, 20, 8, r);
    });
    // the channel 1.3 MHz beside the centre of a 10 Msps recording
    add([] {
        Params p; p.mode = 3; p.guard = kGi8; p.layer[0] = L(13, k16Qam, kR23);
        auto x = toRate(frames(p, 10), 10e6);
        impair::shift(x, 1.3e6, 10e6);
        impair::noise(x, 28);
        Result r = receive(x, 10e6);
        expectLayers("mode 3, channel 1.3 MHz off centre", p, 10, 6, r);
    });
    // I and Q swapped
    add([] {
        Params p; p.mode = 2; p.guard = kGi8; p.partial = true; p.layer[0] = L(1, kQpsk, kR12, 1); p.layer[1] = L(12, k64Qam, kR34, 1);
        auto x = toRate(frames(p, 26), 10e6);
        impair::shift(x, 6000, 10e6);
        impair::swapIq(x);
        impair::noise(x, 28);
        Result r = receive(x, 10e6);
        expectLayers("mode 2, I and Q swapped", p, 26, 14, r);
    });
    // NaN and infinite samples in the middle of the stream (a broken file or driver): they must not stop the reception for good
    add([] {
        Params p; p.mode = 1; p.guard = kGi8; p.layer[0] = L(13, kQpsk, kR12);
        auto x = toRate(frames(p, 40), 10e6);
        impair::noise(x, 25);
        const size_t at = x.size() / 3;
        for (size_t i = 0; i < 64; i++) x[at + i * 7] = cf32(std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity());
        Result r = receive(x, 10e6, at + 300000);
        const long per = packetsPerFrame(p.mode, p.layer[0]);
        jprintf("  NaN samples: %ld good, %ld after them\n", r.good[0], r.goodAfter);
        check(r.goodAfter > 22 * per, "NaN samples", "reception right after the bad samples");
    });
    // a USB drop and a file that starts in the middle of a frame
    add([] {
        Params p; p.mode = 3; p.guard = kGi16; p.layer[0] = L(13, k16Qam, kR12, 1);
        auto x = toRate(frames(p, 30), 10e6);
        impair::skip(x, 777777);
        const size_t at = x.size() / 3;
        impair::drop(x, at, 12345);
        impair::noise(x, 26);
        Result r = receive(x, 10e6, at + 600000);
        const long per = packetsPerFrame(p.mode, p.layer[0]);
        jprintf("  drop: %ld good, %ld after it\n", r.good[0], r.goodAfter);
        check(r.goodAfter > 8 * per, "USB drop", "reception after the drop");
    });
    // a reconfiguration while receiving: the modulation and the layers change
    add([] {
        Params p1; p1.mode = 3; p1.guard = kGi8; p1.layer[0] = L(13, k64Qam, kR34);
        Params p2 = p1; p2.partial = true; p2.layer[0] = L(1, kQpsk, kR12); p2.layer[1] = L(12, k16Qam, kR23);
        auto a = frames(p1, 8), b = frames(p2, 14, 9);
        const size_t at = a.size();
        a.insert(a.end(), b.begin(), b.end());
        auto x = toRate(a, 10e6);
        impair::noise(x, 28);
        Result r = receive(x, 10e6, (size_t)(at * 10e6 / kSampleRate));
        jprintf("  reconfiguration: partial %d, A %ld B %ld, %ld after the change\n", (int)r.t.isdbt.partial, r.good[0], r.good[1], r.goodAfter);
        check(r.t.isdbt.tmccOk && r.t.isdbt.partial && r.t.isdbt.layer[1].segments == 12, "reconfiguration", "new parameters");
        check(r.good[1] > 6L * packetsPerFrame(p2.mode, p2.layer[1]), "reconfiguration", "layer B after the change");
    });
    // everything at once: the worst UHF offset, a +80 ppm clock, an echo inside the guard interval and an overdriven 8-bit radio
    add([] {
        Params p; p.mode = 3; p.guard = kGi8; p.layer[0] = L(13, k16Qam, kR23, 1);
        auto x = impair::clock(toRate(frames(p, 16), 10e6), 80);
        impair::shift(x, 43000, 10e6);
        impair::echo(x, 300, -6, 2.0);
        impair::noise(x, 30);
        impair::clip8(x, 0.35);
        Result r = receive(x, 10e6);
        expectLayers("combined: +43 kHz, +80 ppm, echo, 8-bit clip", p, 16, 8, r);
    });
    jobs.run();
    jprintf(fails ? "isdbt impair: FAILED\n" : "isdbt impair: ok\n");
    return fails ? 1 : 0;
}
