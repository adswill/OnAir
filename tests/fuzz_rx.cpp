// Robustness: feed the receiver noise, NaN/Inf samples, bursts of corruption, dropped chunks, resets and re-configurations
// and make sure it neither crashes nor hangs (build with -fsanitize=address,undefined to make this meaningful).
#include "dect2/dvbt_gen.h"
#include "dect2/dvbt_rx.h"
#include "dect2/atsc_rx.h"
#include "dect2/atsc_gen.h"
#include "dect2/dvbt_gen.h"
#include "dect2/t2gen.h"
#include "dect2/spectrum.h"
#include "dect2/t2rx.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <algorithm>
#include <cstdint>
#include <vector>
using namespace dect2;

static int fails = 0;
static std::mt19937 rng(12345);

static void feedChunks(T2Receiver& rx, std::vector<cf32>& v, size_t chunk = 1 << 15) {
    for (size_t i = 0; i < v.size(); i += chunk) rx.feed(v.data() + i, std::min(chunk, v.size() - i));
}

static std::vector<cf32> genFrames(TxParams tp, int frames) {
    T2Generator g(tp);
    std::vector<cf32> all, f;
    for (int i = 0; i < frames; i++) { g.nextFrame(f); all.insert(all.end(), f.begin(), f.end()); }
    return all;
}

