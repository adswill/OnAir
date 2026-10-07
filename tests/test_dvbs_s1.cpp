// DVB-S (EN 300 421) transmit chain against an independent model written from the standard in this file: energy dispersal, Reed-Solomon (204,188),
// convolutional interleaver (I = 12), convolutional code (171,133 octal) with puncturing, QPSK mapping. The generator's symbols must be the model's
// symbols exactly. Then the receiver's building blocks: sync byte search, rate search, code rate and puncture phase on symbols with noise and a rotation.
#include "dect2/dvbs_tx.h"
#include "dect2/dvbt.h"
#include "../core/src/dvbs_s1.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

using namespace dect2;
using namespace dect2::dvbs;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

// ---- the model
namespace model {

// energy dispersal, clause 4.4.1: 1 + X^14 + X^15, loaded with 100101010000000 at the start of every eight packets, output enabled during the
// 187 bytes after the sync byte, the generator keeps running (not enabled) during the sync bytes; the sync byte of the first packet is inverted
std::vector<uint8_t> disperse(const std::vector<uint8_t>& ts) {      // ts: a multiple of 8 packets
    std::vector<uint8_t> out(ts);
    for (size_t g = 0; g + 8 * 188 <= ts.size(); g += 8 * 188) {
        int reg[15] = {1, 0, 0, 1, 0, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0};      // x1 .. x15
        auto bit = [&] { const int fb = reg[13] ^ reg[14]; for (int i = 14; i > 0; i--) reg[i] = reg[i - 1]; reg[0] = fb; return fb; };
        for (int p = 0; p < 8; p++) {
            uint8_t* pk = &out[g + (size_t)p * 188];
            if (p == 0) pk[0] = 0xB8;
            for (int j = 1; j < 188; j++) { int by = 0; for (int b = 0; b < 8; b++) by = (by << 1) | bit(); pk[j] ^= (uint8_t)by; }
            if (p != 7) for (int b = 0; b < 8; b++) bit();                  // the sync byte of the next packet goes by
        }
    }
    return out;
}

// Reed-Solomon (255,239) shortened to (204,188): field generator x^8 + x^4 + x^3 + x^2 + 1, g(x) = (x - a^0)(x - a^1) ... (x - a^15)
struct Rs {
    int ex[512], lg[256];
    Rs() { int x = 1; for (int i = 0; i < 255; i++) { ex[i] = x; lg[x] = i; x <<= 1; if (x & 256) x ^= 0x11D; } for (int i = 255; i < 512; i++) ex[i] = ex[i - 255]; }
    int mul(int a, int b) const { return a && b ? ex[lg[a] + lg[b]] : 0; }
    void encode(const uint8_t* in, uint8_t* out) const {
        int g[17] = {1};
        int deg = 0;
        for (int i = 0; i < 16; i++) {           // multiply by (x + a^i)
            int h[17] = {};
            for (int k = 0; k <= deg; k++) { h[k + 1] ^= g[k]; h[k] ^= mul(g[k], ex[i]); }
            deg++;
            for (int k = 0; k <= deg; k++) g[k] = h[k];
        }
        int reg[16] = {};
        for (int i = 0; i < 188; i++) {
            const int fb = in[i] ^ reg[15];
            for (int k = 15; k > 0; k--) reg[k] = reg[k - 1] ^ mul(fb, g[k]);
            reg[0] = mul(fb, g[0]);
            out[i] = in[i];
        }
        for (int k = 0; k < 16; k++) out[188 + k] = (uint8_t)reg[15 - k];
    }
};

// convolutional interleaver, clause 4.4.3: 12 branches, branch j delays by j * 17 bytes, the byte stream is sent round robin starting with branch 0
struct Interleaver {
    std::vector<std::vector<uint8_t>> f;
    size_t n = 0;
    Interleaver() { f.resize(12); for (int j = 0; j < 12; j++) f[(size_t)j].assign((size_t)j * 17, 0); }
    uint8_t next(uint8_t in) {
        std::vector<uint8_t>& q = f[n % 12];
        n++;
        if (q.empty()) return in;
        const uint8_t o = q.front();
        q.erase(q.begin());
        q.push_back(in);
        return o;
    }
};

// inner code: G1 = 171, G2 = 133 (octal), X output first. The register holds the last seven input bits, newest in the top position
struct Conv {
    unsigned reg = 0;
    void step(int bit, int& x, int& y) {
        reg = (reg >> 1) | ((unsigned)bit << 6);
        x = __builtin_parity(reg & 0171);
        y = __builtin_parity(reg & 0133);
    }
};

} // namespace model

