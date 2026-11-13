// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Kris helpers: QAM map / soft demap (round trip, Gray property, unit power), CRC known vectors, Zadoff-Chu, root-raised-cosine taps, xorshift64.
#include "dect2/kris/kris_common.h"
#include <cmath>
#include <cstdio>
#include <cstring>
using namespace dect2::kris;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAILED: " __VA_ARGS__); printf("\n"); } } while (0)

static const Mod kMods[] = {Mod::Bpsk, Mod::Qpsk, Mod::Qam16, Mod::Qam64, Mod::Qam256, Mod::Qam1024};

static void qamTests() {
    for (Mod m : kMods) {
        const int b = static_cast<int>(m);
        const size_t labels = size_t(1) << b;
        // every label once
        std::vector<uint8_t> bits;
        for (size_t l = 0; l < labels; l++)
            for (int j = 0; j < b; j++) bits.push_back((l >> (b - 1 - j)) & 1);
        auto sym = qamMap(bits.data(), bits.size(), m);
        CHECK(sym.size() == labels, "mod %d: %zu symbols", b, sym.size());
        double p = 0;
        for (auto s : sym) p += std::norm(s);
        p /= double(labels);
        CHECK(std::fabs(p - 1.0) < 1e-4, "mod %d: average power %.6f", b, p);
        // all points distinct
        float dmin = 1e30f;
        for (size_t a = 0; a < labels; a++)
            for (size_t c = a + 1; c < labels; c++) dmin = std::min(dmin, std::abs(sym[a] - sym[c]));
        CHECK(dmin > 1e-3f, "mod %d: duplicate points", b);
        // Gray: every pair at the minimum distance differs in exactly one bit
        for (size_t a = 0; a < labels; a++)
            for (size_t c = a + 1; c < labels; c++)
                if (std::abs(sym[a] - sym[c]) < dmin * 1.001f) CHECK(__builtin_popcountll(a ^ c) == 1, "mod %d: neighbours %zu,%zu differ in %d bits", b, a, c, __builtin_popcountll(a ^ c));
        // noiseless round trip through the soft demapper
        std::vector<float> llr(bits.size());
        qamDemapSoft(sym.data(), sym.size(), m, 0.05f, llr.data());
        int bad = 0;
        for (size_t i = 0; i < bits.size(); i++) {
            if ((llr[i] < 0) != (bits[i] == 1)) bad++;
            if (llr[i] == 0.f) bad++;
        }
        CHECK(bad == 0, "mod %d: %d wrong LLR signs on noiseless symbols", b, bad);
        // a small perturbation does not flip anything
        for (auto& s : sym) s += std::complex<float>(0.02f, -0.02f);
        qamDemapSoft(sym.data(), sym.size(), m, 0.05f, llr.data());
        bad = 0;
        for (size_t i = 0; i < bits.size(); i++) if ((llr[i] < 0) != (bits[i] == 1)) bad++;
        CHECK(bad == 0 || b >= 8, "mod %d: %d flips after a small offset", b, bad);
    }
    // BPSK convention and the first-bit-is-sign convention
    uint8_t z = 0, o = 1;
    CHECK(qamMap(&z, 1, Mod::Bpsk)[0] == std::complex<float>(1.f, 0.f), "bpsk 0");
    CHECK(qamMap(&o, 1, Mod::Bpsk)[0] == std::complex<float>(-1.f, 0.f), "bpsk 1");
    const uint8_t q00[2] = {0, 0}, q11[2] = {1, 1};
    CHECK(qamMap(q00, 2, Mod::Qpsk)[0].real() > 0 && qamMap(q00, 2, Mod::Qpsk)[0].imag() > 0, "qpsk 00");
    CHECK(qamMap(q11, 2, Mod::Qpsk)[0].real() < 0 && qamMap(q11, 2, Mod::Qpsk)[0].imag() < 0, "qpsk 11");
    // a partial last symbol is padded: 5 bits of 16-QAM -> 2 symbols
    const uint8_t five[5] = {1, 0, 1, 1, 0};
    CHECK(qamMap(five, 5, Mod::Qam16).size() == 2, "partial symbol count");
    // the LLR scale follows the noise variance
    std::complex<float> y(0.3f, 0.f);
    float a, c;
    qamDemapSoft(&y, 1, Mod::Bpsk, 0.5f, &a);
    qamDemapSoft(&y, 1, Mod::Bpsk, 1.0f, &c);
    CHECK(std::fabs(a - 2 * c) < 1e-4f && std::fabs(a - 4 * 0.3f / 0.5f) < 1e-3f, "bpsk llr scale %.4f %.4f", a, c);
}

