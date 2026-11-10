// L-band carrier quality log (see carrier_log.h).
#include "dect2/smart5/carrier_log.h"
#include <algorithm>

namespace dect2 {
namespace smart5 {

const char* CarrierLog::reasonName(OutageReason r) {
    switch (r) {
    case OutageReason::SunOutage: return "sun outage";
    case OutageReason::LowElevation: return "satellite low";
    case OutageReason::SignalFade: return "signal faded";
    default: return "unexplained";
    }
}

void CarrierLog::sample(double f, double t, const CarrierSample& s) {
    C& c = c_[key(f)];
    if (c.minStart < 0) c.minStart = t;
    if (t - c.minStart >= 60) {
        c.min.push_back({c.minStart, c.n ? c.snrSum / c.n : 0, c.frames});
        c.minStart = t; c.snrSum = 0; c.n = 0; c.frames = 0;
    }
    c.snrSum += s.snrDb; c.n++; c.frames += s.frames;
    if (s.frames > 0) {
        if (c.inOut) {
            c.cur.end = t;
            // reason: the marks seen during the outage, else a drop of SNR of 6 dB or more, else unexplained
            c.cur.reason = c.sun ? OutageReason::SunOutage : c.low ? OutageReason::LowElevation
                         : c.snrMin < c.cur.snrBeforeDb - 6 ? OutageReason::SignalFade : OutageReason::Unexplained;
            c.out.push_back(c.cur);
            c.inOut = false;
        }
        c.lastFrame = t; c.snrPrev = s.snrDb;
    } else if (c.lastFrame >= 0 && !c.inOut && t - c.lastFrame >= outSec_) {
        c.inOut = true; c.cur = {}; c.cur.start = c.lastFrame; c.cur.snrBeforeDb = c.snrPrev;
        c.sun = false; c.low = false; c.snrMin = 1e9;
    }
    if (c.inOut || (c.lastFrame >= 0 && s.frames == 0)) {
        c.sun = c.sun || s.sunOutage;
        c.low = c.low || s.elDeg < 10;
        c.snrMin = std::min(c.snrMin, s.snrDb);
    }
}

std::vector<CarrierMinute> CarrierLog::minutes(double f) const { auto it = c_.find(key(f)); return it == c_.end() ? std::vector<CarrierMinute>{} : it->second.min; }
std::vector<CarrierOutage> CarrierLog::outages(double f) const { auto it = c_.find(key(f)); return it == c_.end() ? std::vector<CarrierOutage>{} : it->second.out; }

double CarrierLog::availabilityPct(double f) const {
    auto it = c_.find(key(f));
    if (it == c_.end() || it->second.min.empty()) return 0;
    int ok = 0;
    for (auto& m : it->second.min) if (m.frames > 0) ok++;
    return 100.0 * ok / it->second.min.size();
}

std::vector<double> CarrierLog::carriers() const {
    std::vector<double> v;
    for (auto& kv : c_) v.push_back(kv.first * 100.0);
    return v;
}

} // namespace smart5
} // namespace dect2
