// ATSC 3.0 LDPC (16200): the encoder satisfies every parity check of the matrix built from the tables, and the decoder recovers a code
// word from noisy soft bits, for all code rates.
#include "../core/src/atsc3_ldpc.h"
#include <cmath>
#include <cstdio>
#include <random>

using namespace dect2::atsc3;

static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); fails++; } } while (0)

int main() {
    std::mt19937 rng(11);
    std::normal_distribution<float> g(0.f, 1.f);
    for (int r = 2; r <= 13; r++) {
        const Ldpc& c = ldpc16200(r);
        char m[100];
        snprintf(m, sizeof m, "code %d/15 exists", r);
        CHECK(c.ok() && c.k() == 1080 * r, m);
        std::vector<uint8_t> bits(c.k());
        for (auto& b : bits) b = rng() & 1;
        c.encode(bits);
        // the decoder's own check says the noiseless code word is valid iff the encoder matches the graph built from the tables
        std::vector<float> llr(16200);
        for (int i = 0; i < 16200; i++) llr[i] = bits[i] ? -8.f : 8.f;
        std::vector<uint8_t> hard;
        int it = 0;
        bool ok = c.decode(llr, 5, hard, &it);
        snprintf(m, sizeof m, "noiseless code word of rate %d/15 is a code word", r);
        CHECK(ok && hard == bits, m);
        // noisy: BPSK at an Eb/N0 comfortably above the threshold of these codes
        double rate = r / 15.0, ebn0 = std::pow(10.0, (r <= 5 ? 1.5 : r <= 9 ? 2.5 : r <= 12 ? 3.5 : 5.0) / 10.0);
        float sigma = (float)std::sqrt(1.0 / (2 * rate * ebn0));
        for (int i = 0; i < 16200; i++) {
            float x = bits[i] ? -1.f : 1.f;
            float y = x + sigma * g(rng);
            llr[i] = 2 * y / (sigma * sigma);
        }
        ok = c.decode(llr, 80, hard, &it);
        snprintf(m, sizeof m, "decode noisy rate %d/15", r);
        CHECK(ok && hard == bits, m);
        printf("  rate %2d/15: decoded in %d iterations\n", r, it);
    }
    // the 64800-bit codes
    for (int r = 2; r <= 13; r++) {
        const Ldpc& c = ldpcCode(64800, r);
        char m[100];
        snprintf(m, sizeof m, "64800 code %d/15 exists", r);
        CHECK(c.ok() && c.k() == 4320 * r, m);
        std::vector<uint8_t> bits(c.k());
        for (auto& b : bits) b = rng() & 1;
        c.encode(bits);
        std::vector<float> llr(64800);
        for (int i = 0; i < 64800; i++) llr[i] = bits[i] ? -8.f : 8.f;
        std::vector<uint8_t> hard;
        int it = 0;
        bool ok = c.decode(llr, 5, hard, &it);
        snprintf(m, sizeof m, "noiseless 64800 code word of rate %d/15 is a code word", r);
        CHECK(ok && hard == bits, m);
        double rate = r / 15.0, ebn0 = std::pow(10.0, (r <= 5 ? 1.0 : r <= 9 ? 1.8 : r <= 12 ? 2.8 : 4.0) / 10.0);
        float sigma = (float)std::sqrt(1.0 / (2 * rate * ebn0));
        for (int i = 0; i < 64800; i++) llr[i] = 2 * ((bits[i] ? -1.f : 1.f) + sigma * g(rng)) / (sigma * sigma);
        ok = c.decode(llr, 80, hard, &it);
        snprintf(m, sizeof m, "decode noisy 64800 rate %d/15", r);
        CHECK(ok && hard == bits, m);
        printf("  64800 rate %2d/15: decoded in %d iterations\n", r, it);
    }
    printf(fails ? "atsc3 ldpc: FAILED\n" : "atsc3 ldpc: ok\n");
    return fails ? 1 : 0;
}