int main() {
    // ---- the model against the generator, for every code rate
    std::mt19937 rng(77);
    for (int rate = 0; rate < 5; rate++) {
        std::vector<uint8_t> ts(8 * 188 * 12);
        for (size_t i = 0; i < ts.size(); i++) ts[i] = (uint8_t)rng();
        for (size_t p = 0; p < ts.size() / 188; p++) ts[p * 188] = 0x47;
        size_t next = 0;
        DvbsTxConfig cfg; cfg.standard = 1; cfg.rate = rate;
        auto tx = makeDvbsTx(cfg, [&](uint8_t* p) { memcpy(p, &ts[next * 188], 188); next++; });
        // the model
        static const char* X[5] = {"1", "10", "101", "10101", "1000101"};
        static const char* Y[5] = {"1", "11", "110", "11010", "1111010"};
        static const int N[5] = {1, 2, 3, 5, 7};
        const std::vector<uint8_t> d = model::disperse(ts);
        model::Rs rs;
        std::vector<uint8_t> coded204;
        for (size_t p = 0; p < d.size() / 188; p++) { uint8_t o[204]; rs.encode(&d[p * 188], o); coded204.insert(coded204.end(), o, o + 204); }
        model::Interleaver il;
        std::vector<uint8_t> bytes;
        for (uint8_t b : coded204) bytes.push_back(il.next(b));
        model::Conv cv;
        std::vector<int> serial;
        size_t step = 0;
        for (uint8_t b : bytes)
            for (int k = 7; k >= 0; k--) {
                int x, y;
                cv.step((b >> k) & 1, x, y);
                const size_t ph = step++ % (size_t)N[rate];
                if (X[rate][ph] == '1') serial.push_back(x);
                if (Y[rate][ph] == '1') serial.push_back(y);
            }
        // symbols: first bit of each pair to I, second to Q; 0 -> +1, 1 -> -1, unit power
        const size_t nsym = serial.size() / 2;
        std::vector<cf32> sym(nsym);
        tx->generate(sym.data(), nsym);
        size_t wrong = 0;
        for (size_t i = 0; i < nsym; i++) {
            const cf32 e((1 - 2 * serial[2 * i]) * 0.70710678f, (1 - 2 * serial[2 * i + 1]) * 0.70710678f);
            if (std::abs(sym[i] - e) > 1e-4f) wrong++;
        }
        static const char* rn[5] = {"1/2", "2/3", "3/4", "5/6", "7/8"};
        CHECK(wrong == 0, "DVB-S %s: %zu of %zu symbols differ from the model of the standard", rn[rate], wrong, nsym);
        printf("DVB-S %s: %zu symbols identical to the model\n", rn[rate], nsym);
    }

    // ---- the receiver's blocks: the stream with noise, turned by 90 degrees and conjugated, must be recognised with the right rate
    for (int rate = 0; rate < 5; rate++) {
        DvbsTxConfig cfg; cfg.standard = 1; cfg.rate = rate;
        uint64_t n = 0;
        auto tx = makeDvbsTx(cfg, [&](uint8_t* p) { memset(p, 0, 188); p[0] = 0x47; p[1] = 0; p[2] = 0x30; p[3] = 0x10 | (n & 15); p[4] = (uint8_t)(n >> 8); p[5] = (uint8_t)n; n++; });
        std::vector<cf32> sym(40000 + 1234);
        tx->generate(sym.data(), sym.size());
        std::normal_distribution<float> nd(0.f, 1.f);
        static const double esn0[5] = {5.0, 7.3, 8.6, 10.0, 11.0};                // about 2 dB above the threshold of each rate
        const float s = (float)std::sqrt(std::pow(10.0, -esn0[rate] / 10.0) / 2.0);
        std::vector<cf32> y(sym.size() - 1234);
        for (size_t i = 0; i < y.size(); i++) {
            const cf32 z = sym[i + 1234] + cf32(nd(rng), nd(rng)) * s;      // starts in the middle of the puncturing period
            y[i] = s1Variant(z, 3);                                         // conjugate and +90 degrees
        }
        const S1Hypothesis h = s1Search(y.data(), y.size());
        CHECK(h.ok && h.rate == rate, "DVB-S rate search: rate %d found as %d (ok %d, score %.2f, second %.2f)", rate, h.rate, (int)h.ok, h.score, h.second);
    }

    printf(fails ? "dvbs s1: FAILED (%d)\n" : "dvbs s1: ok\n", fails);
    return fails ? 1 : 0;
}
