// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Chinese GTH radiosondes (HT03G-1U / GTH3 / GTH6), simulated frames only (no recording was available): the frame round trip with every field, the
// decoder on a symbol stream with bit errors in the preamble, inverted polarity and garbage before it, rejection of a damaged CRC, an all-zero frame
// and a CF-06-AH marker, and the whole receiver on the simulated signal.
#include "dect2/sonde/sonde_testkit.h"
#include "dect2/sonde/sonde_bits.h"
#include <ctime>
using namespace dect2;
using namespace dect2::sondetest;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static SondeTruth truth() {
    SondeTruth t;
    t.lat = 28.5801; t.lon = 115.8946; t.altM = 2995.6; t.hSpeed = 11.3; t.headingDeg = 206.9; t.vSpeed = 4.7;
    t.unixTime = (double)(time(nullptr) - 30); t.tempC = -12.34; t.humidity = 45.5; t.pressureHpa = 712.3;
    return t;
}

int main() {
    const SondeTruth t = truth();
    const std::vector<uint8_t> f = gthFrameBytes(t, 2214, 0x26401483u);
    SondeFix o;
    CHECK(f.size() == 109 && gthParseFrame(f.data(), f.size(), o), "the round trip fails");
    CHECK(o.serial == "GTH-26401483" && o.frame == 2214, "serial %s frame %d", o.serial.c_str(), o.frame);
    CHECK(std::fabs(o.lat - t.lat) < 1e-4 && std::fabs(o.lon - t.lon) < 1e-4 && std::fabs(o.altM - t.altM) < 0.11, "position %.5f %.5f %.1f", o.lat, o.lon, o.altM);
    CHECK(std::fabs(o.hSpeed - t.hSpeed) < 0.02 && std::fabs(o.headingDeg - t.headingDeg) < 0.3 && std::fabs(o.vSpeed - t.vSpeed) < 0.02, "velocity %.2f %.1f %.2f", o.hSpeed, o.headingDeg, o.vSpeed);
    CHECK(o.hasTime && std::fabs(o.unixTime - t.unixTime) < 0.11, "time %.2f against %.2f", o.unixTime, t.unixTime);
    CHECK(o.hasTemp && std::fabs(o.tempC - t.tempC) < 0.01 && o.hasHumidity && std::fabs(o.humidity - t.humidity) < 0.01 && o.hasPressure && std::fabs(o.pressureHpa - t.pressureHpa) < 0.01, "PTU %.2f %.2f %.2f", o.tempC, o.humidity, o.pressureHpa);
    {   // a damaged byte, an all-zero frame and the CF-06-AH marker are refused
        auto g = f; g[40] ^= 0x10; SondeFix x;
        CHECK(!gthParseFrame(g.data(), g.size(), x), "a damaged frame was accepted");
        auto z = gthFrameBytes(truth(), 0, 0); SondeFix y;
        CHECK(!gthParseFrame(z.data(), z.size(), y), "a frame with serial 0 and counter 0 was accepted");
        auto c6 = f; c6[10] = 0xAA; c6[11] = 0xAA; SondeFix w;
        CHECK(!gthParseFrame(c6.data(), c6.size(), w), "a CF-06-AH marker was accepted");
    }
    // the decoder on a stream: garbage, two frames, the second inverted and with 2 wrong preamble bits
    for (int inv = 0; inv < 2; inv++) {
        auto d = makeGthDecoder();
        std::vector<uint8_t> s;
        for (int i = 0; i < 300; i++) s.push_back((uint8_t)((i * 7 + i / 3) & 1));
        std::vector<uint8_t> a = gthSymbols(t, 100, 0x26407413u), b = gthSymbols(t, 101, 0x26407413u);
        b[3] ^= 1; b[20] ^= 1;
        for (auto& v : b) v ^= (uint8_t)inv;
        s.insert(s.end(), a.begin(), a.end());
        for (int i = 0; i < 100; i++) s.push_back((uint8_t)(i & 1));
        s.insert(s.end(), b.begin(), b.end());
        std::vector<SondeFix> out;
        d->push(s.data(), s.size(), 0.0, out);
        CHECK(out.size() == 2u, "inv %d: %zu frames", inv, out.size());
        CHECK(!out.empty() && out[0].frame == 100 && out[0].serial == "GTH-26407413", "inv %d: first frame wrong", inv);
    }
    // the whole receiver
    for (double cfo : {0.0, 12000.0}) {
        SynthConfig c; c.mode = 15; c.snrDb = 30; c.modeOpt[0] = 1024; c.modeOpt[1] = 1; c.cfoHz = cfo;
        Result r = run(c, 8e6, 16);
        const SondeInfo* s = find(r.tel, "GTH-26407413");
        CHECK(s != nullptr, "cfo %.0f: nothing decoded", cfo);
        if (!s) continue;
        CHECK(s->framesOk >= 2 && s->framesBad == 0, "cfo %.0f: ok %llu bad %llu", cfo, (unsigned long long)s->framesOk, (unsigned long long)s->framesBad);
        CHECK(s->type == "GTH" && s->kind == SondeGth, "type %s", s->type.c_str());
        CHECK(s->hasPos && std::fabs(s->lat - 25.28) < 1.5, "position");
    }
    printf(fails ? "FAILED %d\n" : "ok\n", fails);
    return fails ? 1 : 0;
}
