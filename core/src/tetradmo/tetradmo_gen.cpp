// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// TETRA Direct Mode test signal (see tetradmo_gen.h).
#include "dect2/tetradmo/tetradmo_gen.h"
#include <cmath>
#include <random>

namespace dect2 {

using namespace tetra;
using namespace tetradmo;

TetraDmoGenOut tetraDmoSignal(const TetraDmoGenConfig& c) {
    TetraDmoGenOut out;
    std::mt19937 rng(c.seed);
    Modulator mod;
    std::vector<cf32> base;
    Bits idle(510, 0);
    int frame = 1, tn = 1;
    const auto advance = [&]() { if (++tn > 4) { tn = 1; if (++frame > 18) frame = 1; } };
    std::vector<uint8_t> gate;
    const auto sendSlot = [&](const Bits* burst) {
        Bits slot(510, 0);
        for (int i = 0; i < 510; i++) slot[(size_t)i] = (uint8_t)(i & 1);
        if (burst) std::copy(burst->begin(), burst->end(), slot.begin() + kBurstStartBit);
        size_t before = base.size();
        mod.push(slot, base);
        for (size_t i = before; i < base.size(); i++) {
            const size_t bit = (i - before) * 2 / kSps;
            gate.push_back(burst && bit >= kBurstStartBit && bit < kBurstStartBit + kBurstBits ? 1 : 0);
        }
    };
    const uint32_t z = zeroInit();
    const uint32_t dcc = dccInit(c.mni, c.src);
    const auto dsbSlot = [&](Dm type, int fc) {
        SyncS s; s.commType = c.commType; s.slot = tn; s.frame = frame; s.aiEncState = c.aiEncState; s.tvp = 0x12345; s.ksg = 2; s.keyNumber = 3;
        DmMessage m;
        m.frameCountdown = fc; m.dest = c.dest; m.src = c.src; m.mni = c.mni; m.messageType = (int)type; m.setupKnown = type == Dm::Setup;
        m.groupCall = c.groupCall; m.e2eEncrypted = c.e2eEncrypted; m.circuitMode = 1; m.priority = 2; m.powerClass = 4; m.timingFlag = true;
        Bits bs = encodeChannel(encodeSyncS(s), kBsch, z);
        Bits bh = encodeChannel(encodeSyncH(m, c.commType), kSchHd, z);
        Bits b = dsb(bs, bh);
        sendSlot(&b);
    };
    for (int i = 0; i < 4; i++) { sendSlot(nullptr); advance(); }
    // set-up: DSBs in slot 1 of consecutive frames
    for (int r = 0; r < c.setupRepeats; r++) {
        while (tn != 1) { sendSlot(nullptr); advance(); }
        dsbSlot(Dm::Setup, c.setupRepeats - 1 - r); advance();
        for (int k = 0; k < 3; k++) { sendSlot(nullptr); advance(); }
    }
    // traffic in slot 1 of frames 1..17
    int sent = 0;
    while (sent < c.trafficFrames) {
        if (tn == 1 && frame < 18) {
            Bits t4(432, 0);
            for (auto& v : t4) v = (uint8_t)(rng() & 1);
            out.trafficBits.push_back(t4);
            Bits t5 = t4;
            scramble(t5.data(), 432, dcc);
            Bits b1(t5.begin(), t5.begin() + 216), b2(t5.begin() + 216, t5.end());
            Bits b = dnb(b1, b2, false);
            sendSlot(&b); advance();
            sent++;
        } else if (tn == 1 && frame == 18) {
            dsbSlot(Dm::Occupied, 0);
            advance();
        } else { sendSlot(nullptr); advance(); }
    }
    if (c.sendRelease) {
        while (!(tn == 1)) { sendSlot(nullptr); advance(); }
        dsbSlot(Dm::TxCeased, 0); advance();
        for (int k = 0; k < 8; k++) { sendSlot(nullptr); advance(); }
    }
    // resample to the output rate, apply gate, cfo, noise
    const double ratio = c.rate / kRate * (1.0 + c.sroPpm * 1e-6);
    const size_t N = (size_t)(base.size() * ratio);
    out.iq.resize(N);
    std::normal_distribution<float> g(0.f, (float)c.noise);
    std::mt19937 nrng(c.seed + 77);
    for (size_t i = 0; i < N; i++) {
        const double pos = (double)i / ratio;
        const size_t k = (size_t)pos;
        const double fr = pos - k;
        const size_t k1 = std::min(k + 1, base.size() - 1);
        cf32 v = base[k] * (float)(1 - fr) + base[k1] * (float)fr;
        v *= gate[k] ? 0.5f : 0.f;
        const double ph = 2 * M_PI * c.cfoHz * (double)i / c.rate;
        v *= cf32((float)std::cos(ph), (float)std::sin(ph));
        if (c.noise > 0) v += cf32(g(nrng), g(nrng));
        out.iq[i] = v;
    }
    return out;
}

} // namespace dect2
