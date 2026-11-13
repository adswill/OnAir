// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Seeded mutation run of the DRM signalling and data layers, meant for DECT2_SANITIZE=ON: FAC / SDC parsing, the CRC checks,
// the super frame split, the text segment check and the data service helpers must never crash or hang on any bytes at any length (the input is copied to an
// exactly sized heap block). Also a libFuzzer target with -DONAIR_LIBFUZZER.
#include "dect2/drm/drm_proto.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>
namespace P = dect2::drmp;

static void one(const uint8_t* d, size_t n) {
    uint8_t* p = n ? (uint8_t*)malloc(n) : nullptr;
    if (n) memcpy(p, d, n);
    std::vector<uint8_t> bits(p, p + n);
    for (auto& b : bits) b &= 1;
    P::crc8(bits.data(), bits.size()); P::crc16(bits.data(), bits.size());
    dect2::drm::FacInfo f;
    if (bits.size() >= 72) P::facParse(bits.data(), false, f);
    if (bits.size() >= 116) P::facParse(bits.data(), true, f);
    dect2::drm::SdcInfo s; P::sdcParse(p, (int)n, s); P::sdcParse(p, (int)n, s);
    int afs;
    for (int fb = 0; fb < 12; fb++) if (bits.size() >= (size_t)(4 + 8 * fb + 16)) P::sdcCheckBits(bits.data(), fb, afs);
    dect2::drm::AacSuperFrame a;
    for (int nf : {5, 10, 3, 16}) P::aacSuperFrameParse(p, (int)n, n ? p[0] : 0, nf, a);
    P::aacSuperFrameParse(p, (int)n, 0, 10, a);
    size_t off, len; P::groupPayload(p, n, off, len);
    P::displayText(p, n); P::looksText(p, n);
    if (n) { const int need = P::textSegmentNeed(p[0]); if ((size_t)need <= n) P::textSegment(p, n, need); }
    int y, m, dd; P::mjdToDate(n > 3 ? (int)(p[0] << 8 | p[1]) << 1 : 0, y, m, dd);
    P::appName(n ? p[0] % 4 : 0, n > 1 ? p[1] | (n > 2 ? p[2] << 8 : 0) : 0);
    free(p);
}

#ifdef ONAIR_LIBFUZZER
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* d, size_t n) { one(d, n); return 0; }
#else
int main() {
    const long iters = getenv("ONAIR_FUZZ_ITERS") ? atol(getenv("ONAIR_FUZZ_ITERS")) : 60000;
    std::mt19937 g(97531);
    std::vector<uint8_t> seed = {0x06, 0x01, 0x00, 0x00, 0x0A, 0x10, 0x20, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x14, 0x80, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    for (long it = 0; it < iters; it++) {
        std::vector<uint8_t> b = seed;
        const int k = (int)(g() % 6);
        for (int i = 0; i < k; i++) {
            switch (g() % 5) {
            case 0: b[g() % b.size()] = (uint8_t)g(); break;
            case 1: b.resize(g() % 300); break;
            case 2: b.insert(b.begin() + g() % (b.size() + 1), (uint8_t)g()); break;
            case 3: if (b.size() > 1) b.erase(b.begin() + g() % b.size()); break;
            default: for (auto& x : b) if (g() % 8 == 0) x = (uint8_t)g();
            }
            if (b.empty()) b.push_back(0);
        }
        one(b.data(), b.size());
        if (g() % 3 == 0) { std::vector<uint8_t> r(g() % 400); for (auto& x : r) x = (uint8_t)g(); one(r.data(), r.size()); }
    }
    printf("fuzz_drm_proto: %ld iterations ok\n", iters);
    return 0;
}
#endif
