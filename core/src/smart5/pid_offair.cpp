// Service off-air detector (see pid_offair.h).
#include "dect2/smart5/pid_offair.h"
#include <cstdio>

namespace dect2 {
namespace smart5 {

void PidOffAir::mapPid(unsigned service, uint16_t pid, bool video) {
    Pid& p = pids_[pid];
    p.service = service; p.video = video;
}

void PidOffAir::tick(double now, double sec) {
    if (sec <= 0) sec = 1;
    const double tot = total_ / sec;
    // the mux is fine when packets keep arriving near their usual rate with few continuity errors
    const bool ccOk = total_ == 0 ? false : 100.0 * cc_ / total_ < p_.ccBadPct;
    muxFine_ = ccOk && (totalEwma_ <= 0 || tot > 0.5 * totalEwma_);
    totalEwma_ = totalEwma_ <= 0 ? tot : 0.9 * totalEwma_ + 0.1 * tot;
    for (auto& kv : pids_) {
        Pid& p = kv.second;
        auto it = cur_.find(kv.first);
        const double r = (it == cur_.end() ? 0 : it->second) / sec;   // packets per second
        p.nowRate = r;
        if (!muxFine_) { p.lowFor = 0; continue; }   // nothing is judged while the signal itself is bad
        const bool low = p.learned >= p_.learnSec && r < p_.dropFrac * p.ewma;
        if (low) {
            if (p.lowFor == 0) p.since = now - sec;
            p.lowFor += sec;
            if (!p.off && p.lowFor >= p_.holdSec) {
                p.off = true;
                log_.push_back({p.service, kv.first, p.video, p.since, p.ewma * 188 * 8 / 1000, r * 188 * 8 / 1000});
            }
        } else {
            p.lowFor = 0; p.off = false;
            // learn only from normal seconds, so a stopped encoder does not teach a low rate
            p.ewma = p.ewma <= 0 ? r : p.ewma + (r - p.ewma) * (p.learned < p_.learnSec ? 1.0 / (p.learned + 1) : 0.05);
            if (r > 0) p.learned += sec;
        }
    }
    cur_.clear(); total_ = cc_ = 0;
}

std::vector<OffAirEvent> PidOffAir::offAir() const {
    std::vector<OffAirEvent> v;
    for (auto& kv : pids_)
        if (kv.second.off) v.push_back({kv.second.service, kv.first, kv.second.video, kv.second.since, kv.second.ewma * 188 * 8 / 1000, kv.second.nowRate * 188 * 8 / 1000});
    return v;
}

std::string PidOffAir::text(const OffAirEvent& e) const {
    char b[200];
    snprintf(b, sizeof b, "service %u: %s PID 0x%04X %s (usual %.0f kbit/s, now %.0f) while the multiplex is fine - off air at the source",
             e.service, e.video ? "video" : "audio", e.pid, e.nowKbps < 1 ? "stopped" : "almost stopped", e.usualKbps, e.nowKbps);
    return b;
}

} // namespace smart5
} // namespace dect2
