// L1 codec round trip: encode -> noise -> decode, plus P2 distribution and interleaver self-consistency.
#include "dect2/t2interleave.h"
#include "dect2/t2l1.h"
#include <cmath>
#include <cstdio>
#include <random>
#include <algorithm>
#include <vector>

using namespace dect2;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

int main() {
    std::mt19937 rng(5);
    std::normal_distribution<float> nd(0.f, 1.f);
    // L1-pre
    L1Pre pre;
    pre.s1 = 0; pre.s2 = (1 << 1); pre.guardInterval = 2; pre.papr = 2; pre.l1Mod = 1; pre.pilotPattern = 6; pre.cellId = 0x1234;
    pre.networkId = 0x3041; pre.systemId = 0x8000; pre.numFrames = 2; pre.numDataSyms = 59; pre.version = 3; pre.bwtExt = 1;
    auto cells = encodeL1Pre(pre);
    CHECK(cells.size() == 1840, "L1-pre cell count %zu", cells.size());
    for (float snrDb : {20.f, 6.f, 2.f}) {
        float n0 = std::pow(10.f, -snrDb / 10.f) / 2.f; // per real dimension
        auto y = cells;
        for (auto& v : y) v += cf32(nd(rng), nd(rng)) * std::sqrt(n0);
        L1Pre got;
        L1Result r = decodeL1Pre(y, n0, got);
        CHECK(r.ok && r.crcOk && r.bchOk, "L1-pre decode failed at %.0f dB (ldpc iters %d bch %d)", snrDb, r.ldpcIters, r.bchOk);
        if (r.ok) CHECK(got.numDataSyms == 59 && got.cellId == 0x1234 && got.networkId == 0x3041 && got.pilotPattern == 6 && got.bwtExt == 1 && got.s2 == 2 && got.guardInterval == 2 && got.l1Mod == 1,
                        "L1-pre fields mismatch");
        printf("L1-pre @%4.1f dB: ok=%d iters=%d bch=%d\n", snrDb, r.ok, r.ldpcIters, r.bchOk);
    }
    // L1-post for each modulation and P2 count
    for (int mod = 0; mod < 4; mod++) for (int nP2 : {1, 2, 4, 16}) for (int scr : {0, 1}) {
        L1Pre p = pre;
        p.l1Mod = mod; p.postScrambled = scr;
        L1Post post;
        post.numPlp = 2; post.rf.resize(1);
        post.rf[0].freq = 522000000;
        post.plps.resize(2);
        post.plps[0].id = 0; post.plps[0].mod = 3; post.plps[0].cod = 2; post.plps[0].numBlocksMax = 120;
        post.plps[1].id = 1; post.plps[1].mod = 2; post.plps[1].cod = 1; post.plps[1].type = 1;
        post.dyn.resize(2);
        post.dyn[0].numBlocks = 55; post.dyn[1].start = 777; post.dyn[1].numBlocks = 12;
        post.frameIdx = 1; post.changeCounter = 9;
        auto pc = encodeL1Post(p, post, nP2, false);
        CHECK((int)pc.size() == p.postSize, "L1-post cells %zu vs %d", pc.size(), p.postSize);
        float snrDb = mod == 3 ? 24.f : mod == 2 ? 18.f : 8.f;
        float n0 = std::pow(10.f, -snrDb / 10.f) / 2.f;
        auto y = pc;
        for (auto& v : y) v += cf32(nd(rng), nd(rng)) * std::sqrt(n0);
        L1Post got;
        L1Result r = decodeL1Post(y, n0, p, nP2, false, got);
        bool same = r.ok && got.numPlp == 2 && got.plps[0].mod == 3 && got.plps[1].cod == 1 && got.dyn[1].start == 777 && got.rf[0].freq == 522000000u && got.changeCounter == 9;
        CHECK(same && r.bchOk, "L1-post mod %d nP2 %d scr %d failed (ok %d crc %d bch %d iters %d)", mod, nP2, scr, r.ok, r.crcOk, r.bchOk, r.ldpcIters);
    }
    // P2 distribution round trip
    for (int nP2 : {1, 2, 4, 8, 16}) {
        int cP2 = nP2 == 1 ? 8944 : 4472 / (nP2 / 2 ? nP2 / 2 : 1);
        cP2 = std::max(cP2, 2000);
        std::vector<cf32> st((size_t)nP2 * cP2);
        for (size_t i = 0; i < st.size(); i++) st[i] = cf32((float)i, 0);
        std::vector<std::vector<cf32>> sym;
        p2Distribute(st, nP2, cP2, 1840, 960, sym);
        std::vector<cf32> back;
        p2Gather(sym, nP2, cP2, 1840, 960, back);
        bool same = back.size() == st.size();
        for (size_t i = 0; same && i < st.size(); i++) if (back[i] != st[i]) same = false;
        CHECK(same, "P2 distribute/gather mismatch nP2 %d", nP2);
    }
    // frequency interleaver: permutation property
    for (int fft : {3, 0, 2, 1, 4, 5}) for (int cells : {558, 1118, 764, 2236}) for (int odd : {0, 1}) {
        int maxC = fft == 3 ? 1024 : fft == 0 ? 2048 : fft == 2 ? 4096 : fft == 1 ? 8192 : fft == 4 ? 16384 : 32768;
        if (cells > maxC) continue;
        std::vector<int> H;
        freqInterleaverSeq(fft, cells, odd, H);
        std::vector<char> seen(cells, 0);
        bool perm = (int)H.size() == cells;
        for (int v : H) if (v < 0 || v >= cells || seen[v]++) perm = false;
        CHECK(perm, "freq interleaver not a permutation fft %d cells %d odd %d (size %zu)", fft, cells, odd, H.size());
    }
    printf("%s\n", fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}
