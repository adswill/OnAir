// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Fast scan v2: turns raw hits into a trustworthy channel list. A hit is only "confirmed" once it was seen in several separate sweeps; hits that
// are the mirror image of a much stronger signal about the hop centre, sit on the hop's DC/IF spot, or never change at all (a steady spur) are
// flagged so the list can hide them; the rough signal class comes from the bandwidth and the activity pattern. Pure code, no radio.
#pragma once
#include "dect2/fastscan/hits.h"
#include "dect2/fastscan/plan.h"
#include <string>
#include <unordered_map>
#include <vector>

namespace dect2 {
namespace fastscan {

enum class SigClass { Unknown, Carrier, Voice, Narrowband, Wideband, Broadcast, Digital };
const char* sigClassName(SigClass c);

// Rough class from the occupied bandwidth and the frequency (cheap, no demodulation). burstFrac: share of looks the signal was present (1: constant).
SigClass classify(double freqHz, double bwHz, double burstFrac);
// Short hint for the class column: "AM", "NFM", "carrier", "WFM broadcast", "digital" ...
std::string classHint(double freqHz, double bwHz, double burstFrac);

// True when a signal at freqHz/peakDb is the image of the one at otherHz/otherDb about the hop centre: it sits at the mirrored offset (within
// tolHz) and is at least rejectDb weaker.
bool isImageOf(double freqHz, float peakDb, double otherHz, float otherDb, double centreHz, double tolHz, float rejectDb);

struct QualifyParams {
    int confirmSweeps = 2;           // distinct sweeps a hit must be seen in before it is confirmed
    int steadyLooks = 24;            // an unchanging narrow signal seen this often is a spur
    double steadyStdDb = 0.7;        // ... if its peak level wobbles less than this
    double steadyMinPresence = 0.97; // ... and it was present on at least this share of the looks
    double imageRejectDb = 18;       // a mirrored signal this much weaker than its twin is the image
    double imageTolBins = 1.6;       // mirror position tolerance in bins
    double dcWidthHz = 0;            // narrow hits this close to a hop centre are the radio's DC/IF spike (0: 2 bins)
};

struct ChannelInfo {
    Hit hit;
    SigClass cls = SigClass::Unknown;
    std::string hint;
    int sweeps = 0;                  // distinct sweeps it was seen in
    int looks = 0;                   // looks at its hop
    float presence = 0;              // share of the looks it was present in (0..1)
    float peakMeanDb = -150, peakStdDb = 0;
    bool confirmed = false;
    bool spur = false;               // steady narrow signal that never changes
    bool image = false;              // mirror image of a stronger signal
    bool dc = false;                 // on the hop centre
    bool reliable() const { return confirmed && !spur && !image && !dc; }
};

class Qualifier {
public:
    void configure(const QualifyParams& p) { p_ = p; }
    const QualifyParams& params() const { return p_; }
    // One processed look at hop `hop` taken at time t in sweep number `sweep`. hits: the tracker's current list.
    void look(const FastPlan& plan, int hop, int sweep, double t, const std::vector<Hit>& hits);
    // The channel list: every hit with its quality flags. reliableOnly: drop unconfirmed / spur / image / dc.
    std::vector<ChannelInfo> channels(const std::vector<Hit>& hits, bool reliableOnly) const;
    void clear() { st_.clear(); }
private:
    struct St {
        int sweeps = 0, lastSweep = -1, looks = 0, seen = 0;
        double mean = 0, m2 = 0;     // Welford on the peak level while present
        bool image = false, dc = false;
        double bwMax = 0;
    };
    QualifyParams p_;
    double binHz_ = 0;
    std::unordered_map<uint64_t, St> st_;
};

}  // namespace fastscan
}  // namespace dect2
