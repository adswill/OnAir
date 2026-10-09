// End to end: a DVB-T2 signal that carries real payload (T2-Base and T2-Lite configurations) goes through noise and the whole receiver
// (sync, L1, channel estimation, de-interleaving, LDPC, BCH), and every decoded baseband frame must equal what was sent.
#include "dect2/t2gen.h"
#include "dect2/t2rx.h"
#include "impair.h"
#include "jobs.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <random>
#include <thread>
using namespace dect2;
using testjobs::jprintf;
static std::atomic<int> fails{0};
#define CHECK(c, ...) do { if (!(c)) { jprintf("FAIL: " __VA_ARGS__); jprintf("\n"); fails++; } } while (0)

struct Case { const char* name; int s1; bool shortFrame; int mod, cod; bool rot; int ti; double snr; double bw = 8; int fft = 1, gi = 2, pp = 0; bool ext = false; int t2Version = 2; double cfoHz = 0; double sroPpm = 0; };

static void runCase(const Case& c) {
    const double fn = nativeRateHz(c.bw);
    TxParams tp;
    tp.s1 = c.s1; tp.s2field1 = c.fft; tp.giIdx = c.gi; tp.ext = c.ext; tp.pp = c.pp; tp.t2Version = c.t2Version; tp.l1Scrambled = true;
    tp.payload = true; tp.plpShort = c.shortFrame; tp.plpMod = c.mod; tp.plpCod = c.cod; tp.plpRot = c.rot; tp.plpTi = c.ti;
    T2Generator gen(tp);
    T2Receiver rx;
    rx.configure(fn, c.bw);
    std::mutex mu;
    std::vector<PlpResult> results;
    rx.setPlpCallback([&](const PlpResult& r) { std::lock_guard<std::mutex> lk(mu); results.push_back(r); });

    const int nFrames = 16;
    std::map<int, std::vector<std::vector<uint8_t>>> sent;   // by L1 frame index
    std::vector<cf32> frame;
    std::mt19937 rng(7);
    std::normal_distribution<float> nd(0.f, 1.f);
    // the whole transmission first: a sample-clock error (the receiver's clock c.sroPpm fast) resamples it as one stream (tests/impair.h)
    std::vector<cf32> tx;
    std::vector<size_t> txEnd;
    for (int i = 0; i < nFrames; i++) {
        gen.nextFrame(frame);
        sent[i & 0xff] = gen.lastBbFrames();
        tx.insert(tx.end(), frame.begin(), frame.end());
        txEnd.push_back(tx.size());
    }
    std::vector<cf32> rxs;
    if (c.sroPpm != 0) {   // (16 samples of silence each side: the interpolator starts 16 samples in)
        std::vector<cf32> pad(16, cf32(0, 0));
        tx.insert(tx.begin(), pad.begin(), pad.end());
        tx.insert(tx.end(), pad.begin(), pad.end());
        rxs = impair::clock(tx, c.sroPpm);
    }
    const double stretch = 1.0 + c.sroPpm * 1e-6;
    double sigma = 0, ph = 0;
    size_t from = 0;
    for (int i = 0; i < nFrames; i++) {
        const size_t to = c.sroPpm != 0 ? std::min(rxs.size(), (size_t)std::llround(txEnd[i] * stretch)) : txEnd[i];
        const std::vector<cf32>& src = c.sroPpm != 0 ? rxs : tx;
        frame.assign(src.begin() + from, src.begin() + to);
        from = to;
        if (i == 0) {
            double pw = 0;
            for (auto& v : frame) pw += std::norm(v);
            pw /= frame.size();
            sigma = std::sqrt(pw / std::pow(10.0, c.snr / 10.0) / 2.0);
        }
        for (auto& v : frame) {
            if (c.cfoHz != 0) { v *= cf32((float)std::cos(ph), (float)std::sin(ph)); ph = std::fmod(ph + 2 * M_PI * c.cfoHz / fn, 2 * M_PI); }
            v += cf32(nd(rng), nd(rng)) * (float)sigma;
        }
        for (size_t o = 0; o < frame.size(); o += 1 << 16) rx.feed(frame.data() + o, std::min<size_t>(1 << 16, frame.size() - o));
        // a radio delivers a frame every 100-250 ms; fed all at once, the data decoder (a thread that drops a frame it is still busy
        // for) lost half of them on a loaded CI machine. Wait for it to come within a few frames of what was fed (at most 2 s a frame).
        RxTelemetry tt; uint64_t sq = 0;
        for (int w = 0; w < 200; w++) {
            rx.telemetry(tt, sq);
            size_t n;
            { std::lock_guard<std::mutex> lk(mu); n = results.size(); }
            if ((int)n + 4 >= i) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    // let the decoder finish: telemetry() collects the finished frames. Done when no frame has come out for 3 s (at most 15 s), not after
    // a fixed 5 s: the decoder takes a few milliseconds per frame, the rest of the wait was idle. 3 s because on a busy CI machine (all
    // cases at once, next to other tests) the decoder thread can stall for over a second.
    RxTelemetry t; uint64_t seq = 0;
    size_t seen = 0;
    for (int i = 0, still = 0; i < 1500 && still < 300; i++) {
        rx.telemetry(t, seq);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        size_t n;
        { std::lock_guard<std::mutex> lk(mu); n = results.size(); }
        still = n == seen ? still + 1 : 0;
        seen = n;
    }
    int frames = 0, blocks = 0, ok = 0, same = 0;
    {
        std::lock_guard<std::mutex> lk(mu);
        for (auto& r : results) {
            auto it = sent.find(r.t2Frame);
            if (it == sent.end() || r.blocks == 0) continue;
            frames++;
            for (size_t b = 0; b < r.frames.size() && b < it->second.size(); b++) {
                blocks++;
                if (r.frames[b].bits.empty()) continue;
                ok++;
                if (r.frames[b].bits == it->second[b]) same++;
            }
        }
    }
    jprintf("%-28s %s %-3s @ %4.1f dB: %2d frames, %4d blocks, %4d decoded, %4d identical, %d per frame\n", c.name, c.shortFrame ? "short " : "normal", rateName(c.cod), c.snr, frames, blocks, ok, same, gen.plpBlocks());
    CHECK(frames >= nFrames / 2, "%s: only %d frames reached the data stage", c.name, frames);
    CHECK(same == ok, "%s: %d decoded blocks differ from what was sent", c.name, ok - same);
    // the steady state: from the third frame on (the first one or two can come out partly while the receiver locks: with the long 19/128
    // and 19/256 guard intervals it finds the frame a little later)
    int blocks2 = 0, ok2 = 0;
    {
        std::lock_guard<std::mutex> lk(mu);
        for (auto& r : results) {
            if (r.t2Frame < 2 || sent.find(r.t2Frame) == sent.end() || r.blocks == 0) continue;
            for (size_t b = 0; b < r.frames.size() && b < sent[r.t2Frame].size(); b++) { blocks2++; if (!r.frames[b].bits.empty()) ok2++; }
        }
    }
    CHECK(blocks2 > 0 && ok2 >= 0.98 * blocks2, "%s: only %d of %d blocks decoded after the first two frames", c.name, ok2, blocks2);
    if (c.sroPpm != 0) {   // a clock error is no excuse: once locked, every block and every L1 must decode
        CHECK(ok2 == blocks2, "%s: %d of %d blocks lost after the first two frames", c.name, blocks2 - ok2, blocks2);
        CHECK(t.l1preGood >= (uint64_t)nFrames - 3 && t.l1postGood >= (uint64_t)nFrames - 3, "%s: L1-pre %llu ok / %llu bad, L1-post %llu ok / %llu bad", c.name,
              (unsigned long long)t.l1preGood, (unsigned long long)t.l1preBad, (unsigned long long)t.l1postGood, (unsigned long long)t.l1postBad);
        jprintf("  %s: clock error measured %+.1f ppm, L1-pre %llu/%llu, L1-post %llu/%llu\n", c.name, t.sroPpm, (unsigned long long)t.l1preGood,
                (unsigned long long)(t.l1preGood + t.l1preBad), (unsigned long long)t.l1postGood, (unsigned long long)(t.l1postGood + t.l1postBad));
    }
    if (blocks > 0 && ok < blocks) {   // which frames lost blocks, for the log
        std::lock_guard<std::mutex> lk(mu);
        std::string m;
        for (auto& r : results) {
            int bad = 0;
            for (auto& f : r.frames) if (f.bits.empty()) bad++;
            if (bad) m += " frame " + std::to_string(r.t2Frame) + ": " + std::to_string(bad) + " of " + std::to_string(r.frames.size()) + ";";
        }
        jprintf("  %s: lost blocks in%s\n", c.name, m.c_str());
    }
}

int main() {
    // thresholds (Es/N0, short frames, from bench_threshold) plus a margin for the OFDM losses
    const Case cases[] = {
        {"T2-Base 64QAM 2/3",       0, false, 2, 2, true,  3, 22},
        {"T2-Lite QPSK 1/3",        3, true,  0, 6, false, 0, 8},
        {"T2-Lite QPSK 2/5",        3, true,  0, 7, false, 3, 9},
        {"T2-Lite 16QAM 1/3",       3, true,  1, 6, true,  3, 9},
        {"T2-Lite 16QAM 2/5",       3, true,  1, 7, false, 3, 10},
        {"T2-Lite 64QAM 1/3",       3, true,  2, 6, true,  3, 13},
        {"T2-Lite 64QAM 2/5",       3, true,  2, 7, false, 3, 14},
        {"T2-Lite 256QAM 1/3",      3, true,  3, 6, true,  3, 17},
        {"T2-Lite 256QAM 2/5",      3, true,  3, 7, false, 3, 19},
        {"T2-Lite 64QAM 1/2",       3, true,  2, 0, true,  3, 16},
        {"T2-Lite 1.7 MHz 16QAM 1/3", 3, true, 1, 6, true,  3, 9, 1.7},
        // the guard intervals 1/128, 19/256 and 19/128 make P1 signal its own FFT codes (7 for 32K, 6 for 8K): a real 32K 19/128 mux (Hungary)
        // never got to L1-pre, because the receiver read its tables with the raw code
        {"32K GI 1/8 (P1 code 5)",      0, false, 2, 2, true,  3, 22, 8, 5, 2, 7},
        {"32K GI 19/128 (P1 code 7)",   0, false, 2, 2, true,  3, 22, 8, 5, 5, 1},
        {"32K GI 19/256 ext (code 7)",  0, false, 2, 2, true,  3, 22, 8, 5, 6, 3, true},
        {"8K GI 1/128 (P1 code 6)",     0, false, 2, 2, true,  3, 22, 8, 1, 4, 6},
        // the Hungarian mux: 32K ext PP2 19/128 64-QAM 3/4 (the guard nearly fills the pilot delay window), V1.2.1 with the reserved bit
        // that is L1_POST_SCRAMBLED from V1.3.1 on set but a plain L1-post, recorded 280 kHz off centre
        {"32K ext PP2 19/128 64QAM 3/4", 0, false, 2, 3, true, 3, 24, 8, 5, 5, 1, true},
        {"T2 V1.2.1, scrambled bit set", 0, false, 2, 2, true, 3, 22, 8, 1, 2, 0, false, 1},
        {"P1 280 kHz off centre",       0, false, 2, 2, true,  3, 22, 8, 1, 2, 0, false, 2, 280000},
        {"32K ext PP2 19/128 V1.2.1 +280k", 0, false, 2, 3, true, 3, 24, 8, 5, 5, 1, true, 1, 280000},
        // a sample clock 58 ppm off (a real 32K 1/16 PP4 recording): placing the symbols on a stretched grid is not enough, inside a 32K
        // symbol carrier k is off by k * 58e-6 carriers (0.8 at the edge), and L1-pre never decoded. Both signs, with a -12 kHz carrier offset.
        {"32K GI 1/16 PP4 +58 ppm -12k", 0, false, 2, 2, true, 3, 22, 8, 5, 1, 3, false, 2, -12000, 58},
        {"32K GI 1/16 PP4 -58 ppm -12k", 0, false, 2, 2, true, 3, 22, 8, 5, 1, 3, false, 2, -12000, -58},
        {"32K GI 1/16 PP4 +100 ppm",     0, false, 2, 2, true, 3, 22, 8, 5, 1, 3, false, 2, 0, 100},
        {"8K GI 1/8 PP2 -100 ppm",       0, false, 2, 2, true, 3, 22, 8, 1, 2, 1, false, 2, 0, -100},
    };
    // the cases are independent: each one runs on its own thread, the output keeps the order of the cases
    testjobs::Jobs jobs;
    const char* only = getenv("T2_CASE");   // runs only the cases whose name contains this (for debugging one)
    for (auto& c : cases) if (!only || strstr(c.name, only)) jobs.add([&c] { runCase(c); });
    jobs.run();
    jprintf(fails ? "T2 payload tests FAILED\n" : "T2 payload tests passed\n");
    return fails ? 1 : 0;
}
