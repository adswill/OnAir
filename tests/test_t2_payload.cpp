// End to end: a DVB-T2 signal that carries real payload (T2-Base and T2-Lite configurations) goes through noise and the whole receiver
// (sync, L1, channel estimation, de-interleaving, LDPC, BCH), and every decoded baseband frame must equal what was sent.
#include "dect2/t2gen.h"
#include "dect2/t2rx.h"
#include "jobs.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <map>
#include <mutex>
#include <random>
#include <thread>
using namespace dect2;
using testjobs::jprintf;
static std::atomic<int> fails{0};
#define CHECK(c, ...) do { if (!(c)) { jprintf("FAIL: " __VA_ARGS__); jprintf("\n"); fails++; } } while (0)

struct Case { const char* name; int s1; bool shortFrame; int mod, cod; bool rot; int ti; double snr; double bw = 8; };

static void runCase(const Case& c) {
    const double fn = nativeRateHz(c.bw);
    TxParams tp;
    tp.s1 = c.s1; tp.s2field1 = 1; tp.giIdx = 2; tp.ext = false; tp.pp = 0;
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
    double sigma = 0;
    for (int i = 0; i < nFrames; i++) {
        gen.nextFrame(frame);
        sent[i & 0xff] = gen.lastBbFrames();
        if (i == 0) {
            double pw = 0;
            for (auto& v : frame) pw += std::norm(v);
            pw /= frame.size();
            sigma = std::sqrt(pw / std::pow(10.0, c.snr / 10.0) / 2.0);
        }
        for (auto& v : frame) v += cf32(nd(rng), nd(rng)) * (float)sigma;
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
    CHECK(blocks > 0 && ok >= 0.98 * blocks, "%s: only %d of %d blocks decoded", c.name, ok, blocks);
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
    };
    // the cases are independent: each one runs on its own thread, the output keeps the order of the cases
    testjobs::Jobs jobs;
    for (auto& c : cases) jobs.add([&c] { runCase(c); });
    jobs.run();
    jprintf(fails ? "T2 payload tests FAILED\n" : "T2 payload tests passed\n");
    return fails ? 1 : 0;
}
