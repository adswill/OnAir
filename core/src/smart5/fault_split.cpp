// Fault or reception (see fault_split.h).
#include "dect2/smart5/fault_split.h"
#include <cstdio>

namespace dect2 {
namespace smart5 {

const char* FaultSplit::name(FaultKind k) {
    switch (k) {
    case FaultKind::Reception: return "reception";
    case FaultKind::ServiceFeed: return "station feed";
    case FaultKind::NoData: return "no data from the station";
    default: return "ok";
    }
}

double FaultSplit::pct(const std::deque<Cnt>& w) {
    unsigned g = 0, b = 0;
    for (auto& c : w) { g += c.g; b += c.b; }
    return g + b ? 100.0 * b / (g + b) : -1;
}

void FaultSplit::common(double now, unsigned good, unsigned bad) { now_ = now; commonCur_.g += good; commonCur_.b += bad; }
void FaultSplit::service(unsigned id, unsigned good, unsigned bad) { Svc& s = svc_[id]; s.cur.g += good; s.cur.b += bad; }

void FaultSplit::tick(double now) {
    now_ = now;
    common_.push_back(commonCur_); commonCur_ = {};
    while ((int)common_.size() > p_.windowSec) common_.pop_front();
    const double cp = pct(common_);
    const bool commonBad = cp >= p_.badPct, commonClean = cp >= 0 && cp <= p_.cleanPct;
    int failing = 0, total = 0, clean = 0;
    for (auto& kv : svc_) {
        Svc& s = kv.second;
        if (s.cur.g + s.cur.b) s.lastData = now;
        if (s.lastData < 0) s.lastData = now;
        s.w.push_back(s.cur); s.cur = {};
        while ((int)s.w.size() > p_.windowSec) s.w.pop_front();
        total++;
        const double sp = pct(s.w);
        if (sp >= p_.badPct) failing++;
        else if (sp >= 0 && sp <= p_.cleanPct) clean++;
    }
    // the reading: every service bad with the common channel -> reception; one or a few bad while the common channel and others
    // are clean -> that service's feed; nothing at all from a service that was sending, with a clean common channel -> no data
    overall_ = commonBad || (total && failing == total && failing > 1) ? FaultKind::Reception : FaultKind::Ok;
    for (auto& kv : svc_) {
        Svc& s = kv.second;
        const double sp = pct(s.w);
        FaultKind k = FaultKind::Ok;
        if (overall_ == FaultKind::Reception) k = sp >= p_.badPct || sp < 0 ? FaultKind::Reception : FaultKind::Ok;
        else if (now - s.lastData >= p_.noDataSec && commonClean) k = FaultKind::NoData;
        else if (sp >= p_.badPct && commonClean && (clean > 0 || total == 1)) k = FaultKind::ServiceFeed;
        else if (sp >= p_.badPct) k = FaultKind::Reception;
        if (k != s.kind) { s.kind = k; s.since = now; }
        if (k != FaultKind::Ok && k != FaultKind::Reception && overall_ == FaultKind::Ok) overall_ = FaultKind::ServiceFeed;
    }
}

std::vector<ServiceFault> FaultSplit::verdict() const {
    std::vector<ServiceFault> v;
    for (auto& kv : svc_) v.push_back({kv.first, kv.second.kind, pct(kv.second.w), kv.second.since});
    return v;
}

std::string FaultSplit::text(unsigned id, const std::string& label) const {
    auto it = svc_.find(id);
    if (it == svc_.end() || it->second.kind == FaultKind::Ok) return "";
    char b[200];
    const double sp = pct(it->second.w);
    switch (it->second.kind) {
    case FaultKind::ServiceFeed: snprintf(b, sizeof b, "%s: %.0f %% errors while the rest of the multiplex is clean - the station's feed, not your reception (%.0f s)", label.c_str(), sp, now_ - it->second.since); break;
    case FaultKind::NoData: snprintf(b, sizeof b, "%s: no data for %.0f s while the multiplex is clean - off air at the source", label.c_str(), now_ - it->second.lastData); break;
    default: snprintf(b, sizeof b, "%s: %.0f %% errors with the whole signal - reception", label.c_str(), sp); break;
    }
    return b;
}

} // namespace smart5
} // namespace dect2
