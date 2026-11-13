// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Seeded mutation run of the mesh packet layers, meant for DECT2_SANITIZE=ON: every decoder must survive any bytes of
// any length (also longer than a LoRa frame, which the Rust records cut instead of overflowing). Also a libFuzzer target with -DONAIR_LIBFUZZER.
#include "dect2/mesh/mesh_proto.h"
#include "dect2/mesh/mesh_pb.h"
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>
using namespace dect2;

static void one(const uint8_t* d, size_t n) {
    static MeshProto mp;
    const MeshRadioInfo radio{};
    mp.decodeMeshtastic(d, n, radio);
    mp.decodeMeshCore(d, n, radio);
    meshDecodeLorawan(d, n);
    meshDecodeLoraAprs(d, n);
    meshDecodeMeshCom(d, n);
    meshPrintable(d, n);
    meshHexString(d, n);
    meshpb::Reader r(d, n);
    meshpb::Field f;
    std::vector<uint32_t> a;
    std::vector<uint64_t> b;
    while (r.next(f)) { meshpb::readRepeatedFixed32(f, a); meshpb::readRepeatedVarint(f, b); }
    if (n >= 4) { lorawanNetwork((uint32_t)d[0] << 24 | (uint32_t)d[1] << 16 | (uint32_t)d[2] << 8 | d[3]); lorawanMacName(d[0], d[1] & 1); meshtasticNodeIdString(d[0]); }
}

#ifdef ONAIR_LIBFUZZER
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* d, size_t n) { one(d, n); return 0; }
#else
int main() {
    const char* it = getenv("ONAIR_FUZZ_ITERS");
    const long iters = it ? atol(it) : 20000;
    std::mt19937 g(909);
    const std::vector<uint8_t> seeds[] = {
        {0xFF, 0xFF, 0xFF, 0xFF, 1, 2, 3, 4, 5, 6, 7, 8, 0x63, 0x08, 0, 1, 0x08, 0x01, 0x12, 0x02, 'h', 'i'},
        {0x15, 0x01, 0xAB, 0x11, 0xF9, 0x20, 0xA6, 0x1F, 0x33, 0xA7, 0x90, 0x6D, 0x3A, 0x9D, 0xCD, 0x17, 0x51, 0xD8, 0xC3, 0x31},
        {0x40, 0xDA, 0x1B, 0x01, 0x26, 0x05, 0x01, 0x00, 0x06, 0x02, 0x01, 0x1, 2, 3, 4},
        {0x3C, 0xFF, 0x01, 'L', 'Y', '1', '>', 'A', 'P', ':', '!', '5', '4', '4', '1'},
    };
    for (long i = 0; i < iters; i++) {
        std::vector<uint8_t> s = seeds[g() % 4];
        const size_t extra = g() % 300;
        for (size_t k = 0; k < extra; k++) s.push_back((uint8_t)g());
        for (int m = (int)(g() % 6); m > 0 && !s.empty(); m--) s[g() % s.size()] = (uint8_t)g();
        if (g() % 4 == 0) s.resize(g() % (s.size() + 1));
        one(s.data(), s.size());
    }
    printf("fuzz_mesh_proto: %ld inputs, no crash\n", iters);
    return 0;
}
#endif
