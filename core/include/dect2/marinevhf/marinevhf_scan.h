// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Marine VHF: the channel scan (dwell, hang, priority watch on channel 16) and the per-channel call log. No engine, no audio: the receiver and the
// tests feed them the time and which channels are active, and act on what they answer.
//
// ChannelScan has two ways to work:
//   whole span (singleTuner false): the activity of every listed channel is known at once (the carriers of the captured span), so the scan does not
//     step at all: it plays the first active channel in list order after the one it last played, holds it until it has been quiet for the hang time,
//     and the priority channel takes over the moment it is active (the interrupted channel is resumed when the priority one ends and it is still
//     active). With nothing active, update() answers -1 (nothing plays).
//   single tuner (singleTuner true): the radio sees one channel at a time. The scan tunes to each listed channel for the dwell time, stops on the
//     first that is active, holds until quiet for the hang time, and every priorityEverySec it visits the priority channel for priorityDwellSec
//     (it stays there while that channel is active, and then goes on with the list). update() always answers the channel to tune to.
//
// A channel is a small integer key (the app uses the index in the region's channel list; the tests use channel numbers).
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <vector>

namespace dect2 {
namespace marinevhf {

struct ScanConfig {
    double dwellSec = 0.4;           // single tuner: time on each channel before the next
    double hangSec = 2.0;            // stay on a channel this long after it went quiet
    int priorityKey = -1;            // the channel watched first (channel 16), -1 none
    double priorityEverySec = 2.0;   // single tuner: look at the priority channel this often
    double priorityDwellSec = 0.3;   // single tuner: for this long
    bool singleTuner = false;
};

class ChannelScan {
public:
    enum State { Idle, Scanning, Holding, Priority, PriorityProbe };

    void setChannels(const std::vector<int>& keys);   // the channels to scan, in scan order (the priority channel need not be among them)
    void setConfig(const ScanConfig& c) { cfg_ = c; }
    const ScanConfig& config() const { return cfg_; }
    void start(double now);
    void stop();
    bool running() const { return state_ != Idle; }
    State state() const { return state_; }

    // Call about every 50 ms. active(key) tells whether that channel has a carrier now (single tuner: only the key update() last answered is asked).
    // Returns the key to play (whole span; -1 = none) or tune to (single tuner).
    int update(double now, const std::function<bool(int)>& active);

    int current() const { return cur_; }              // the key last answered
    int visited() const { return visits_; }           // how many times the scan moved to a channel (tests)

private:
    int nextAfter(int key) const;                     // the list entry after `key` (the first when it is not in the list)
    void goTo(int key, double now, State s) { cur_ = key; slotStart_ = now; state_ = s; visits_++; }
    std::vector<int> keys_;
    ScanConfig cfg_;
    State state_ = Idle;
    int cur_ = -1;
    int resume_ = -1;                                 // the channel a priority takeover interrupted
    int lastPlayed_ = -1;                             // the last channel that was held (the next search starts after it)
    double slotStart_ = 0, lastActive_ = 0, lastPrio_ = 0, savedActive_ = 0;
    State resumeState_ = Scanning;                    // single tuner: what the scan was doing before it looked at the priority channel
    int visits_ = 0;
};

// One transmission on a channel
struct CallEntry {
    int key = -1;
    int64_t wallTime = 0;            // unix seconds at the start
    double startSec = 0, endSec = 0; // on the caller's clock
    double peakDb = -200, meanDb = -200;
    bool open = false;               // still going
    double durationSec() const { return endSec > startSec ? endSec - startSec : 0; }
};

class CallLog {
public:
    explicit CallLog(double hangSec = 1.0, size_t maxEntries = 2000) : hang_(hangSec), max_(maxEntries) {}
    void setHang(double s) { hang_ = s; }
    void setClock(std::function<int64_t()> wall) { wall_ = std::move(wall); }
    void setOnClose(std::function<void(const CallEntry&)> f) { onClose_ = std::move(f); }

    // Call for every channel at each tick (or only for the ones that changed): active = carrier / squelch open, levelDb its level.
    // A call ends when the channel has been inactive for the hang time (its end is the last active moment).
    void update(double now, int key, bool active, double levelDb);
    void closeAll(double now);                         // the stream ended: end every open call at its last active moment
    void clear() { done_.clear(); open_.clear(); stats_.clear(); }

    const std::vector<CallEntry>& entries() const { return done_; }   // finished calls, oldest first
    std::vector<CallEntry> openCalls() const;
    struct Stats { int calls = 0; double totalSec = 0; double lastHeardSec = -1; int64_t lastHeardWall = 0; double peakDb = -200; };
    Stats stats(int key) const;                       // finished calls plus the open one
    std::vector<CallEntry> entriesFor(int key) const;

private:
    struct Open { CallEntry e; double lastActive = 0; double sum = 0; long n = 0; };
    void finish(Open& o);
    double hang_;
    size_t max_;
    std::function<int64_t()> wall_;
    std::function<void(const CallEntry&)> onClose_;
    std::vector<CallEntry> done_;
    std::map<int, Open> open_;
    std::map<int, Stats> stats_;                      // finished calls only
};

} // namespace marinevhf
} // namespace dect2
