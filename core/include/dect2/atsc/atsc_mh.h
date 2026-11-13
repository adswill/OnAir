// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// ATSC-M/H (A/153) presence detector, working on the decoded main transport stream. An M/H Group is 118 consecutive MHE packets at the start of a
// 156-packet M/H Slot (A/153 Part 2, 5.3.1): one PID that is not null, in runs of exactly 118, whose starts are whole multiples of 156 packets apart.
// This finds M/H and measures its share of the channel. It does not decode the M/H services (the SCCC / RS Frame chain is not built).
#pragma once
#include <cstddef>
#include <cstdint>

namespace dect2 {

struct AtscMhStatus {
    bool present = false;
    int pid = -1;               // the MHE PID
    uint64_t groups = 0;        // M/H Groups seen
    double slotShare = 0;       // share of the M/H Slots that carry a Group (0..1)
};

class AtscMhDetector {
public:
    void feed(const uint8_t* packets, size_t count);    // 188-byte packets, in order; packets with the transport error flag break a run
    const AtscMhStatus& status() const { return st_; }
    void reset() { *this = AtscMhDetector(); }

private:
    void endRun();
    AtscMhStatus st_;
    int pid_ = -1;
    uint32_t run_ = 0;
    uint64_t pos_ = 0, runStart_ = 0;
    int candPid_ = -1;
    uint64_t lastStart_ = 0, firstStart_ = 0, candGroups_ = 0;
};

} // namespace dect2
