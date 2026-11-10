// DVB-S/S2 rain-fade and margin log (see sat_fade.h).
#include "dect2/smart5/sat_fade.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace dect2 {
namespace smart5 {

namespace {
// EN 302 307-1 V1.4.1 table 13: ideal Es/N0 (dB) for FECFRAME length 64 800, PER 1e-7, AWGN
const double kS2[29] = {
    std::numeric_limits<double>::quiet_NaN(),
    -2.35, -1.24, -0.30, 1.00, 2.23, 3.10, 4.03, 4.68, 5.18, 6.20, 6.42,   // QPSK 1/4 1/3 2/5 1/2 3/5 2/3 3/4 4/5 5/6 8/9 9/10
    5.50, 6.62, 7.91, 9.35, 10.69, 10.98,                                // 8PSK 3/5 2/3 3/4 5/6 8/9 9/10
    8.97, 10.21, 11.03, 11.61, 12.89, 13.13,                             // 16APSK 2/3 3/4 4/5 5/6 8/9 9/10
    12.73, 13.64, 14.28, 15.69, 16.05};                                  // 32APSK 3/4 4/5 5/6 8/9 9/10
const char* kS2Name[29] = {"dummy",
    "QPSK 1/4", "QPSK 1/3", "QPSK 2/5", "QPSK 1/2", "QPSK 3/5", "QPSK 2/3", "QPSK 3/4", "QPSK 4/5", "QPSK 5/6", "QPSK 8/9", "QPSK 9/10",
    "8PSK 3/5", "8PSK 2/3", "8PSK 3/4", "8PSK 5/6", "8PSK 8/9", "8PSK 9/10",
    "16APSK 2/3", "16APSK 3/4", "16APSK 4/5", "16APSK 5/6", "16APSK 8/9", "16APSK 9/10",
    "32APSK 3/4", "32APSK 4/5", "32APSK 5/6", "32APSK 8/9", "32APSK 9/10"};
}

double s2QefEsN0(int m) { return m >= 1 && m <= 28 ? kS2[m] : std::numeric_limits<double>::quiet_NaN(); }
const char* s2ModcodName(int m) { return m >= 1 && m <= 28 ? kS2Name[m] : "?"; }

double s1QefEsN0(int num, int den) {
    double eb;
    if (num == 1 && den == 2) eb = 4.5;
    else if (num == 2 && den == 3) eb = 5.0;
    else if (num == 3 && den == 4) eb = 5.5;
    else if (num == 5 && den == 6) eb = 6.0;
    else if (num == 7 && den == 8) eb = 6.4;
    else return std::numeric_limits<double>::quiet_NaN();
    // Es = Eb x 2 bits x inner rate x 188/204
    return eb + 10 * std::log10(2.0 * num / den * 188.0 / 204.0);
}

void SatFadeLog::sample(double t, double esn0, int modcod, double qef, bool locked) {
    if (modcod_ >= 0 && modcod != modcod_) changes_.push_back({t, modcod_, modcod});
    modcod_ = modcod;
    last_ = locked ? esn0 : -30;
    margin_ = locked && !std::isnan(qef) ? esn0 - qef : -99;
    if (locked) hist_.push_back({t, esn0});
    while (!hist_.empty() && hist_.front().first < t - p_.clearWindowSec) hist_.pop_front();
    // clear sky = the 90th percentile of the last half hour, so fades and dips do not pull it down
    if (!hist_.empty()) {
        std::vector<double> v; v.reserve(hist_.size());
        for (auto& h : hist_) v.push_back(h.second);
        const size_t k = (size_t)(0.9 * (v.size() - 1));
        std::nth_element(v.begin(), v.begin() + k, v.end());
        if (!inFade_) clear_ = v[k];   // frozen during a fade
    }
    const double depth = clear_ - last_;
    if (!inFade_ && depth >= p_.fadeDb) { inFade_ = true; cur_ = {}; cur_.start = t; cur_.minMarginDb = margin_; }
    if (inFade_) {
        cur_.depthDb = std::max(cur_.depthDb, depth);
        cur_.minMarginDb = std::min(cur_.minMarginDb, margin_);
        if (!locked) cur_.lostLock = true;
        cur_.length = t - cur_.start;
        if (depth < 0.5 * p_.fadeDb) {
            inFade_ = false;
            if (cur_.length >= p_.minSec) fades_.push_back(cur_);
        }
    }
}

std::string SatFadeLog::text() const {
    char b[200];
    if (inFade_) snprintf(b, sizeof b, "fade %.1f dB for %.0f s, margin %.1f dB%s", fadeDb(), cur_.length, margin_, cur_.lostLock ? ", lock lost" : "");
    else snprintf(b, sizeof b, "margin %.1f dB above QEF (clear sky %.1f dB), %zu fades logged", margin_, clear_, fades_.size());
    return b;
}

} // namespace smart5
} // namespace dect2
