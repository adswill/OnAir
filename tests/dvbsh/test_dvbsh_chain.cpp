// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// DVB-SH OFDM (SH-A) generator -> receiver round trips with impairments: unknown start, carrier offset (fractional and integer
// carriers), sampling clock offset, AWGN, a static echo inside the guard, a signal blockage under the long early/late time interleaver,
// and hierarchical 16-QAM (HP and LP). Simulated signals only. ONAIR_TEST_LONG=1 adds the remaining FFT size / constellation combinations.
#include "dect2/dvbsh/dvbsh_gen.h"
#include "dect2/dvbsh/dvbsh_rx.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <random>

using namespace dect2;
using namespace dect2::dvbsh;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

// packets that carry their own sequence number, so every received packet can be checked
static Generator::PacketSource seqSource(unsigned pid) {
    return [pid, n = 0u](uint8_t* p) mutable {
        p[0] = 0x47; p[1] = (uint8_t)(pid >> 8); p[2] = (uint8_t)pid; p[3] = (uint8_t)(0x10 | (n & 15));
        p[4] = (uint8_t)(n >> 24); p[5] = (uint8_t)(n >> 16); p[6] = (uint8_t)(n >> 8); p[7] = (uint8_t)n;
        uint32_t h = n * 2654435761u + pid;
        for (int i = 8; i < 188; i++) { h ^= h << 13; h ^= h >> 17; h ^= h << 5; p[i] = (uint8_t)h; }
        n++;
    };
}
static bool packetGood(const uint8_t* p, unsigned pid) {
    if (p[0] != 0x47 || (p[1] & 0x80) || (((p[1] & 0x1F) << 8) | p[2]) != (int)pid) return false;
    const uint32_t n = (uint32_t)p[4] << 24 | (uint32_t)p[5] << 16 | (uint32_t)p[6] << 8 | p[7];
    uint32_t h = n * 2654435761u + pid;
    for (int i = 8; i < 188; i++) { h ^= h << 13; h ^= h >> 17; h ^= h << 5; if (p[i] != (uint8_t)h) return false; }
    return true;
}

struct Impair {
    double cfoHz = 0, snrDb = 30, scoPpm = 0;
    int delay = 0;
    float echoGain = 0; int echoDelay = 0;
    double blockStart = -1, blockLen = 0;   // seconds: signal off (noise stays)
};

// windowed-sinc interpolation for the clock offset (the OFDM fills 83 % of the band: linear interpolation would distort it)
constexpr int kHalf = 24;
static float interpTap(double x) {
    if (std::fabs(x) < 1e-9) return 1.f;
    const double w = 0.42 + 0.5 * std::cos(M_PI * x / kHalf) + 0.08 * std::cos(2 * M_PI * x / kHalf);   // Blackman
    return (float)(std::sin(M_PI * x) / (M_PI * x) * w);
}

struct Result { RxStatus st; long good = 0, bad = 0; };

