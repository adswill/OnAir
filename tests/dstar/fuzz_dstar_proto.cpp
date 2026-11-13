// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Seeded mutation run of the D-STAR frame layer, meant for DECT2_SANITIZE=ON: header unpack / decode, the FEC pieces, the
// flag text and both GPS parsers must never crash or hang on any bytes. Also a libFuzzer target with -DONAIR_LIBFUZZER.
#include "dect2/dstar/dstar_proto.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>
namespace D = dect2::dstar;

static void one(const uint8_t* d, size_t n) {
    if (n < 4) return;
    auto at = [&](size_t i) { return d[i % n]; };
    uint8_t h41[41];
    for (int i = 0; i < 41; i++) h41[i] = at((size_t)i);
    D::Header h;
    D::headerUnpack(h41, h);
    D::headerPack(h, h41);
    D::flagText(h41);
    float soft[660];
    for (int i = 0; i < 660; i++) soft[i] = ((float)at((size_t)i) - 128.f) / 32.f;
    uint8_t out41[41], bits[660];
    D::headerDecode(soft, out41);
    D::headerEncode(out41, bits);
    float dil[660];
    D::deinterleave(soft, dil);
    uint8_t v[400];
    D::viterbi(soft, (int)(1 + at(0) % 330), v);
    D::convEncode(bits, (int)(at(1) % 330), v);
    D::scrambleBits(bits, (int)(at(2) % 660));
    char fr[4][24];
    D::ambeFrame(bits, fr);
    const std::string s((const char*)d, n);
    D::Fix f;
    D::parseNmea(s, f);
    D::parseDprs(s, f);
    D::parseNmea("$" + s.substr(0, n / 2) + "*" + D::nmeaChecksum(s.substr(0, n / 2)), f);
    char b[24];
    snprintf(b, sizeof b, "$$CRC%04X,", D::dprsCrc(s));
    D::parseDprs(b + s, f);
    D::nmeaChecksum(s);
}

#ifdef ONAIR_LIBFUZZER
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* d, size_t n) { one(d, n); return 0; }
#else
int main() {
    const char* it = getenv("ONAIR_FUZZ_ITERS");
    const long iters = it ? atol(it) : 50000;
    std::mt19937 g(777);
    const char seed[] = "LY1TST-7>API282,DSTAR*:!5441.23N/02516.78E>123/045hello/A=000200";
    for (long i = 0; i < iters; i++) {
        std::vector<uint8_t> s;
        if (g() % 2) s.assign(seed, seed + sizeof seed - 1);
        const size_t extra = 4 + g() % 100;
        for (size_t k = 0; k < extra; k++) s.push_back((uint8_t)(g() % 3 ? g() : (g() & 1)));
        for (int m = (int)(g() % 5); m > 0 && !s.empty(); m--) s[g() % s.size()] = (uint8_t)g();
        one(s.data(), s.size());
    }
    printf("fuzz_dstar_proto: %ld inputs, no crash\n", iters);
    return 0;
}
#endif
