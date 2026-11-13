// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Transmitter model -> channel decoder at the symbol level (no radio part): every profile, bit-exact packets, then noise.
#include "dect2/dtmb/dtmb_chain.h"
#include "dect2/common/stable_random.h"
#include "dect2/dtmb/dtmb_gen.h"
#include "dect2/dtmb/dtmb_map.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>

using namespace dect2;
using namespace dect2::dtmb;

static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL line %d: %s  ", __LINE__, #c); printf(__VA_ARGS__); printf("\n"); } } while (0)

// The byte stream a chain delivers for a fixed noisy signal (the same noise every time), with the given number of decoder threads
static std::vector<uint8_t> streamOf(Header h, const Profile& p, double snrDb, int frames, int workers) {
    TxConfig tc; tc.header = h; tc.profile = p;
    FrameTx tx(tc, testPacketSource(3));
    FecChain chain(p, h, workers);
    std::vector<cf32> fr((size_t)tx.frameLength()), bins((size_t)kBody), si(36), data((size_t)kDataSymbols);
    std::vector<float> var((size_t)kDataSymbols, (float)std::pow(10.0, -snrDb / 10.0));
    MixedFft fft(kBody);
    std::mt19937 rng(21);
    dect2::normal_distribution<float> nd;
    const float sd = (float)std::sqrt(std::pow(10.0, -snrDb / 10.0) / 2.0);
    std::vector<uint8_t> out;
    auto cb = [&](const uint8_t* pk, size_t n, double) { out.insert(out.end(), pk, pk + n * 188); };
    for (int f = 0; f < frames; f++) {
        tx.nextFrame(fr.data());
        std::copy(fr.begin() + headerInfo(h).length, fr.end(), bins.begin());
        fft.forward(bins.data());
        for (auto& b : bins) b *= 1.f / std::sqrt((float)kBody);
        splitBody(bins.data(), si.data(), data.data());
        for (auto& d : data) d += cf32{sd * nd(rng), sd * nd(rng)};
        chain.pushFrame(data.data(), var.data());
        chain.poll(cb);
    }
    chain.flush(cb);
    return out;
}

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
    dect2::normal_distribution<float> nd;
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
        for (auto& d : data) d += cf32{sd * nd(rng), sd * nd(rng)};
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
    // the decoder threads must not change a single byte of the output (clean signal and a noisy one, where the LDPC works for several iterations)
    for (const double snr : {40.0, 11.0}) {
        Profile p;
        profileFromSi(10, p);   // 4QAM at the highest rate: the one of the recording of issue #88
        const int frames = (int)((51L * 52 * interleaverDelay(p) + kDataSymbols - 1) / kDataSymbols) + 60;
        const std::vector<uint8_t> a = streamOf(Header::Pn595, p, snr, frames, 0), b = streamOf(Header::Pn595, p, snr, frames, 3);
        printf("  threads against none at %.0f dB: %zu and %zu bytes\n", snr, a.size(), b.size());
        CHECK(!a.empty() && a == b, "the output with decoder threads differs from the output without (%zu and %zu bytes)", a.size(), b.size());
    }
    printf(failures ? "dtmb_chain: %d FAILED\n" : "dtmb_chain: all passed\n", failures);
    return failures ? 1 : 0;
}
