// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// NB-IoT physical layer pieces (nbiot_phy.h): the messages, the synchronisation sequences, the resource element maps, OFDM, the channel estimate,
// the NPBCH code and the generator.
#include "dect2/nbiot/nbiot_phy.h"
#include "dect2/nbiot/nbiot_gen.h"
#include <cmath>
#include <cstdio>
#include <random>
using namespace dect2;
using namespace dect2::nbiot;
#include "../common/test_util.h"

int main() {
    // ---- NPSS: cover code of Table 10.2.7.1.1-1, a Zadoff-Chu sequence of constant amplitude
    {
        const int exp[11] = {1, 1, 1, 1, -1, -1, 1, 1, 1, -1, 1};
        for (int l = 3; l <= 13; l++) CHECK(npssCover(l) == exp[l - 3], "cover %d", l);
        cf32 d[11];
        npssSeq(d);
        for (int n = 0; n < 11; n++) CHECK(std::fabs(std::abs(d[n]) - 1) < 1e-5, "|d| %d", n);
        CHECK(std::abs(d[0] - cf32(1, 0)) < 1e-6 && std::abs(d[1] - std::polar(1.f, (float)(-M_PI * 5 * 2 / 11))) < 1e-5, "d(1)");
    }
    // ---- NSSS: amplitude one, 504 identities that differ, the shift theta_f rotates by a quarter turn per n
    {
        cf32 a[132], b[132], c[132];
        nsssSeq(0, 0, a); nsssSeq(1, 0, b); nsssSeq(0, 1, c);
        double xab = 0, xac = 0, ea = 0;
        for (int n = 0; n < 132; n++) { CHECK(std::fabs(std::abs(a[n]) - 1) < 1e-5, "nsss amplitude"); xab += std::abs(a[n] * std::conj(b[n])); ea += 1; xac += std::norm(a[n] * std::conj(c[n]) - cf32(1, 0)); }
        cf32 s(0, 0);
        for (int n = 0; n < 132; n++) s += a[n] * std::conj(b[n]);
        CHECK(std::abs(s) < 0.4 * 132, "identities 0 and 1 correlate %.1f", std::abs(s));
        cf32 t(0, 0);
        for (int n = 0; n < 132; n++) t += a[n] * std::conj(c[n]);
        CHECK(std::abs(t) < 0.3 * 132, "shift 0 and 1 correlate %.1f", std::abs(t));
        // the Hadamard rows: identity 126 is q = 1, u = 3, as identity 0 with a different b_q
        cf32 q0[132], q1[132];
        nsssSeq(5, 0, q0); nsssSeq(5 + 126, 0, q1);
        double same = 0;
        for (int n = 0; n < 128; n++) same += (std::abs(q0[n] - q1[n]) < 1e-5) ? 1 : 0;
        CHECK(same > 40 && same < 90, "q rows differ in about half the positions (%.0f)", same);
        (void)xab; (void)xac; (void)ea;
    }
    // ---- MIB-NB: 34 bits, every operating mode round trips
    {
        for (int mode = 0; mode < 4; mode++) {
            Mib m; m.sfnMsb = 9; m.hsfnLsb = 2; m.sched = 7; m.valueTag = 21; m.ab = true; m.mode = mode; m.crsSeq = 17; m.numCrs = 1; m.raster = 3;
            const Bits b = mibEncode(m);
            CHECK(b.size() == 34, "MIB size %zu", b.size());
            Mib r;
            CHECK(mibParse(b, r) && r.sfnMsb == 9 && r.hsfnLsb == 2 && r.sched == 7 && r.valueTag == 21 && r.ab && r.mode == mode, "MIB fields (mode %d)", mode);
            if (mode == kInbandSame) CHECK(r.crsSeq == 17, "crs seq");
            if (mode == kInbandDiff) CHECK(r.numCrs == 1 && r.raster == 3, "other-PCI fields");
            if (mode == kGuardband) CHECK(r.raster == 3, "guard-band raster");
        }
        CHECK(rasterOffsetKhz(0) == -7.5 && rasterOffsetKhz(3) == 7.5, "raster offsets");
    }
    // ---- SIB1-NB: PLMNs, tracking area, cell identity, band
    {
        Sib1 s;
        cell::Plmn p; p.mcc = 246; p.mnc = 1; p.mncDigits = 2; s.plmns.push_back(p);
        p.mcc = 310; p.mnc = 260; p.mncDigits = 3; p.reserved = true; s.plmns.push_back(p);
        s.tac = 0xABCD; s.cellId = 0x9ABCDEF & 0xFFFFFFF; s.barred = true; s.intraFreqReselAllowed = false; s.qRxLevMinDbm = -128; s.qQualMinDb = -18; s.pMax = 23; s.band = 20; s.hsfnMsb = 200;
        for (size_t pad : {(size_t)0, (size_t)208}) {
            Bits b = sib1Encode(s, pad);
            if (pad) CHECK(b.size() == pad, "padded size %zu", b.size());
            Sib1 r;
            CHECK(sib1Parse(b, r), "SIB1 parses");
            CHECK(r.plmns.size() == 2 && r.plmns[0].mcc == 246 && r.plmns[0].mnc == 1 && r.plmns[0].mncDigits == 2 && r.plmns[1].mcc == 310 && r.plmns[1].mnc == 260 && r.plmns[1].mncDigits == 3 && r.plmns[1].reserved && !r.plmns[0].reserved, "PLMNs");
            CHECK(r.tac == 0xABCD && r.cellId == s.cellId && r.barred && !r.intraFreqReselAllowed && r.qRxLevMinDbm == -128 && r.qQualMinDb == -18 && r.pMax == 23 && r.band == 20 && r.hsfnMsb == 200, "SIB1 fields");
        }
        s.pMax = -99;
        Sib1 r;
        CHECK(sib1Parse(sib1Encode(s), r) && r.pMax == -99 && r.band == 20, "no p-Max");
        // a system information message is not a SIB1
        Bits b = sib1Encode(s); b[1] = 0;
        CHECK(!sib1Parse(b, r), "SI message rejected");
        CHECK(sib1Reps(0) == 4 && sib1Reps(4) == 8 && sib1Reps(11) == 16 && sib1Reps(12) == 0, "repetitions");
        CHECK(sib1Tbs(0) == 208 && sib1Tbs(5) == 328 && sib1Tbs(8) == 440 && sib1Tbs(11) == 680 && sib1Tbs(15) == 0, "transport block sizes");
        CHECK(sib1StartFrame(4, 6) == 32 && sib1StartFrame(4, 7) == 48 && sib1StartFrame(8, 5) == 16 && sib1StartFrame(8, 4) == 0 && sib1StartFrame(16, 5) == 1 && sib1StartFrame(16, 4) == 0, "start frames");
    }
    // ---- resource elements: the NPBCH has 100, a SIB1 subframe depends on the mode
    {
        CHECK(npbchRes(0).size() == 100 && npbchRes(5).size() == 100 && npbchRes(401).size() == 100, "NPBCH REs");
        CHECK(npdschRes(0, 1, false, 0, 0).size() == 160 && npdschRes(0, 2, false, 0, 0).size() == 152, "standalone REs");
        CHECK(npdschRes(0, 1, true, 1, 0).size() == 118 && npdschRes(0, 2, true, 2, 0).size() == 104 && npdschRes(0, 2, true, 4, 0).size() == 100, "in-band REs %zu %zu %zu",
              npdschRes(0, 1, true, 1, 0).size(), npdschRes(0, 2, true, 2, 0).size(), npdschRes(0, 2, true, 4, 0).size());
        for (int pci : {0, 1, 2, 3, 4, 5, 77}) { const auto r = npbchRes(pci); CHECK(r.size() % 2 == 0, "even count"); }
        // NRS: port 0 and port 1 never share a resource element, two per symbol
        for (int l : {5, 6, 12, 13}) { const int a = nrsBase(0, l, 7), b = nrsBase(1, l, 7); CHECK(a >= 0 && b >= 0 && a != b && (a - b + 6) % 6 == 3, "NRS ports l=%d", l); }
        CHECK(nrsBase(0, 4, 7) < 0, "no NRS in symbol 4");
        CHECK(std::fabs(std::abs(nrsValue(7, 0, 5, 0)) - 1) < 1e-5, "NRS unit amplitude");
    }
    // ---- OFDM: the demodulator returns the grid of the modulator
    {
        std::mt19937 rng(3);
        Grid g;
        for (int i = 0; i < kSyms * kSc; i++) g.v[i] = cf32((float)(rng() % 100) / 50 - 1, (float)(rng() % 100) / 50 - 1);
        std::vector<cf32> x(kSubframe + 2 * 16, cf32(0, 0));
        modulateSubframe(g, x.data() + 16);
        Grid r;
        demodSubframe(x.data() + 16, r);
        double err = 0;
        for (int i = 0; i < kSyms * kSc; i++) err += std::norm(r.v[i] - g.v[i]);
        CHECK(err < 1e-6, "modulator / demodulator error %.2g", err);
        // a timing error of two samples costs only a phase ramp inside the cyclic prefix: the magnitudes stay
        Grid r2;
        demodSubframe(x.data() + 16 + 2, r2);
        double mag = 0;
        for (int i = 0; i < kSyms * kSc; i++) mag += std::fabs(std::abs(r2.v[i]) - std::abs(g.v[i]));
        CHECK(mag < 1e-3 * kSyms * kSc, "magnitudes with a 2 sample offset (%.4f)", mag);
    }
    // ---- the generator: NPSS, NSSS and NRS where the specification puts them; the channel estimate reads the port channels
    {
        const GenCfg cfg = nbiotTestCell(1);
        Generator gen(cfg);
        std::vector<cf32> f(2 * kFrame + 32, cf32(0, 0));
        gen.nextFrame(f.data() + 16);   // frame 37 * 2 = 74: even
        gen.nextFrame(f.data() + 16 + kFrame);
        CHECK(gen.frame() == 76, "frame counter %d", gen.frame());
        Grid g5;
        demodSubframe(f.data() + 16 + 5 * kSubframe, g5);
        cf32 d[11]; npssSeq(d);
        double e = 0, m = 0;
        for (int l = 3; l < 14; l++) for (int k = 0; k < 11; k++) { m += std::norm(g5.at(l, k) - 0.3f / std::sqrt(12.f) * (float)npssCover(l) * d[k]); e += std::norm(g5.at(l, k)); }
        CHECK(m < 1e-6 * e + 1e-9, "NPSS subframe");
        Grid g9;
        demodSubframe(f.data() + 16 + 9 * kSubframe, g9);
        cf32 n0[132];
        nsssSeq(cfg.pci, (74 / 2) % 4, n0);
        cf32 s(0, 0); double en = 0;
        for (int n = 0; n < 132; n++) { s += g9.at(3 + n / 12, n % 12) * std::conj(n0[n]); en += std::norm(g9.at(3 + n / 12, n % 12)); }
        CHECK(std::abs(s) * std::abs(s) / (132.0 * en) > 0.99, "NSSS of the even frame matches (%.3f)", std::abs(s) * std::abs(s) / (132.0 * en));
        // the 2-port channel from the NRS in subframe 0
        Grid g0;
        demodSubframe(f.data() + 16, g0);
        Chan c;
        CHECK(estimateChannel(g0, cfg.pci, 0, 2, c), "channel estimate");
        const cf32 h1 = std::polar(cfg.port1Gain, cfg.port1Phase);
        CHECK(std::abs(c.h[0][6] - cf32(0.3f / std::sqrt(12.f), 0)) < 1e-3 && std::abs(c.h[1][6] - h1 * 0.3f / std::sqrt(12.f)) < 1e-3, "port channels %.4f %.4f", std::abs(c.h[0][6]), std::abs(c.h[1][6]));
    }
    // ---- the NPBCH codeword decodes back to the MIB-NB, with the antenna mask
    {
        for (int ports = 1; ports <= 2; ports++) {
            Mib m; m.sfnMsb = 5; m.sched = 3; m.mode = kGuardband; m.raster = 2;
            const Bits e = npbchCode(m, ports, 321);
            CHECK(e.size() == 1600, "NPBCH bits");
            const Bits c = cell::gold(321, 1600);
            std::vector<float> full(1600);
            for (size_t i = 0; i < 1600; i++) full[i] = (e[i] ? -1.f : 1.f) * (c[i] ? -1.f : 1.f);   // the received scrambled bit, descrambled
            const auto d3 = cell::convRateRecover(full.data(), 1600, 50);
            Bits b = cell::viterbiTb(d3.data(), 50);
            CHECK(cell::checkCrc(b, cell::kCrc16, 16, ports == 2 ? 0xFFFFu : 0u), "CRC with the %d port mask", ports);
            CHECK(!cell::checkCrc(b, cell::kCrc16, 16, ports == 2 ? 0u : 0xFFFFu), "not with the other mask");
            b.resize(34);
            Mib r;
            CHECK(mibParse(b, r) && r.sfnMsb == 5 && r.sched == 3 && r.mode == kGuardband && r.raster == 2, "MIB back");
        }
    }
    printf(fails ? "nbiot phy: %d FAILED\n" : "nbiot phy: all passed\n", fails);
    return fails ? 1 : 0;
}
