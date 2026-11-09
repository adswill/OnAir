#include "dect2/iridium/iridium_doppler.h"
#include <algorithm>
#include <cmath>

namespace dect2 {

void IridiumDoppler::setObserver(const Observer& o) { std::lock_guard<std::mutex> lk(mu_); obs_ = o; haveObs_ = true; fit_.clear(); lastRefresh_ = -1e18; }

void IridiumDoppler::setTles(const std::vector<Tle>& all) {
    std::lock_guard<std::mutex> lk(mu_);
    cand_.clear();
    for (const Tle& t : all) {
        std::string n = t.name;
        for (auto& c : n) c = (char)std::toupper((unsigned char)c);
        if (n.rfind("IRIDIUM", 0) != 0 || n.find("DEB") != std::string::npos) continue;
        Cand c;
        c.tle = t;
        if (!c.sgp.init(t)) continue;
        cand_.push_back(std::move(c));
    }
    fit_.clear(); lastRefresh_ = -1e18;
}
size_t IridiumDoppler::satellites() const { std::lock_guard<std::mutex> lk(mu_); return cand_.size(); }

void IridiumDoppler::clear() { std::lock_guard<std::mutex> lk(mu_); bursts_.clear(); beams_.clear(); fit_.clear(); calKnown_ = false; calOff_ = 0; lastRefresh_ = -1e18; }

void IridiumDoppler::addBurst(double unixSec, int satId, double freqHz) {
    if (satId < 0 || satId > 127 || !std::isfinite(unixSec + freqHz)) return;
    const double d = freqHz - kIridiumRingHz;
    if (std::fabs(d) > 60000) return;                    // not the ring channel
    std::lock_guard<std::mutex> lk(mu_);
    auto& v = bursts_[satId];
    v.push_back({unixSec, d});
    while ((int)v.size() > cfg_.maxObs || (!v.empty() && unixSec - v.front().t > cfg_.windowSec)) v.erase(v.begin());
}

void IridiumDoppler::addBeam(double unixSec, int satId, int beam, double lat, double lon) {
    if (satId < 0 || beam < 0 || !std::isfinite(lat + lon) || std::fabs(lat) > 90 || std::fabs(lon) > 180) return;
    std::lock_guard<std::mutex> lk(mu_);
    Beam b; b.t = unixSec; b.lat = lat; b.lon = lon;
    beams_[{satId, beam}] = b;
}

// For every candidate satellite above the horizon at each measurement: the residual of (measured - predicted), then its robust spread. With useOffset the
// receiver's error is the median residual (free); else it is `off`.
IridiumDoppler::Fit IridiumDoppler::fitSat(const std::vector<Obs>& obs, bool useOffset, double off) const {
    Fit best;
    for (size_t ci = 0; ci < cand_.size(); ci++) {
        std::vector<double> r;
        r.reserve(obs.size());
        bool ok = true;
        for (const Obs& o : obs) {
            Look lk;
            if (!lookAt(cand_[ci].sgp, obs_, o.t, lk) || lk.elDeg < cfg_.minElDeg) { ok = false; break; }
            r.push_back(o.f - dopplerHz(kIridiumRingHz, lk.rangeRateKmS));
        }
        if (!ok) continue;
        double b = off;
        if (useOffset) {
            std::vector<double> s = r;
            std::nth_element(s.begin(), s.begin() + (long)s.size() / 2, s.end());
            b = s[s.size() / 2];
            if (std::fabs(b) > cfg_.maxOffsetHz) continue;
        }
        double ss = 0;
        for (double x : r) ss += (x - b) * (x - b);
        const double rms = std::sqrt(ss / (double)r.size());
        if (rms < best.rms) { best.second = best.rms; best.rms = rms; best.cand = (int)ci; best.off = b; best.n = (int)r.size(); }
        else if (rms < best.second) best.second = rms;
    }
    return best;
}

void IridiumDoppler::refreshLocked(double now) {
    if (!haveObs_ || cand_.empty() || now - lastRefresh_ < 2) return;
    lastRefresh_ = now;
    // forget old measurements
    for (auto it = bursts_.begin(); it != bursts_.end();) {
        auto& v = it->second;
        while (!v.empty() && now - v.front().t > cfg_.windowSec) v.erase(v.begin());
        it = v.empty() ? bursts_.erase(it) : std::next(it);
    }
    std::map<int, Fit> f;
    std::vector<double> offs;
    for (const auto& kv : bursts_) {
        const auto& v = kv.second;
        if ((int)v.size() < cfg_.minObs || v.back().t - v.front().t < cfg_.minSpanSec) continue;
        Fit x = fitSat(v, true, 0);
        if (x.cand >= 0) { f[kv.first] = x; if (x.second >= cfg_.sureMargin * x.rms + 60) offs.push_back(x.off); }
    }
    if (offs.size() >= 1) {   // the receiver's own error: the median of the confident ones
        std::nth_element(offs.begin(), offs.begin() + (long)offs.size() / 2, offs.end());
        calOff_ = offs[offs.size() / 2]; calKnown_ = true;
        // a second pass for the unsure ones with the error fixed (a curve that fits only with a different offset is another satellite)
        for (auto& kv : f) {
            const Fit& x = kv.second;
            if (x.second >= cfg_.sureMargin * x.rms + 60) continue;
            const Fit y = fitSat(bursts_[kv.first], false, calOff_);
            if (y.cand >= 0 && y.rms < 600) kv.second = y;
        }
    }
    fit_.swap(f);
}

std::vector<IridiumIdent> IridiumDoppler::identify(double now) {
    std::lock_guard<std::mutex> lk(mu_);
    refreshLocked(now);
    std::vector<IridiumIdent> out;
    for (const auto& kv : fit_) {
        const Fit& f = kv.second;
        const Cand& c = cand_[(size_t)f.cand];
        IridiumIdent d;
        d.satId = kv.first; d.norad = c.tle.norad; d.name = c.tle.name; d.obs = f.n; d.rmsHz = f.rms;
        d.confidence = f.second > 1e17 ? 1.0 : std::min(1.0, std::max(0.0, (f.second - f.rms) / (f.second + 60.0)));
        d.sure = f.second >= cfg_.sureMargin * f.rms + 60;
        const auto& v = bursts_[kv.first];
        d.lastUnix = v.empty() ? 0 : v.back().t;
        Look lk;
        if (lookAt(c.sgp, obs_, now, lk)) { d.azDeg = lk.azDeg; d.elDeg = lk.elDeg; d.rangeKm = lk.rangeKm; d.dopplerHz = dopplerHz(kIridiumRingHz, lk.rangeRateKmS); }
        out.push_back(d);
    }
    std::sort(out.begin(), out.end(), [](const IridiumIdent& a, const IridiumIdent& b) { return a.confidence > b.confidence; });
    return out;
}

double IridiumDoppler::offsetHz() const { std::lock_guard<std::mutex> lk(mu_); return calKnown_ ? calOff_ : 0; }
bool IridiumDoppler::offsetKnown() const { std::lock_guard<std::mutex> lk(mu_); return calKnown_; }

std::vector<IridiumDoppler::SkyPoint> IridiumDoppler::sky(double now) {
    std::lock_guard<std::mutex> lk(mu_);
    refreshLocked(now);
    std::vector<SkyPoint> out;
    if (!haveObs_) return out;
    std::map<int, int> idOf;
    for (const auto& kv : fit_) idOf[cand_[(size_t)kv.second.cand].tle.norad] = kv.first;
    for (const Cand& c : cand_) {
        Look lk;
        if (!lookAt(c.sgp, obs_, now, lk) || lk.elDeg < 0) continue;
        SkyPoint p;
        p.norad = c.tle.norad; p.name = c.tle.name; p.azDeg = lk.azDeg; p.elDeg = lk.elDeg; p.rangeKm = lk.rangeKm; p.dopplerHz = dopplerHz(kIridiumRingHz, lk.rangeRateKmS);
        auto it = idOf.find(c.tle.norad);
        p.satId = it == idOf.end() ? -1 : it->second;
        out.push_back(p);
    }
    return out;
}

IridiumBeamCircle IridiumDoppler::beamCircle(int satId, int beam, double now) {
    std::lock_guard<std::mutex> lk(mu_);
    IridiumBeamCircle o;
    auto it = beams_.find({satId, beam});
    if (it == beams_.end()) return o;
    const Beam& b = it->second;
    o.ageSec = now - b.t;
    if (o.ageSec < -5 || o.ageSec > cfg_.beamMaxAgeSec) return o;
    o.valid = true; o.lat = b.lat; o.lon = b.lon;
    auto f = fit_.find(satId);
    if (f != fit_.end() && o.ageSec > 1) {   // the footprint moves with the satellite: carry the centre by the sub-point's motion
        double la0, lo0, a0, la1, lo1, a1;
        const Sgp4& s = cand_[(size_t)f->second.cand].sgp;
        if (subPoint(s, b.t, la0, lo0, a0) && subPoint(s, now, la1, lo1, a1)) {
            double dlo = lo1 - lo0;
            while (dlo > 180) dlo -= 360;
            while (dlo < -180) dlo += 360;
            o.lat = std::max(-90.0, std::min(90.0, b.lat + (la1 - la0)));
            o.lon = b.lon + dlo;
            while (o.lon > 180) o.lon -= 360;
            while (o.lon < -180) o.lon += 360;
            o.moved = true;
        }
    }
    o.radiusKm = 250 + (o.moved ? 0 : 7.0 * o.ageSec);   // not carried along: the satellite's 7 km/s widens the circle
    return o;
}

} // namespace dect2
