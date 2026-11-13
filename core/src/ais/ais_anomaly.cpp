// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/ais/ais_anomaly.h"
#include "dect2/ais/ais_alerts.h"
#include "dect2/ais/ais_mid.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace dect2 {
namespace {

constexpr double kPi = 3.14159265358979323846;

double bearingDeg(double lat1, double lon1, double lat2, double lon2) {
    const double r = kPi / 180, dl = (lon2 - lon1) * r;
    const double y = std::sin(dl) * std::cos(lat2 * r);
    const double x = std::cos(lat1 * r) * std::sin(lat2 * r) - std::sin(lat1 * r) * std::cos(lat2 * r) * std::cos(dl);
    return std::fmod(std::atan2(y, x) / r + 360.0, 360.0);
}
// where a point that started at (lat, lon) with course and speed is after sec seconds (flat earth: short legs only)
void project(double lat, double lon, double sog, double cog, double sec, double& la, double& lo) {
    la = lat; lo = lon;
    if (sog < 0.3 || sog > 102.2 || cog < 0 || cog >= 360) return;
    const double nm = sog * sec / 3600.0, r = kPi / 180;
    la = lat + nm * std::cos(cog * r) / 60.0;
    lo = lon + nm * std::sin(cog * r) / (60.0 * std::max(0.05, std::cos(lat * r)));
}
int editDistance(const std::string& a, const std::string& b) {
    std::vector<int> p(b.size() + 1), c(b.size() + 1);
    for (size_t j = 0; j <= b.size(); j++) p[j] = (int)j;
    for (size_t i = 1; i <= a.size(); i++) {
        c[0] = (int)i;
        for (size_t j = 1; j <= b.size(); j++) c[j] = std::min({p[j] + 1, c[j - 1] + 1, p[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1)});
        p.swap(c);
    }
    return p[b.size()];
}
std::string clean(const std::string& s) {
    std::string o;
    for (char c : s) if (c != '@' && c != ' ') o += (char)std::toupper((unsigned char)c);
    return o;
}
// the highest speed (knots) that is believable for a ship type of ITU-R M.1371-5 table 50; 0 = no limit known
double typeLimit(int t) {
    if (t == 30) return 25;                       // fishing
    if (t == 31 || t == 32) return 22;            // towing
    if (t == 33) return 16;                       // dredging
    if (t == 36) return 30;                       // sailing
    if (t >= 60 && t <= 69) return 50;            // passenger
    if (t >= 70 && t <= 79) return 32;            // cargo
    if (t >= 80 && t <= 89) return 28;            // tanker
    return 0;
}

} // namespace

void AisAnomalyEngine::clear() {
    st_.clear(); lastScan_ = 0; raised_ = 0;
    for (int s = 0; s < kSectors; s++) { ringN_[s] = 0; ringPos_[s] = 0; }
}

void AisAnomalyEngine::sector(double range, double bearing) {
    const int s = std::min(kSectors - 1, std::max(0, (int)(bearing / 10.0)));
    ring_[s][ringPos_[s]] = (float)range;
    ringPos_[s] = (ringPos_[s] + 1) % kRing;
    if (ringN_[s] < kRing) ringN_[s]++;
}

double AisAnomalyEngine::sectorRangeNm(double bearing) const {
    auto own = [&](int s) -> double {
        s = (s + kSectors) % kSectors;
        if (ringN_[s] < cfg_.coverageMinSamples) return 0;
        std::vector<float> v(ring_[s], ring_[s] + ringN_[s]);
        const size_t k = std::min(v.size() - 1, v.size() * 9 / 10);
        std::nth_element(v.begin(), v.begin() + (long)k, v.end());
        return v[k];
    };
    const int s = std::min(kSectors - 1, std::max(0, (int)(std::fmod(bearing + 360.0, 360.0) / 10.0)));
    const double r = own(s);
    if (r > 0) return r;
    const double l = own(s - 1), h = own(s + 1);   // a sector with too few reports takes the mean of its two neighbours, when both are known
    return (l > 0 && h > 0) ? 0.5 * (l + h) : 0;
}