static Result run(const Params& p, const Impair& im, double seconds, bool lp = false, const char* name = "") {
    Generator gen(p, seqSource(0x101), seqSource(0x202));
    const double fs = gen.sampleRate();
    Receiver rx;
    rx.configure(fs);
    rx.setLowPriority(lp);
    Result r;
    const unsigned pid = lp ? 0x202 : 0x101;
    rx.setPacketCallback([&](const uint8_t* pk, size_t n) { for (size_t i = 0; i < n; i++) (packetGood(pk + i * 188, pid) ? r.good : r.bad)++; });
    std::mt19937 rng(42);
    std::normal_distribution<float> g(0.f, 1.f);
    const float sigma = (float)std::sqrt(std::pow(10.0, -im.snrDb / 10) / 2);
    std::vector<cf32> sym, out, hist;
    const long total = (long)(seconds * fs);
    for (int i = 0; i < im.delay; i++) out.push_back(cf32(sigma * g(rng), sigma * g(rng)));
    rx.feed(out.data(), out.size());
    out.clear();
    // resampler state for the clock offset (linear interpolation; the receiver sees fs (1 + ppm))
    std::vector<cf32> src;
    double t = 0;
    const double step = 1.0 / (1.0 + im.scoPpm * 1e-6);
    long produced = 0;
    double ph = 0;
    const double dph = 2 * M_PI * im.cfoHz / fs;
    while (produced < total) {
        gen.nextSymbol(sym);
        for (auto& s : sym) {
            cf32 v = s;
            if (im.echoGain != 0) {
                hist.push_back(s);
                if ((int)hist.size() > im.echoDelay) { v += im.echoGain * hist[hist.size() - 1 - im.echoDelay]; }
                if (hist.size() > 4096) hist.erase(hist.begin(), hist.begin() + 2048);
            }
            src.push_back(v);
        }
        // resample
        while (t + kHalf + 1 < (double)src.size()) {
            const long i = (long)t;
            const double f = t - i;
            cf32 v = 0;
            if (f == 0 && i >= kHalf) v = src[i];
            else for (int j = -kHalf + 1; j <= kHalf; j++) { if (i + j >= 0) v += src[i + j] * interpTap(j - f); }
            const double sec = (double)produced / fs;
            if (im.blockStart >= 0 && sec >= im.blockStart && sec < im.blockStart + im.blockLen) v = 0;
            v *= cf32((float)std::cos(ph), (float)std::sin(ph));
            ph += dph;
            if (ph > M_PI) ph -= 2 * M_PI;
            out.push_back(v + cf32(sigma * g(rng), sigma * g(rng)));
            produced++;
            t += step;
        }
        const long drop = std::max(0L, (long)t - kHalf);
        src.erase(src.begin(), src.begin() + drop);
        t -= drop;
        if (out.size() > 200000) { rx.feed(out.data(), out.size()); out.clear(); }
    }
    rx.feed(out.data(), out.size());
    r.st = rx.status();
    std::printf("%-34s %s %s GI1/%d rate %s%s: level %d, CFO %.0f Hz, SNR %.1f dB, codewords %ld ok %ld bad, packets %ld good %ld bad, iter %.2f\n",
                name, modeName(p.mode), p.mod ? (p.hier ? "16-QAM hier" : "16-QAM") : "QPSK", 32 >> p.guard, rateName(lp ? p.patLp : p.patHp), lp ? " (LP)" : "",
                r.st.level, r.st.cfoHz, r.st.snrDb, r.st.codewordsOk, r.st.codewordsBad, r.good, r.bad, r.st.avgIterations);
    return r;
}

static Params mk(int mode, int guard, int mod, int pat, double bw = 8) {
    Params p;
    p.mode = mode; p.guard = guard; p.mod = mod; p.patHp = pat; p.patLp = pat; p.bwMhz = bw; p.cellId = 0x1234;
    p.ti = TiConfig{1, 48, 1, 0, 0};
    return p;
}

