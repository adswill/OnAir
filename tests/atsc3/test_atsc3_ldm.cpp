// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// ATSC 3.0, the Core layer of an LDM signal through the convolutional time interleaver. A Core PLP (QPSK 9/15, 64K LDPC) and an Enhanced PLP
// (256QAM 7/15, 64K LDPC) at 5 dB injection level are sent on the same cells as beta (core + alpha enh) (A/322 6.4), the Core PLP through the
// transmitter's CTI (512 rows), then white noise. The receiver side is what decodeFrame does for the Core PLP: each cell with its thermal noise
// (what the pilots measure) through ldmCoreCell, then Atsc3Receiver::commitFrame, which de-interleaves (runCti) and decodes. Also: the injection
// level table, which PLP an Enhanced PLP overlaps, and a non-LDM QPSK 9/15 CTI PLP near its threshold (the erasure of cells far outside the
// constellation must leave the good cells of a noisy signal alone).
// Pass: every block after the de-interleaver's start-up decodes.
// Simulated signals only.
#include "dect2/atsc3/atsc3_bb.h"
#include "dect2/atsc3/atsc3_bicm.h"
#include "dect2/atsc3/atsc3_frame.h"
#include "dect2/atsc3/atsc3_receiver.h"
#include "dect2/atsc3/atsc3_ti.h"
#include <cmath>
#include <cstdio>
#include <random>

using namespace dect2;
using namespace dect2::atsc3;

static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); fails++; } } while (0)

struct Result { long good = 0, bad = 0, blocks = 0, startup = 0; };

// injCode < 0: no Enhanced layer
static Result run(double snrDb, int injCode, int frames, uint32_t seed) {
    FramePlp cc; cc.id = 0; cc.fecType = 1; cc.mod = 0; cc.cod = 7;   // QPSK 9/15, BCH + 64K
    FramePlp ec; ec.id = 1; ec.fecType = 1; ec.mod = 3; ec.cod = 5;   // 256QAM 7/15, BCH + 64K
    const Bicm cb(plpBicm(cc)), eb(plpBicm(ec));
    if (!cb.ok() || !eb.ok()) { printf("bicm config not supported\n"); fails++; return {}; }
    L1DetailSubframe sf;
    L1DetailPlp core, enh;
    const int perFrame = 2, F = perFrame * cb.cells(), rows = ctiRows(0, false);
    core.id = 0; core.layer = 0; core.start = 0; core.size = F;
    enh.id = 1; enh.layer = 1; enh.start = 0; enh.size = F; enh.ldmInjectionLevel = injCode;
    sf.plps.push_back(core);
    if (injCode >= 0) sf.plps.push_back(enh);
    const LdmCore ldm = ldmCoreOf(sf, core);
    const float alpha = (float)std::sqrt(ldm.alpha2), beta = (float)ldm.beta;
    CtiInterleaver ti(rows, 2, cb.constellation(), 0);
    Atsc3Receiver rx;
    std::mt19937 rng(seed);
    std::normal_distribution<float> g(0.f, 1.f);
    const double sigma2 = std::pow(10.0, -snrDb / 10.0);
    for (int f = 0; f < frames; f++) {
        std::vector<cf32> tx;
        for (int b = 0; b < perFrame; b++) {
            auto bits = bytesToBits(makeBbPacket(cb.kPayload() / 8, {}, -1, -1));
            bbScramble(bits);
            auto c = cb.encode(bits);
            tx.insert(tx.end(), c.begin(), c.end());
        }
        if (ldm.on) {
            std::vector<cf32> e;
            while ((int)e.size() < F) {
                auto bits = bytesToBits(makeBbPacket(eb.kPayload() / 8, {}, -1, -1));
                bbScramble(bits);
                auto c = eb.encode(bits);
                e.insert(e.end(), c.begin(), c.end());
            }
            for (int i = 0; i < F; i++) tx[(size_t)i] = (tx[(size_t)i] + e[(size_t)i] * alpha) * beta;
        }
        const int startRow = (int)(((long)f * F) % rows);
        const std::vector<cf32> il = ti.process(tx);
        PlpResult pr;
        pr.id = 0; pr.cti = true; pr.cfg = cc; pr.ctiRows = rows; pr.ctiStartRow = startRow;
        pr.ctiFecBlockStart = rows * startRow;   // C = 0: every frame starts with a whole FEC block
        pr.cells.resize(il.size());
        double nsum = 0;
        for (size_t i = 0; i < il.size(); i++) {
            cf32 y = il[i] + cf32(g(rng), g(rng)) * (float)std::sqrt(sigma2 / 2);
            double n = sigma2;   // the thermal noise, all the pilots see
            ldmCoreCell(ldm, y, n, nullptr);
            pr.cells[i] = y;
            nsum += n;
        }
        pr.noise = (float)(nsum / il.size());
        FrameResult fr;
        fr.ok = true;
        fr.plps.push_back(pr);
        rx.commitFrame(fr, Bootstrap());
    }
    Result r;
    r.blocks = (long)frames * perFrame;
    r.bad = rx.stats().bbBad;
    r.good = rx.stats().bbPackets;   // the blocks that decoded
    r.startup = (long)rows * (rows - 1) / cb.cells() + 1;   // blocks the de-interleaver's start-up holds back (its delay is rows (rows - 1) cells)
    return r;
}

