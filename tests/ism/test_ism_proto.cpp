// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// ISM sensors, the protocol layer alone: every test device's frame is decoded to the right model, ID and values; cut-off frames,
// frames with a flipped bit and frames with timing errors give the right reading or none, never a wrong one; noise gives nothing.
#include "dect2/ism/ism_gen.h"
#include "dect2/ism/ism_proto.h"
#include "dect2/ism/ism_tel.h"
#include <cmath>
#include <cstdio>
#include <random>
using namespace dect2;
#include "../common/test_util.h"

static int wrongOf(const std::vector<IsmReading>& rs) {
    int w = 0;
    for (const auto& r : rs) if (!ismTruthMatches(r)) { w++; printf("    wrong: %s id %s: %s\n", r.model.c_str(), r.id.c_str(), r.fields.empty() ? "" : r.fields[0].name.c_str()); }
    return w;
}

int main() {
    CHECK(ismProtocols().size() >= 25, "%zu protocols", ismProtocols().size());
    int cutTried = 0, cutWrong = 0, flipTried = 0, flipWrong = 0, flipOk = 0;
    std::mt19937 rng(7);
    for (int p = 0; p < (int)IsmTx::Count; p++) {
        const IsmTx tx = (IsmTx)p;
        for (int k = 0; k < 3; k++) {
            const IsmTrain t = ismEncode(tx, k);
            const auto rs = ismDecode(t);
            const IsmTruth tr = ismTruth(tx, k);
            IsmTx w = IsmTx::Count; int kk = -1;
            const bool ok = rs.size() == 1 && ismTruthMatches(rs[0], &w, &kk) && w == tx;
            CHECK(ok, "%s k=%d: %zu readings%s%s", ismTxName(tx), k, rs.size(), rs.empty() ? "" : ", first ", rs.empty() ? "" : (rs[0].model + " " + rs[0].id).c_str());
            if (k == 0 && !rs.empty()) printf("  %-24s -> %-22s id %-9s %s\n", ismTxName(tx), rs[0].model.c_str(), rs[0].id.c_str(), tr.field);
            // timing errors: 200 ppm either way, and 6 % slow (an old battery)
            for (double sc : {1.0002, 0.9998, 1.06, 0.94}) {
                IsmTrain s = t;
                for (auto& q : s.p) { q.pulseUs *= (float)sc; q.gapUs *= (float)sc; }
                const auto r2 = ismDecode(s);
                CHECK(wrongOf(r2) == 0, "%s scaled %.4f: wrong reading", ismTxName(tx), sc);
                if (std::fabs(sc - 1) < 0.001) CHECK(!r2.empty(), "%s scaled %.4f: not decoded", ismTxName(tx), sc);
            }
            // cut off after every pulse: right or nothing
            for (size_t n = 1; n < t.p.size(); n++) {
                IsmTrain c = t;
                c.p.resize(n);
                c.p.back().gapUs = t.fsk ? 60.f : 50000.f;
                const auto r3 = ismDecode(c);
                cutTried++;
                cutWrong += wrongOf(r3);
            }
            // every pulse and every gap lengthened or shortened in turn (a bit flipped by noise): the same set on every platform
            for (size_t i = 0; i < t.p.size(); i++)
                for (int gap = 0; gap < 2; gap++)
                    for (float sc : {2.0f, 0.5f}) {
                        IsmTrain c = t;
                        (gap ? c.p[i].gapUs : c.p[i].pulseUs) *= sc;
                        const auto r4 = ismDecode(c);
                        flipTried++;
                        const int wr = wrongOf(r4);
                        flipWrong += wr;
                        flipOk += !r4.empty() && !wr;
                        if (wr) printf("    flip: %s k=%d pulse %zu of %zu, %s x%.1f\n", ismTxName(tx), k, i, t.p.size(), gap ? "gap" : "pulse", sc);
                    }
        }
    }
    printf("  cut-off frames: %d tried, %d wrong readings; flipped: %d tried, %d still right, %d wrong\n", cutTried, cutWrong, flipTried, flipOk, flipWrong);
    CHECK(cutWrong == 0, "%d wrong readings from cut-off frames", cutWrong);
    // a few flips cannot be told from a real frame: the 8-bit sums of Efergy and Z-Wave R2 pass on a shifted frame now and then, and a
    // stretched last element of a WH24 gives the longer postamble that rtl_433 (and we) take for a WH65B. 7 of the ~31000 flips now.
    CHECK(flipWrong * 3000 <= flipTried, "%d wrong readings from %d flipped bits", flipWrong, flipTried);
    // noise: random pulse trains give no reading
    int noiseReadings = 0;
    for (int i = 0; i < 3000; i++) {
        IsmTrain t; t.fsk = i & 1;
        const int n = 20 + (int)(rng() % 300);
        const float base = t.fsk ? 50.f : 400.f;
        for (int j = 0; j < n; j++) t.p.push_back({base * (0.5f + (float)(rng() % 400) / 100.f), base * (0.5f + (float)(rng() % 400) / 100.f)});
        noiseReadings += (int)ismDecode(t).size();
    }
    printf("  random trains: %d readings\n", noiseReadings);
    CHECK(noiseReadings <= 2, "%d readings from random pulse trains", noiseReadings);
    // line-code guess
    CHECK(ismClassify(ismEncode(IsmTx::Nexus, 0)).code == "PPM", "nexus guess %s", ismClassify(ismEncode(IsmTx::Nexus, 0)).code.c_str());
    CHECK(ismClassify(ismEncode(IsmTx::Ev1527, 0)).code == "PWM", "ev1527 guess %s", ismClassify(ismEncode(IsmTx::Ev1527, 0)).code.c_str());
    CHECK(ismClassify(ismEncode(IsmTx::Schrader, 0)).code == "Manchester", "schrader guess %s", ismClassify(ismEncode(IsmTx::Schrader, 0)).code.c_str());
    CHECK(ismClassify(ismEncode(IsmTx::Bresser6in1, 0)).code == "NRZ", "bresser guess %s", ismClassify(ismEncode(IsmTx::Bresser6in1, 0)).code.c_str());
    // Z-Wave: checksum / CRC errors are rejected, command classes are named, S0 / S2 stay encrypted
    for (int rate = 1; rate <= 3; rate++) {
        const std::vector<uint8_t> pl = {0x25, 0x03, 0xFF};
        auto good = ismDecode(ismEncodeZwave(rate, 0xAABBCCDD, 3, 1, pl));
        CHECK(good.size() == 1 && good[0].id == "AABBCCDD:3", "zwave R%d good frame: %zu readings", rate, good.size());
        CHECK(ismDecode(ismEncodeZwave(rate, 0xAABBCCDD, 3, 1, pl, 0, true)).empty(), "zwave R%d bad check accepted", rate);
        const auto s0 = ismDecode(ismEncodeZwave(rate, 0xAABBCCDD, 3, 1, {0x98, 0x81, 1, 2, 3, 4, 5, 6, 7, 8, 9}));
        bool enc = false;
        for (const auto& r : s0) for (const auto& f : r.fields) enc |= f.name == "Command" && f.text == "encrypted";
        CHECK(s0.size() == 1 && enc, "zwave R%d S0 not shown as encrypted", rate);
        const auto s2 = ismDecode(ismEncodeZwave(rate, 0xAABBCCDD, 3, 1, {0x9F, 0x03, 1, 2, 3, 4}));
        enc = false;
        for (const auto& r : s2) for (const auto& f : r.fields) enc |= f.name == "Command" && f.text == "encrypted";
        CHECK(s2.size() == 1 && enc, "zwave R%d S2 not shown as encrypted", rate);
        const auto bat = ismDecode(ismEncodeZwave(rate, 0xAABBCCDD, 3, 1, {0x80, 0x03, 77}));
        CHECK(bat.size() == 1 && ismReadingText(bat[0].fields).find("Battery level 77 %") != std::string::npos, "zwave R%d battery: %s", rate, bat.empty() ? "none" : ismReadingText(bat[0].fields).c_str());
        const auto sw = ismDecode(ismEncodeZwave(rate, 0xAABBCCDD, 3, 1, {0x25, 0x03, 0xFF}));
        CHECK(sw.size() == 1 && ismReadingText(sw[0].fields).find("Switch Binary") != std::string::npos, "zwave R%d class name", rate);
    }
    // NaN input
    IsmTrain bad; bad.p.push_back({NAN, 10}); bad.p.push_back({10, 10}); bad.p.push_back({10, 10});
    CHECK(ismDecode(bad).empty(), "NaN train");
    printf(fails ? "ism proto: %d FAILED\n" : "ism proto: all passed\n", fails);
    return fails ? 1 : 0;
}
