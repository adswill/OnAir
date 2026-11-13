// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Activity detection for the fast scanner: signals above the local floor (findCandidates: runs of bins, so a signal that spans several bins
// is one candidate with a bandwidth) and the hit tracker that turns the candidates of successive hops into hits with hysteresis, a minimum
// duration and a hang time. Pure code, no radio: tests/fastscan/test_fastscan.cpp drives it with synthetic spectra.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {
namespace fastscan {

struct FreqRange { double loHz = 0, hiHz = 0; };
inline bool inRanges(const std::vector<FreqRange>& r, double hz) { for (const auto& x : r) if (hz >= x.loHz && hz <= x.hiHz) return true; return false; }

struct Candidate {
    double freqHz = 0, bwHz = 0;     // power-weighted centre, width of the run of bins above the edge level
    float peakDb = -150, snrDb = 0;  // snr: the strongest bin over its floor
};

// Runs of bins above floor + edgeDb that hold at least one bin above floor + onDb (gaps of `bridge` bins are bridged).
// db/floor: n bins from firstFreqHz at binHz; mask (optional, n entries): a non-zero entry is ignored (excluded ranges, known spurs).
std::vector<Candidate> findCandidates(const float* db, const float* floor, size_t n, double firstFreqHz, double binHz, float onDb, float edgeDb,
                                      int bridge = 1, const uint8_t* mask = nullptr);

struct HitParams {
    float onDb = 8, offDb = 5;       // a hit starts above onDb over the floor; once active it holds while above offDb (hysteresis)
    double minDurSec = 0;            // above onDb for at least this long before it counts (0: at once)
    double hangSec = 2;              // stays active this long after the signal is gone
    double staleSec = 30;            // an active hit outside the spans looked at ends after this long without being seen
    double mergeHz = 5000;           // candidates this close to a known hit are the same hit (a wide one: 0.6 x its width, whichever is larger)
    double gridStepHz = 0;           // > 0: a candidate snaps to this channel raster (origin gridOriginHz)
    double gridOriginHz = 0;
    size_t maxHits = 2000;           // the list is cut here (oldest inactive hits go first)
};

struct Hit {
    uint64_t id = 0;
    double freqHz = 0, bwHz = 0;
    float peakDb = -150, snrDb = 0, maxSnrDb = 0;
    double firstSeen = 0, lastSeen = 0;   // lastSeen: the last time it was above the off level
    double startedAt = 0;                 // start of the current activation
    double activeSec = 0;                 // total time active
    int count = 0;                        // activations
    bool active = false;
    bool priority = false;
};

struct HitEvent {
    enum Kind { Start, End } kind = Start;
    Hit hit;
};

class HitTracker {
public:
    void configure(const HitParams& p) { p_ = p; }
    const HitParams& params() const { return p_; }
    void setExcluded(std::vector<FreqRange> r) { excl_ = std::move(r); }
    void setLocked(std::vector<double> hz, double tolHz = 2500) { locked_ = std::move(hz); lockTol_ = tolHz; }
    void setPriority(std::vector<double> hz, double tolHz = 2500) { prio_ = std::move(hz); prioTol_ = tolHz; }
    bool isLocked(double hz) const;
    bool isPriority(double hz) const;
    // The level findCandidates() is run at for this tracker: the off level (the tracker itself asks for the on level to start a hit).
    float detectDb() const { return p_.offDb; }
    // One observation: the candidates found in the span [loHz, hiHz] at time t. Hits inside the span that are not among them are judged absent.
    void observe(double t, double loHz, double hiHz, const std::vector<Candidate>& c, std::vector<HitEvent>& ev);
    void endAll(double t, std::vector<HitEvent>& ev);   // stop: every active hit ends
    const std::vector<Hit>& hits() const { return hits_; }
    int activeCount() const;
    void clear() { hits_.clear(); st_.clear(); nextId_ = 1; }
    // a hit removed by the user (it comes back as a new one if the signal returns)
    void remove(uint64_t id);
private:
    struct St { bool pending = false; double pendingSince = 0, lastAbove = 0; };
    HitParams p_;
    std::vector<Hit> hits_;
    std::vector<St> st_;
    std::vector<FreqRange> excl_;
    std::vector<double> locked_, prio_;
    double lockTol_ = 2500, prioTol_ = 2500;
    uint64_t nextId_ = 1;
    int match(double hz, double bw) const;
    void trim();
};

}  // namespace fastscan
}  // namespace dect2
