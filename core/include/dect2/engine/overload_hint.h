// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// "Too slow for this computer" hint: pure threshold logic. Fed the engine's cumulative ring-drop event count (SampleLoss::cpuEvents) once
// per UI frame; says true once per start when more than `threshold` new drop events arrived within the last `windowSec` seconds.
#pragma once
#include <cstdint>
#include <deque>
#include <utility>

namespace dect2 {
class OverloadHint {
public:
    explicit OverloadHint(int threshold = 3, double windowSec = 5.0) : threshold_(threshold), window_(windowSec) {}
    void reset() { hist_.clear(); last_ = 0; shown_ = false; }   // call at every start
    // events: cumulative count; nowSec: monotonic seconds. Returns true exactly once per start, when the warning should be shown.
    bool update(uint64_t events, double nowSec) {
        if (events < last_) last_ = events;   // counter restarted
        if (events > last_) { hist_.emplace_back(nowSec, events - last_); last_ = events; }
        while (!hist_.empty() && nowSec - hist_.front().first > window_) hist_.pop_front();
        uint64_t sum = 0;
        for (auto& h : hist_) sum += h.second;
        if (!shown_ && sum > (uint64_t)threshold_) { shown_ = true; return true; }
        return false;
    }
private:
    int threshold_; double window_;
    std::deque<std::pair<double, uint64_t>> hist_;
    uint64_t last_ = 0; bool shown_ = false;
};
}
