// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAWS time structure and planning. See kaws_sched.h.
#include "dect2/kaws/kaws_sched.h"
#include <cmath>

namespace dect2::kaws {

BlockTiming blockTiming(int p) {
    switch (p) {
        case 0: return {52, 1024, 3};
        case 1: return {103, 1024, 1};
        case 2: return {112, 512, 1};
        case 3: return {128, 256, 1};
        case 4: return {170, 80, 1};
        case 5: return {40, 60, 1};
        case 6: case 7: return {24, 60, 1};
    }
    return {0, 0, 0};
}

double blockSeconds(int p) {
    BlockTiming t = blockTiming(p);
    return (double)t.symbolsPerBlock * t.symbolSamples * t.reps / kBaseRate;
}

double onAirSeconds(int p, int nblocks) { return kTxOffsetSec + (double)burstSamples(p, nblocks) / kBaseRate; }

int usableBits(int p) { return (p < 0 || p >= kNumProfiles) ? 0 : kProfiles[p].K - kBlockOverhead; }

int blocksPerSlot(int p) {
    if (p < 0 || p >= kNumProfiles || kProfiles[p].slotSec == 0) return 0;
    if (p <= 1) return 1;
    double room = kProfiles[p].slotSec - kTxOffsetSec - kEndMarginSec - kUnitSec - kClosingSec;
    int n = (int)std::floor(room / blockSeconds(p) + 1e-9);
    return n < 1 ? 1 : n;
}

MessagePlan planMessage(int p, int messageBits) {
    MessagePlan m;
    m.usableBits = usableBits(p);
    if (m.usableBits <= 0) return m;
    m.blocks = (messageBits + m.usableBits - 1) / m.usableBits;
    m.blocksPerSlot = blocksPerSlot(p);
    if (m.blocksPerSlot == 0) {   // voice profiles used for data: one block per 10 s data slot
        m.slots = m.blocks;
        m.messageSec = 10.0 * m.blocks;
        m.onAirSec = onAirSeconds(p, 1) * m.blocks;
        m.withAckSec = m.messageSec + 10.0;
        return m;
    }
    m.slots = (m.blocks + m.blocksPerSlot - 1) / m.blocksPerSlot;
    m.messageSec = (double)m.slots * kProfiles[p].slotSec;
    for (int left = m.blocks; left > 0; left -= m.blocksPerSlot) m.onAirSec += onAirSeconds(p, std::min(left, m.blocksPerSlot));
    m.withAckSec = m.messageSec + kProfiles[p].slotSec;
    return m;
}

int64_t SlotGrid::index(double utc) const { return (int64_t)std::floor(utc / slotSec); }
int64_t SlotGrid::nextIndex(double utc) const { return (int64_t)std::ceil(utc / slotSec - 1e-9); }

double clockErrorSec(double tau) { return tau / kBaseRate - (kWindowBeforeSec + kTxOffsetSec); }

// Measured full-chain KW-MOD 10 % block-error SNRs (KAWS-MEASURED-v0.1.md), 200 blocks per point (100 for P0).
const double kSuggestThresholdDb[6] = {-19.1, -16.1, -12.6, -6.8, 0.0, 0.0};   // P4 never reached 10 % on KW-MOD (floor), so it is never preferred over P5

int suggestProfile(double snrDb, double marginDb) {
    int best = 0;
    for (int p = 0; p < 6; p++)
        if (snrDb >= kSuggestThresholdDb[p] + marginDb) best = p;
    return best;
}

}  // namespace dect2::kaws
