// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAWS time structure and planning (spec section 3 and 2.1): the UTC slot grid, the transmit start, the receive window, burst timing per profile, blocks per
// slot, message time for a given size, and the profile suggestion from a measured SNR.
#pragma once
#include "dect2/kaws/kaws_phy.h"
#include <cstdint>

namespace dect2::kaws {

constexpr double kTxOffsetSec = 1.5;       // nominal transmit start = slot start + 1.5 s
constexpr double kWindowBeforeSec = 1.5;   // the receiver looks for the first lattice unit from slot start - 1.5 s ...
constexpr double kWindowAfterSec = 4.5;    // ... to slot start + 4.5 s (nominal +- 3 s)
constexpr double kEndMarginSec = 3.0;      // a burst ends at least 3 s before the next slot
constexpr double kUnitSec = 3.5, kClosingSec = 1.0;

// Data-section timing of a profile.
struct BlockTiming {
    int symbolsPerBlock;   // data symbols of one transmitted block (the repetitions are counted in reps)
    int symbolSamples;     // samples (2400/s) of one symbol including guard / cyclic prefix
    int reps;
};
BlockTiming blockTiming(int profile);
double blockSeconds(int profile);                 // data seconds of one block (P0: 3 repetitions included)
double onAirSeconds(int profile, int nblocks);    // spec definition: 1.5 s start offset + the burst (preamble units, blocks, 1 s closing weave)
int usableBits(int profile);                      // K - 24 (block header and CRC-16)
int blocksPerSlot(int profile);                   // floor((slot - 1.5 - 3 - 3.5 - 1) / block seconds) for single-unit profiles, 1 for P0 / P1; 0 for unslotted profiles

struct MessagePlan {
    int usableBits = 0, blocks = 0, blocksPerSlot = 0, slots = 0;
    double messageSec = 0;    // to the end of the last slot used
    double onAirSec = 0;      // all slots
    double withAckSec = 0;    // + one slot for the first ack
};
// A message of `messageBits` bits (401 for 50 chat characters).
MessagePlan planMessage(int profile, int messageBits);

// UTC slot grid. Times are seconds since 00:00:00 UTC of any day (the grid repeats; all slot lengths divide 86400 except 120 s which also divides it).
struct SlotGrid {
    int slotSec;
    explicit SlotGrid(int sec) : slotSec(sec) {}
    int64_t index(double utc) const;                 // slot number containing utc
    double start(int64_t idx) const { return (double)idx * slotSec; }
    bool callerSlot(int64_t idx) const { return (idx & 1) == 0; }   // even slots: the caller (initiator), odd: the responder
    double txStart(int64_t idx) const { return start(idx) + kTxOffsetSec; }
    int64_t nextIndex(double utc) const;             // first slot starting at or after utc
    // the receive window of slot idx in seconds (absolute): stream start and the latest first-unit start
    double windowStart(int64_t idx) const { return start(idx) - kWindowBeforeSec; }
    double windowEnd(int64_t idx) const { return start(idx) + kWindowAfterSec; }
};
// Sample indices for receiveBurst when the stream starts at windowStart: the first unit starts in [0, 6 s].
constexpr int kSearchLo = 0;
constexpr int kSearchHi = (int)((kWindowBeforeSec + kWindowAfterSec) * kBaseRate);
// A transmit start error: how far (seconds) a measured first-unit start (receiver stream index / 2400) lies from the nominal slot start + 1.5 s. Positive = late.
double clockErrorSec(double tauSamples);

// The profile to use next from a measured SNR (dB in 2.4 kHz): the fastest slotted profile whose moderate-channel threshold plus `marginDb` is below it
// (profiles 0..5; the voice profiles are never suggested), 0 when even P0 does not fit.
int suggestProfile(double snrDb, double marginDb = 2.0);
extern const double kSuggestThresholdDb[6];   // measured KW-MOD 10 % block error SNRs (KAWS-MEASURED-v0.1.md), P0..P5

}  // namespace dect2::kaws