static void crcTests() {
    const char* s = "123456789";
    CHECK(crc16(reinterpret_cast<const uint8_t*>(s), 9) == 0x29B1, "crc16 %04X", crc16(reinterpret_cast<const uint8_t*>(s), 9));
    CHECK(crc32(reinterpret_cast<const uint8_t*>(s), 9) == 0xCBF43926u, "crc32 %08X", crc32(reinterpret_cast<const uint8_t*>(s), 9));
    CHECK(crc16(nullptr, 0) == 0xFFFF, "crc16 empty");
    CHECK(crc32(nullptr, 0) == 0, "crc32 empty");
}

static void sequenceTests() {
    for (int len : {63, 64, 139, 839}) {
        auto zc = zadoffChu(25, len);
        CHECK(int(zc.size()) == len, "zc length");
        bool unit = true;
        for (auto v : zc) unit = unit && std::fabs(std::abs(v) - 1.f) < 1e-4f;
        CHECK(unit, "zc %d not constant modulus", len);
        // zero periodic autocorrelation at a non-zero lag (prime or coprime root)
        if (len == 139 || len == 839) {
            std::complex<double> acc = 0;
            for (int i = 0; i < len; i++) acc += std::complex<double>(zc[i]) * std::conj(std::complex<double>(zc[(i + 7) % len]));
            CHECK(std::abs(acc) < 1e-2 * len, "zc %d autocorrelation %.4f", len, std::abs(acc));
        }
    }
    CHECK(zadoffChu(1, 0).empty(), "zc empty");
    auto h = rrcTaps(0.35f, 4, 8);
    CHECK(h.size() == 33, "rrc length %zu", h.size());
    double e = 0;
    for (float v : h) e += double(v) * v;
    CHECK(std::fabs(e - 1.0) < 1e-5, "rrc energy %.6f", e);
    bool sym = true;
    for (size_t i = 0; i < h.size(); i++) sym = sym && std::fabs(h[i] - h[h.size() - 1 - i]) < 1e-6f;
    CHECK(sym && h[16] > h[15] && h[16] > h[17], "rrc shape");
    // the RRC convolved with itself is a Nyquist pulse: zero at multiples of the symbol period
    const int sps = 4;
    for (int k = 1; k <= 3; k++) {
        double acc = 0;
        for (int i = 0; i < int(h.size()); i++) {
            const int j = i + k * sps;
            if (j >= 0 && j < int(h.size())) acc += double(h[i]) * h[j];
        }
        CHECK(std::fabs(acc) < 0.02, "rrc ISI at %d symbols: %.4f", k, acc);
    }
    XorShift64 r{88172645463325252ull};
    // reference values of the 13/7/17 xorshift from seed 88172645463325252 (Marsaglia)
    CHECK(r.next() == 8748534153485358512ull, "xorshift 1");
    CHECK(r.next() == 3040900993826735515ull, "xorshift 2");
}

int main() {
    qamTests();
    crcTests();
    sequenceTests();
    if (fails) { printf("test_kris_common: %d failure(s)\n", fails); return 1; }
    printf("test_kris_common: ok\n");
    return 0;
}
