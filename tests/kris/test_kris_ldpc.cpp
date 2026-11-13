// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Kris channel codes through the Rust C ABI (dect2::kris wrappers): LDPC encode / decode for every n and rate with a fixed error pattern, argument checks, and the
// K = 7 convolutional code (both rates, terminated and not) with a Viterbi decode of a damaged block.
#include "dect2/kris/kris_common.h"
#include "dect2/kris/kris_fec.h"
#include <cstdio>
using namespace dect2::kris;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAILED: " __VA_ARGS__); printf("\n"); } } while (0)

static void ldpcTests() {
    XorShift64 rng{0x1234567ull};
    const int lens[] = {1920, 2304, 7680};
    const int expectK1920[] = {480, 640, 960, 1280, 1440, 1600, 1728};
    int codes = 0;
    for (int n : lens) {
        for (int ri = 0; ri < 7; ri++) {
            const LdpcRate r = static_cast<LdpcRate>(ri);
            const int k = ldpcK(n, r);
            if (n == 2304 && ri == 6) { CHECK(k == -1, "2304 at 9/10 has no code, got k=%d", k); continue; }
            CHECK(k > 0 && k < n, "n=%d r=%d: k=%d", n, ri, k);
            if (n == 1920) CHECK(k == expectK1920[ri], "n=1920 r=%d: k=%d", ri, k);
            if (k <= 0) continue;
            codes++;
            std::vector<uint8_t> info(k), cw(n), out(k);
            for (auto& b : info) b = (rng.next() >> 33) & 1;
            CHECK(ldpcEncode(n, r, info.data(), cw.data()), "n=%d r=%d encode", n, ri);
            bool sys = true;
            for (int i = 0; i < k; i++) sys = sys && cw[i] == info[i];
            CHECK(sys, "n=%d r=%d not systematic", n, ri);
            // clean: no iteration needed
            std::vector<float> llr(n);
            for (int i = 0; i < n; i++) llr[i] = cw[i] ? -4.f : 4.f;
            CHECK(ldpcDecode(n, r, llr.data(), out.data()) == 0 && out == info, "n=%d r=%d clean decode", n, ri);
            // fixed error pattern: weak wrong LLRs, fewer at the high rates
            const int flips = ri >= 5 ? n / 400 : n / 100;
            for (int i = 0; i < flips; i++) { const int p = (i * 997 + 13) % n; llr[p] = cw[p] ? 0.5f : -0.5f; }
            const int it = ldpcDecode(n, r, llr.data(), out.data(), 50);
            CHECK(it > 0 && out == info, "n=%d r=%d decode after %d weak errors: iterations %d", n, ri, flips, it);
            // a very short iteration budget on a damaged word reports failure, not garbage success
            CHECK(ldpcDecode(n, r, llr.data(), out.data(), 0) == -1, "n=%d r=%d zero iterations", n, ri);
        }
    }
    CHECK(codes == 20, "%d codes", codes);
    CHECK(ldpcK(1000, LdpcRate::R1_2) == -1, "unsupported n");
    CHECK(ldpcK(1920, static_cast<LdpcRate>(9)) == -1, "unsupported rate");
    std::vector<uint8_t> one(8), two(8);
    CHECK(!ldpcEncode(1000, LdpcRate::R1_2, one.data(), two.data()), "unsupported encode");
    CHECK(!ldpcEncode(1920, LdpcRate::R1_2, nullptr, two.data()), "null encode");
}

static void convTests() {
    XorShift64 rng{0xABCDEFull};
    for (int kind = 0; kind < 2; kind++) {
        for (int term = 0; term < 2; term++) {
            std::vector<uint8_t> bits(500);
            for (auto& b : bits) b = (rng.next() >> 29) & 1;
            auto coded = convEncode(kind, bits, term);
            CHECK(coded.size() == (bits.size() + (term ? 6 : 0)) * (kind ? 4 : 2), "kind %d term %d: coded size %zu", kind, term, coded.size());
            std::vector<float> llr(coded.size());
            for (size_t i = 0; i < coded.size(); i++) llr[i] = coded[i] ? -2.f : 2.f;
            for (size_t i = 5; i < llr.size(); i += 50) llr[i] = -llr[i];   // one hard error every 50 coded bits
            auto dec = viterbiDecode(kind, llr, term);
            CHECK(dec == bits, "kind %d term %d: decoded block differs (%zu bits)", kind, term, dec.size());
        }
    }
    CHECK(convEncode(2, {1, 0}, true).empty(), "bad kind");
    CHECK(viterbiDecode(0, std::vector<float>(8, 1.f), true).empty(), "block shorter than the tail");
    // first output symbols of an impulse: all generators have the newest tap set
    auto c = convEncode(1, {1}, false);
    CHECK(c.size() == 4 && c[0] == 1 && c[1] == 1 && c[2] == 1 && c[3] == 1, "impulse response");
}

int main() {
    ldpcTests();
    convTests();
    if (fails) { printf("test_kris_ldpc: %d failure(s)\n", fails); return 1; }
    printf("test_kris_ldpc: ok\n");
    return 0;
}
