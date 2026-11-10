// Satellite identification from a Doppler curve (see sat_id.h).
#include "dect2/satmisc/sat_id.h"
#include <algorithm>
#include <cmath>

namespace dect2 {
namespace satmisc {

namespace { constexpr double kCkm = 299792.458; }

bool predictDopplerHz(const Sgp4& s, const Observer& rx, double t, const SatIdOptions& opt, double& hz, double& elDeg) {
    Look l;
    if (!lookAt(s, rx, t, l)) return false;
    elDeg = l.elDeg;
    if (l.elDeg < opt.minElDeg) return false;
    double rate = l.rangeRateKmS;
    if (opt.bistatic) {
        Look lt;
        if (!lookAt(s, opt.tx, t, lt) || lt.elDeg < opt.minElDeg) return false;
        rate += lt.rangeRateKmS;
    }
    hz = -opt.refHz * rate / kCkm;
    return true;
}

SatIdResult identifySatellite(const std::vector<Tle>& tles, const Observer& rx, const std::vector<DopplerSample>& in, const SatIdOptions& opt) {
    SatIdResult r;
    if (in.size() < 6) { r.why = "Too few points on the track."; return r; }
    if (opt.refHz <= 0) { r.why = "No frequency given."; return r; }
    // at most 48 points, evenly over the track: enough for the shape, and bounds the work over thousands of satellites
    std::vector<DopplerSample> s;
    const size_t step = std::max<size_t>(1, in.size() / 48);
    for (size_t i = 0; i < in.size(); i += step) s.push_back(in[i]);
    double mean = 0;
    for (const auto& x : s) mean += x.hz;
    mean /= (double)s.size();
    double spread = 0;
    for (const auto& x : s) spread += (x.hz - mean) * (x.hz - mean);
    r.curveHz = std::sqrt(spread / (double)s.size());

    std::vector<SatIdMatch> all;
    std::vector<double> pred(s.size());
    for (const Tle& t : tles) {
        Sgp4 p;
        if (!p.init(t)) continue;
        bool visible = true;
        double peak = -90, off = 0;
        for (size_t i = 0; i < s.size() && visible; i++) {
            double el = 0;
            if (!predictDopplerHz(p, rx, s[i].t, opt, pred[i], el)) visible = false;
            peak = std::max(peak, el);
            off += s[i].hz - pred[i];
        }
        if (!visible) continue;
        r.considered++;
        off = opt.fitOffset ? off / (double)s.size() : 0.0;
        if (std::fabs(off) > opt.maxOffsetHz) continue;
        double e = 0;
        for (size_t i = 0; i < s.size(); i++) { const double d = s[i].hz - pred[i] - off; e += d * d; }
        SatIdMatch m;
        m.tle = t; m.rmsHz = std::sqrt(e / (double)s.size()); m.offsetHz = off; m.peakElDeg = peak;
        all.push_back(std::move(m));
    }
    std::sort(all.begin(), all.end(), [](const SatIdMatch& a, const SatIdMatch& b) { return a.rmsHz < b.rmsHz; });
    if (all.empty()) { r.why = "No satellite with known elements was above the horizon for the whole track."; return r; }
    if ((int)all.size() > std::max(1, opt.maxResults)) all.resize((size_t)std::max(1, opt.maxResults));
    if (!opt.bistatic)
        for (int i = 0; i < opt.refine && i < (int)all.size(); i++) {
            DopplerFit f;
            DopplerFitOptions fo; fo.fitOffset = opt.fitOffset;
            if (fitDopplerOrbit(all[(size_t)i].tle, rx, opt.refHz, in, fo, f) && f.ok) all[(size_t)i].refinedRmsHz = f.rmsAfterHz;
        }
    r.ranked = std::move(all);
    const SatIdMatch& best = r.ranked[0];
    if (best.rmsHz > opt.goodRmsHz) { r.why = "No candidate fits the measured curve."; return r; }
    if (r.curveHz < 3 * std::max(1.0, best.rmsHz)) { r.why = "The track has too little Doppler to tell satellites apart."; return r; }
    r.ok = true;
    if (r.ranked.size() < 2) { r.confident = true; return r; }
    const double next = r.ranked[1].rmsHz;
    if (next > 3 * std::max(best.rmsHz, 3.0)) r.confident = true;
    else if (next < 1.5 * std::max(best.rmsHz, 3.0)) r.ambiguous = true;
    return r;
}

} // namespace satmisc
} // namespace dect2
