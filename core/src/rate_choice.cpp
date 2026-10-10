#include "dect2/rate_choice.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace dect2 {

static std::string msps(double hz) {
    char b[32];
    snprintf(b, sizeof b, "%.3f", hz / 1e6);
    std::string s = b;   // 2.048, 10, 0.25: no trailing zeros
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s;
}

RateLimits rateLimitsOf(const DeviceInfo& d) {
    RateLimits L;
    if (!d.isRadio()) return L;
    L.minHz = d.minRateHz; L.maxHz = d.maxRateHz; L.ranges = d.rateRanges;
    if (d.kind == DeviceInfo::HackRF) { if (L.minHz <= 0) L.minHz = 2e6; if (L.maxHz <= 0) L.maxHz = 20e6; }   // hackrf_set_sample_rate: 2 to 20 Msps
    for (auto& r : L.ranges) if (r.first > r.second) std::swap(r.first, r.second);
    std::sort(L.ranges.begin(), L.ranges.end());
    return L;
}

static bool allSingle(const RateLimits& L) {
    if (L.ranges.empty()) return false;
    for (const auto& r : L.ranges) if (r.second - r.first > 1.0) return false;
    return true;
}

double deliveredRate(const RateLimits& L, double hz) {
    if (!L.ranges.empty()) {
        double above = 0, fastest = 0;
        for (const auto& r : L.ranges) {
            if (L.maxHz > 0 && r.first > L.maxHz + 1) continue;   // a range beyond what the driver lets through (the RTL-SDR's 3.2 Msps)
            const double hi = L.maxHz > 0 ? std::min(r.second, L.maxHz) : r.second;
            const double c = std::min(std::max(hz, r.first), hi);
            fastest = std::max(fastest, c);
            if (c >= hz - 1.0 && (above == 0 || c < above)) above = c;
        }
        if (above > 0 || fastest > 0) return above > 0 ? above : fastest;
    }
    double r = hz;
    if (L.maxHz > 0) r = std::min(r, L.maxHz);
    if (L.minHz > 0) r = std::max(r, L.minHz);
    return r;
}

std::vector<RateEntry> rateEntries(const RateLimits& L, double modeMinHz) {
    static const double kLadder[] = {0.25e6, 0.5e6, 1e6, 1.024e6, 2e6, 2.048e6, 2.4e6, 2.56e6, 3.2e6, 4e6, 5e6, 6e6, 8e6, 10e6, 12.5e6, 16e6, 20e6};
    std::vector<double> ask;
    if (allSingle(L)) for (const auto& r : L.ranges) ask.push_back(r.first);
    else ask.assign(std::begin(kLadder), std::end(kLadder));
    std::vector<RateEntry> out;
    for (double a : ask) {
        if (L.minHz > 0 && a < L.minHz - 1) continue;
        if (L.maxHz > 0 && a > L.maxHz + 1) continue;
        const double g = deliveredRate(L, a);
        if (modeMinHz > 0 && g < modeMinHz - 1) continue;
        if (!L.ranges.empty() && !allSingle(L) && std::fabs(g - a) > 1.0) continue;   // in a gap between the radio's ranges
        bool dup = false;
        for (const auto& e : out) if (std::fabs(e.getHz - g) <= 1.0) dup = true;
        if (!dup) out.push_back({a, g});
    }
    std::sort(out.begin(), out.end(), [](const RateEntry& x, const RateEntry& y) { return x.getHz < y.getHz; });
    return out;
}

RateCheck checkManualRate(const RateLimits& L, double modeMinHz, double hz) {
    RateCheck c;
    if (!(hz > 0) || !std::isfinite(hz)) { c.why = "enter a rate in Msps"; return c; }
    const bool lo = L.minHz > 0 && hz < L.minHz - 1, hi = L.maxHz > 0 && hz > L.maxHz + 1;
    if (lo || hi) {
        if (L.minHz > 0 && L.maxHz > 0) c.why = "this radio runs at " + msps(L.minHz) + " to " + msps(L.maxHz) + " Msps";
        else c.why = lo ? "this radio needs at least " + msps(L.minHz) + " Msps" : "this radio gives at most " + msps(L.maxHz) + " Msps";
        return c;
    }
    if (!L.ranges.empty() && !allSingle(L)) {
        double below = 0, above = 0;
        bool inside = false;
        for (const auto& r : L.ranges) {
            if (hz >= r.first - 1 && hz <= r.second + 1) inside = true;
            if (r.second < hz) below = std::max(below, r.second);
            if (r.first > hz && (above == 0 || r.first < above)) above = r.first;
        }
        if (!inside) {
            if (below > 0 && above > 0) c.why = "this radio cannot run between " + msps(below) + " and " + msps(above) + " Msps";
            else c.why = "this radio does not offer " + msps(hz) + " Msps";
            return c;
        }
    }
    c.hz = deliveredRate(L, hz);
    if (modeMinHz > 0 && c.hz < modeMinHz - 1) { c.hz = 0; c.why = "this mode needs at least " + msps(modeMinHz) + " Msps"; return c; }
    c.ok = true;
    return c;
}

} // namespace dect2
