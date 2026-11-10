// A mixed DVB-T2 signal: T2-Base frames with T2-Lite frames in the FEF parts. Each layer has its own P1; the receiver must follow the one asked
// for (or the first one seen, automatically), step over the other, and decode its baseband frames exactly. Simulated signals only.
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

struct Case { const char* name; int interval; double snr, cfo; };

static void run(const Case& c, int want, int expectLayer) {
    const double bw = 8, fn = nativeRateHz(bw);
    TxParams b; b.s1 = 0; b.s2field1 = 1; b.giIdx = 2; b.pp = 0; b.mixed = true; b.payload = true; b.plpMod = 1; b.plpCod = 2; b.plpTi = 0; b.dataSymbols = 40;
    TxParams l; l.s1 = 3; l.s2field1 = 0; l.giIdx = 2; l.pp = 0; l.mixed = true; l.payload = true; l.plpShort = true; l.plpMod = 0; l.plpCod = 6; l.plpTi = 0; l.dataSymbols = 40; l.seed = 9;
    {   T2Generator probe(l); b.fefLength = (int)probe.frameLength(); b.fefInterval = c.interval; }
    {   T2Generator probe(b); l.fefLength = (int)probe.frameLength(); l.fefInterval = 1; }
    T2Generator gb(b), gl(l);
    T2Receiver rx; rx.configure(fn, bw); rx.selectLayer(want);
    std::mutex mu; std::vector<PlpResult> res;
    rx.setPlpCallback([&](const PlpResult& r) { std::lock_guard<std::mutex> k(mu); res.push_back(r); });
    std::map<int, std::vector<std::vector<uint8_t>>> sentB, sentL;
    std::mt19937 rng(3); dect2::normal_distribution<float> nd(0.f, 1.f);
    std::vector<cf32> f; double ph = 0;
    int bi = 0, li = 0;
    for (int n = 0; n < 12; n++) {
        for (int m = 0; m < c.interval; m++) {
            gb.nextFrame(f); sentB[bi++ & 0xff] = gb.lastBbFrames();
            double pw = 0; for (auto& v : f) pw += std::norm(v); pw /= f.size();
            float sg = (float)std::sqrt(pw / std::pow(10.0, c.snr / 10) / 2);
            for (auto& v : f) { v = v * cf32((float)cos(ph), (float)sin(ph)) + cf32{nd(rng), nd(rng)} * sg; ph = std::fmod(ph + 2 * M_PI * c.cfo / fn, 2 * M_PI); }
            for (size_t o = 0; o < f.size(); o += 1 << 16) rx.feed(f.data() + o, std::min<size_t>(1 << 16, f.size() - o));
        }
        gl.nextFrame(f); sentL[li++ & 0xff] = gl.lastBbFrames();
        double pw = 0; for (auto& v : f) pw += std::norm(v); pw /= f.size();
        float sg = (float)std::sqrt(pw / std::pow(10.0, c.snr / 10) / 2);
        for (auto& v : f) { v = v * cf32((float)cos(ph), (float)sin(ph)) + cf32{nd(rng), nd(rng)} * sg; ph = std::fmod(ph + 2 * M_PI * c.cfo / fn, 2 * M_PI); }
        for (size_t o = 0; o < f.size(); o += 1 << 16) rx.feed(f.data() + o, std::min<size_t>(1 << 16, f.size() - o));
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
    }
    RxTelemetry t; uint64_t q = 0; size_t seen = 0;
    for (int i = 0, still = 0; i < 800 && still < 150; i++) { rx.telemetry(t, q); std::this_thread::sleep_for(std::chrono::milliseconds(10)); size_t n; { std::lock_guard<std::mutex> k(mu); n = res.size(); } still = n == seen ? still + 1 : 0; seen = n; }
    auto& sent = expectLayer == 1 ? sentB : sentL;
    int frames = 0, blocks = 0, same = 0, bad = 0;
    for (auto& r : res) { auto it = sent.find(r.t2Frame); if (it == sent.end() || !r.blocks) continue; frames++;
        for (size_t k = 0; k < r.frames.size() && k < it->second.size(); k++) { if (r.frames[k].bits.empty()) continue; blocks++; (r.frames[k].bits == it->second[k] ? same : bad)++; } }
    printf("%-22s want %d: layer %d, other P1 %llu, %d frames, %d blocks, %d identical, %d wrong\n", c.name, want, t.t2Layer, (unsigned long long)t.t2OtherLayerP1, frames, blocks, same, bad);
    CHECK(t.t2Layer == expectLayer, "%s: followed layer %d", c.name, t.t2Layer);
    CHECK(t.t2OtherLayerP1 >= 6, "%s: the other layer was not stepped over", c.name);
    CHECK(frames >= 5 && bad == 0 && same >= 8, "%s: %d frames, %d good, %d bad", c.name, frames, same, bad);
    for (auto& u : t.unsupported) CHECK(u.find("FEF") == std::string::npos, "%s: still flagged: %s", c.name, u.c_str());
}

int main() {
    const Case cases[] = {{"every frame, clean", 1, 30, 0}, {"every 2nd frame, CFO", 2, 25, 3200}, {"every 3rd frame, 18 dB", 3, 18, -900}};
    for (auto& c : cases) { run(c, 1, 1); run(c, 2, 2); }
    run(cases[1], 0, 1);   // automatic: the base frame comes first
    printf(fails ? "t2 fef layers FAILED\n" : "t2 fef layers passed\n");
    return fails ? 1 : 0;
}
