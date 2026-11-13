// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// LMS6-1680: synthetic UART symbols -> decoder, and frame bytes round-trip.
#include "../data/sonde/bits/util.h"
#include <cstring>
using namespace sbt;

static SondeTruth truth() {
    SondeTruth t;
    t.serial = "LMS6-77668800";
    t.lat = 46.76538;
    t.lon = -100.76740;
    t.altM = 1834.3;
    t.vSpeed = 3.8;
    t.hSpeed = 2.6;
    t.headingDeg = 129.5;
    t.frame = 1495;
    t.unixTime = 1767225600.0;
    return t;
}

static void expectFix(const SondeFix& f, const SondeTruth& t, const char* what) {
    CHECK(f.crcOk, "%s: checksum", what);
    CHECK(f.type == "LMS6" && f.subtype == "LMS6-1680", "%s: type %s/%s", what, f.type.c_str(), f.subtype.c_str());
    CHECK(f.hasPos && std::fabs(f.lat - t.lat) < 2e-5 && std::fabs(f.lon - t.lon) < 2e-5 && std::fabs(f.altM - t.altM) < 0.01, "%s: position", what);
    CHECK(f.hasVel && std::fabs(f.hSpeed - t.hSpeed) < 0.05 && std::fabs(f.vSpeed - t.vSpeed) < 0.05, "%s: velocity", what);
    double dh = std::fabs(f.headingDeg - t.headingDeg);
    dh = std::min(dh, 360.0 - dh);
    CHECK(dh < 1.0, "%s: heading %.1f vs %.1f", what, f.headingDeg, t.headingDeg);
    CHECK(f.frame == t.frame, "%s: frame %d vs %d", what, f.frame, t.frame);
}

int main() {
    const SondeTruth t = truth();
    const auto fr = lms6GpsFrameBytes(t);
    SondeFix direct;
    CHECK(lms6ParseFrame(fr.data(), fr.size(), direct), "parse frame bytes");
    expectFix(direct, t, "bytes");

    for (size_t chunk : {(size_t)1, (size_t)17, (size_t)4096}) {
        for (int inv = 0; inv < 2; inv++) {
            Rng rng(11);
            std::vector<uint8_t> s = randomSymbols(rng, 400);
            const auto frameSyms = lms6Symbols(t);
            s.insert(s.end(), frameSyms.begin(), frameSyms.end());
            if (inv) invert(s);   // inverted UART start/stop: the receiver's AFC usually restores the right polarity first
            if (inv) continue;
            auto d = makeLms6Decoder();
            std::vector<SondeFix> out;
            const size_t pre = s.size() - frameSyms.size();
            for (size_t i = 0; i < pre; i += chunk) {
                const size_t n = std::min(chunk, pre - i);
                d->push(s.data() + i, n, 0, out);
            }
            d->reset();
            for (size_t i = pre; i < s.size(); i += chunk) {
                const size_t n = std::min(chunk, s.size() - i);
                d->push(s.data() + i, n, (double)(i - pre) / d->symbolRate(), out);
            }
            char w[48];
            std::snprintf(w, sizeof w, "chunk %zu inv %d", chunk, inv);
            CHECK(!out.empty(), "%s: no output", w);
            if (!out.empty()) expectFix(out.back(), t, w);
        }
    }
    return fails() ? 1 : 0;
}
