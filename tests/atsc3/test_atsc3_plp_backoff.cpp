// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The PLP backoff of Atsc3Receiver (decodePlan): frames with a robust PLP (QPSK 7/15) that always decodes and a fragile one (256QAM 13/15) that
// decodes only while the signal is strong. A word that cannot decode costs the LDPC decoder all its iterations, so a PLP far below its threshold
// costs the receiver the time of all its blocks in every frame. After kDeadFrames frames in which the fragile PLP decoded nothing (and kPlanLag
// frames for the plan to reach the decoder) only kProbeBlocks of its blocks are tried per frame, counted as demapper runs ("a3.demap" of the
// stage profiler, one per word the decoder takes). Short fades (3 frames) do not back it off; when the signal comes back the first frame decodes
// in full; the robust PLP is never touched; the receiver logs the backoff once.
#include "dect2/atsc3/atsc3_bb.h"
#include "dect2/atsc3/atsc3_frame.h"
#include "dect2/atsc3/atsc3_receiver.h"
#include "dect2/common/diag.h"
#include "dect2/common/stable_random.h"
#include "dect2/common/stage_prof.h"
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <random>
#include <string>

using namespace dect2;
using namespace dect2::atsc3;

static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); fails++; } } while (0)

static std::atomic<int> backoffLines{0};
static void sink(diag::Level, const std::string& line) {
    if (line.find("[E406502]") != std::string::npos && line.find("PLP 2 ") != std::string::npos) backoffLines++;
}

int main() {
    std::mt19937 rng(17);
    dect2::normal_distribution<float> g(0.f, 1.f);
    FrameSetup fs;
    fs.bs.preambleStructure = 0; fs.bs.bsrCoefficient = 8; fs.bs.numSymbols = 4;
    fs.fftCode = 0; fs.guardCode = 1; fs.spPattern = 4; fs.numSymbols = 40; fs.preambleSymbols = 1; fs.sbsNullCells = 16; fs.l1DetailMode = 3;
    const int nRobust = 4, nFragile = 24;
    std::vector<FramePlp> plps(2);
    plps[0].id = 1; plps[0].fecType = 0; plps[0].mod = 0; plps[0].cod = 5;    // BCH + 16K, QPSK 7/15
    plps[1].id = 2; plps[1].fecType = 0; plps[1].mod = 3; plps[1].cod = 11;   // BCH + 16K, 256QAM 13/15
    for (int k = 0; k < 2; k++) {
        Bicm b(plpBicm(plps[k]));
        CHECK(b.ok(), "PLP configuration");
        for (int i = 0; i < (k == 0 ? nRobust : nFragile); i++) {
            std::vector<uint8_t> alp(b.kPayload() / 8);
            for (auto& v : alp) v = rng() & 255;
            plps[k].bbPackets.push_back(makeBbPacket(b.kPayload() / 8, alp, 0, i));
        }
    }
    auto tx = buildFrame(fs, plps);
    CHECK(!tx.empty(), "frame built");
    if (tx.empty()) return 1;
    double sig = 0;
    for (auto& v : tx) sig += std::norm(v);
    sig /= tx.size();

    // the fragile PLP: two short fades (3 frames), then a long one (16 frames), then the signal back
    std::string pattern = "DDDGDDDG" + std::string(16, 'D') + "GGGGGG";
    const int lag = Atsc3Receiver::kPlanLag, dead = Atsc3Receiver::kDeadFrames, probe = Atsc3Receiver::kProbeBlocks;
    const int firstProbed = 8 + dead + lag;   // the long fade starts at frame 8
    Atsc3Receiver rx;
    diag::setTestSink(sink);
    prof::stageProfEnable(true);
    for (int f = 0; f < (int)pattern.size(); f++) {
        const bool good = pattern[(size_t)f] == 'G';
        std::vector<cf32> r(tx.size() + 200, cf32(0, 0));
        for (size_t s = 0; s < tx.size(); s++) r[s] += tx[s];
        for (size_t s = 0; s + 25 < tx.size(); s++) r[s + 25] += tx[s] * cf32(0.2f, -0.1f);
        const float sigma = (float)std::sqrt(sig / std::pow(10.0, (good ? 32.0 : 14.0) / 10.0) / 2.0);
        for (auto& v : r) v += cf32{g(rng), g(rng)} * sigma;
        prof::stageProfReset();
        const bool ok = rx.pushFrame(r.data(), r.size(), fs.bs);
        uint64_t demaps = 0;
        for (auto& s : prof::stageProfSnapshot()) if (s.first == "a3.demap") demaps += s.second.second;
        FrameInfo fi = rx.frameInfo();
        int r1 = -1, r2 = -1, b2 = -1;
        for (auto& p : fi.plps) { if (p.id == 1) r1 = p.blocksOk; if (p.id == 2) { r2 = p.blocksOk; b2 = p.blocks; } }
        printf("  frame %2d  %s  robust %d/%d  fragile %d/%d  demapper runs %llu\n", f, good ? "strong" : "weak  ", r1, nRobust, r2, b2, (unsigned long long)demaps);
        std::array<char, 160> m;
        snprintf(m.data(), m.size(), "frame %d decoded, robust PLP in full", f);
        CHECK(ok && r1 == nRobust && b2 == nFragile, m.data());
        if (good) {
            snprintf(m.data(), m.size(), "frame %d: strong signal, fragile PLP decoded in full (%d of %d)", f, r2, nFragile);
            CHECK(r2 == nFragile, m.data());
        } else {
            snprintf(m.data(), m.size(), "frame %d: weak signal, fragile PLP decodes nothing (%d)", f, r2);
            CHECK(r2 == 0, m.data());
        }
        // backed off: the robust PLP's words (twice at most, a second pass on a failure) and the probe words of the fragile one
        if (!good && f >= firstProbed) {
            snprintf(m.data(), m.size(), "frame %d: fragile PLP backed off, at most %d demapper runs (%llu)", f, 2 * (nRobust + probe), (unsigned long long)demaps);
            CHECK(demaps <= (uint64_t)(2 * (nRobust + probe)), m.data());
        }
        // not yet backed off (and never after a short fade): every word of the fragile PLP is tried
        if (!good && f < firstProbed) {
            snprintf(m.data(), m.size(), "frame %d: fragile PLP not backed off yet, all its words tried (%llu demapper runs)", f, (unsigned long long)demaps);
            CHECK(demaps >= (uint64_t)(nRobust + nFragile), m.data());
        }
    }
    prof::stageProfEnable(false);
    diag::setTestSink(nullptr);
    CHECK(backoffLines.load() == 1, "one log line when the fragile PLP is backed off");
    printf("atsc3 plp backoff: %s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
