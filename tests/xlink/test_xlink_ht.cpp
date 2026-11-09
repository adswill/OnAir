// HT PHY (core/src/xlink/ht_phy.cpp): every MCS, both guard intervals, carrier and clock offsets, noise sweep, multipath, two frames overlapping, noise only.
// Simulated signals only: no real HT capture is used here.
#include "dect2/xlink/ht_phy.h"
#include "dect2/wifi/wifi_proto.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

using namespace dect2;
using namespace dect2::xlink::ht;

static std::vector<uint8_t> frame(int n, uint8_t fill) {
    std::vector<uint8_t> f((size_t)n);
    for (int i = 0; i < n; i++) f[(size_t)i] = (uint8_t)(fill + i * 3);
    const uint32_t c = wifi::crc32(f.data(), f.size());
    for (int i = 0; i < 4; i++) f.push_back((uint8_t)(c >> (8 * i)));
    return f;
}

static std::vector<cf32> impair(std::vector<cf32> x, double cfo, double sigma, unsigned seed, double phase0 = 0.7) {
    std::mt19937 g(seed);
    std::normal_distribution<float> n(0, (float)sigma);
    std::vector<cf32> out;
    out.resize(2000 + x.size() + 2000, cf32(0, 0));
    for (size_t i = 0; i < x.size(); i++) out[2000 + i] = x[i];
    double ph = phase0;
    for (size_t i = 0; i < out.size(); i++) {
        const double a = ph + 2 * 3.141592653589793 * cfo * (double)i / kRate;
        out[i] *= cf32((float)std::cos(a), (float)std::sin(a));
        out[i] += cf32(n(g), n(g));
    }
    return out;
}

struct Result { int good = 0; std::vector<HtInfo> infos; std::vector<std::vector<uint8_t>> frames; HtRx::Stats s; };
static Result decode(const std::vector<cf32>& x, size_t block = 777) {
    HtRx rx;
    Result r;
    rx.setCallback([&](const uint8_t* m, size_t n, const HtInfo& in) { r.good++; r.infos.push_back(in); r.frames.emplace_back(m, m + n); });
    for (size_t i = 0; i < x.size(); i += block) rx.feed(x.data() + i, std::min(block, x.size() - i));
    r.s = rx.stats();
    return r;
}

int main() {
    // each MCS, both guard intervals, a tx scrambler seed that varies, and offsets
    for (int mcs = 0; mcs < 8; mcs++)
        for (int sgi = 0; sgi < 2; sgi++)
            for (double cfo : {0.0, 90e3, -210e3}) {
                const std::vector<uint8_t> f = frame(100 + mcs * 37, (uint8_t)(mcs + 1));
                const auto x = impair(htPpdu(f, mcs, sgi, (uint8_t)(0x11 + mcs * 7)), cfo, 0.005, (unsigned)(mcs * 10 + sgi));
                const Result r = decode(x);
                if (r.good != 1 || r.infos[0].mcs != mcs || r.infos[0].shortGi != !!sgi || r.infos[0].length != (int)f.size() || r.frames[0] != std::vector<uint8_t>(f.begin(), f.end() - 4)) {
                    printf("FAIL mcs %d sgi %d cfo %.0f good %d trig %llu nosync %llu nonht %llu sigbad %llu uns %llu bad %llu\n", mcs, sgi, cfo, r.good,
                           (unsigned long long)r.s.triggers, (unsigned long long)r.s.noSync, (unsigned long long)r.s.nonHt, (unsigned long long)r.s.sigBad, (unsigned long long)r.s.unsupported, (unsigned long long)r.s.framesBad);
                    return 1;
                }
                if (fabs(r.infos[0].cfoHz - cfo) > 5e3) { printf("FAIL cfo est %.0f want %.0f\n", r.infos[0].cfoHz, cfo); return 1; }
            }
    // a real long frame (wfb-ng blocks are about 1.4 kB), sampling clock 15 ppm off
    {
        const std::vector<uint8_t> f = frame(1460, 9);
        auto x0 = htPpdu(f, 3, false);
        const double ppm = 15e-6;
        std::vector<cf32> y(x0.size());
        for (size_t i = 0; i < y.size(); i++) {
            const double t = (double)i * (1 + ppm);
            const size_t a = (size_t)t;
            const float fr = (float)(t - (double)a);
            y[i] = a + 1 < x0.size() ? x0[a] * (1 - fr) + x0[a + 1] * fr : cf32(0, 0);
        }
        const Result r = decode(impair(y, 60e3, 0.003, 42));
        if (r.good != 1) { printf("FAIL clock offset\n"); return 1; }
    }
    // multipath inside the guard interval (ground bounce: 150 ns echo, half power)
    {
        const std::vector<uint8_t> f = frame(500, 3);
        auto x0 = htPpdu(f, 4, false);
        std::vector<cf32> y(x0.size() + 3, cf32(0, 0));
        for (size_t i = 0; i < x0.size(); i++) { y[i] += x0[i]; y[i + 3] += x0[i] * cf32(0.5f, -0.4f); }
        const Result r = decode(impair(y, 0, 0.003, 43));
        if (r.good != 1) { printf("FAIL multipath\n"); return 1; }
    }
    // noise sweep at MCS 1 (wfb-ng's default): prints the threshold, checks the operating point
    {
        int okAt6 = 0, ok10 = 0;
        for (double snr = 2; snr <= 14; snr += 2) {
            int ok = 0;
            for (int t = 0; t < 20; t++) {
                const std::vector<uint8_t> f = frame(300, (uint8_t)t);
                const auto x = htPpdu(f, 1, false, (uint8_t)(0x15 + t));
                const double sigma = std::pow(10.0, -snr / 20) / std::sqrt(2.0);
                if (decode(impair(x, 30e3, sigma, (unsigned)t * 11 + 7)).good == 1) ok++;
            }
            printf("MCS1 SNR %.0f dB: %d/20\n", snr, ok);
            if (snr == 6) okAt6 = ok;
            if (snr == 10) ok10 = ok;
        }
        if (ok10 < 18) { printf("FAIL noise sweep at 10 dB: %d/20 (at 6 dB %d)\n", ok10, okAt6); return 1; }
    }
    // two frames in one buffer, the second after a short gap, and a corrupted frame in between: nothing wrong is reported
    {
        const auto f1 = frame(200, 1), f2 = frame(220, 2);
        auto a = htPpdu(f1, 2, false), b = htPpdu(f2, 1, true);
        std::vector<cf32> c = htPpdu(frame(150, 3), 3, false);
        for (size_t i = c.size() / 2; i < c.size() / 2 + 200; i++) c[i] = cf32(0, 0);    // a burst of interference removes part of it
        std::vector<cf32> all = a;
        all.resize(all.size() + 400, cf32(0, 0));
        all.insert(all.end(), c.begin(), c.end());
        all.resize(all.size() + 400, cf32(0, 0));
        all.insert(all.end(), b.begin(), b.end());
        const Result r = decode(impair(all, -33e3, 0.004, 99));
        if (r.good != 2) { printf("FAIL two frames: good %d\n", r.good); return 1; }
        if (r.s.framesBad != 1) { printf("FAIL interference frame not marked bad: %llu\n", (unsigned long long)r.s.framesBad); return 1; }
    }
    // noise only: nothing
    {
        std::mt19937 g(5);
        std::normal_distribution<float> n(0, 0.05f);
        std::vector<cf32> x(400000);
        for (auto& v : x) v = cf32(n(g), n(g));
        const Result r = decode(x);
        if (r.good) { printf("FAIL noise gave frames\n"); return 1; }
    }
    printf("ht ok\n");
    return 0;
}