int main() {
    const bool lng = getenv("ONAIR_TEST_LONG") != nullptr;
    if (getenv("DVBSH_ONE")) {
        Params p = mk(k1K, 0, k16Qam, 7, 1.7);
        Impair im; im.delay = 777; im.snrDb = 30; im.scoPpm = atof(getenv("DVBSH_ONE"));
        run(p, im, 2.2, false, "debug");
        return 0;
    }
    {   // 2K QPSK 1/3 with offsets and noise
        Params p = mk(k2K, 2, kQpsk, 4);
        Impair im; im.cfoHz = 7300; im.snrDb = 4; im.delay = 12345; im.scoPpm = 15;
        Result r = run(p, im, 0.45, false, "2K offsets 4 dB");
        CHECK(r.st.level == 3 && r.st.p.patHp == 4 && r.st.p.mode == k2K && r.st.p.cellId == 0x1234, "2K: parameters");
        CHECK(r.good > 400 && r.bad == 0 && r.st.codewordsBad == 0, "2K QPSK 1/3 at 4 dB: %ld good %ld bad", r.good, r.bad);
        CHECK(std::fabs(r.st.cfoHz - 7300) < 100, "CFO estimate %.0f", r.st.cfoHz);
    }
    {   // 8K 16-QAM 1/2, echo inside the guard
        Params p = mk(k8K, 3, k16Qam, 8);
        Impair im; im.cfoHz = -23000; im.snrDb = 12; im.delay = 5000; im.echoGain = 0.5f; im.echoDelay = 300;
        Result r = run(p, im, 1.0, false, "8K echo -23 kHz 12 dB");
        CHECK(r.good > 400 && r.bad == 0, "8K 16-QAM 1/2: %ld good %ld bad", r.good, r.bad);
    }
    {   // 1K 16-QAM 2/5 (complementary) in a 1.7 MHz channel, clock offset
        Params p = mk(k1K, 0, k16Qam, 7, 1.7);
        Impair im; im.cfoHz = 1200; im.snrDb = 13; im.delay = 777; im.scoPpm = -30;
        Result r = run(p, im, 2.2, false, "1K 1.7 MHz clock -30 ppm");
        CHECK(r.good > 300 && r.bad == 0, "1K 16-QAM 2/5: %ld good %ld bad", r.good, r.bad);
    }
    {   // hierarchical 16-QAM alpha 2: HP 1/3, LP 1/2
        Params p = mk(k2K, 1, k16Qam, 4);
        p.hier = 2; p.patLp = 9;
        Impair im; im.snrDb = 16; im.delay = 3000; im.cfoHz = 2500;
        Result hp = run(p, im, 0.6, false, "2K hierarchical HP");
        Result lp = run(p, im, 0.6, true, "2K hierarchical LP");
        CHECK(hp.good > 300 && hp.bad == 0, "hierarchical HP: %ld good %ld bad", hp.good, hp.bad);
        CHECK(lp.good > 300 && lp.bad == 0 && lp.st.p.patLp == 9 && lp.st.p.hier == 2, "hierarchical LP: %ld good %ld bad", lp.good, lp.bad);
    }
    {   // long early/late interleaver (late half one SH frame after the early half) against a blockage of a quarter SH frame
        Params p = mk(k2K, 2, k16Qam, 4);
        p.ti = TiConfig{2, 24, 2, 1, 1};
        Impair im; im.snrDb = 14; im.delay = 1000;
        const double shSec = 4 * 68 * 2304 / sampleRate(8);   // 2K 16-QAM: four OFDM frames
        im.blockStart = 9 * shSec / 4 + 2.5 * shSec; im.blockLen = 0.25 * shSec;
        Result r = run(p, im, 7.5 * shSec + 9 * shSec / 4, false, "2K early/late, 25 % blockage");
        CHECK(r.st.tiLmax == 318 && r.st.p.ti == p.ti, "interleaver profile from the TPS: Lmax %d", r.st.tiLmax);
        CHECK(r.good > 600 && r.bad == 0, "blockage under the long interleaver: %ld good %ld bad", r.good, r.bad);
        p.ti = TiConfig{1, 48, 1, 0, 0};   // the same blockage with the short uniform interleaver loses codewords
        Result s = run(p, im, 7.5 * shSec + 9 * shSec / 4, false, "2K uniform, 25 % blockage");
        CHECK(s.st.codewordsBad >= 5, "the blockage did not hurt the short interleaver (%ld bad)", s.st.codewordsBad);
    }
    {   // below threshold the CRCs must catch every bad packet (nothing wrong passes as good)
        Params p = mk(k2K, 2, kQpsk, 10);
        Impair im; im.snrDb = 2; im.delay = 100;
        Result r = run(p, im, 0.45, false, "2K 2/3 at 2 dB (fails)");
        CHECK(r.st.codewordsBad > 0, "2/3 at 2 dB decoded everything");
    }
    if (lng) {
        for (int mode : {k1K, k2K, k4K, k8K})
            for (int mod : {kQpsk, k16Qam})
                for (int pat : {0, 3, 6, 11}) {
                    Params p = mk(mode, (mode + pat) % 4, mod, pat);
                    Impair im; im.snrDb = mod ? 15 : 9; im.delay = 4321; im.cfoHz = 3100; im.scoPpm = 5;
                    const double sec = 68.0 * 4 * (fftN(mode) * 1.25) / sampleRate(8) * (2 + framesPerShFrame(p) / 4.0 * 2.2);
                    Result r = run(p, im, sec, false, "sweep");
                    CHECK(r.good > 100 && r.bad == 0, "sweep %s %d %d", modeName(mode), mod, pat);
                }
    }
    std::printf(fails ? "dvbsh_chain: %d failures\n" : "dvbsh_chain: all passed\n", fails);
    return fails ? 1 : 0;
}
