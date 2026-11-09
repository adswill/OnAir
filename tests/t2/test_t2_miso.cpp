// DVB-T2 MISO: both transmitter groups through their own channel (delay and gain), the receiver must read the L1 and the PLP. Simulated signals.
#include "dect2/t2/t2gen.h"
#include "dect2/t2/t2rx.h"
#include "dect2/common/stable_random.h"
#include <chrono>
#include <cstdio>
#include <map>
#include <mutex>
#include <random>
#include <thread>
using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)
struct Case { const char* name; int s1; int fft; float g2; int delay; double snr; int mod; };
static void run(const Case& c) {
    TxParams tp; tp.s1 = c.s1; tp.s2field1 = c.fft; tp.giIdx = 2; tp.pp = 0; tp.payload = true; tp.plpMod = c.mod; tp.plpCod = 2; tp.plpRot = false; tp.plpTi = 0; tp.plpShort = c.s1 == 4; if (c.s1 == 4) tp.plpCod = 6;
    T2Generator gen(tp);
    const double fn = nativeRateHz(8);
    T2Receiver rx; rx.configure(fn, 8);
    std::mutex mu; std::vector<PlpResult> res;
    rx.setPlpCallback([&](const PlpResult& r) { std::lock_guard<std::mutex> k(mu); res.push_back(r); });
    std::map<int, std::vector<std::vector<uint8_t>>> sent;
    std::mt19937 rng(9); dect2::normal_distribution<float> nd(0.f, 1.f);
    std::vector<cf32> f, y, tail(c.delay + 1, cf32(0, 0));
    for (int i = 0; i < 12; i++) {
        gen.nextFrame(f); sent[i & 0xff] = gen.lastBbFrames();
        const auto& f2 = gen.lastTx2();
        y.resize(f.size());
        for (size_t n = 0; n < f.size(); n++) {
            const cf32 d = c.delay ? (n >= (size_t)c.delay ? f2[n - c.delay] : tail[n]) : f2[n];
            y[n] = f[n] + std::polar(c.g2, 0.9f) * d;
        }
        if (c.delay) for (int n = 0; n < c.delay; n++) tail[n] = f2[f2.size() - c.delay + n];
        double pw = 0; for (auto& v : y) pw += std::norm(v); pw /= y.size();
        const float sg = (float)std::sqrt(pw / std::pow(10.0, c.snr / 10) / 2);
        for (auto& v : y) v += cf32{nd(rng), nd(rng)} * sg;
        for (size_t o = 0; o < y.size(); o += 1 << 16) rx.feed(y.data() + o, std::min<size_t>(1 << 16, y.size() - o));
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
    }
    RxTelemetry t; uint64_t q = 0; size_t seen = 0;
    for (int i = 0, still = 0; i < 800 && still < 150; i++) { rx.telemetry(t, q); std::this_thread::sleep_for(std::chrono::milliseconds(10)); size_t n; { std::lock_guard<std::mutex> k(mu); n = res.size(); } still = n == seen ? still + 1 : 0; seen = n; }
    int fr = 0, same = 0, bad = 0;
    for (auto& r : res) { auto it = sent.find(r.t2Frame); if (it == sent.end() || !r.blocks) continue; fr++; for (size_t k = 0; k < r.frames.size() && k < it->second.size(); k++) { if (r.frames[k].bits.empty()) continue; (r.frames[k].bits == it->second[k] ? same : bad)++; } }
    printf("%-30s L1-pre %d, %d frames, %d blocks identical, %d wrong\n", c.name, t.l1preOk, fr, same, bad);
    CHECK(t.l1preOk, "%s: no L1", c.name);
    CHECK(fr >= 4 && same >= 16 && bad == 0, "%s: %d frames %d same %d bad", c.name, fr, same, bad);
}
int main() {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    run({"MISO, equal paths, 8K", 1, 1, 1.0f, 0, 30, 1});
    run({"MISO, group 2 6 dB down", 1, 1, 0.5f, 7, 28, 1});
    run({"MISO, group 2 delayed", 1, 1, 0.8f, 30, 28, 1});
    run({"MISO 64-QAM", 1, 1, 0.8f, 12, 30, 2});
    run({"T2-Lite MISO", 4, 1, 0.7f, 10, 22, 1});
    run({"MISO, group 2 off", 1, 1, 0.0f, 0, 25, 1});
    printf(fails ? "t2 miso FAILED\n" : "t2 miso passed\n");
    return fails ? 1 : 0;
}
