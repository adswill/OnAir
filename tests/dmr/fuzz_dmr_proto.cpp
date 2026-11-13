// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Seeded mutation run of the DMR frame layer, meant for DECT2_SANITIZE=ON: every decoder, parser and text converter
// must never crash or hang on any bytes. Also a libFuzzer target with -DONAIR_LIBFUZZER.
#include "dect2/dmr/dmr_proto.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>
namespace D = dect2::dmr;

static void one(const uint8_t* d, size_t n) {
    if (n < 4) return;
    D::Bits bits(n * 8);
    for (size_t i = 0; i < bits.size(); i++) bits[i] = (d[i / 8] >> (7 - i % 8)) & 1;
    auto slice = [&](size_t want) { D::Bits b(want); for (size_t i = 0; i < want; i++) b[i] = bits[(i + d[0]) % bits.size()]; return b; };
    D::Bits o;
    D::bptc196Decode(slice(196), o);
    D::Bits frag[4] = {slice(32), slice(32), slice(32), slice(32)};
    uint8_t lc[9];
    D::embLcDecode(frag, lc);
    D::Bits piece[4] = {slice(17), slice(17), slice(17), slice(17)};
    uint32_t l28;
    D::shortLcDecode(piece, l28);
    int at, tc, ls; unsigned p17;
    D::cachDecode(slice(24), at, tc, ls, p17);
    float sym[98];
    for (int i = 0; i < 98; i++) sym[i] = ((float)d[i % n] - 128.f) / 24.f;
    uint8_t data[24];
    D::trellis34Decode(sym, data);
    D::rate1Decode(slice(196), data);
    const D::Bits burst = slice(264);
    D::burstSlotType(burst); D::burstEmb(burst);
    D::matchSync(slice(48), (int)(d[1] % 25));
    D::lcBurstDecode(slice(96), d[2] & 1, lc);
    uint8_t ten[10];
    D::crcBlockCheck(slice(96), (uint16_t)(d[2] << 8 | d[3]), ten);
    D::Csbk cs; D::csbkParse(slice(96), cs);
    D::DataHeader h;
    memcpy(ten, d, std::min<size_t>(n, 10));
    D::parseDataHeader(ten, h);
    uint8_t pk[10]; D::packDataHeader(h, pk);
    D::DataBlock db;
    for (int dt = 6; dt < 12; dt++) { uint8_t oct[24] = {}; memcpy(oct, d, std::min<size_t>(n, 24)); D::dataBlockParse(dt, oct, d[1] & 1, db); D::dataBlockEncode(dt, d, n, d[1] & 1, d[2], o); }
    for (int f = -1; f < 27; f++) { D::textFromBytes(d, n, f, false); D::textFromBytes(d, n, f & 3, true); }
    D::TalkerAlias ta;
    D::FullLc full;
    for (size_t i = 0; i + 9 <= n; i += 9) { D::parseFullLc(d + i, full); ta.add(full); }
    ta.complete(); ta.text();
    D::serviceOptionsText(d[0]);
}

#ifdef ONAIR_LIBFUZZER
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* d, size_t n) { one(d, n); return 0; }
#else
int main() {
    const char* it = getenv("ONAIR_FUZZ_ITERS");
    const long iters = it ? atol(it) : 50000;
    std::mt19937 g(777);
    for (long i = 0; i < iters; i++) {
        std::vector<uint8_t> s(4 + g() % 120);
        for (auto& x : s) x = (uint8_t)(g() % 3 ? g() : (g() & 1));
        one(s.data(), s.size());
    }
    printf("fuzz_dmr_proto: %ld inputs, no crash\n", iters);
    return 0;
}
#endif
