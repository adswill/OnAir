// Learned horizon and next good pass (see horizon_learn.h).
#include "dect2/smart5/horizon_learn.h"
#include <algorithm>
#include <cmath>
#include <sstream>

namespace dect2 {
namespace smart5 {

int HorizonLearn::sector(double az) {
    az = std::fmod(az, 360); if (az < 0) az += 360;
    return std::min(kSectors - 1, (int)(az / 30));
}

void HorizonLearn::sample(double az, double el, bool dec) {
    if (el < 0 || el > 90) return;
    Bin& b = h_[sector(az)][(int)el];
    b.all += 1; if (dec) b.ok += 1;
}

bool HorizonLearn::learned(double az) const {
    float n = 0;
    for (auto& b : h_[sector(az)]) n += b.all;
    return n >= 60;   // a minute of samples in the sector
}

double HorizonLearn::horizon(double az) const {
    if (!learned(az)) return def_;
    const auto& s = h_[sector(az)];
    // the lowest elevation from which, over a 3 deg window and everything above, decoding succeeds more often than not
    for (int e = 0; e < kBins - 3; e++) {
        float ok = 0, all = 0;
        for (int k = e; k < e + 3; k++) { ok += s[k].ok; all += s[k].all; }
        if (all < 3) continue;
        if (ok / all < 0.5f) continue;
        float okUp = 0, allUp = 0;
        for (int k = e; k < kBins; k++) { okUp += s[k].ok; allUp += s[k].all; }
        if (okUp / allUp >= 0.6f) return e;
    }
    return 90;   // never decoded in this sector
}

double HorizonLearn::predictMinutes(const std::vector<TrackPoint>& p) const {
    double sec = 0;
    for (size_t i = 1; i < p.size(); i++) {
        const double el = 0.5 * (p[i].elDeg + p[i - 1].elDeg);
        if (el >= horizon(p[i].azDeg)) sec += p[i].t - p[i - 1].t;
    }
    return sec / 60;
}

PassScore HorizonLearn::best(const std::vector<std::vector<TrackPoint>>& passes) const {
    PassScore b;
    for (size_t i = 0; i < passes.size(); i++) {
        const double m = predictMinutes(passes[i]);
        double mx = 0; for (auto& q : passes[i]) mx = std::max(mx, q.elDeg);
        if (b.index < 0 || m > b.decodeMin + 1e-9) { b.index = (int)i; b.decodeMin = m; b.maxElDeg = mx; }
    }
    return b;
}

std::string HorizonLearn::save() const {
    std::ostringstream o;
    for (int s = 0; s < kSectors; s++)
        for (int e = 0; e < kBins; e++)
            if (h_[s][e].all > 0) o << s << ' ' << e << ' ' << h_[s][e].ok << ' ' << h_[s][e].all << '\n';
    return o.str();
}

void HorizonLearn::load(const std::string& str) {
    std::istringstream in(str);
    int s, e; float ok, all;
    while (in >> s >> e >> ok >> all)
        if (s >= 0 && s < kSectors && e >= 0 && e < kBins) { h_[s][e].ok = ok; h_[s][e].all = all; }
}

} // namespace smart5
} // namespace dect2
