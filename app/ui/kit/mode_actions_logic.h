// The pure parts of the per-mode "Auto setup" and "Reset this mode" buttons (app/ui/kit/mode_actions.cpp), tested by tests/app/test_mode_actions.cpp.
// Auto setup = gain (Auto gain), bandwidth and sample rate (the mode's own again) and the fine frequency (this file): one click, nothing to type.
#pragma once
#include <algorithm>
#include <cmath>
#include <vector>

namespace modeactions {

constexpr double kNarrowMaxMhz = 0.6;     // a capture this narrow or narrower holds one narrow signal: its peak is the signal; a wider one holds many or a wide signal, so no snap
constexpr double kProminenceDb = 8.0;     // the peak must stand this far over the median of the window, or it is noise and the frequency stays
constexpr double kMaxWindowHz = 25e3;     // never move further than this (a crystal that is 25 kHz off is a different station)

// Where is the signal that sits near offsetHz (from the centre of the spectrum frame)? Looks in +-windowHz around it, smooths over 3 bins and returns
// the offset of the strongest bin when it stands kProminenceDb over the median of the window; false (nothing found) leaves the frequency alone.
inline bool refineOffsetHz(const std::vector<float>& dbfs, double sampleRate, double offsetHz, double windowHz, double* outHz) {
    const size_t n = dbfs.size();
    if (n < 16 || !(sampleRate > 0) || !std::isfinite(offsetHz)) return false;
    windowHz = std::min(windowHz, kMaxWindowHz);
    const double binHz = sampleRate / (double)n;
    if (!(windowHz >= 3 * binHz)) return false;                       // the window holds too few bins to say anything
    const long c0 = (long)std::lround((offsetHz / binHz) + (double)n / 2.0);   // the bin of the tuned frequency (DC is bin n/2)
    const long half = (long)std::floor(windowHz / binHz);
    const long lo = std::max<long>(1, c0 - half), hi = std::min<long>((long)n - 2, c0 + half);
    if (hi - lo < 4) return false;
    std::vector<float> sm;
    for (long i = lo; i <= hi; i++) sm.push_back((dbfs[(size_t)i - 1] + dbfs[(size_t)i] + dbfs[(size_t)i + 1]) / 3.f);
    std::vector<float> sorted = sm;
    std::nth_element(sorted.begin(), sorted.begin() + (long)sorted.size() / 2, sorted.end());
    const float med = sorted[sorted.size() / 2];
    const long best = std::max_element(sm.begin(), sm.end()) - sm.begin();
    if (!(sm[(size_t)best] - med >= (float)kProminenceDb)) return false;
    if (outHz) *outHz = ((double)(lo + best) - (double)n / 2.0) * binHz;
    return true;
}

// The window to look in for a capture of bandwidthMhz: a tenth of it, at most kMaxWindowHz; 0 (no snap) for a wide capture.
inline double snapWindowHz(double bandwidthMhz) {
    if (!(bandwidthMhz > 0) || bandwidthMhz > kNarrowMaxMhz) return 0;
    return std::min(kMaxWindowHz, std::max(2e3, bandwidthMhz * 1e6 / 10.0));
}

}  // namespace modeactions