AisAnomalyEngine::Stats AisAnomalyEngine::stats() const {
    Stats o;
    o.tracked = st_.size(); o.raised = raised_;
    for (int s = 0; s < kSectors; s++) {
        const double r = sectorRangeNm(s * 10 + 5);
        if (r > 0) { o.sectorsKnown++; o.maxRangeNm = std::max(o.maxRangeNm, r); }
    }
    return o;
}

bool AisAnomalyEngine::fits(const Track& t, double lat, double lon, double now, double maxKn, double slackNm) const {
    const double dt = std::max(0.0, now - t.utc);
    return aisDistanceNm(t.lat, t.lon, lat, lon) <= slackNm + maxKn * dt / 3600.0;
}

double AisAnomalyEngine::medianGap(const State& s) const {
    if (s.gapN < 3) return 0;
    double g[8];
    const int n = std::min(8, s.gapN);
    for (int i = 0; i < n; i++) g[i] = s.gaps[i];
    std::sort(g, g + n);
    return g[n / 2];
}

void AisAnomalyEngine::raise(std::vector<AisAnomaly>& out, State& s, Kind k, const AisVessel& v, double now, int sev, const std::string& text) {
    if (now - s.cool[k] < cfg_.cooldownSec && s.cool[k] != 0) return;
    s.cool[k] = now;
    static const char* const kn[KKinds] = {"jump", "clash", "aton_moved", "aton_off_position", "mmsi_class", "speed_type", "name_change", "dark", "reappeared"};
    AisAnomaly a;
    a.utc = now; a.kind = kn[k]; a.severity = sev; a.mmsi = v.mmsi; a.name = v.name; a.lat = v.lat; a.lon = v.lon; a.text = text;
    out.push_back(a);
    raised_++;
}

