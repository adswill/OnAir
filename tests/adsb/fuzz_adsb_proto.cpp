// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Seeded mutation run of the ADS-B / Mode S message layer, meant for DECT2_SANITIZE=ON: decoding, the CRC,
// single-bit correction, Comm-B inference and CPR must never crash or hang on any bytes or numbers. Also a libFuzzer target with -DONAIR_LIBFUZZER.
#include "dect2/adsb/adsb_msg.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>
using namespace dect2::adsb;

static unsigned long long sink = 0;

static void one(const uint8_t* d, size_t n) {
    std::vector<uint8_t> m(d, d + n);
    m.resize(14);   // the callers always hold a whole 112 bit buffer
    for (int bits : {56, 112, 8, 24, 0, 88}) {
        Msg msg;
        sink += decodeMsg(m.data(), bits, msg) + msg.callsign.size() + msg.commb.callsign.size() + (unsigned)msg.altFt;
        if (bits >= 24) {
            const uint32_t r = crc24(m.data(), bits);
            sink += r + crcParity(m.data(), bits) + (unsigned)singleBitPosition(bits, r);
        }
    }
    sink += inferCommB(m.data(), (int)(m[0] * 200) - 1000).bds + inferCommB(m.data()).bds;
    int alt = 0;
    const unsigned v = (unsigned)(m[1] << 8 | m[2]);
    sink += decodeAc13(v, alt) + decodeAlt12(v, alt) + gillhamToAltitude(v, alt) + (unsigned)alt + (unsigned)decodeId13(v);
    double lat = 0, lon = 0, a, b;
    memcpy(&a, m.data(), 8); memcpy(&b, m.data() + 6, 8);
    const int le = (int)(v * 2), lo = (int)(m[3] << 9);
    sink += cprGlobalAirborne(le, lo, le + 5, lo + 9, m[4] & 1, lat, lon) + cprLocalAirborne(le, lo, m[4] & 1, a, b, lat, lon) +
            cprGlobalSurface(le, lo, le + 5, lo + 9, m[4] & 1, a, b, lat, lon) + cprLocalSurface(le, lo, m[4] & 1, a, b, lat, lon);
    // the inputs are any 8 bytes taken as doubles, so the results may be NaN or huge: fold them without a float-to-int cast (UB out of range)
    auto fold = [](double x) { uint64_t u; memcpy(&u, &x, 8); return u; };
    sink += (unsigned long long)cprNl(a) + fold(distanceNm(a, b, b, a)) + fold(bearingDeg(a, b, a, b)) + fold(magDeclinationDeg(a, b));
    sink += raText(v, m[5], m[6] & 1).size();
}

#ifdef ONAIR_LIBFUZZER
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* d, size_t n) { one(d, n); return 0; }
#else
int main() {
    const char* it = getenv("ONAIR_FUZZ_ITERS");
    const long iters = it ? atol(it) : 200000;
    std::mt19937 g(31337);
    const uint8_t seed[14] = {0x8D, 0x48, 0x40, 0xD6, 0x20, 0x2C, 0xC3, 0x71, 0xC3, 0x2C, 0xE0, 0x57, 0x60, 0x98};
    for (long i = 0; i < iters; i++) {
        uint8_t d[14];
        memcpy(d, seed, 14);
        if (g() % 3 == 0) for (auto& c : d) c = (uint8_t)g();
        const int k = (int)(g() % 6);
        for (int j = 0; j < k; j++) d[g() % 14] ^= (uint8_t)(1u << (g() % 8));
        if (g() % 5 == 0) d[0] = (uint8_t)((g() % 32) << 3 | (g() & 7));
        one(d, 14);
    }
    printf("fuzz_adsb_proto: %ld inputs, no crash (%llu)\n", iters, sink & 1);
    return 0;
}
#endif
