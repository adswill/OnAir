// Radio clock from GNSS (see gnss_clock.h).
#include "dect2/smart5/gnss_clock.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace dect2 {
namespace smart5 {

void GnssClockCal::fix(double t, double drift, bool valid, int nSats) {
    if (!valid || nSats < 4 || !std::isfinite(drift) || std::fabs(drift) > 200e-6) return;
    const double p = drift * 1e6;
    // reject a fix that disagrees with the running median by more than 5 sigma (a bad PVT), once settled
    if (n_ >= 10 && sigma_ > 0 && std::fabs(p - ppm_) > std::max(5 * sigma_ * std::sqrt((double)w_.size()), 0.05)) return;
    w_.push_back({t, p}); n_++;
    while (w_.size() > 120) w_.pop_front();
    // robust level: a linear fit over the window (the warm-up slope) evaluated now
    const size_t m = w_.size();
    double st = 0, sp = 0;
    for (auto& x : w_) { st += x.first; sp += x.second; }
    st /= m; sp /= m;
    double stt = 0, stp = 0;
    for (auto& x : w_) { stt += (x.first - st) * (x.first - st); stp += (x.first - st) * (x.second - sp); }
    const double b = stt > 0 ? stp / stt : 0;
    ppm_ = sp + b * (t - st);
    trend_ = b * 60;
    double r = 0;
    for (auto& x : w_) { const double e = x.second - (sp + b * (x.first - st)); r += e * e; }
    sigma_ = m > 2 ? std::sqrt(r / (m - 2) / m) : 0;   // standard error of the level
}

std::string GnssClockCal::text() const {
    char b[160];
    if (!ready()) return "SDR clock: waiting for GNSS fixes";
    snprintf(b, sizeof b, "SDR clock %+.3f ppm (+-%.3f)%s, %+.0f Hz at 100 MHz", ppm_, sigma_, settled() ? "" : ", still warming up", correctionHz(100e6));
    return b;
}

} // namespace smart5
} // namespace dect2
