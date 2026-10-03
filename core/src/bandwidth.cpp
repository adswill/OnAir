#include "dect2/bandwidth.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace dect2 {

double snapBandwidth(double occ) {
    // the occupied width of an OFDM signal is about 95% of the channel (1.54 / 4.76 / 5.71 / 6.66 / 7.61 MHz)
    if (occ < 3.2) return 1.7;
    if (occ < 5.3) return 5;
    if (occ < 6.2) return 6;
    if (occ < 7.2) return 7;
    return 8;
}

void BandwidthDetector::add(const std::vector<float>& dbfs, double fsMhz) {
    if (dbfs.empty() || fsMhz <= 0) return;
    if (sum_.size() != dbfs.size() || fsMhz != fsMhz_) { sum_.assign(dbfs.size(), 0.0); frames_ = 0; fsMhz_ = fsMhz; }
    for (size_t i = 0; i < dbfs.size(); i++) sum_[i] += std::pow(10.0, dbfs[i] / 10.0);
    frames_++;
}

BandwidthEstimate BandwidthDetector::estimate() const {
    BandwidthEstimate e;
    const size_t n = sum_.size();
    if (frames_ < 4 || n < 256) return e;
    const double binMhz = fsMhz_ / n;
    // smooth over about 0.15 MHz, in dB
    const int w = std::max(1, (int)std::lround(0.15 / binMhz));
    std::vector<double> db(n);
    {
        std::vector<double> pre(n + 1, 0.0);
        for (size_t i = 0; i < n; i++) pre[i + 1] = pre[i] + sum_[i];
        for (size_t i = 0; i < n; i++) {
            const size_t a = i >= (size_t)w / 2 ? i - w / 2 : 0, b = std::min(n, i + w / 2 + 1);
            db[i] = 10.0 * std::log10(std::max(1e-14, (pre[b] - pre[a]) / (double)(b - a) / frames_));
        }
    }
    auto freq = [&](size_t i) { return ((double)i / n - 0.5) * fsMhz_; };
    const double nyq = fsMhz_ * 0.5;
    // plateau level: the high end of the centre region
    std::vector<double> centre;
    for (size_t i = 0; i < n; i++) if (std::fabs(freq(i)) < std::min(0.3 * nyq, 2.0) && std::fabs(freq(i)) > 0.15) centre.push_back(db[i]);
    // noise floor: the quietest part of the spectrum (other channels may fill the rest)
    std::vector<double> all;
    for (size_t i = 0; i < n; i++) if (std::fabs(freq(i)) < 0.96 * nyq) all.push_back(db[i]);
    if (centre.size() < 8 || all.size() < 32) return e;
    std::sort(centre.begin(), centre.end());
    std::sort(all.begin(), all.end());
    const double P = centre[(size_t)(centre.size() * 0.85)];
    const double F = all[all.size() / 20];
    e.snrDb = P - F;
    if (e.snrDb < 8.0) { e.noFloor = true; return e; }
    const double thr = F + 0.5 * (P - F);
    // from the middle outwards until the level stays below the threshold for 0.25 MHz
    const int gap = std::max(2, (int)std::lround(0.25 / binMhz));
    size_t mid = n / 2;
    // start at the strongest bin within 1 MHz of the centre, so a fade or the DC spike in the middle does not stop the search
    {
        double best = -1e9;
        for (size_t i = 0; i < n; i++) if (std::fabs(freq(i)) < 1.0 && db[i] > best) { best = db[i]; mid = i; }
        if (best < thr) return e;
    }
    size_t hi = mid, lo = mid;
    int below = 0;
    for (size_t i = mid; i + 1 < n; i++) { if (db[i] >= thr) { hi = i; below = 0; } else if (++below >= gap) break; }
    below = 0;
    for (size_t i = mid; i > 0; i--) { if (db[i] >= thr) { lo = i; below = 0; } else if (++below >= gap) break; }
    e.occupiedMhz = freq(hi) - freq(lo);
    e.bwMhz = snapBandwidth(e.occupiedMhz);
    e.valid = e.occupiedMhz > 1.0;
    return e;
}

} // namespace dect2
