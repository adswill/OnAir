// DMR subscriber TX slot timing (see dmr_slot_sync.h): a fit of the repeater's outbound TDMA frame from clean CACH reports, and the inbound
// burst times derived from it per TS 102 361-1 clauses 4.2, 4.5 and 5.1.1.
#include "dect2/dmr/dmr_slot_sync.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <vector>

namespace dect2 {

double dmrSteadySec() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

DmrSlotClock& DmrSlotClock::global() {
    static DmrSlotClock c;
    return c;
}

void DmrSlotClock::observe(double centreSec, int slot, int at) {
    if (slot != 1 && slot != 2) return;
    std::lock_guard<std::mutex> lk(mu_);
    // a clock that went back (a new file, a restarted receiver): start again
    if (!obs_.empty() && centreSec < obs_.back().t - 1.0) obs_.clear();
    obs_.push_back({centreSec, slot, at});
    if (at >= 0) busy_[slot - 1] = at;
    while (obs_.size() > 400) obs_.pop_front();
}

void DmrSlotClock::clear() {
    std::lock_guard<std::mutex> lk(mu_);
    obs_.clear();
    busy_[0] = busy_[1] = -1;
}

namespace {
// x into [-h, h) for a period 2h
double wrap(double x, double period) {
    x = std::fmod(x, period);
    if (x >= period / 2) x -= period;
    if (x < -period / 2) x += period;
    return x;
}
} // namespace

DmrSlotClock::Fit DmrSlotClock::fit(double windowSec) const {
    Fit f;
    std::vector<Obs> o;
    {
        std::lock_guard<std::mutex> lk(mu_);
        f.busy[0] = busy_[0]; f.busy[1] = busy_[1];
        if (obs_.empty()) return f;
        const double newest = obs_.back().t;
        for (const Obs& x : obs_) if (x.t >= newest - windowSec) o.push_back(x);
        f.lastSec = newest;
    }
    f.seen = (int)o.size();
    if (o.size() < 8) return f;
    // every observation as the centre of an outbound slot 1 burst (slot 2 is 30 ms after slot 1)
    const double ref = o.front().t - (o.front().slot == 2 ? kDmrSlotSec : 0);
    std::vector<double> ph(o.size()), c1(o.size());
    for (size_t i = 0; i < o.size(); i++) {
        c1[i] = o[i].t - (o[i].slot == 2 ? kDmrSlotSec : 0);
        ph[i] = wrap(c1[i] - ref, kDmrFrameSec);
    }
    // the phase most observations agree on: the circular mean of those within 3 ms of the median, which drops a TC that decoded wrong
    std::vector<double> s = ph;
    std::nth_element(s.begin(), s.begin() + (long)(s.size() / 2), s.end());
    const double med = s[s.size() / 2];
    // least squares of centre = a + frame * k over the consistent observations (k = frame number from the reference)
    double sk = 0, st = 0, skk = 0, skt = 0;
    int n = 0;
    std::vector<std::pair<double, double>> kt;
    for (size_t i = 0; i < o.size(); i++) {
        if (std::fabs(wrap(ph[i] - med, kDmrFrameSec)) > 0.003) continue;
        const double k = std::round((c1[i] - ref - med) / kDmrFrameSec);
        const double t = c1[i] - ref;
        kt.push_back({k, t});
        sk += k; st += t; skk += k * k; skt += k * t; n++;
    }
    if (n < 8 || n * 10 < (int)o.size() * 6) return f;
    const double den = n * skk - sk * sk;
    double frame = kDmrFrameSec, a;
    // the frame length is fitted only over a long span (a repeater's clock is within a few ppm; a short span would only add noise)
    if (den > 0 && kt.back().first - kt.front().first > 20) {
        frame = (n * skt - sk * st) / den;
        if (std::fabs(frame / kDmrFrameSec - 1) > 2e-4) frame = kDmrFrameSec;   // more than 200 ppm: not a fit, keep the nominal
    }
    a = (st - frame * sk) / n;
    double e2 = 0;
    for (const auto& p : kt) { const double r = p.second - (a + frame * p.first); e2 += r * r; }
    f.rmsMs = std::sqrt(e2 / n) * 1e3;
    f.ts1CentreSec = ref + a;
    f.frameSec = frame;
    f.used = n;
    f.ok = f.rmsMs < 1.0;
    // move the reference close to the newest observation so that a later prediction does not multiply a frame error by many frames
    const double kNew = std::floor((f.lastSec - f.ts1CentreSec) / frame);
    f.ts1CentreSec += kNew * frame;
    return f;
}

double DmrSlotClock::inboundBurstStart(const Fit& f, int logicalChannel, bool aligned, double earliest) {
    const int in = inboundSlot(logicalChannel, aligned);
    // centre of inbound slot N = centre of outbound slot N + 30 ms; outbound slot 2 is 30 ms after outbound slot 1
    const double centre0 = f.ts1CentreSec + (in == 2 ? kDmrSlotSec : 0) + kDmrSlotSec;
    const double start0 = centre0 - kDmrBurstCentreSym / 4800.0;
    const double k = std::ceil((earliest - start0) / f.frameSec - 1e-9);
    return start0 + k * f.frameSec;
}

DmrTxLead dmrTxLeadFor(const DmrSlotClock::Fit& f, int logicalChannel, bool aligned, double txStartSec, double genDelaySec, double maxAgeSec) {
    DmrTxLead l;
    l.inboundSlot = DmrSlotClock::inboundSlot(logicalChannel, aligned);
    l.ageSec = f.lastSec > 0 ? txStartSec - f.lastSec : 1e9;
    l.rmsMs = f.rmsMs;
    l.busy = f.busy[l.inboundSlot - 1];
    if (!f.ok || l.ageSec > maxAgeSec || l.ageSec < -1.0) return l;
    const double first = DmrSlotClock::inboundBurstStart(f, logicalChannel, aligned, txStartSec + genDelaySec);
    l.leadSec = std::max(0.0, first - txStartSec - genDelaySec);
    l.ok = true;
    return l;
}

} // namespace dect2