int main() {
    // L1D_plp_ldm_injection_level (A/322 Table 9.24)
    CHECK(ldmInjectionDb(0) == 0.0 && ldmInjectionDb(10) == 5.0 && ldmInjectionDb(11) == 6.0 && ldmInjectionDb(30) == 25.0, "injection level table");
    CHECK(ldmInjectionDb(31) < 0, "reserved injection level");
    {   // the Enhanced PLP applies to the Core PLP whose cells it shares, not to a PLP next to it
        L1DetailSubframe sf;
        L1DetailPlp a, e, b;
        a.layer = 0; a.start = 0; a.size = 1000;
        e.layer = 1; e.start = 0; e.size = 1000; e.ldmInjectionLevel = 10;
        b.layer = 0; b.start = 1000; b.size = 500;
        sf.plps = {a, e, b};
        const LdmCore la = ldmCoreOf(sf, a), lb = ldmCoreOf(sf, b), le = ldmCoreOf(sf, e);
        CHECK(la.on && std::fabs(la.alpha2 - 0.3162) < 1e-3 && std::fabs(la.beta - 0.8716) < 1e-3, "Core PLP under the Enhanced PLP: alpha^2 and beta of 5 dB");
        CHECK(!lb.on && !le.on, "no LDM for the PLP beside it or for the Enhanced PLP itself");
        cf32 c(0.8716f, 0.f);
        double n = 0.01;
        float w = 0.5f;   // a cell in a fade: its noise was 0.02
        ldmCoreCell(la, c, n, &w);
        CHECK(std::fabs(c.real() - 1.f) < 1e-3 && std::fabs(n - (0.01 / 0.7597 + 0.3162)) < 1e-3, "cell back to the Core constellation, the Enhanced layer in the noise");
        CHECK(std::fabs(w - n / (0.02 / 0.7597 + 0.3162)) < 1e-3, "weight: the ratio of the new noise to the cell's own");
    }
    struct Case { const char* name; double snr; int inj; };
    const Case cases[] = {
        {"LDM Core, 5 dB injection, clean", 30, 10},
        {"LDM Core, 5 dB injection, 10 dB SNR", 10, 10},
        {"QPSK 9/15 CTI without LDM, 3.5 dB SNR", 3.5, -1},
    };
    const int frames = 20;   // 40 blocks, the first 9 go into the de-interleaver's start-up
    for (const Case& c : cases) {
        const Result r = run(c.snr, c.inj, frames, 11);
        const long want = r.blocks - r.startup;
        printf("  %-42s blocks decoded %ld of %ld after the start-up (%ld bad)\n", c.name, r.good, want, r.bad);
        fflush(stdout);
        CHECK(r.good >= want, c.name);
    }
    printf("atsc3 ldm: %s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
