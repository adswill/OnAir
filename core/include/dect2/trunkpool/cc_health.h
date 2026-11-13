// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Control channel health: a rolling hour of decode quality per control channel frequency (good and bad messages, grants, age) and the decision to
// move the control radio to a clearly better one, with hysteresis so it never flaps. Also the site book: control channels learned from the adjacent-site
// broadcasts over a whole drive (system, RFSS, site, frequency, when last heard), for the hunt and for export. Plain logic, no threads.
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace dect2::tpool {

struct CcScore {
    double hz = 0;
    double score = 0;          // 0..1, higher is better
    double errorRate = 0;      // bad / (good + bad) over the window
    double msgPerMin = 0;
    double grantsPerMin = 0;
    double ageSec = 1e9;       // since the last good message
    int minutes = 0;           // minutes of data behind the score
};

struct CcHealthConfig {
    double windowMin = 60;
    double recentMin = 3;          // the move decision looks at this many minutes
    double minDataMin = 1.0;       // a channel needs this much observation before it can be a target
    double leaveBelow = 0.45;      // the current channel must score below this ...
    double betterBy = 0.25;        // ... and the other this much higher ...
    double badForSec = 30;         // ... for this long, and
    double minDwellSec = 120;      // at least this long since the last move
};

class ControlHealth {
public:
    explicit ControlHealth(const CcHealthConfig& c = CcHealthConfig()) : cfg_(c) {}
    void setConfig(const CcHealthConfig& c) { cfg_ = c; }
    void onGood(double hz, double now);
    void onBad(double hz, double now);
    void onGrant(double hz, double now);
    // windowMin: minutes to look back (0: the configured hour). The move decision uses the last few minutes, the table the hour.
    CcScore score(double hz, double now, double windowMin = 0) const;
    std::vector<CcScore> all(double now) const;
    // The frequency to move the control radio to, 0 to stay. current: where it is now.
    double advise(double current, double now);
    void moved(double now) { lastMove_ = now; }

    // site book
    struct Site { int sysId = -1, rfss = -1, site = -1; double ccHz = 0, lastSec = 0; int seen = 0; };
    void learnSite(int sysId, int rfss, int site, double ccHz, double now);
    const std::vector<Site>& sites() const { return sites_; }
    std::string exportCsv() const;
    int importCsv(const std::string& text);

private:
    struct Bucket { int64_t minute = -1; int good = 0, bad = 0, grants = 0; };
    struct Ch { std::vector<Bucket> b; double lastGood = -1e9; double belowSince = -1; };
    std::map<int64_t, Ch> ch_;             // keyed by the frequency in Hz
    CcHealthConfig cfg_;
    double lastMove_ = -1e9;
    std::vector<Site> sites_;
    Bucket& bucket(Ch& c, double now);
    static int64_t key(double hz) { return (int64_t)(hz + 0.5); }
};

} // namespace dect2::tpool
