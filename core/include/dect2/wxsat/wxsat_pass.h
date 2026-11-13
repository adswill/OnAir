// Weather-satellite pass planner helpers (pure): a quality rating for each predicted pass, and the choice among passes that overlap in time when only one receiver is
// available (the auto-record schedule). The prediction itself (TLE, SGP4) is in core/src/track; this takes its output.
//   rate()    1 - 5 stars from the highest elevation (a low pass has a long, lossy path and the antenna's horizon in the way), the usable length, the
//             sun on the ground track (visible-light channels are black at night: the infrared channels still work) and the antenna's horizon angle.
//   plan()    from a list of passes: drop those under the user's minimum stars, and where two overlap keep the better one (a half-overlap is trimmed only if the
//             remaining part is still at least minUsefulS long; otherwise the weaker pass is dropped).
// Tests: tests/wxsat40/test_wxsat_pass.cpp
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2::wxpass {

struct Pass {
    std::string sat;
    double aosS = 0, losS = 0;      // start and end, seconds (any common epoch)
    double maxElDeg = 0;
    double sunElDeg = -90;          // sun elevation at the receiver at the time of the highest point (> 0 daylight on the track, a good proxy)
    bool visibleChannels = true;    // the mode's images use visible light (APT, LRPT, HRPT: yes); false = all-infrared product
    int stars = 0;
    std::string why;
};

struct RateOptions {
    double horizonDeg = 5;          // lowest elevation the antenna sees over the roofs
    double minUsefulS = 120;
};

// Elevation: 5 stars from 60 degrees, 1 star under 15. Pass length counts the part above the horizon limit. Night: a visible-light product loses a star (infrared remains).
inline int rate(Pass& p, const RateOptions& o = {}) {
    const double dur = p.losS - p.aosS;
    double s;
    if (p.maxElDeg >= 60) s = 5;
    else if (p.maxElDeg >= 40) s = 4;
    else if (p.maxElDeg >= 25) s = 3;
    else if (p.maxElDeg >= 15) s = 2;
    else s = 1;
    std::string w;
    char b[120];
    std::snprintf(b, sizeof b, "max %.0f deg, %.0f min", p.maxElDeg, dur / 60.0);
    w = b;
    if (p.maxElDeg < o.horizonDeg + 5) { s = 1; w += "; barely clears your horizon"; }
    if (dur < o.minUsefulS) { s = std::min(s, 1.0); w += "; too short"; }
    else if (dur < 360 && s > 2) { s -= 1; w += "; short pass"; }
    if (p.visibleChannels && p.sunElDeg < -6) { s = std::max(1.0, s - 1); w += "; night: only the infrared channels will show anything"; }
    p.stars = (int)s;
    p.why = w;
    return p.stars;
}

// The passes to record, in time order. All passes are rated first.
inline std::vector<Pass> plan(std::vector<Pass> in, int minStars, const RateOptions& o = {}) {
    for (Pass& p : in) rate(p, o);
    in.erase(std::remove_if(in.begin(), in.end(), [&](const Pass& p) { return p.stars < minStars; }), in.end());
    // strongest first wins its slot
    std::sort(in.begin(), in.end(), [](const Pass& a, const Pass& b) { return a.stars != b.stars ? a.stars > b.stars : a.maxElDeg > b.maxElDeg; });
    std::vector<Pass> kept;
    for (Pass p : in) {
        bool ok = true;
        for (const Pass& k : kept) {
            if (p.aosS < k.losS && p.losS > k.aosS) {   // overlap: trim the start or end of the weaker one
                if (p.aosS < k.aosS) p.losS = k.aosS; else if (p.losS > k.losS) p.aosS = k.losS; else { ok = false; break; }
                if (p.losS - p.aosS < o.minUsefulS) { ok = false; break; }
            }
        }
        if (ok) kept.push_back(p);
    }
    std::sort(kept.begin(), kept.end(), [](const Pass& a, const Pass& b) { return a.aosS < b.aosS; });
    return kept;
}

}  // namespace dect2::wxpass
