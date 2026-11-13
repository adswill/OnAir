// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Marine VHF channel scan and call log: see marinevhf_scan.h.
#include "dect2/marinevhf/marinevhf_scan.h"
#include <algorithm>
#include <ctime>

namespace dect2 {
namespace marinevhf {

void ChannelScan::setChannels(const std::vector<int>& keys) {
    keys_ = keys;
    if (state_ != Idle && cfg_.singleTuner && cur_ < 0 && !keys_.empty()) cur_ = keys_[0];
}

int ChannelScan::nextAfter(int key) const {
    if (keys_.empty()) return cfg_.priorityKey;
    for (size_t i = 0; i < keys_.size(); i++) if (keys_[i] == key) return keys_[(i + 1) % keys_.size()];
    return keys_[0];
}

void ChannelScan::start(double now) {
    resume_ = -1; lastPlayed_ = -1; lastPrio_ = now; lastActive_ = now; visits_ = 0;
    state_ = Scanning; slotStart_ = now; cur_ = -1;
    if (cfg_.singleTuner) {
        cur_ = !keys_.empty() ? keys_[0] : cfg_.priorityKey;
        if (cur_ < 0) state_ = Idle;
        else visits_ = 1;
    }
}

void ChannelScan::stop() { state_ = Idle; cur_ = -1; resume_ = -1; }

int ChannelScan::update(double now, const std::function<bool(int)>& active) {
    if (state_ == Idle) return -1;
    const bool prio = cfg_.priorityKey >= 0;

    if (!cfg_.singleTuner) {
        if (prio && active(cfg_.priorityKey)) {
            if (cur_ != cfg_.priorityKey) {
                if (state_ == Holding && cur_ >= 0) resume_ = cur_;
                goTo(cfg_.priorityKey, now, Priority);
            }
            lastActive_ = now;
            return cur_;
        }
        if (state_ == Priority) {
            if (now - lastActive_ < cfg_.hangSec) return cur_;
            lastPlayed_ = cfg_.priorityKey;
            const int back = resume_;
            resume_ = -1;
            if (back >= 0 && active(back)) { goTo(back, now, Holding); lastActive_ = now; return cur_; }
            if (back >= 0) lastPlayed_ = back;
            state_ = Scanning; cur_ = -1;
        }
        if (state_ == Holding) {
            if (active(cur_)) { lastActive_ = now; return cur_; }
            if (now - lastActive_ < cfg_.hangSec) return cur_;
            lastPlayed_ = cur_; cur_ = -1; state_ = Scanning;
        }
        int k = lastPlayed_;
        for (size_t i = 0; i < keys_.size(); i++) {
            k = nextAfter(k);
            if (active(k)) { goTo(k, now, Holding); lastActive_ = now; return k; }
        }
        cur_ = -1;
        return -1;
    }

    // ---- single tuner
    const bool prioDue = prio && cur_ != cfg_.priorityKey && now - lastPrio_ >= cfg_.priorityEverySec;
    switch (state_) {
    case Scanning:
        if (prioDue) { resume_ = cur_; resumeState_ = Scanning; goTo(cfg_.priorityKey, now, PriorityProbe); lastPrio_ = now; return cur_; }
        if (active(cur_)) { state_ = Holding; lastActive_ = now; return cur_; }
        if (now - slotStart_ >= cfg_.dwellSec) goTo(nextAfter(cur_), now, Scanning);
        return cur_;
    case Holding:
        if (prioDue) { resume_ = cur_; resumeState_ = Holding; savedActive_ = lastActive_; goTo(cfg_.priorityKey, now, PriorityProbe); lastPrio_ = now; return cur_; }
        if (active(cur_)) lastActive_ = now;
        else if (now - lastActive_ >= cfg_.hangSec) goTo(nextAfter(cur_), now, Scanning);
        return cur_;
    case PriorityProbe:
        if (active(cur_)) { state_ = Priority; lastActive_ = now; return cur_; }
        if (now - slotStart_ >= cfg_.priorityDwellSec) {
            const int back = resume_ >= 0 ? resume_ : nextAfter(-1);
            const State bs = resume_ >= 0 ? resumeState_ : Scanning;
            resume_ = -1;
            goTo(back, now, bs);
            if (bs == Holding) lastActive_ = savedActive_;
            lastPrio_ = now;
        }
        return cur_;
    case Priority:
        if (active(cur_)) { lastActive_ = now; return cur_; }
        if (now - lastActive_ >= cfg_.hangSec) {
            const int back = resume_ >= 0 ? resume_ : nextAfter(-1);
            resume_ = -1;
            goTo(back, now, Scanning);
            lastPrio_ = now;
        }
        return cur_;
    default: return cur_;
    }
}

// ---- call log

void CallLog::finish(Open& o) {
    o.e.open = false;
    o.e.endSec = o.lastActive;
    Stats& s = stats_[o.e.key];
    s.calls++;
    s.totalSec += o.e.durationSec();
    s.lastHeardSec = o.e.endSec;
    s.lastHeardWall = o.e.wallTime;
    s.peakDb = std::max(s.peakDb, o.e.peakDb);
    done_.push_back(o.e);
    if (done_.size() > max_) done_.erase(done_.begin(), done_.begin() + (long)(done_.size() - max_));
    if (onClose_) onClose_(o.e);
}

void CallLog::update(double now, int key, bool active, double levelDb) {
    auto it = open_.find(key);
    if (active) {
        if (it == open_.end()) {
            Open o;
            o.e.key = key; o.e.startSec = now; o.e.endSec = now; o.e.open = true;
            o.e.wallTime = wall_ ? wall_() : (int64_t)std::time(nullptr);
            o.e.peakDb = levelDb; o.lastActive = now;
            it = open_.emplace(key, o).first;
        }
        Open& o = it->second;
        o.lastActive = now; o.e.endSec = now;
        o.e.peakDb = std::max(o.e.peakDb, levelDb);
        o.sum += levelDb; o.n++;
        o.e.meanDb = o.sum / (double)o.n;
    } else if (it != open_.end() && now - it->second.lastActive >= hang_) {
        finish(it->second);
        open_.erase(it);
    }
}

void CallLog::closeAll(double) {
    for (auto& kv : open_) finish(kv.second);
    open_.clear();
}

std::vector<CallEntry> CallLog::openCalls() const {
    std::vector<CallEntry> v;
    for (const auto& kv : open_) v.push_back(kv.second.e);
    return v;
}

CallLog::Stats CallLog::stats(int key) const {
    Stats s;
    auto it = stats_.find(key);
    if (it != stats_.end()) s = it->second;
    auto o = open_.find(key);
    if (o != open_.end()) {
        s.calls++;
        s.totalSec += o->second.e.durationSec();
        s.lastHeardSec = o->second.lastActive;
        s.lastHeardWall = o->second.e.wallTime;
        s.peakDb = std::max(s.peakDb, o->second.e.peakDb);
    }
    return s;
}

std::vector<CallEntry> CallLog::entriesFor(int key) const {
    std::vector<CallEntry> v;
    for (const CallEntry& e : done_) if (e.key == key) v.push_back(e);
    return v;
}

} // namespace marinevhf
} // namespace dect2
