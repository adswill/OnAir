// ISDB-T channel coding loop-back: generator -> FFT with ideal timing -> demodulator core -> transport stream packets, noise free and noisy.
#include "dect2/fftutil.h"
#include "dect2/isdbt_demod.h"
#include "dect2/isdbt_gen.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

using namespace dect2;
using namespace dect2::isdbt;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

struct Case { const char* name; Params p; int frames; double snrDb; };

static bool run(const Case& c) {
    Generator gen(c.p, countingSource(3), 1);
    Demod dem;
    dem.configure(c.p);
    const int N = fftN(c.p.mode), G = guardSamples(c.p.mode, c.p.guard), K = totalCarriers(c.p.mode), kc = centerCarrier(c.p.mode);
    Fft fft(N);
    std::mt19937 rng(7);
    std::normal_distribution<float> nd(0.f, 1.f);
    std::vector<cf32> frame, sym((size_t)N), Y((size_t)K);
    long good[3] = {0, 0, 0}, bad[3] = {0, 0, 0};
    long lastCounter[3] = {-1, -1, -1}, gaps[3] = {0, 0, 0}, order[3] = {0, 0, 0};
    std::vector<uint8_t> pk;
    int warm = 0;
    for (int li = 0; li < 3; li++) if (c.p.layer[li].used()) warm = std::max(warm, (95 * interleavingLength(c.p.mode, c.p.layer[li].ti) + timeInterleaveAdjust(c.p.mode, c.p.layer[li].ti)) / kSymbolsPerFrame + 3);
    const auto t0 = std::chrono::steady_clock::now();
    // noise relative to the mean sample power (about 1)
    const float sigma = (float)std::sqrt(std::pow(10.0, -c.snrDb / 10.0) / 2.0);
    for (int f = 0; f < c.frames; f++) {
        gen.nextFrame(frame);
        for (int s = 0; s < kSymbolsPerFrame; s++) {
            const cf32* base = frame.data() + (size_t)s * (N + G) + G;
            for (int i = 0; i < N; i++) sym[(size_t)i] = base[i] + (c.snrDb < 200 ? cf32(nd(rng), nd(rng)) * sigma : cf32(0, 0));
            fft.forward(sym.data());
            for (int k = 0; k < K; k++) Y[(size_t)k] = sym[(size_t)(((k - kc) % N + N) % N)];
            dem.pushSymbol(Y.data(), s);
        }
        for (int li = 0; li < 3; li++) {
            pk.clear();
            dem.takePackets(li, pk);
            if (f < warm) continue;   // the interleavers are still filling up
            for (size_t i = 0; i + 188 <= pk.size(); i += 188) {
                int layer; unsigned counter;
                const bool ok = !(pk[i + 1] & 0x80) && checkCountingPacket(&pk[i], &layer, &counter) && layer == li;
                if (!ok) { bad[li]++; continue; }
                if (lastCounter[li] >= 0 && (long)counter != lastCounter[li] + 1) { if ((long)counter > lastCounter[li]) gaps[li]++; else order[li]++; }
                lastCounter[li] = (long)counter;
                good[li]++;
            }
        }
    }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    bool ok = true;
    for (int li = 0; li < 3; li++) {
        if (!c.p.layer[li].used()) continue;
        const int per = packetsPerFrame(c.p.mode, c.p.layer[li]);
        const LayerStats& st = dem.layerStats(li);
        printf("  layer %c: %d segments %s %s I=%d: %d packets/frame, %ld good, %ld bad, %ld gaps (RS clean %llu corrected %llu failed %llu)\n", 'A' + li, c.p.layer[li].segments,
               modName(c.p.layer[li].mod), rateName(c.p.layer[li].rate), interleavingLength(c.p.mode, c.p.layer[li].ti), per, good[li], bad[li], gaps[li],
               (unsigned long long)st.rsClean, (unsigned long long)st.rsCorrected, (unsigned long long)st.rsFailed);
        // after the start-up the packets must all be good; allow the first frames for the interleavers to fill
        const long expect = (long)per * (c.frames - warm - 2);
        CHECK(good[li] >= expect && bad[li] == 0 && gaps[li] == 0 && order[li] == 0, "%s layer %c: %ld good (need %ld), %ld bad, %ld gaps", c.name, 'A' + li, good[li], expect, bad[li], gaps[li]);
        if (!(good[li] >= expect && bad[li] == 0 && gaps[li] == 0)) ok = false;
    }
    printf("  %s: %.1f s for %d frames (%.2f s of signal)\n", c.name, secs, c.frames, c.frames * frameSeconds(c.p.mode, c.p.guard));
    return ok;
}

static Layer L(int seg, int mod, int rate, int ti = 0) { Layer l; l.segments = seg; l.mod = mod; l.rate = rate; l.ti = ti; return l; }

int main(int argc, char** argv) {
    const bool quick = argc > 1 && !strcmp(argv[1], "quick");
    std::vector<Case> cases;
    {
        Params p; p.mode = 1; p.guard = kGi8; p.layer[0] = L(13, k64Qam, kR34);
        cases.push_back({"mode 1, 13 segments 64QAM 3/4", p, 14, 300});
    }
    {
        Params p; p.mode = 2; p.guard = kGi16; p.partial = true; p.layer[0] = L(1, kQpsk, kR12, 2); p.layer[1] = L(12, k16Qam, kR23, 1);
        cases.push_back({"mode 2, one-segment layer + 12 segments", p, 20, 300});
    }
    {
        Params p; p.mode = 1; p.guard = kGi4; p.partial = true; p.layer[0] = L(1, kQpsk, kR12, 1); p.layer[1] = L(4, kDqpsk, kR12, 1); p.layer[2] = L(8, k64Qam, kR78, 1);
        cases.push_back({"mode 1, partial + DQPSK + 64QAM", p, 28, 300});
    }
    if (!quick) {
        {
            Params p; p.mode = 3; p.guard = kGi32; p.layer[0] = L(13, kQpsk, kR56, 0);
            cases.push_back({"mode 3, 13 segments QPSK 5/6", p, 8, 300});
        }
        {
            Params p; p.mode = 2; p.guard = kGi8; p.layer[0] = L(6, kDqpsk, kR23, 3); p.layer[1] = L(7, k16Qam, kR78, 0);
            cases.push_back({"mode 2, DQPSK 2/3 + 16QAM 7/8", p, 40, 300});
        }
        {
            Params p; p.mode = 1; p.guard = kGi8; p.layer[0] = L(13, k64Qam, kR34);
            cases.push_back({"mode 1, 13 segments 64QAM 3/4 at 28 dB", p, 14, 28});
        }
    }
    for (auto& c : cases) {
        printf("%s\n", c.name);
        std::string why;
        CHECK(c.p.valid(&why), "%s: invalid configuration: %s", c.name, why.c_str());
        run(c);
    }
    printf(fails ? "isdbt chain: FAILED\n" : "isdbt chain: ok\n");
    return fails ? 1 : 0;
}
