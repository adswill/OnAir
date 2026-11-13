// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/specan/burst_monitor.h"
#include <algorithm>

namespace dect2 {

namespace {
const size_t kMinQuiet = 3;   // no detection until the median has this many quiet samples
}

BurstDetector::BurstDetector(const BurstConfig& cfg) : cfg_(cfg) {
    if (cfg_.medianWindow < kMinQuiet) cfg_.medianWindow = kMinQuiet;
    quiet_.reserve(cfg_.medianWindow);
}

void BurstDetector::reset() {
    quiet_.clear();
    next_ = 0;
    inRun_ = false;
    n_ = 0;
    lastBase_ = 0;
}

float BurstDetector::median() const {
    std::vector<float> v(quiet_);
    const size_t mid = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + mid, v.end());
    return v[mid];
}

// Only quiet samples go into the median, so a long burst cannot pull the baseline up.
void BurstDetector::addQuiet(float db) {
    if (quiet_.size() < cfg_.medianWindow) quiet_.push_back(db);
    else { quiet_[next_] = db; next_ = (next_ + 1) % cfg_.medianWindow; }
}

bool BurstDetector::finish(BurstEvent& ev) {
    inRun_ = false;
    const double span = lastT_ - startT_;
    if (span < cfg_.minMs) return false;
    ev.startMs = startT_;
    ev.durationMs = span;
    ev.peakDb = peak_;
    ev.baseDb = base_;
    ev.samples = n_;
    return true;
}

bool BurstDetector::push(float db, double tMs, BurstEvent& ev) {
    if (quiet_.size() < kMinQuiet) { addQuiet(db); return false; }
    const float base = median();
    lastBase_ = base;
    if (!inRun_) {
        if (db - base >= cfg_.riseDb) {
            inRun_ = true;
            startT_ = lastT_ = tMs;
            base_ = base;
            peak_ = db;
            n_ = 1;
            return false;
        }
        addQuiet(db);
        return false;
    }
    if (db - base_ >= cfg_.riseDb) {
        lastT_ = tMs;
        peak_ = std::max(peak_, db);
        n_++;
        return false;
    }
    const bool got = finish(ev);
    addQuiet(db);
    return got;
}

bool BurstDetector::flush(BurstEvent& ev) {
    if (!inRun_) return false;
    return finish(ev);
}

std::vector<BurstEvent> detectBursts(const std::vector<float>& db, double dtMs, const BurstConfig& cfg) {
    std::vector<BurstEvent> out;
    BurstDetector d(cfg);
    BurstEvent ev;
    for (size_t i = 0; i < db.size(); i++)
        if (d.push(db[i], (double)i * dtMs, ev)) out.push_back(ev);
    if (d.flush(ev)) out.push_back(ev);
    return out;
}

} // namespace dect2
