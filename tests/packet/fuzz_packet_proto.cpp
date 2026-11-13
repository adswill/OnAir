// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Seeded mutation run of the AX.25 / APRS layer, meant for DECT2_SANITIZE=ON: frame parser, text forms, FCS, the
// HDLC receiver and the APRS parser must never crash or hang on any bytes. Also a libFuzzer target with -DONAIR_LIBFUZZER.
#include "dect2/packet/packet_ax25.h"
#include "dect2/packet/packet_aprs.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>
namespace X = dect2::ax25;

static void one(const uint8_t* d, size_t n) {
    X::Frame f;
    if (X::parseFrame(d, n, f)) { f.tnc2(); f.pathStr(); f.typeName(); f.from.str(); X::buildFrame(f); }
    X::fcs(d, n); X::fcsOk(d, n);
    X::HdlcRx rx([](const std::vector<uint8_t>&, bool) {}, (n & 1) != 0);
    for (size_t i = 0; i < n; i++) for (int k = 0; k < 8; k++) rx.bit((d[i] >> k) & 1);
    const size_t h = n / 2;
    const std::string dest((const char*)d, h), info((const char*)d + h, n - h);
    dect2::aprs::Info o;
    dect2::aprs::parse(dest, info, o);
    dect2::aprs::parse("APRS", info, o);
    dect2::aprs::parse(info, info, o);
    double v;
    if (n >= 8) { memcpy(&v, d, 8); dect2::aprs::fmtLat(v); dect2::aprs::fmtLon(v); }
}

#ifdef ONAIR_LIBFUZZER
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* d, size_t n) { one(d, n); return 0; }
#else
int main() {
    const char* it = getenv("ONAIR_FUZZ_ITERS");
    const long iters = it ? atol(it) : 200000;
    std::mt19937 g(909);
    const std::string seeds[] = {"!4903.50N/07201.75W-Test", "`(_f\"Oj/>comment}ab!}", ":WB4APR   :Hello{3", ";LEADER   *092345z4903.50N/07201.75W>088/036", "!/5L!!<*e7>7P[",
                                 "_10090556c220s004g005t077r000p000P000h50b09900wRSW", ")AID #2!4903.50N/07201.75Wa", ">092345zStatus"};
    for (long i = 0; i < iters; i++) {
        std::string s = seeds[g() % 8];
        if (g() % 2) s = "S32U6T" + s;
        const int k = 1 + (int)(g() % 8);
        for (int j = 0; j < k; j++) {
            switch (g() % 4) {
            case 0: if (!s.empty()) s[g() % s.size()] = (char)g(); break;
            case 1: if (!s.empty()) s.erase(g() % s.size(), 1 + g() % 4); break;
            case 2: s.insert(g() % (s.size() + 1), 1, (char)g()); break;
            default: s.resize(g() % (s.size() + 1)); break;
            }
        }
        one((const uint8_t*)s.data(), s.size());
        std::vector<uint8_t> raw(g() % 90);                  // plain random bytes too
        for (auto& x : raw) x = (uint8_t)g();
        one(raw.data(), raw.size());
    }
    printf("fuzz_packet_proto: %ld inputs, no crash\n", iters);
    return 0;
}
#endif
