// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Datawell HXV real-time format (datawell_hxv.h): the parity of the three example lines in the Datawell RX-C4 manual (appendix B), error correction,
// and the block layer fed with a bit stream built from the documented layout (datawell_gen.h): system words, spectrum points, WR-SG vectors, a bit
// slip, polarity.
#include "dect2/datawell/datawell_gen.h"
#include "dect2/datawell/datawell_hxv.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>
using namespace dect2;
using namespace dect2::datawell;
#include "../common/test_util.h"

static uint64_t line(unsigned a, unsigned b, unsigned c, unsigned d) { return ((uint64_t)a << 48) | ((uint64_t)b << 32) | ((uint64_t)c << 16) | d; }

int main() {
    {   // the RX-C4 manual's examples: SSNN,VVVV,VVVV,VVVV,VVVV - the four words are the vector, and they pass the parity code as published
        const uint64_t ex[3] = {line(0x49F4, 0x0060, 0x0381, 0x0AB7), line(0x766C, 0x8090, 0x0980, 0xA134), line(0x7FFF, 0x80E0, 0x0300, 0x1689)};
        for (int i = 0; i < 3; i++) {
            uint64_t v = ex[i];
            CHECK(hxvCorrect(v) == 0 && v == ex[i], "example %d fails the parity code", i);
            const HxvVector d = hxvDecodeVector(ex[i]);
            const uint64_t e = hxvEncode(d.cyc, d.h, d.n, d.w);
            // bit 27 (the least significant heave bit) is not covered by the code but is part of the vector: it is carried through
            CHECK(e == ex[i], "example %d: encode %016llX, manual %016llX", i, (unsigned long long)e, (unsigned long long)ex[i]);
            if (i == 2) CHECK(d.cyc == 0x7FFF && d.h == -14 && d.n == 3 && d.w == 1, "example 2 decoded %04X h %d n %d w %d", d.cyc, d.h, d.n, d.w);
            if (i == 0) CHECK(d.h == 6 && d.n == 3 && d.w == -16, "example 0 decoded h %d n %d w %d", d.h, d.n, d.w);
        }
    }
    std::mt19937 rng(5);
    {   // up to two bit errors are repaired, three are (nearly always) not mistaken for a good vector
        int fixed1 = 0, fixed2 = 0, wrongFix = 0, gaveUp = 0, trials = 400;
        for (int i = 0; i < trials; i++) {
            const uint64_t v = hxvEncode((uint16_t)rng(), (int)(rng() % 200) - 100, (int)(rng() % 200) - 100, (int)(rng() % 200) - 100);
            for (int ne = 1; ne <= 3; ne++) {
                uint64_t bad = v;
                int pos[3] = {-1, -1, -1};
                for (int k = 0; k < ne; k++) {
                    int p;
                    do { p = (int)(rng() % 64); } while (p == 36 /* idx 27 */ || p == pos[0] || p == pos[1]);
                    pos[k] = p; bad ^= 1ull << p;
                }
                const int r = hxvCorrect(bad);
                if (ne <= 2) { if (r == ne && bad == v) (ne == 1 ? fixed1 : fixed2)++; else wrongFix++; }
                else if (r < 0) gaveUp++;
            }
        }
        CHECK(fixed1 == trials && fixed2 == trials && wrongFix == 0, "repair: 1 error %d, 2 errors %d of %d, wrong %d", fixed1, fixed2, trials, wrongFix);
        printf("  3 errors: %d of %d not repaired (the code repairs 2; a 3 error vector sometimes looks like a 1 or 2 error one)\n", gaveUp, trials);
        CHECK(gaveUp > trials / 4, "3 errors: only %d of %d refused", gaveUp, trials);
    }
    {   // the block layer on a stream of vectors: a start in the middle of a block, a bit slip, an inverted stream, noise first
        // the bit stream from the documented words (the same ones the test signal carries)
        std::vector<int> bits;
        for (int i = 0; i < 20; i++) bits.push_back((int)(rng() & 1));   // noise before the first vector
        const int nVec = 18 * 17;                                          // 17 blocks: every block number and one more
        for (int vi = 0; vi < nVec; vi++) {
            const int pos = vi % 18, blk = (vi / 18) % 16;
            uint16_t cyc = pos == 0 ? 0x7FFF : pos == 1 ? (uint16_t)((blk << 12) | dwTestSysWord(blk)) : dwTestSpecWord(4 * blk + (pos - 2) / 4, (pos - 2) % 4);
            int h, n, w;
            dwTestDisp(vi / 1.28, h, n, w);
            const uint64_t v = hxvEncode(cyc, h, n, w);
            for (int i = 63; i >= 0; i--) bits.push_back((int)((v >> i) & 1));
        }
        for (int pass = 0; pass < 2; pass++) {
            HxvDecoder d;
            std::vector<int> b = bits;
            if (pass == 1) {   // inverted, three bit errors scattered, one bit dropped in the middle
                for (auto& x : b) x ^= 1;
                for (int k = 0; k < 3; k++) b[300 + 700 * k + (int)(rng() % 50)] ^= 1;
                b.erase(b.begin() + 5000);
            }
            for (int x : b) d.pushBit(x);
            printf("  pass %d: locked %d inverted %d, %llu vectors (%llu clean, %llu repaired, %llu lost), %llu blocks\n", pass, d.locked(), d.inverted(), (unsigned long long)d.vectors(),
                   (unsigned long long)d.clean(), (unsigned long long)d.repaired(), (unsigned long long)d.lost(), (unsigned long long)d.blocks());
            CHECK(d.locked() && d.inverted() == (pass == 1), "pass %d: lock %d inverted %d", pass, d.locked(), d.inverted());
            CHECK(d.vectors() >= (uint64_t)(nVec - 6) && d.lost() <= 3, "pass %d: %llu vectors, %llu lost", pass, (unsigned long long)d.vectors(), (unsigned long long)d.lost());
            CHECK(d.blocks() >= 15, "pass %d: %llu blocks", pass, (unsigned long long)d.blocks());
            const HxvSystem& s = d.system();
            for (int i = 0; i < 16; i++) CHECK(s.have[i] && s.word[i] == dwTestSysWord(i), "pass %d: system word %d %s %03X (want %03X)", pass, i, s.have[i] ? "is" : "missing", s.word[i], dwTestSysWord(i));
            CHECK(std::fabs(s.hrms - 0.85) < 1e-9 && std::fabs(s.hs - 3.4) < 1e-9, "Hrms %.3f Hs %.3f", s.hrms, s.hs);
            CHECK(std::fabs(s.tz - 400.0 / 56) < 1e-9, "Tz %.3f", s.tz);
            CHECK(std::fabs(s.psdMax - 5000 * std::exp(-5.0)) < 1e-9, "PSDmax %.4f", s.psdMax);
            CHECK(std::fabs(s.tr - 25) < 1e-9 && std::fabs(s.tw - 18.5) < 1e-9, "Tr %.2f Tw %.2f", s.tr, s.tw);
            CHECK(s.tolWeeks == 26 && s.battery == 1 && s.tn == 2 && s.gpsFix && s.gpsModuleOk, "tol %d batt %d Tn %d fix %d ok %d", s.tolWeeks, s.battery, s.tn, s.gpsFix, s.gpsModuleOk);
            CHECK(std::fabs(s.av0 - 0.2) < 1e-9 && std::fabs(s.ay0 + 0.1) < 1e-9, "accelerometer offsets %.3f %.3f", s.av0, s.ay0);
            CHECK(s.havePos && std::fabs(s.lat - 54.5) < 1e-4 && std::fabs(s.lon + 5.2) < 1e-4, "position %.5f %.5f (have %d)", s.lat, s.lon, s.havePos);
            CHECK(std::fabs(s.orientDeg - 90) < 1e-9 && std::fabs(s.inclDeg - (90.0 / 128) * (225 - 128 + 6.0 / 16)) < 1e-9, "orientation %.2f inclination %.3f", s.orientDeg, s.inclDeg);
            int got = 0;
            for (int n = 0; n < 64; n++) {
                const HxvSpecPoint& p = d.spectrum()[n];
                if (!p.valid) continue;
                got++;
                const DwTestSpecRaw r = dwTestSpecRaw(n);
                const double f = n < 16 ? 0.025 + 0.005 * n : 0.11 + 0.01 * (n - 16);
                CHECK(std::fabs(p.hz - f) < 1e-9 && std::fabs(p.dirDeg - r.dir * 360.0 / 256) < 1e-9, "n %d: %.3f Hz dir %.2f", n, p.hz, p.dirDeg);
                CHECK(std::fabs(p.spreadDeg - 0.4476 * (r.s + r.slsb / 4.0)) < 1e-9, "n %d spread %.3f", n, p.spreadDeg);
                CHECK(std::fabs(p.m2 - (r.m2 + r.m2lsb / 4.0 - 128) / 128) < 1e-9 && std::fabs(p.n2 - (r.n2 + r.n2lsb / 4.0 - 128) / 128) < 1e-9, "n %d m2 %.3f n2 %.3f", n, p.m2, p.n2);
                CHECK(std::fabs(p.rpsd - std::exp(-(double)r.rpsd / 200)) < 1e-9 && std::fabs(p.k - r.k / 100.0) < 1e-9, "n %d rpsd %.4f K %.2f", n, p.rpsd, p.k);
                CHECK(std::fabs(p.psd - p.rpsd * s.psdMax) < 1e-9, "n %d psd", n);
            }
            CHECK(got >= 60, "pass %d: %d of 64 spectrum points", pass, got);
            // the displacements: the last vectors are the test waves
            const auto& rec = d.recent();
            CHECK(rec.size() == 128, "recent %zu", rec.size());
            for (const auto& v : rec) CHECK(std::abs(v.h) <= 100 && std::abs(v.n) <= 40 && std::abs(v.w) <= 30, "displacement outside the test waves: %d %d %d", v.h, v.n, v.w);
        }
    }
    {   // WR-SG vectors (two vertical samples and the 0xAAA dummy) are told from the DWR ones
        const uint64_t v = hxvEncode(0x1234, 10, -20, -682);
        const HxvVector d = hxvDecodeVector(v);
        CHECK(d.wrsg && d.h == 10 && d.n == -20, "wrsg %d h %d n %d w %d", d.wrsg, d.h, d.n, d.w);
        CHECK(!hxvDecodeVector(hxvEncode(0x1234, 10, -20, 5)).wrsg, "a DWR vector taken for a WR-SG one");
    }
    printf(fails ? "datawell hxv: %d FAILED\n" : "datawell hxv: all passed\n", fails);
    return fails ? 1 : 0;
}
