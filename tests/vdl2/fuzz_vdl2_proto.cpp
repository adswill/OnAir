// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Seeded mutation run of the VDL Mode 2 layers, meant for DECT2_SANITIZE=ON: the frame decoder, the HDLC deframer, the burst
// body gatherer with Reed-Solomon, the header code and the address functions must never crash or hang on any bytes. Also a libFuzzer target with -DONAIR_LIBFUZZER.
#include "dect2/vdl2/vdl2_proto.h"
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>
using namespace dect2;

static void one(const uint8_t* d, size_t n) {
    Vdl2Message m;
    vdl2ParseFrame(d, n, m);
    std::vector<uint8_t> bits(d, d + n);
    for (auto& b : bits) b &= 1;
    vdl2HdlcFrames(bits);
    std::vector<uint8_t> s = bits;
    vdl2Scramble(s, n ? d[0] % (n + 1) : 0);
    vdl2FcsOk(d, n);
    vdl2Fcs(d, n);
    if (n >= 4) {
        const Vdl2Addr a = vdl2GetAddr(d);
        uint8_t o[4];
        vdl2PutAddr(o, a, d[0] & 1);
        vdl2AddrTypeName(a.type);
        const uint32_t word = (uint32_t)(d[0] | d[1] << 8 | d[2] << 16 | (uint32_t)d[3] << 24);
        uint32_t len = 0;
        bool corr = false;
        vdl2HeaderDecode(word & 0x1FFFFFF, len, corr);
        Vdl2Layout l;
        if (vdl2Layout(len & 0x3FFF, l)) {
            std::vector<uint8_t> body(8 * (size_t)(l.dataOct + l.fecOct));
            for (size_t i = 0; i < body.size(); i++) body[i] = (uint8_t)(d[i % n] >> (i % 8)) & 1;
            std::vector<uint8_t> out;
            int fixed = 0;
            vdl2BurstData(body.data(), l, len & 0x3FFF, out, fixed);
        }
    }
    if (n >= 255) { uint8_t cw[255]; for (int i = 0; i < 255; i++) cw[i] = d[i]; vdl2RsDecode(cw, (int)(d[0] % 7)); }
}

#ifdef ONAIR_LIBFUZZER
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* d, size_t n) { one(d, n); return 0; }
#else
int main() {
    const char* it = getenv("ONAIR_FUZZ_ITERS");
    const long iters = it ? atol(it) : 30000;
    std::mt19937 g(5150);
    for (long i = 0; i < iters; i++) {
        std::vector<uint8_t> s(g() % 700);
        for (auto& b : s) b = (uint8_t)(g() % 3 ? g() : (g() % 2 ? 0x7E : 0xFF));
        if (s.size() >= 9 && g() % 2) s[8] = (uint8_t)(g() % 2 ? 0xAF : g());
        one(s.data(), s.size());
    }
    printf("fuzz_vdl2_proto: %ld inputs, no crash\n", iters);
    return 0;
}
#endif