std::vector<AisAnomaly> AisAnomalyEngine::update(const AisVessel& v, double now) {
    std::vector<AisAnomaly> out;
    if (!cfg_.enabled || !v.hasPos || v.mmsi == 0 || !std::isfinite(v.lat + v.lon)) return out;
    if (st_.size() > 40000 && st_.find(v.mmsi) == st_.end()) return out;
    State& s = st_[v.mmsi];
    const AisMmsiInfo mi = aisMmsiInfo(v.mmsi);
    const bool device = mi.kind == AIS_MMSI_SART || mi.kind == AIS_MMSI_MOB || mi.kind == AIS_MMSI_EPIRB;
    char b[240];
    // ---- back after a silence
    if (s.darkRaised && now - s.lastUtc > 60) {
        snprintf(b, sizeof b, "%s is back after %.0f min", v.name.empty() ? std::to_string(v.mmsi).c_str() : v.name.c_str(), (now - s.lastUtc) / 60.0);
        s.cool[KBack] = 0;
        raise(out, s, KBack, v, now, 0, b);
    }
    if (s.darkRaised) { s.darkRaised = false; s.cool[KDark] = 0; }

    // ---- position tracks: jump and clash
    const bool lr = v.lrPosition;
    const double maxKn = v.cls == AIS_CLASS_SAR ? 500 : (v.shipType >= 40 && v.shipType <= 49) ? cfg_.hscMaxKn : cfg_.maxSpeedKn;
    if (!s.a.valid) {
        s.a = Track{v.lat, v.lon, now, v.sog, v.cog, 1, true, lr};
        s.firstUtc = now;
    } else if (now - s.a.utc >= 1.0 || (s.b.valid && now - s.b.utc >= 1.0)) {
        const double slackA = 0.3 + (lr || s.a.lr ? 1.0 : 0.0);
        const bool fa = fits(s.a, v.lat, v.lon, now, maxKn, slackA);
        const bool fb = s.b.valid && fits(s.b, v.lat, v.lon, now, maxKn, 0.3 + (lr || s.b.lr ? 1.0 : 0.0));
        auto upd = [&](Track& t) { t.lat = v.lat; t.lon = v.lon; t.utc = now; t.sog = v.sog; t.cog = v.cog; t.hits++; t.lr = lr; };
        const double da = aisDistanceNm(s.a.lat, s.a.lon, v.lat, v.lon), db = s.b.valid ? aisDistanceNm(s.b.lat, s.b.lon, v.lat, v.lon) : 1e18;
        if (fa && (!fb || da <= db)) upd(s.a);
        else if (fb) upd(s.b);
        else {   // fits neither track
            const double d = aisDistanceNm(s.a.lat, s.a.lon, v.lat, v.lon);
            if (cfg_.jump && d > cfg_.jumpMinNm) {
                const double dt = std::max(1.0, now - s.a.utc);
                snprintf(b, sizeof b, "position jumped %.1f nm in %.0f s (%.0f kn): not a speed this ship can make", d, dt, d / (dt / 3600.0));
                raise(out, s, KJump, v, now, 1, b);
            }
            // the report starts (or replaces) the second track
            if (!s.b.valid || s.b.utc <= s.a.utc) s.b = Track{v.lat, v.lon, now, v.sog, v.cog, 1, true, lr};
            else { s.a = s.b; s.b = Track{v.lat, v.lon, now, v.sog, v.cog, 1, true, lr}; }
        }
        if (s.b.valid && now - s.b.utc > cfg_.clashWindowSec * 2) { s.b = Track(); s.clashSeen = 0; }
        if (s.b.valid && s.a.valid && cfg_.clash && s.a.hits >= cfg_.clashHits && s.b.hits >= cfg_.clashHits &&
            std::fabs(s.a.utc - s.b.utc) <= cfg_.clashWindowSec && now - std::min(s.a.utc, s.b.utc) <= cfg_.clashWindowSec) {
            const double sep = aisDistanceNm(s.a.lat, s.a.lon, s.b.lat, s.b.lon);
            if (sep > cfg_.clashSepNm) {
                snprintf(b, sizeof b, "MMSI %u reports from two places %.0f nm apart at the same time: duplicate or spoofed identity", (unsigned)v.mmsi, sep);
                raise(out, s, KClash, v, now, 2, b);
            }
        }
    }

    // ---- aids to navigation do not move
    if (cfg_.aton && v.cls == AIS_CLASS_ATON) {
        if (!s.anchored) { s.anchored = true; s.anchorLat = v.lat; s.anchorLon = v.lon; }
        else {
            const double d = aisDistanceNm(s.anchorLat, s.anchorLon, v.lat, v.lon);
            if (v.offPosition) { snprintf(b, sizeof b, "aid to navigation reports itself off position (%.2f nm from where it was first heard)", d); raise(out, s, KAtonOff, v, now, 1, b); }
            else if (d > cfg_.atonMoveNm) { snprintf(b, sizeof b, "aid to navigation is %.2f nm from where it was first heard and does not report off position", d); raise(out, s, KAton, v, now, 1, b); }
        }
    }

    // ---- identity against class and MMSI
    if (cfg_.mmsiClass && !device) {
        const bool shipSender = v.cls == AIS_CLASS_A || v.cls == AIS_CLASS_B;
        const bool bad = (shipSender && (mi.kind == AIS_MMSI_COAST || mi.kind == AIS_MMSI_GROUP || mi.kind == AIS_MMSI_ATON)) ||
                         ((v.cls == AIS_CLASS_ATON || v.cls == AIS_CLASS_BASE) && mi.kind == AIS_MMSI_SHIP);
        if (bad) {
            static const char* const cn[] = {"Class A", "Class B", "base station", "aid to navigation", "SAR aircraft", "other"};
            snprintf(b, sizeof b, "%s sends as %s: the number is a %s number", std::to_string(v.mmsi).c_str(), cn[v.cls >= 0 && v.cls <= 5 ? v.cls : 5], aisMmsiKindText(mi.kind));
            raise(out, s, KClass, v, now, 1, b);
        }
    }
    if (cfg_.nameChange) {
        const std::string n = clean(v.name);
        if (!n.empty()) {
            if (!s.name.empty() && n != s.name && n.size() >= 3 && s.name.size() >= 3 && n.find(s.name) == std::string::npos && s.name.find(n) == std::string::npos &&
                editDistance(n, s.name) > 3) {
                snprintf(b, sizeof b, "name changed from %s to %s", s.name.c_str(), n.c_str());
                raise(out, s, KName, v, now, 1, b);
            }
            s.name = n;
        }
    }
    if (cfg_.speedType && v.sog >= 0 && v.sog < 102.2f) {
        const double lim = typeLimit(v.shipType);
        if (lim > 0 && v.sog > lim) {
            if (++s.speedRun >= 3) {
                snprintf(b, sizeof b, "%.0f kn for ship type %s (%d): over what such a ship makes (%.0f kn)", (double)v.sog, aisShipTypeText(v.shipType), v.shipType, lim);
                raise(out, s, KSpeed, v, now, 0, b);
            }
        } else s.speedRun = 0;
    }

    // ---- bookkeeping for the coverage model and the dark-ship scan
    if (s.lastUtc > 0 && now - s.lastUtc >= 1.0) { s.gaps[s.gapPos] = now - s.lastUtc; s.gapPos = (s.gapPos + 1) % 8; s.gapN++; }
    s.lastUtc = std::max(s.lastUtc, now);
    s.reports++;
    s.lastSog = v.sog; s.lastNav = v.navStatus; s.cls = v.cls; s.shipType = v.shipType;
    if (cfg_.ownValid && !device && v.cls != AIS_CLASS_ATON && v.cls != AIS_CLASS_BASE && v.cls != AIS_CLASS_SAR) {
        s.lastRange = aisDistanceNm(cfg_.ownLat, cfg_.ownLon, v.lat, v.lon);
        s.lastBearing = bearingDeg(cfg_.ownLat, cfg_.ownLon, v.lat, v.lon);
        s.hasRange = true;
        if (!lr && s.lastRange < 200 && now - s.lastSample >= 120) { s.lastSample = now; sector(s.lastRange, s.lastBearing); }   // one sample per ship per two minutes: a dense nearby ship must not set the range
    }
    if (cfg_.dark && cfg_.ownValid && now - lastScan_ >= 30) { lastScan_ = now; scanDark(now, out); }
    return out;
}

