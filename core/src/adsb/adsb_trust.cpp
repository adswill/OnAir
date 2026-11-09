#include "dect2/adsb/adsb_trust.h"
#include "dect2/adsb/adsb_reg.h"
#include <algorithm>
#include <cctype>
#include <cmath>

namespace dect2 {

namespace {
double distNm(double la1, double lo1, double la2, double lo2) {
    const double r = M_PI / 180, p1 = la1 * r, p2 = la2 * r, dl = (lo2 - lo1) * r;
    const double c = std::sin(p1) * std::sin(p2) + std::cos(p1) * std::cos(p2) * std::cos(dl);
    return std::acos(std::max(-1.0, std::min(1.0, c))) * 3440.065;
}
int flagIndex(unsigned f) { int i = 0; while (f > 1) { f >>= 1; i++; } return i; }
// What a finding costs, the order of AdsbTrustFlag
const int kPenalty[7] = {55, 25, 45, 40, 25, 45, 35};
const char* const kReason[7] = {
    "much louder than aircraft at that range", "much weaker than aircraft at that range", "claimed position is beyond the radio horizon",
    "position jumps", "claimed speed does not match the positions", "registration callsign does not match the address", "same callsign on another address far away",
};
std::string squeeze(const std::string& s) {
    std::string o;
    for (char c : s) if (std::isalnum((unsigned char)c)) o += (char)std::toupper((unsigned char)c);
    return o;
}
// A callsign that is a registration in the blocks where the address follows from the registration: does the address agree?
bool registrationClash(const std::string& callsign, uint32_t icao) {
    const std::string c = squeeze(callsign);
    if (c.size() < 2) return false;
    uint32_t want = 0;
    if (c[0] == 'N' && c[1] >= '1' && c[1] <= '9' && c.size() <= 6) {
        if (!adsbIcaoFromNNumber(c, want)) return false;
        return want != icao;
    }
    if (c.size() == 5 && c[0] == 'C' && (c[1] == 'F' || c[1] == 'G') && std::isalpha((unsigned char)c[2]) && std::isalpha((unsigned char)c[3]) && std::isalpha((unsigned char)c[4])) {
        if (!adsbIcaoFromCanada("C-" + c.substr(1), want)) return false;
        return want != icao;
    }
    if (c.size() == 5 && c[0] == 'V' && c[1] == 'H' && std::isalpha((unsigned char)c[2]) && std::isalpha((unsigned char)c[3]) && std::isalpha((unsigned char)c[4])) {
        if (!adsbIcaoFromAustralia("VH-" + c.substr(2), want)) return false;
        return want != icao;
    }
    return false;
}
int binOf(double v) { return (int)std::floor((v + 80.0) * 2.0); }
}

void AdsbTrustModel::clear() {
    for (auto& s : sector_) s = Hist();
    all_ = Hist();
    total_ = 0; learned_.clear(); rec_.clear();
}

void AdsbTrustModel::shiftTime(double dt) {
    for (auto& kv : rec_) {
        Rec& r = kv.second;
        r.lastSeen += dt; r.lastT += dt; r.baseT += dt; r.learnT += dt;
        for (double& h : r.hit) h += dt;
        for (double& h : r.seen) h += dt;
    }
    for (auto& kv : learned_) kv.second += dt;
}

void AdsbTrustModel::tick(double now) {
    for (auto it = rec_.begin(); it != rec_.end();) { if (now - it->second.lastSeen > 600) it = rec_.erase(it); else ++it; }
}

bool AdsbTrustModel::pct(const Hist& h, double p, double& out) const {
    if (h.n == 0) return false;
    const uint32_t want = (uint32_t)std::ceil(p * h.n);
    uint32_t c = 0;
    for (int i = 0; i < kBins; i++) { c += h.bin[i]; if (c >= std::max<uint32_t>(want, 1)) { out = i * 0.5 - 80.0 + 0.25; return true; } }
    return false;
}

bool AdsbTrustModel::curvePercentile(double bearingDeg, double p, double& out) const {
    double b = std::fmod(bearingDeg, 360.0); if (b < 0) b += 360.0;
    const Hist& s = sector_[std::min(kSectors - 1, (int)(b / 30.0))];
    return pct(s.n >= 80 ? s : all_, p, out);     // a sector with too little behind it uses the whole sky
}

bool AdsbTrustModel::calibrated() const { return total_ >= 150 && learned_.size() >= 6; }

void AdsbTrustModel::note(Rec& r, int f, double t, int need) {
    const int i = flagIndex((unsigned)f);
    if (t - r.seen[i] > kTrustHoldSec) r.count[i] = 0;   // findings further apart than the hold time do not add up
    r.seen[i] = t;
    if (++r.count[i] >= need) r.hit[i] = t;
}

void AdsbTrustModel::observe(const AdsbTrustObs& o) {
    Rec& r = rec_[o.icao];
    const double t = o.timeSec;
    r.lastSeen = t;
    r.positions++;
    if (!o.callsign.empty()) r.callsign = o.callsign;

    // level against the curve of this station
    bool levelSuspect = false;
    if (o.hasRange && o.rangeNm >= 5 && o.hasLevel && !o.relayed && !o.ground && o.confirmed) {
        const double v = o.levelDbfs + 20.0 * std::log10(o.rangeNm);
        if (calibrated()) {
            double hi = 0, lo = 0;
            if (curvePercentile(o.bearingDeg, 0.98, hi) && curvePercentile(o.bearingDeg, 0.02, lo)) {
                if (v > hi + 10.0) { note(r, kTrustTooLoud, t, 3); levelSuspect = true; }
                else if (v < lo - 16.0) { note(r, kTrustTooWeak, t, 5); levelSuspect = true; }
            }
        }
        const bool anyFinding = t - r.hit[flagIndex(kTrustJump)] < kTrustHoldSec || t - r.hit[flagIndex(kTrustTooLoud)] < kTrustHoldSec;
        if (!levelSuspect && !anyFinding && t - r.learnT >= 10.0) {  // one sample per aircraft per 10 s, so a loud one cannot fill the curve
            const int b = std::max(0, std::min(kBins - 1, binOf(v)));
            double br = std::fmod(o.bearingDeg, 360.0); if (br < 0) br += 360.0;
            Hist& s = sector_[std::min(kSectors - 1, (int)(br / 30.0))];
            s.bin[b]++; s.n++; all_.bin[b]++; all_.n++;
            total_++; r.learnT = t; learned_[o.icao] = t;
        }
    }
    // radio horizon: 1.23 sqrt(h ft) in nm for the aircraft, plus 100 nm for the antenna height and ordinary refraction
    if (o.hasRange && o.hasAlt && !o.relayed && o.hasLevel && !o.ground && o.confirmed && o.altFt > 0) {
        if (o.rangeNm > 1.23 * std::sqrt((double)o.altFt) + 100.0) note(r, kTrustHorizon, t, 3);
    }
    // kinematics
    if (r.lastT > -1e8 && t > r.lastT && !o.ground) {
        const double dt = t - r.lastT, d = distNm(r.lastLat, r.lastLon, o.lat, o.lon);
        if (dt <= 120 && d > 2.0 && d / (dt / 3600.0) > 1500.0) note(r, kTrustJump, t, 1);
    }
    r.lastLat = o.lat; r.lastLon = o.lon; r.lastT = t;
    // the claimed speed against the speed of the positions, over at least half a minute
    if (r.baseT < -1e8) { r.baseLat = o.lat; r.baseLon = o.lon; r.baseT = t; r.speedSum = 0; r.speedN = 0; }
    if (o.hasGroundSpeed) { r.speedSum += o.groundSpeedKt; r.speedN++; }
    if (t - r.baseT >= 30.0) {
        const double dt = t - r.baseT, d = distNm(r.baseLat, r.baseLon, o.lat, o.lon);
        if (r.speedN >= 3 && dt <= 120 && !o.ground) {
            const double claimed = r.speedSum / r.speedN, moved = d / (dt / 3600.0);
            if (std::fabs(claimed - moved) > std::max(120.0, 0.5 * std::max(claimed, moved))) note(r, kTrustSpeed, t, 2);
        }
        r.baseLat = o.lat; r.baseLon = o.lon; r.baseT = t; r.speedSum = 0; r.speedN = 0;
    }
    // identity
    if (!r.callsign.empty() && !o.relayed) {
        if (registrationClash(r.callsign, o.icao)) note(r, kTrustRegistration, t, 1);
        for (auto& kv : rec_) {
            if (kv.first == o.icao || kv.second.callsign != r.callsign) continue;
            Rec& q = kv.second;
            if (t - q.lastT <= 60 && distNm(q.lastLat, q.lastLon, o.lat, o.lon) > 50) { note(r, kTrustDuplicate, t, 2); note(q, kTrustDuplicate, t, 2); }
        }
    }
}

AdsbTrustVerdict AdsbTrustModel::verdict(uint32_t icao, double now) const {
    AdsbTrustVerdict v;
    auto it = rec_.find(icao);
    if (it == rec_.end() || it->second.positions < 2) return v;
    int s = 100;
    for (int i = 0; i < 7; i++) {
        if (now - it->second.hit[i] > kTrustHoldSec) continue;
        s -= kPenalty[i];
        v.flags |= 1u << i;
        if (!v.why.empty()) v.why += ", ";
        v.why += kReason[i];
    }
    v.score = std::max(0, s);
    return v;
}

} // namespace dect2
