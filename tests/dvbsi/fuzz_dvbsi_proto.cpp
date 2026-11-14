// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Seeded mutation run of the DVB service information layers, meant for DECT2_SANITIZE=ON: the character set converters, the
// DVB text field, the teletext decoder and renderer, the subtitle segment parsers, the pixel decoder and both decoder classes must never crash or hang on any
// bytes, at any length (the input is copied to an exactly sized heap block). Also a libFuzzer target with -DONAIR_LIBFUZZER.
#include "dect2/common/text_charset.h"
#include "dect2/tv/dvb_text.h"
#include "dect2/tv/dvbsub.h"
#include "dect2/tv/teletext.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>
namespace D = dect2;

static void one(const uint8_t* d, size_t n) {
    uint8_t* p = n ? (uint8_t*)malloc(n) : nullptr;
    if (n) memcpy(p, d, n);
    D::text::ebuLatin(p, n); D::text::ucs2be(p, n); D::text::iso6937(p, n); D::text::dabCharset(n ? p[0] % 17 : 0, p, n); D::text::clean(std::string((const char*)p, n));
    D::dvbsi::dvbText(p, (int)n, {static_cast<D::dvbsi::TextEncoding>(n % 8)});
    // teletext
    if (n >= 44) { D::TtxUnit u = D::ttxDecodeUnit(p); (void)u; }
    uint32_t off[64]; D::ttxPesUnits(p, n, off, 64);
    D::TeletextDecoder td; td.setPid(1); td.feedPes(p, n);
    D::TtxPage pg; pg.national = n ? p[0] % 9 : 0; pg.rowMask = n > 4 ? (uint32_t)(p[1] | p[2] << 8 | p[3] << 16 | (uint32_t)p[4] << 24) : 0xFFFFFFFFu;
    for (size_t i = 0; i < n && i < 1000; i++) pg.rows[i / 40][i % 40] = p[i];
    D::TtxGrid g; D::ttxRender(pg, g); for (int r = 0; r < 25; r++) D::ttxRowText(g, r);
    if (n >= 3) D::ttxHamming2418(p);
    // subtitles
    D::DvbSubDecoder sd; sd.setStream(1, 1, 2); sd.feedPes(p, n); sd.displayAt(0);
    sd.feedSegments(p, n, 0.5);
    D::DvbSubPesInfo pi; D::dvbSubPesInfo(p, n, pi); D::dvbSubSegments(p, n);
    D::DvbSubPageInfo a; D::dvbSubParsePage(p, (int)n, a);
    D::DvbSubRegionInfo b; D::dvbSubParseRegion(p, (int)n, b);
    int id, w, h; std::vector<D::DvbSubClutEntry> ce; D::dvbSubParseClut(p, (int)n, id, ce);
    D::dvbSubParseDisplay(p, (int)n, w, h);
    D::DvbSubObjectInfo oi; D::dvbSubParseObject(p, (int)n, oi);
    uint8_t pix[20 * 10]; memset(pix, 0, sizeof pix);
    static const int depths[3] = {2, 4, 8};
    D::dvbSubDecodeField(p, n, n ? p[0] % 30 - 5 : 0, n > 1 ? p[1] % 14 - 2 : 0, 2, n & 1, 20, 10, depths[n % 3], pix);
    free(p);
}

#ifdef ONAIR_LIBFUZZER
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* d, size_t n) { one(d, n); return 0; }
#else
int main() {
    const long iters = getenv("ONAIR_FUZZ_ITERS") ? atol(getenv("ONAIR_FUZZ_ITERS")) : 60000;
    std::mt19937 g(2468);
    std::vector<uint8_t> seed = {0, 0, 1, 0xBD, 0, 60, 0x80, 0x80, 5, 0x21, 0, 1, 0, 1, 0x20, 0, 0x0F, 0x10, 0, 1, 0, 8, 10, 0x04, 0, 0, 0, 0, 0, 0x10, 0x0F, 0x80, 0, 1, 0, 0};
    for (long it = 0; it < iters; it++) {
        std::vector<uint8_t> b = seed;
        const int k = (int)(g() % 6);
        for (int i = 0; i < k; i++) {
            switch (g() % 5) {
            case 0: b[g() % b.size()] = (uint8_t)g(); break;
            case 1: b.resize(g() % 200); break;
            case 2: b.insert(b.begin() + g() % (b.size() + 1), (uint8_t)g()); break;
            case 3: if (b.size() > 1) b.erase(b.begin() + g() % b.size()); break;
            default: for (auto& x : b) if (g() % 8 == 0) x = (uint8_t)g();
            }
            if (b.empty()) b.push_back(0);
        }
        one(b.data(), b.size());
        if (g() % 3 == 0) { std::vector<uint8_t> r(g() % 1100); for (auto& x : r) x = (uint8_t)g(); one(r.data(), r.size()); }
    }
    printf("fuzz_dvbsi_proto: %ld iterations ok\n", iters);
    return 0;
}
#endif
