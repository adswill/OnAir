// DVB-T and DVB-T2 through a lossy USB link (a PlutoSDR on its USB 2 cable run above the ~4 Msps it carries, rate_choice.h linkRateFor):
// gaps of 1-10 ms of lost samples, spaced so 0.5 %, 2 % or 5 % of the samples are gone (tests/impair.h drop). Prints the packet (DVB-T)
// and baseband-frame (DVB-T2) error rates, which are what the app can promise; checks only that the clean signal decodes fully and that
// the receiver still decodes most of the stream with light losses (0.5 % in 10 ms gaps). Many short gaps hurt far more than a few long
// ones: each gap moves the symbol timing and the receiver has to find it again.
#include "dect2/dvbt_gen.h"
#include "dect2/dvbt_rx.h"
#include "dect2/t2.h"
#include "dect2/t2gen.h"
#include "dect2/t2rx.h"
#include "impair.h"
#include "jobs.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <random>
#include <thread>
#include <vector>
using namespace dect2;
using testjobs::jprintf;
static std::atomic<int> fails{0};
#define CHECK(c, ...) do { if (!(c)) { jprintf("FAIL: " __VA_ARGS__); jprintf("\n"); fails++; } } while (0)

struct Loss { double fraction; double gapMs; };   // fraction of the samples lost, in gaps of gapMs (each 0.5x to 1.5x of it)

// The samples a lossy link hands on: gaps of random length around gapMs, their spacing set so `fraction` of the samples are lost
static std::vector<cf32> lossy(const std::vector<cf32>& x, double rate, Loss l, unsigned seed, size_t start) {
    if (l.fraction <= 0) return x;
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> u(0.5, 1.5);
    const double gapMean = l.gapMs * 1e-3 * rate, period = gapMean / l.fraction;
    std::vector<cf32> out;
    out.reserve(x.size());
    size_t pos = 0;
    double next = start + period * u(rng);
    while (pos < x.size()) {
        const size_t to = std::min(x.size(), (size_t)next);
        out.insert(out.end(), x.begin() + (ptrdiff_t)pos, x.begin() + (ptrdiff_t)to);
        pos = to + (size_t)(gapMean * u(rng));   // the gap: impair::drop on the stream as it goes
        next = pos + period * u(rng);
    }
    return out;
}

static void dvbtCase(Loss l) {
    const double bw = 8, fn = nativeRateHz(bw);
    dvbt::Params p; p.mode = dvbt::k8K; p.guard = dvbt::kGi4; p.mod = dvbt::k64Qam; p.crHp = dvbt::kR23;   // the common European 8K mux
    uint32_t counter = 0;
    dvbt::Generator gen(p, [&](uint8_t* pkt) {
        pkt[0] = 0x47; pkt[1] = 0x01; pkt[2] = 0x00; pkt[3] = 0x10;
        memcpy(pkt + 4, &counter, 4);
        for (int i = 8; i < 188; i++) pkt[i] = (uint8_t)(counter * 31 + i * 7);
        counter++;
    });
    std::vector<cf32> tx(40000, cf32(0, 0)), sym;
    for (int s = 0; s < 68 * 70; s++) { gen.nextSymbol(sym); tx.insert(tx.end(), sym.begin(), sym.end()); }   // 70 frames, 5.3 s
    impair::noise(tx, 28);
    const std::vector<cf32> rx = lossy(tx, fn, l, 11, (size_t)(0.5 * fn));   // the first half second clean: the receiver locks first
    std::vector<cf32>().swap(tx);
    DvbtReceiver r;
    r.configure(fn, bw);
    size_t good = 0, bad = 0, missing = 0;
    uint32_t last = 0, first = 0;
    r.setPacketCallback([&](const uint8_t* pk, size_t n, double) {
        for (size_t i = 0; i < n; i++) {
            const uint8_t* q = pk + i * 188;
            uint32_t c; memcpy(&c, q + 4, 4);
            bool ok = !(q[1] & 0x80) && q[0] == 0x47 && q[1] == 0x01;
            for (int j = 8; j < 188 && ok; j++) ok = q[j] == (uint8_t)(c * 31 + j * 7);
            if (!ok) { if (good) bad++; continue; }
            if (good && c > last + 1) missing += c - last - 1;
            if (!good) first = c;
            if (!good || c > last) last = c;
            good++;
        }
    });
    for (size_t i = 0; i < rx.size(); i += 1 << 14) r.feed(rx.data() + i, std::min<size_t>(1 << 14, rx.size() - i));
    // every packet sent from the first good one on (the receiver's lock-up time not counted): what did not come out intact is lost
    const size_t sentAfter = counter > first ? counter - first : 1;
    const double per = good ? 1.0 - std::min(1.0, (double)good / (double)sentAfter) : 1;
    jprintf("DVB-T  8K 64QAM 2/3  lost %4.1f %% in %4.1f ms gaps: %6zu good of %6zu sent, %5zu damaged, %5zu missing between -> PER %5.1f %%\n",
            l.fraction * 100, l.gapMs, good, sentAfter, bad, missing, per * 100);
    if (l.fraction == 0) CHECK(good > 1000 && bad + missing == 0, "DVB-T clean: %zu good, %zu bad, %zu missing", good, bad, missing);
    if (l.fraction > 0 && l.fraction <= 0.005 && l.gapMs >= 10) CHECK(good > 1000 && per < 0.25, "DVB-T 0.5 %% losses in 10 ms gaps: PER %.2f", per);
}

