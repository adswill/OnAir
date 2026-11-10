// Analogue TV ghost measurement: a ghost is a late copy of the whole signal, so the averaged line shows a second sync pulse, its two edges the sync width apart. Looks for that pair after
// the main pulse, in the averaged line (blanking 0, sync negative). Delays that put the ghost's falling edge on the main pulse's rising edge cannot be measured and are skipped.
#pragma once
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace dect2 {

struct AtvGhost { bool found = false; float delayUs = 0, levelDb = 0; bool inverted = false; };

// avg: one line, startUs: the time of avg[0] relative to the sync's leading edge is -leadUs; ptsPerUs points per microsecond; syncUs the pulse width
inline AtvGhost atvFindGhost(const std::vector<float>& avg, double leadUs, double ptsPerUs, double syncUs, double minDb = -30) {
    AtvGhost g;
    const int n = (int)avg.size(), w = std::max(1, (int)std::lround(0.25 * ptsPerUs));
    auto edge = [&](double us) {       // the step over the half microsecond around time us: > 0 rising
        const int k = (int)std::lround((us + leadUs) * ptsPerUs);
        if (k - w < 0 || k + w >= n) return 0.f;
        return avg[(size_t)(k + w)] - avg[(size_t)(k - w)];
    };
    // the main pulse: its edges, searched within half a microsecond
    float fall = 0, rise = 0;
    for (double d = -0.5; d <= 0.5; d += 1.0 / ptsPerUs) { fall = std::min(fall, edge(d)); rise = std::max(rise, edge(syncUs + d)); }
    const float main = 0.5f * (rise - fall);
    if (main <= 0.05f) return g;
    float best = 0; double at = 0;
    for (double t = 0.8; t <= 40.0; t += 1.0 / ptsPerUs) {
        if (std::fabs(t - syncUs) < 0.9 || t < 1.0) continue;   // its falling edge would sit on the main rising edge
        // (no gap at half a line: the averaged lines are normal ones, and a narrow equalising pulse has no partner edge a sync width later)
        const float e1 = -edge(t), e2 = edge(t + syncUs);
        if (e1 * e2 <= 0) continue;                                            // both edges of one pulse, the same sign
        const float a = std::min(std::fabs(e1), std::fabs(e2));
        if (a < 0.6f * std::max(std::fabs(e1), std::fabs(e2))) continue;       // a single picture edge has no partner
        if (a > std::fabs(best)) { best = e1 > 0 ? a : -a; at = t; }
    }
    const float db = (float)(20 * std::log10(std::max(1e-6f, std::fabs(best) / main)));
    if (best == 0 || db < minDb) return g;
    g.found = true; g.delayUs = (float)at; g.levelDb = db; g.inverted = best < 0;
    return g;
}

inline std::string atvGhostText(const AtvGhost& g) {
    char b[160];
    if (!g.found) return "no ghost";
    snprintf(b, sizeof b, "ghost %.1f us late (%.1f km of extra path), %.0f dB below the picture", g.delayUs, g.delayUs * 0.2998, -g.levelDb);
    return b;
}

} // namespace dect2
