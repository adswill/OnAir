// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The tracker book: which tracker advertisements have been heard, for how long and how close, and which of them stay with the receiver.
// Pure bookkeeping on signal time (no DSP), so it is tested with synthetic hours. Identifiers live in memory only, are dropped after
// kBletrkForgetSec without a packet and are never written anywhere.
//
// A tracker follows when it has been heard for at least alertSec, in at least minPresence of the minutes in that span, and was near in at
// least minNear of the seconds it was heard. Rotating addresses (a tag changes its address every so often) are chained: a new advertiser of
// the same kind that appears within linkGapSec of another going silent, at a similar level, continues that tracker.
#pragma once
#include "dect2/bletrk/bletrk_tel.h"
#include <memory>
#include <vector>

namespace dect2 {

struct BletrkConfig {
    double alertSec = 600;
    float nearDbfs = -60;                // averaged level (dBFS) counted as near; 6 dB below it is medium, lower is far
    double forgetSec = kBletrkForgetSec;
    double minPresence = 0.6;
    double minNear = 0.5;
    double linkGapSec = 20;              // rotation: the old address silent for this long at most before the new one appears
    float linkDb = 8;                    // and the levels this close
};

class BletrkBook {
public:
    BletrkBook();
    ~BletrkBook();
    void setConfig(const BletrkConfig& c);
    BletrkConfig config() const;
    void clear();
    void hear(const std::string& addr, const bletrk::Detection& d, float rssi, double sec, int64_t wall);
    // Housekeeping, about every quarter second: levels, trends, following, forgetting. New alerts are returned (once per tracker).
    std::vector<BletrkAlert> tick(double sec, int64_t wall);
    void ignore(uint32_t id, bool on);
    void fill(BletrkTelemetry& out, double sec) const;   // trackers, alerts, the counts

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace dect2
