// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// SSDV packets: the encoder / decoder of fsphil/ssdv through the C++ face (ssdv_proto.h): a test picture cut into packets, the CRC and Reed-Solomon
// check (up to 16 corrupted bytes are corrected, more are refused), a lost packet, duplicates and packets out of order, a packet without FEC.
#include "dect2/ssdv/ssdv_proto.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
using namespace dect2;
#include "../common/test_util.h"

static double meanAbsDiff(const ssdv::Rgb& a, const ssdv::Rgb& b) {
    if (a.w != b.w || a.h != b.h) return 1e9;
    double s = 0;
    for (size_t i = 0; i < a.rgb.size(); i++) s += std::abs((int)a.rgb[i] - (int)b.rgb[i]);
    return s / (double)a.rgb.size();
}

int main() {
    const std::vector<uint8_t> jpg = ssdv::testJpeg(96, 64);
    ssdv::Rgb orig;
    CHECK(ssdv::decodeJpeg(jpg.data(), jpg.size(), orig) && orig.w == 96 && orig.h == 64, "test JPEG decodes");
    auto pk = ssdv::encodePackets(jpg, "ONAIR", 7, 5, false);
    printf("  %zu packets for %zu JPEG bytes\n", pk.size(), jpg.size());
    CHECK(pk.size() >= 5 && pk.size() < 60, "packet count %zu", pk.size());
    // every packet checks, with the right header
    for (size_t i = 0; i < pk.size(); i++) {
        auto p = pk[i];
        int e = -1; ssdv::Info in;
        const bool ok = ssdv::check(p.data(), &e, &in);
        CHECK(ok && e == 0 && in.packetId == (int)i && in.imageId == 7 && in.call == "ONAIR" && in.width == 96 && in.height == 64 && in.type == 0, "packet %zu: ok %d e %d id %d call %s", i, ok, e, in.packetId, in.call.c_str());
        if (i + 1 == pk.size()) CHECK(in.eoi, "the last packet has the end flag");
    }
    // the whole picture
    ssdv::Assembler as;
    for (auto& p : pk) as.add(p.data());
    CHECK(as.complete() && as.missing() == 0 && as.packets() == pk.size(), "assembler complete");
    const std::vector<uint8_t> full = as.jpeg();
    ssdv::Rgb got;
    CHECK(!full.empty() && ssdv::decodeJpeg(full.data(), full.size(), got), "rebuilt JPEG decodes");
    const double d0 = meanAbsDiff(got, orig);
    printf("  mean difference to the source picture: %.2f levels\n", d0);
    CHECK(d0 < 20.0, "picture differs from the source by %.2f", d0);
    // Reed-Solomon: corrupt k bytes after the sync byte
    std::mt19937 rng(5);
    for (int k : {1, 5, 16}) {
        auto p = pk[2];
        std::vector<int> pos;
        while ((int)pos.size() < k) { int q = 2 + (int)(rng() % 253); if (std::find(pos.begin(), pos.end(), q) == pos.end()) pos.push_back(q); }
        for (int q : pos) p[(size_t)q] ^= (uint8_t)(1 + rng() % 255);
        int e = 0; ssdv::Info in;
        const bool ok = ssdv::check(p.data(), &e, &in);
        CHECK(ok && e == k && std::memcmp(p.data(), pk[2].data(), ssdv::kPktSize) == 0, "%d corrupted bytes: ok %d corrected %d", k, ok, e);
    }
    {   // 17 or more: refused (no false picture data)
        int refused = 0;
        for (int trial = 0; trial < 20; trial++) {
            auto p = pk[1];
            for (int j = 0; j < 40; j++) p[2 + rng() % 253] ^= (uint8_t)(1 + rng() % 255);
            int e; if (!ssdv::check(p.data(), &e, nullptr)) refused++;
        }
        CHECK(refused == 20, "40 corrupted bytes were accepted %d times", 20 - refused);
    }
    {   // a wrong first byte (the sync is replaced), a wrong type byte repaired by the code
        auto p = pk[3]; p[0] = 0x00;
        int e; CHECK(ssdv::check(p.data(), &e, nullptr), "sync byte value does not matter");
    }
    // a lost packet, a duplicate, shuffled order
    {
        ssdv::Assembler a2;
        std::vector<size_t> order;
        for (size_t i = 0; i < pk.size(); i++) if (i != 3) order.push_back(i);
        std::shuffle(order.begin(), order.end(), rng);
        int added = 0, dup = 0;
        for (size_t i : order) { if (a2.add(pk[i].data())) added++; }
        for (size_t i : order) { if (!a2.add(pk[i].data())) dup++; }
        CHECK(added == (int)pk.size() - 1 && dup == (int)order.size(), "adds %d dups %d", added, dup);
        CHECK(!a2.complete() && a2.missing() == 1, "one packet missing: %d", a2.missing());
        const auto j2 = a2.jpeg();
        ssdv::Rgb g2;
        CHECK(!j2.empty() && ssdv::decodeJpeg(j2.data(), j2.size(), g2), "a picture with a gap still decodes");
        const double d = meanAbsDiff(g2, orig);
        CHECK(d >= d0 - 1e-9 && d < 60.0, "gap picture difference %.2f", d);
        a2.add(pk[3].data());
        CHECK(a2.complete(), "complete after the late packet");
        CHECK(a2.jpeg() == full, "the late packet gives the same JPEG as in order");
    }
    // no FEC type
    {
        auto nf = ssdv::encodePackets(jpg, "ONAIR", 8, 5, true);
        CHECK(!nf.empty() && nf.size() <= pk.size(), "no-FEC packets %zu", nf.size());
        auto p = nf[1]; int e = -1; ssdv::Info in;
        CHECK(ssdv::check(p.data(), &e, &in) && in.type == 1 && e == 0, "no-FEC packet checks");
        p[100] ^= 1;
        CHECK(!ssdv::check(p.data(), &e, nullptr), "a damaged no-FEC packet is refused");
    }
    return testSummary("ssdv proto");
}
