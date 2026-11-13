// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Seeded mutation run of the DAB data layers, meant for DECT2_SANITIZE=ON: every FIG / MOT / PAD / Journaline / packet
// parser must never crash or hang on any bytes at any length (the input is copied to an exactly sized heap block). Also the MotDecoder and DabPadDecoder classes
// are fed the same bytes. Also a libFuzzer target with -DONAIR_LIBFUZZER.
#include "dect2/dab/dab_proto.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>
namespace D = dect2::dabp;

static void one(const uint8_t* d, size_t n) {
    uint8_t* p = n ? (uint8_t*)malloc(n) : nullptr;
    if (n) memcpy(p, d, n);
    int used, ev, lc = 0, lt = 0, len;
    D::fibWalk(p, n, used);
    if (n >= 1) {
        for (int t = 0; t < 4; t++) D::figHead(t, p, n);
        D::fig0Eid(p, n, ev); D::fig0Subchannels(p, n); D::fig0Packet(p, n); D::fig0Services(p, n); D::fig0UserApps(p, n); D::fig0Pty(p, n); D::fig0AnnSupport(p, n); D::fig0AnnSwitch(p, n);
        D::Fig010Time ft; D::fig0Time(p, n, ft); D::Fig1Label l1; D::fig1(p, n, l1); D::Fig2Seg l2; D::fig2(p, n, l2);
    }
    D::MotTime t; D::motParseTime(p, n, t); D::motTimeText(t);
    D::MotHeader h; D::motParseHeader(p, n, h);
    D::motParseGroup(p, n);
    std::vector<D::DirEntry> de; D::motParseDirectory(p, n, de);
    D::PadBytes pb; D::padFromAu(p, n, pb); D::padFromMp2(p, n, lc, pb);
    uint8_t fp[2] = {n ? p[0] : (uint8_t)0, n > 1 ? p[1] : (uint8_t)0};
    D::padSplit(p, n, fp, lt); D::padDgLength(p, n, len); D::dlsStep(p, n); D::dlSegment(p, n);
    bool run; std::vector<D::DlpTag> tags; D::dlPlusTags(p, n, run, tags);
    D::jlDataGroup(p, n);
    D::JlTocInfo ti; std::vector<D::JlTocEntry> te; D::jlParseToc(p, n, ti, te);
    int a, b, c; D::jlUserAppData(p, n, a, b, c);
    if (n >= 5) D::dabPacket(p, n);
    if (n >= 4) D::drmPacket(p, n);
    D::motContentTypeName(n ? p[0] : 0, n > 1 ? p[1] : 0);
    free(p);
}

#ifdef ONAIR_LIBFUZZER
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* d, size_t n) { one(d, n); return 0; }
#else
int main() {
    const long iters = getenv("ONAIR_FUZZ_ITERS") ? atol(getenv("ONAIR_FUZZ_ITERS")) : 100000;
    std::mt19937 g(8642);
    std::vector<uint8_t> seed = {0x00, 0xC1, 0x01, 0x03, 0x00, 0x00, 0xC0, 0x01, 0x20, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x00, 0x00, 0x00, 0x00, 0x30, 0x00, 0x04, 0x40, 0x80, 0x01};
    for (long it = 0; it < iters; it++) {
        std::vector<uint8_t> b = seed;
        const int k = (int)(g() % 6);
        for (int i = 0; i < k; i++) {
            switch (g() % 5) {
            case 0: b[g() % b.size()] = (uint8_t)g(); break;
            case 1: b.resize(g() % 80); break;
            case 2: b.insert(b.begin() + g() % (b.size() + 1), (uint8_t)g()); break;
            case 3: if (b.size() > 1) b.erase(b.begin() + g() % b.size()); break;
            default: for (auto& x : b) if (g() % 8 == 0) x = (uint8_t)g();
            }
            if (b.empty()) b.push_back(0);
        }
        one(b.data(), b.size());
        if (g() % 3 == 0) { std::vector<uint8_t> r(g() % 300); for (auto& x : r) x = (uint8_t)g(); one(r.data(), r.size()); }
    }
    printf("fuzz_dab_proto: %ld iterations ok\n", iters);
    return 0;
}
#endif