static void t2Case(Loss l) {
    const double bw = 8, fn = nativeRateHz(bw);
    TxParams tp;   // a typical mux: 32K 1/16 PP4 extended, 256QAM 2/3 rotated, normal frames, time interleaving
    tp.s1 = 0; tp.s2field1 = 5; tp.giIdx = 1; tp.pp = 3; tp.ext = true; tp.t2Version = 2; tp.l1Scrambled = true;
    tp.payload = true; tp.plpShort = false; tp.plpMod = 3; tp.plpCod = 2; tp.plpRot = true; tp.plpTi = 3;
    T2Generator gen(tp);
    const int nFrames = 24;
    std::map<int, std::vector<std::vector<uint8_t>>> sent;
    std::vector<cf32> tx(40000, cf32(0, 0)), frame;
    for (int i = 0; i < nFrames; i++) { gen.nextFrame(frame); sent[i & 0xff] = gen.lastBbFrames(); tx.insert(tx.end(), frame.begin(), frame.end()); }
    impair::noise(tx, 28);
    const std::vector<cf32> rx = lossy(tx, fn, l, 13, (size_t)(0.6 * fn));
    std::vector<cf32>().swap(tx);
    T2Receiver r;
    r.configure(fn, bw);
    std::mutex mu;
    std::vector<PlpResult> results;
    r.setPlpCallback([&](const PlpResult& p) { std::lock_guard<std::mutex> lk(mu); results.push_back(p); });
    const size_t chunk = (size_t)(0.05 * fn);
    RxTelemetry t; uint64_t seq = 0;
    const double frameLen = (double)(rx.size()) / nFrames;
    for (size_t i = 0; i < rx.size(); i += chunk) {   // 50 ms of samples at a time; the decoder thread (which drops a frame it is busy for)
        r.feed(rx.data() + i, std::min(chunk, rx.size() - i));   // is let come within a few frames of what was fed, as test_t2_payload
        for (int w = 0; w < 200; w++) {
            r.telemetry(t, seq);
            size_t n; { std::lock_guard<std::mutex> lk(mu); n = results.size(); }
            if ((double)n + 2 >= (double)i / frameLen) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    for (int i = 0, still = 0; i < 1500 && still < 300; i++) {
        size_t n0; { std::lock_guard<std::mutex> lk(mu); n0 = results.size(); }
        r.telemetry(t, seq);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        size_t n1; { std::lock_guard<std::mutex> lk(mu); n1 = results.size(); }
        still = n1 == n0 ? still + 1 : 0;
    }
    // from the third frame to the one before last: every baseband frame sent there, and how many came out identical
    int sentBlocks = 0, same = 0;
    std::map<int, int> sameIn;
    {
        std::lock_guard<std::mutex> lk(mu);
        for (auto& p : results) {
            auto it = sent.find(p.t2Frame);
            if (it == sent.end() || p.t2Frame < 2 || p.t2Frame >= nFrames - 1) continue;
            int s = 0;
            for (size_t b = 0; b < p.frames.size() && b < it->second.size(); b++) if (!p.frames[b].bits.empty() && p.frames[b].bits == it->second[b]) s++;
            sameIn[p.t2Frame] = std::max(sameIn[p.t2Frame], s);
        }
    }
    for (int f = 2; f < nFrames - 1; f++) { sentBlocks += (int)sent[f].size(); same += sameIn[f]; }
    const double fer = sentBlocks ? 1.0 - (double)same / sentBlocks : 1;
    jprintf("DVB-T2 32K 256QAM 2/3 lost %4.1f %% in %4.1f ms gaps: %4d of %4d baseband frames intact -> BBFRAME error rate %5.1f %%\n",
            l.fraction * 100, l.gapMs, same, sentBlocks, fer * 100);
    if (l.fraction == 0) CHECK(same == sentBlocks && sentBlocks > 0, "DVB-T2 clean: %d of %d", same, sentBlocks);
    if (l.fraction > 0 && l.fraction <= 0.005 && l.gapMs >= 10) CHECK(fer < 0.25, "DVB-T2 0.5 %% losses in 10 ms gaps: %.2f", fer);
}

int main() {
    const Loss losses[] = {{0, 0}, {0.005, 1}, {0.005, 10}, {0.02, 1}, {0.02, 10}, {0.05, 1}, {0.05, 10}};
    testjobs::Jobs jobs;
    for (const Loss& l : losses) jobs.add([l] { dvbtCase(l); });
    for (const Loss& l : losses) jobs.add([l] { t2Case(l); });
    jobs.run(2);   // the T2 decoder thread drops frames it is busy for: few cases at once, so only the losses count
    jprintf(fails ? "USB drop tests FAILED\n" : "USB drop tests passed\n");
    return fails ? 1 : 0;
}