void AisAnomalyEngine::scanDark(double now, std::vector<AisAnomaly>& out) {
    std::vector<uint32_t> drop;
    for (auto& kv : st_) {
        State& s = kv.second;
        const double age = now - s.lastUtc;
        if (age > 48 * 3600.0) { drop.push_back(kv.first); continue; }
        if (s.darkRaised || !s.hasRange || s.reports < 8 || age < cfg_.darkMinutes * 60.0 || age > 24 * 3600.0) continue;
        const double gap = medianGap(s);
        if (age < 20 * std::max(gap, 1.0)) continue;
        if (s.lastSog < 2.0f || s.lastSog > 102.2) continue;                 // it was not under way: a ship that stops transmitting in port is not news
        if (s.lastNav == 1 || s.lastNav == 5 || s.lastNav == 6) continue;     // at anchor, moored, aground
        if (s.cls != AIS_CLASS_A && s.cls != AIS_CLASS_B) continue;
        // where it would be by now, and the range heard on that bearing: a ship that steamed out of the covered sea is not "dark"
        double la = 0, lo = 0;
        const double cap = std::min(age, 3600.0);
        project(s.a.lat, s.a.lon, s.lastSog, s.a.cog, cap, la, lo);
        const double r0 = s.lastRange, b0 = s.lastBearing;
        const double r1 = aisDistanceNm(cfg_.ownLat, cfg_.ownLon, la, lo), b1 = bearingDeg(cfg_.ownLat, cfg_.ownLon, la, lo);
        const double c0 = sectorRangeNm(b0), c1 = sectorRangeNm(b1);
        if (c0 <= 0 && c1 <= 0) continue;                                    // the model does not know this part of the sea yet: no claim
        if ((c0 > 0 && r0 > cfg_.coverageFrac * c0) || (c1 > 0 && r1 > cfg_.coverageFrac * c1)) continue;
        AisVessel v;
        v.mmsi = kv.first; v.lat = s.a.lat; v.lon = s.a.lon; v.name = s.name; v.cls = s.cls;
        char b[240];
        snprintf(b, sizeof b, "went silent %.0f min ago at %.1f nm (%03.0f deg), %.0f kn; this receiver hears out to %.0f nm there", age / 60.0, r0, b0, (double)s.lastSog, c0 > 0 ? c0 : c1);
        raise(out, s, KDark, v, now, 1, b);
        s.darkRaised = true;
    }
    for (uint32_t m : drop) st_.erase(m);
}

} // namespace dect2
