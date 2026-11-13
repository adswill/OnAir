// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Pure rules of the signal indicator and the CPU meter (no ImGui, so tests/app/test_meter_logic.cpp covers them):
//  - Spark: the 30 s ring buffer of the CPU sparkline
//  - SlowState: "too slow" with hysteresis (CPU over ~85% of the cores it may use, or the receiver falling behind)
//  - Signal / classify: one signal-strength and decode-quality verdict for every mode (No signal / Weak / Locked / Good)
//  - Relief: the one-click fix, the safest relief first, remembered so it can be undone
#pragma once
#include <algorithm>
#include <functional>
#include <string>
#include <vector>

namespace meterlogic {

// ---- the sparkline: the last 30 one-second CPU samples (100 = one core)
struct Spark {
    static constexpr int N = 30;
    float v[N] = {};
    int head = 0, count = 0;   // head = where the next sample goes
    void push(float x) { v[head] = x; head = (head + 1) % N; if (count < N) count++; }
    float at(int i) const { return v[(head - count + i + 2 * N) % N]; }   // i = 0 is the oldest
    float maxv() const { float m = 0; for (int i = 0; i < count; i++) m = std::max(m, at(i)); return m; }
};

// ---- too slow
constexpr float kBusyShare = 0.85f;          // of the cores the process may use
constexpr int kOnAfter = 3, kOffAfter = 5;   // seconds over before the warning, seconds under before it clears
inline bool overloaded(float cpuPct, int cores, float loadPct, bool newDrops) {
    return cpuPct > kBusyShare * 100.f * (float)std::max(1, cores) || loadPct >= 100.f || newDrops;
}
struct SlowState {
    int over = 0, under = 0;
    bool slow = false;
    // call once a second; the warning needs kOnAfter seconds over and clears after kOffAfter seconds under
    // (a sample between 75% and 85% of the cores neither raises nor clears it)
    bool update(float cpuPct, int cores, float loadPct, bool newDrops) {
        if (overloaded(cpuPct, cores, loadPct, newDrops)) { over++; under = 0; }
        else if (cpuPct < 0.75f * 100.f * (float)std::max(1, cores) && loadPct < 90.f) { under++; over = 0; }
        else { over = 0; under = 0; }
        if (!slow && over >= kOnAfter) slow = true;
        else if (slow && under >= kOffAfter) slow = false;
        return slow;
    }
};

// ---- the one signal indicator
enum class Quality { None = 0, Weak = 1, Locked = 2, Good = 3 };
inline const char* qualityWord(Quality q) {
    switch (q) { case Quality::Weak: return "Weak"; case Quality::Locked: return "Locked"; case Quality::Good: return "Good"; default: return "No signal"; }
}
// the 0..4 segments of the bar (the shape says it as well as the colour)
inline int qualitySegments(Quality q) { return q == Quality::Good ? 4 : q == Quality::Locked ? 3 : q == Quality::Weak ? 1 : 0; }

struct Signal {
    bool running = false;
    bool haveLevel = false; float levelDb = -120;   // dBFS at the ADC
    bool haveSnr = false; float snrDb = 0;          // SNR / MER of the mode, dB
    bool haveLock = false; bool locked = false;     // the mode's own lock lamp
    int meters = 0;                                 // number of mode meters seen (ModeMeter bank)
    int worst = 0;                                  // worst meter level: 0 neutral, 1 good, 2 marginal, 3 bad
    bool anyGood = false;
};
constexpr float kFloorDb = -90.f, kSeenDb = -75.f, kGoodSnrDb = 15.f;
inline Quality classify(const Signal& s) {
    if (!s.running) return Quality::None;
    const bool seen = s.haveLevel ? s.levelDb > kSeenDb : true;
    if (s.haveLevel && s.levelDb <= kFloorDb && !(s.haveLock && s.locked)) return Quality::None;
    if (s.haveLock) {
        if (!s.locked) return seen ? Quality::Weak : Quality::None;
        if (s.haveSnr) return s.snrDb >= kGoodSnrDb ? Quality::Good : Quality::Locked;
        return s.meters > 0 && s.anyGood && s.worst <= 1 ? Quality::Good : Quality::Locked;
    }
    if (s.meters > 0) {
        if (s.anyGood && s.worst <= 1) return Quality::Good;
        if (s.anyGood) return Quality::Locked;
        return seen ? Quality::Weak : Quality::None;
    }
    if (s.haveSnr) return s.snrDb >= kGoodSnrDb ? Quality::Good : s.snrDb >= 6.f ? Quality::Locked : s.snrDb > 0.f ? Quality::Weak : Quality::None;
    return seen ? Quality::Weak : Quality::None;   // a level alone never claims a lock
}

// ---- the one-click fix
inline float nextWaterfallRate(float cur) {   // rows a second; 0 = every frame (about 30 a second). -1: nothing left to turn down
    if (cur <= 0.05f || cur > 5.f) return 5.f;
    if (cur > 2.f) return 2.f;
    if (cur > 1.f) return 1.f;
    return -1.f;
}
inline int nextFft(int effective) { return effective > 4096 ? std::max(4096, effective / 2) : 0; }   // bins; 0: already the smallest
struct Step {
    std::string said;                     // what it did, in plain words
    std::function<bool()> apply;          // true when it changed something
    std::function<void()> undo;
};
class Relief {
public:
    // runs the first step that changes something (the caller lists them safest first); returns what it said, "" when none could
    std::string fix(const std::vector<Step>& steps) {
        for (const Step& s : steps) if (s.apply && s.apply()) { undo_.push_back(s.undo); said_.push_back(s.said); return s.said; }
        return "";
    }
    bool canUndo() const { return !undo_.empty(); }
    const std::string& last() const { static const std::string none; return said_.empty() ? none : said_.back(); }
    void undoAll() { for (size_t i = undo_.size(); i-- > 0;) if (undo_[i]) undo_[i](); undo_.clear(); said_.clear(); }
    void forget() { undo_.clear(); said_.clear(); }   // the user changed the setting themselves, or the mode changed
private:
    std::vector<std::function<void()>> undo_;
    std::vector<std::string> said_;
};

}  // namespace meterlogic