int main() {
    const double fs = nativeRateHz(8);
    std::normal_distribution<float> nd(0, 1);
    auto t0 = std::chrono::steady_clock::now();
    auto secs = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); };

    // 1. pure noise, loud noise, silence
    {
        T2Receiver rx; rx.configure(fs, 8);
        std::vector<cf32> v(3'000'000);
        for (auto& x : v) x = cf32(nd(rng), nd(rng)) * 0.2f;
        feedChunks(rx, v);
        for (auto& x : v) x = cf32(nd(rng), nd(rng)) * 50.f;
        feedChunks(rx, v);
        std::fill(v.begin(), v.end(), cf32(0, 0));
        feedChunks(rx, v);
        printf("noise/silence ok (%.1f s)\n", secs());
    }
    // 2. NaN / Inf samples inside a valid signal
    for (int fft : {1, 4, 5}) {
        TxParams tp; tp.s2field1 = fft; tp.giIdx = 3; tp.pp = fft == 5 ? 7 : 3; tp.ext = fft == 5;
        T2Receiver rx; rx.configure(fs, 8);
        auto v = genFrames(tp, 6);
        for (int k = 0; k < 200; k++) {
            size_t i = rng() % v.size();
            const float bad[3] = {NAN, INFINITY, -INFINITY};
            v[i] = cf32(bad[rng() % 3], bad[rng() % 3]);
        }
        feedChunks(rx, v);
        printf("NaN/Inf fft %d ok (%.1f s)\n", fft, secs());
    }
    // 2b. the same and huge values into the spectrum analyzer (floating-point radios: LimeSDR, SoapySDR, cf32 recordings)
    {
        SpectrumAnalyzer sa;
        std::vector<cf32> v(1 << 16, cf32(0.1f, -0.1f));
        const float bad[5] = {NAN, INFINITY, -INFINITY, 1e30f, -3e9f};
        for (int k = 0; k < 500; k++) v[rng() % v.size()] = cf32(bad[rng() % 5], bad[rng() % 5]);
        for (int r = 0; r < 4; r++) sa.feed(v.data(), v.size());
        SpectrumFrame f;
        sa.takeFrame(f);
        printf("NaN/Inf/huge spectrum ok (%.1f s)\n", secs());
    }
    // 3. valid signal with bursts of corruption, dropped chunks, repeated chunks and resets
    for (int round = 0; round < 3; round++) {
        TxParams tp; tp.s2field1 = round == 0 ? 1 : round == 1 ? 2 : 0; tp.giIdx = round + 1; tp.pp = round; tp.dataSymbols = 0;
        T2Receiver rx; rx.configure(fs, 8);
        auto v = genFrames(tp, 12);
        std::vector<cf32> out;
        size_t pos = 0;
        while (pos < v.size()) {
            size_t n = std::min<size_t>(v.size() - pos, 20000 + rng() % 200000);
            switch (rng() % 8) {
            case 0: for (size_t i = 0; i < n; i++) out.push_back(cf32(nd(rng), nd(rng))); break;           // burst of noise
            case 1: break;                                                                                  // dropped chunk
            case 2: out.insert(out.end(), v.begin() + pos, v.begin() + pos + n); out.insert(out.end(), v.begin() + pos, v.begin() + pos + n); break; // repeated
            case 3: for (size_t i = 0; i < n; i++) out.push_back(v[pos + i] * 40.f); break;                // clipped/overload burst
            default: out.insert(out.end(), v.begin() + pos, v.begin() + pos + n); break;
            }
            pos += n;
        }
        for (size_t i = 0; i < out.size(); i += 1 << 16) {
            rx.feed(out.data() + i, std::min<size_t>(1 << 16, out.size() - i));
            if (rng() % 40 == 0) rx.reset();
            if (rng() % 60 == 0) { rx.configure(fs, 8); }
        }
        RxTelemetry t; rx.telemetry(t, 0);
        printf("corruption round %d ok (%.1f s) state %d\n", round, secs(), t.state);
    }
    // 4. odd chunk sizes (1 sample, 0 samples, huge)
    {
        T2Receiver rx; rx.configure(fs, 8);
        TxParams tp; tp.s2field1 = 1; tp.giIdx = 3;
        auto v = genFrames(tp, 4);
        for (size_t i = 0; i < 5000; i++) rx.feed(v.data() + i, 1);
        rx.feed(v.data(), 0);
        rx.feed(v.data() + 5000, v.size() - 5000);
        printf("chunk sizes ok (%.1f s)\n", secs());
    }
    // 5. other sample rates and bandwidths
    for (double bw : {8.0, 7.0, 6.0, 5.0, 1.7}) {
        T2Receiver rx; rx.configure(nativeRateHz(bw), bw);
        TxParams tp; tp.s2field1 = 1; tp.giIdx = 3;
        auto v = genFrames(tp, 3);
        feedChunks(rx, v);
        rx.configure(10e6, bw);
        std::vector<cf32> n(500000);
        for (auto& x : n) x = cf32(nd(rng), nd(rng)) * 0.1f;
        feedChunks(rx, n);
    }
    // 6. DVB-T receiver: noise, NaN/Inf, corrupted/dropped/repeated chunks, resets, mode changes
    {
        std::normal_distribution<float> nd2(0, 1);
        for (int round = 0; round < 4; round++) {
            dvbt::Params p; p.mode = round % 2; p.guard = round % 4; p.mod = round % 3; p.crHp = p.crLp = round % 5;
            dvbt::Generator g(p);
            DvbtReceiver rx; rx.configure(fs, 8);
            size_t pk = 0; rx.setPacketCallback([&](const uint8_t*, size_t n, double) { pk += n; });
            std::vector<cf32> sig, sym;
            for (int i = 0; i < 50000; i++) sig.push_back(cf32(nd2(rng), nd2(rng)) * 0.05f);
            for (int s2 = 0; s2 < 68 * (p.mode ? 4 : 10); s2++) { g.nextSymbol(sym); sig.insert(sig.end(), sym.begin(), sym.end()); }
            for (int k = 0; k < 100; k++) { const size_t i = rng() % sig.size(); const float bad[3] = {NAN, INFINITY, -INFINITY}; sig[i] = cf32(bad[rng() % 3], bad[rng() % 3]); }
            size_t pos = 0;
            while (pos < sig.size()) {
                size_t n = std::min<size_t>(sig.size() - pos, 5000 + rng() % 100000);
                switch (rng() % 7) {
                case 0: { std::vector<cf32> nz(n); for (auto& x : nz) x = cf32(nd2(rng), nd2(rng)); rx.feed(nz.data(), n); break; }
                case 1: break;
                case 2: rx.feed(sig.data() + pos, n); rx.feed(sig.data() + pos, n); break;
                default: rx.feed(sig.data() + pos, n); break;
                }
                pos += n;
                if (rng() % 25 == 0) rx.reset();
                if (rng() % 40 == 0) rx.configure(fs, 8);
            }
            RxTelemetry t; rx.telemetry(t, 0);
            printf("DVB-T fuzz round %d ok (%.1f s) state %d packets %zu\n", round, secs(), t.state, pk);
        }
        // pure noise and silence
        DvbtReceiver rx; rx.configure(fs, 8);
        std::vector<cf32> v(2'000'000);
        for (auto& x : v) x = cf32(nd2(rng), nd2(rng)) * 0.3f;
        for (size_t i = 0; i < v.size(); i += 1 << 15) rx.feed(v.data() + i, std::min<size_t>(1 << 15, v.size() - i));
        std::fill(v.begin(), v.end(), cf32(0, 0));
        for (size_t i = 0; i < v.size(); i += 1 << 15) rx.feed(v.data() + i, std::min<size_t>(1 << 15, v.size() - i));
    }
    // 7. ATSC receiver: the same treatment (the field worker thread is exercised too)
    {
        std::normal_distribution<float> nd3(0, 1);
        for (int round = 0; round < 3; round++) {
            atsc::ChannelConfig cc; cc.snrDb = 22; if (round == 1) cc.echoes.push_back({3.0, -8, 30}); if (round == 2) cc.cfoHz = 15000;
            atsc::Generator g(dvbt::testTsSource(), cc, 8e6, 11 + round);
            AtscReceiver rx; rx.configure(8e6); rx.setBlocking(round != 1);
            size_t pk = 0; rx.setPacketCallback([&](const uint8_t*, size_t n, double) { pk += n; });
            std::vector<cf32> sig;
            for (int i = 0; i < 100000; i++) sig.push_back(cf32(nd3(rng), nd3(rng)) * 0.5f);
            g.generate(2'400'000, sig);
            for (int k = 0; k < 100; k++) { const size_t i = rng() % sig.size(); const float bad[3] = {NAN, INFINITY, -INFINITY}; sig[i] = cf32(bad[rng() % 3], bad[rng() % 3]); }
            size_t pos = 0;
            while (pos < sig.size()) {
                size_t n = std::min<size_t>(sig.size() - pos, 3000 + rng() % 90000);
                switch (rng() % 7) {
                case 0: { std::vector<cf32> nz(n); for (auto& x : nz) x = cf32(nd3(rng), nd3(rng)); rx.feed(nz.data(), n); break; }
                case 1: break;
                case 2: rx.feed(sig.data() + pos, n); rx.feed(sig.data() + pos, n); break;
                default: rx.feed(sig.data() + pos, n); break;
                }
                pos += n;
                if (rng() % 30 == 0) rx.reset();
                if (rng() % 60 == 0) rx.configure(rng() % 2 ? 8e6 : 10e6);
            }
            rx.flush();
            AtscTelemetry t; rx.telemetry(t, 0);
            printf("ATSC fuzz round %d ok (%.1f s) level %d packets %zu\n", round, secs(), rx.detectLevel(), pk);
        }
        AtscReceiver rx; rx.configure(8e6);
        std::vector<cf32> v(2'000'000);
        for (auto& x : v) x = cf32(nd3(rng), nd3(rng)) * 0.3f;
        for (size_t i = 0; i < v.size(); i += 1 << 15) rx.feed(v.data() + i, std::min<size_t>(1 << 15, v.size() - i));
        std::fill(v.begin(), v.end(), cf32(0, 0));
        for (size_t i = 0; i < v.size(); i += 1 << 15) rx.feed(v.data() + i, std::min<size_t>(1 << 15, v.size() - i));
        rx.configure(5e6);   // too low a rate: must be refused quietly
        rx.feed(v.data(), 4096);
    }
    printf("bandwidths ok (%.1f s)\nfuzz passed\n", secs());
    return fails ? 1 : 0;
}
