// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Seeded mutation run of the transport-stream parsing layer, meant for DECT2_SANITIZE=ON: every section parser, the packet
// header, the CRC and the PAT packet must never crash or hang on any bytes, at any length (the input is copied to an exactly sized heap block).
// Also a libFuzzer target with -DONAIR_LIBFUZZER.
#include "dect2/tv/ts_proto.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>
namespace T = dect2::tsp;

static void one(const uint8_t* d, size_t n) {
    uint8_t* p = n ? (uint8_t*)malloc(n) : nullptr;
    if (n) memcpy(p, d, n);
    const int len = (int)n;
    int tsid, onid;
    std::vector<T::PatEntry> pat; T::parsePat(p, len, tsid, pat);
    T::Pmt pmt; T::parsePmt(p, len, pmt);
    std::vector<T::SdtService> sdt; T::parseSdt(p, len, tsid, onid, sdt);
    T::Nit nit; T::parseNit(p, len, nit);
    T::Eit eit; T::parseEit(p, len, eit);
    std::vector<T::Tvct> tv; T::parseTvct(p, len, tsid, tv);
    T::Tdt tdt; T::parseTdt(p, len, tdt);
    T::PacketHeader h; T::packetHeader(p, len, h);
    T::crc32(p, len);
    uint8_t pk[188];
    if (n >= 8) T::patPacket(p[0] << 8 | p[1], p[2] << 8 | p[3], p[4] << 8 | p[5], p[6], pk);
    free(p);
}

#ifdef ONAIR_LIBFUZZER
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* d, size_t n) { one(d, n); return 0; }
#else
int main() {
    const long iters = getenv("ONAIR_FUZZ_ITERS") ? atol(getenv("ONAIR_FUZZ_ITERS")) : 100000;
    std::mt19937 g(1357);
    std::vector<uint8_t> seed = {0x42, 0xF0, 20, 0x12, 0x34, 0xC1, 0, 0, 0x20, 0x01, 0xFF, 0x01, 0x01, 0xFC, 0x80, 12, 0x48, 10, 0x01, 3, 'A', 'B', 'C', 4, 'T', 'e', 's', 't', 0, 0, 0, 0};
    for (long it = 0; it < iters; it++) {
        std::vector<uint8_t> b = seed;
        const int k = (int)(g() % 6);
        for (int i = 0; i < k; i++) {
            switch (g() % 5) {
            case 0: b[g() % b.size()] = (uint8_t)g(); break;
            case 1: b.resize(g() % 400); break;
            case 2: b.insert(b.begin() + g() % (b.size() + 1), (uint8_t)g()); break;
            case 3: if (b.size() > 1) b.erase(b.begin() + g() % b.size()); break;
            default: for (auto& x : b) if (g() % 8 == 0) x = (uint8_t)g();
            }
            if (b.empty()) b.push_back(0);
        }
        b[0] = (uint8_t)(g() % 3 ? (g() % 2 ? 0x4E : 0x02) : g());
        one(b.data(), b.size());
        if (g() % 3 == 0) { std::vector<uint8_t> r(g() % 700); for (auto& x : r) x = (uint8_t)g(); one(r.data(), r.size()); }
    }
    printf("fuzz_ts_proto: %ld iterations ok\n", iters);
    return 0;
}
#endif
