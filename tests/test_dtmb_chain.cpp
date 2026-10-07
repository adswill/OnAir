// Transmitter model -> channel decoder at the symbol level (no radio part): every profile, bit-exact packets, then noise.
#include "dect2/dtmb_chain.h"
#include "dect2/dtmb_gen.h"
#include "dect2/dtmb_map.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>

using namespace dect2;
using namespace dect2::dtmb;

static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL line %d: %s  ", __LINE__, #c); printf(__VA_ARGS__); printf("\n"); } } while (0)

struct Outcome { uint64_t good = 0, wrong = 0, cwOk = 0, cwBad = 0; uint32_t firstNumber = 0xFFFFFFFF, last = 0; bool inOrder = true; bool aligned = false; double iter = 0; };

// Runs `frames` frames through transmitter -> noise -> chain and checks the packets against the numbered sequence
static Outcome run(Header h, const Profile& p, double snrDb, int frames, int workers, uint32_t seed = 5) {
    TxConfig tc; tc.header = h; tc.profile = p;
    FrameTx tx(tc, testPacketSource(seed));
    FecChain chain(p, h, workers);
    const int L = tx.frameLength();
    std::vector<cf32> fr((size_t)L), bins((size_t)kBody), si(36), data((size_t)kDataSymbols);
    std::vector<float> var((size_t)kDataSymbols, (float)std::pow(10.0, -snrDb / 10.0));
    MixedFft fft(kBody);
    std::mt19937 rng(9);
    std::normal_distribution<float> nd;
    Outcome o;
    const float sd = (float)std::sqrt(std::pow(10.0, -snrDb / 10.0) / 2.0);
    uint32_t expect = 0xFFFFFFFF;
    auto cb = [&](const uint8_t* pk, size_t n, double) {
        for (size_t i = 0; i < n; i++) {
            uint32_t num = expect;
            if (checkTestPacket(pk + i * 188, seed, &num)) {
                o.good++;
                if (o.firstNumber == 0xFFFFFFFF) o.firstNumber = num;
                if (expect != 0xFFFFFFFF && num != expect) o.inOrder = false;
                expect = num + 1;
                o.last = num;
            } else o.wrong++;
        }
    };
    for (int f = 0; f < frames; f++) {
        tx.nextFrame(fr.data());
        std::copy(fr.begin() + headerInfo(h).length, fr.end(), bins.begin());
        fft.forward(bins.data());
        for (auto& b : bins) b *= 1.f / std::sqrt((float)kBody);
        splitBody(bins.data(), si.data(), data.data());
        for (auto& d : data) d += cf32(sd * nd(rng), sd * nd(rng));
        chain.pushFrame(data.data(), var.data());
        chain.poll(cb);
    }
    chain.flush(cb);
    const ChainStats st = chain.stats();
    o.cwOk = st.cwOk; o.cwBad = st.cwBad; o.aligned = chain.aligned();
    o.iter = st.cwOk + st.cwBad ? (double)st.iterSum / (double)(st.cwOk + st.cwBad) : 0;
    return o;
}

int main() {
    // every profile, clean, both interleaver modes
    for (int si = 3; si <= 24; si++) {
        Profile p;
        profileFromSi(si, p);
        const int primed = (int)((51L * 52 * interleaverDelay(p) + kDataSymbols - 1) / kDataSymbols);
        const int frames = primed + 40;
        const auto t0 = std::chrono::steady_clock::now();
        const Outcome o = run(Header::Pn945, p, 60.0, frames, si % 2 ? 2 : 0);
        const double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        const int expectPk = 40 * packetsPerFrame(p);
        printf("  SI %2d %-8s %s mode %d: %5llu packets good, %llu wrong, codewords ok %llu bad %llu, %.1f iterations (%.1f s)\n", si, mappingName(p.map), rateName(p.rate),
               p.mode2 ? 2 : 1, (unsigned long long)o.good, (unsigned long long)o.wrong, (unsigned long long)o.cwOk, (unsigned long long)o.cwBad, o.iter, dt);
        CHECK(o.wrong == 0, "SI %d: wrong packets", si);
        CHECK(o.cwBad == 0, "SI %d: %llu bad codewords", si, (unsigned long long)o.cwBad);
        CHECK((int)o.good >= expectPk - 2 * packetsPerFrame(p) - 8, "SI %d: only %llu packets of about %d", si, (unsigned long long)o.good, expectPk);
        CHECK(o.inOrder, "SI %d: packets out of order", si);
    }
    printf(failures ? "dtmb_chain: %d FAILED\n" : "dtmb_chain: all passed\n", failures);
    return failures ? 1 : 0;
}
