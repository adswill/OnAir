// TACAN bearing (tacan.h).
#include "dect2/dme/tacan.h"
#include <cmath>
#include <utility>
#include "dect2/common/mathconst.h"

namespace dect2::dme {

namespace {
constexpr double kPeriod = 1.0 / 15.0;
constexpr double kGapUs = 40.0;             // pulses of a burst are at most 30 us apart
constexpr int kMinBurst = 9;                 // pulses: random replies come as isolated pairs
constexpr double kMainUs = 300.0;            // a burst longer than this is the main group (X 12 pairs, Y 13 pulses at 30 us); the auxiliary group is shorter
constexpr size_t kCycles = 30;               // 2 s of rotation
}

void TacanBearing::reset() { cluster_.clear(); lastT_ = lastMain_ = -1; nMain_ = 0; good_ = 0; cycles_.clear(); cur_ = Cycle(); valid_ = false; radial_ = 0; d15_ = d135_ = 0; }

void TacanBearing::pulse(double t, float a) {
    if (lastT_ >= 0 && (t - lastT_) * 1e6 > kGapUs) endCluster();
    cluster_.push_back({t, a});
    lastT_ = t;
}

void TacanBearing::endCluster() {
    if ((int)cluster_.size() >= kMinBurst) {
        const double dur = (cluster_.back().t - cluster_.front().t) * 1e6;
        if (dur >= kMainUs && cluster_.size() >= 12) {
            const double t0 = cluster_.front().t;
            if (lastMain_ >= 0) {
                const double dt = t0 - lastMain_;
                if (dt < 0.9 * kPeriod) { cluster_.clear(); return; }    // a shorter burst lengthened by a neighbouring reply: not the main group
                if (std::fabs(dt - kPeriod) < 0.01 * kPeriod) { good_++; nMain_++; cycles_.push_back(cur_); if (cycles_.size() > kCycles) cycles_.pop_front(); solve(); }
                else { good_ = 0; cycles_.clear(); }
            } else nMain_++;
            cur_ = Cycle();
            lastMain_ = t0;
        }
        // bursts do not carry the rotation: not counted in the cycle sums
    } else if (lastMain_ >= 0) {
        for (const auto& p : cluster_) addToCycle(p);
    }
    cluster_.clear();
}

void TacanBearing::addToCycle(const P& p) {
    const double ph = 2 * M_PI * 15.0 * (p.t - lastMain_);
    if (ph < 0) return;
    const double x[5] = {1.0, std::cos(ph), std::sin(ph), std::cos(9 * ph), std::sin(9 * ph)};
    cur_.n += 1;
    for (int i = 0; i < 5; i++) { cur_.b[i] += x[i] * p.a; for (int j = 0; j < 5; j++) cur_.a[i][j] += x[i] * x[j]; }
}

void TacanBearing::solve() {
    // least squares of the pulse amplitudes on 1, 15 Hz and 135 Hz: replies are not spread evenly over a rotation (no replies during the bursts)
    Cycle s;
    for (const auto& c : cycles_) { s.n += c.n; for (int i = 0; i < 5; i++) { s.b[i] += c.b[i]; for (int j = 0; j < 5; j++) s.a[i][j] += c.a[i][j]; } }
    if (good_ < 6 || s.n < 500) { valid_ = false; return; }
    double m[5][6];
    for (int i = 0; i < 5; i++) { for (int j = 0; j < 5; j++) m[i][j] = s.a[i][j]; m[i][5] = s.b[i]; }
    for (int c = 0; c < 5; c++) {
        int piv = c;
        for (int r = c + 1; r < 5; r++) if (std::fabs(m[r][c]) > std::fabs(m[piv][c])) piv = r;
        if (std::fabs(m[piv][c]) < 1e-9) { valid_ = false; return; }
        if (piv != c) for (int k = 0; k < 6; k++) std::swap(m[c][k], m[piv][k]);
        for (int r = 0; r < 5; r++) if (r != c) { const double f = m[r][c] / m[c][c]; for (int k = 0; k < 6; k++) m[r][k] -= f * m[c][k]; }
    }
    const double mean = m[0][5] / m[0][0], a1 = m[1][5] / m[1][1], b1 = m[2][5] / m[2][2], a9 = m[3][5] / m[3][3], b9 = m[4][5] / m[4][4];
    if (mean <= 0) { valid_ = false; return; }
    d15_ = (float)(std::hypot(a1, b1) / mean);
    d135_ = (float)(std::hypot(a9, b9) / mean);
    if (d15_ < 0.03) { valid_ = false; return; }
    double coarse = 90.0 + std::atan2(b1, a1) * 180.0 / M_PI;
    double fine = 90.0 + (std::atan2(b9, a9) * 180.0 / M_PI) / 9.0;
    if (d135_ >= 0.03) {
        const double k = std::round((coarse - fine) / 40.0);
        coarse = fine + 40.0 * k;
    }
    radial_ = std::fmod(coarse + 720.0, 360.0);
    valid_ = true;
}

} // namespace dect2::dme
