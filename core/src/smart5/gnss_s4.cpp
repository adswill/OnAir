// Amplitude scintillation index S4 (see gnss_s4.h).
#include "dect2/smart5/gnss_s4.h"
#include <algorithm>
#include <cmath>

namespace dect2 {
namespace smart5 {

const char* GnssS4::levelName(Scint s) {
    switch (s) {
    case Scint::Weak: return "weak";
    case Scint::Moderate: return "moderate";
    case Scint::Strong: return "strong";
    default: return "none";
    }
}

void GnssS4::sample(int prn, double si, double cn0) {
    Sat& s = s_[prn];
    // the trend: a one-pole low-pass at 0.1 Hz; seeded from the first sample
    const double a = 1 - std::exp(-2 * 3.14159265358979323846 * 0.1 / rate_);
    if (!s.init) { s.trend = si; s.init = true; }
    s.trend += a * (si - s.trend);
    const double d = s.trend > 0 ? si / s.trend : 0;
    s.s1 += d; s.s2 += d * d; s.n++;
    s.cn0 += cn0;
    if (s.n >= (int)(win_ * rate_)) {
        const double m = s.s1 / s.n, v = s.s2 / s.n - m * m;
        S4Result r;
        r.s4Raw = m > 0 && v > 0 ? std::sqrt(v) / m : 0;
        const double c = std::pow(10.0, s.cn0 / s.n / 10);
        r.s4Noise = std::sqrt(100.0 / c * (1 + 500.0 / (19 * c)));
        r.s4 = r.s4Raw > r.s4Noise ? std::sqrt(r.s4Raw * r.s4Raw - r.s4Noise * r.s4Noise) : 0;
        r.level = r.s4 >= 0.6 ? Scint::Strong : r.s4 >= 0.3 ? Scint::Moderate : r.s4 >= 0.15 ? Scint::Weak : Scint::None;
        r.valid = true;
        s.last = r;
        s.s1 = s.s2 = 0; s.n = 0; s.cn0 = 0;
    }
}

S4Result GnssS4::result(int prn) const { auto it = s_.find(prn); return it == s_.end() ? S4Result{} : it->second.last; }

} // namespace smart5
} // namespace dect2
