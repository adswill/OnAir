#include "dect2/ais/ais_alerts.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace dect2 {

// ---------------------------------------------------------------------------------------------------- filter

int aisShipGroup(int t) {
    if (t < 1 || t > 99) return AIS_GRP_UNKNOWN;
    if (t == 30) return AIS_GRP_FISHING;
    if (t >= 31 && t <= 35) return AIS_GRP_TOWING_OPS;
    if (t == 36 || t == 37) return AIS_GRP_SAILING_PLEASURE;
    if (t >= 40 && t <= 49) return AIS_GRP_HIGH_SPEED;
    if (t >= 50 && t <= 59) return AIS_GRP_SERVICE;
    if (t >= 60 && t <= 69) return AIS_GRP_PASSENGER;
    if (t >= 70 && t <= 79) return AIS_GRP_CARGO;
    if (t >= 80 && t <= 89) return AIS_GRP_TANKER;
    return AIS_GRP_OTHER;
}

const char* aisShipGroupText(int g) {
    static const char* const n[AIS_GRP_COUNT] = {"Not received", "Fishing", "Towing, dredging, diving, military", "Sailing and pleasure", "High-speed craft",
                                                 "Pilot, tug, SAR, law enforcement", "Passenger", "Cargo", "Tanker", "Other"};
    return g >= 0 && g < AIS_GRP_COUNT ? n[g] : "";
}

bool AisFilter::matches(const AisVessel& v) const {
    const int c = v.cls >= 0 && v.cls < 6 ? v.cls : AIS_CLASS_OTHER;
    if (!(classes >> c & 1)) return false;
    // the ship type only applies to stations that have one (a base station or an aid has none: the class decides)
    if (v.cls == AIS_CLASS_A || v.cls == AIS_CLASS_B || v.cls == AIS_CLASS_OTHER) {
        if (!(groups >> aisShipGroup(v.shipType) & 1)) return false;
    }
    if (movingOnly && !(v.sog >= minSogKn && v.sog < 102.3f)) return false;
    return true;
}

std::string AisFilter::toText() const {
    char b[96];
    snprintf(b, sizeof b, "classes=%u;groups=%u;moving=%d;sog=%.2f", classes, groups, movingOnly ? 1 : 0, minSogKn);
    return b;
}

AisFilter AisFilter::fromText(const std::string& s) {
    AisFilter f;
    size_t p = 0;
    while (p < s.size()) {
        size_t e = s.find(';', p);
        if (e == std::string::npos) e = s.size();
        const std::string kv = s.substr(p, e - p);
        p = e + 1;
        const size_t q = kv.find('=');
        if (q == std::string::npos) continue;
        const std::string k = kv.substr(0, q), val = kv.substr(q + 1);
        char* end = nullptr;
        const double d = strtod(val.c_str(), &end);
        if (end == val.c_str()) continue;
        if (k == "classes") f.classes = (uint32_t)d & 0x3F;
        else if (k == "groups") f.groups = (uint32_t)d & ((1u << AIS_GRP_COUNT) - 1);
        else if (k == "moving") f.movingOnly = d != 0;
        else if (k == "sog" && d >= 0 && d < 100) f.minSogKn = (float)d;
    }
    return f;
}

// ---------------------------------------------------------------------------------------------------- geometry

static const double kPi = 3.14159265358979323846;

double aisDistanceNm(double la1, double lo1, double la2, double lo2) {   // haversine, 1 nm = 1852 m, mean earth radius 6371008.8 m
    const double r = kPi / 180, dla = (la2 - la1) * r, dlo = (lo2 - lo1) * r;
    const double a = std::sin(dla / 2) * std::sin(dla / 2) + std::cos(la1 * r) * std::cos(la2 * r) * std::sin(dlo / 2) * std::sin(dlo / 2);
    return 2 * 6371008.8 * std::asin(std::min(1.0, std::sqrt(a))) / 1852.0;
}

AisCpa aisCpa(double latA, double lonA, double sogA, double cogA, double latB, double lonB, double sogB, double cogB) {
    AisCpa o;
    if (!std::isfinite(latA + lonA + latB + lonB)) return o;
    const double r = kPi / 180;
    // local plane around A, in nautical miles (1 arc minute of latitude = 1 nm)
    double dlo = lonB - lonA;
    while (dlo > 180) dlo -= 360;
    while (dlo < -180) dlo += 360;
    const double xE = dlo * 60.0 * std::cos((latA + latB) * 0.5 * r), yN = (latB - latA) * 60.0;
    o.distNm = aisDistanceNm(latA, lonA, latB, lonB);
    o.bearingDeg = std::fmod(std::atan2(xE, yN) / r + 360.0, 360.0);
    auto vel = [&](double s, double c, double& ve, double& vn) {
        if (s < 0 || s > 102.2 || c < 0 || c >= 360) { ve = vn = 0; return; }   // unknown (or "not available" 102.3 kn): treated as not moving
        ve = s * std::sin(c * r); vn = s * std::cos(c * r);
    };
    double ae, an, be, bn;
    vel(sogA, cogA, ae, an);
    vel(sogB, cogB, be, bn);
    const double ve = be - ae, vn = bn - an;       // knots = nm per hour
    const double v2 = ve * ve + vn * vn;
    o.valid = true;
    if (v2 < 1e-6) { o.dcpaNm = o.distNm; o.tcpaMin = 0; return o; }
    const double tH = -(xE * ve + yN * vn) / v2;   // hours to the closest approach
    o.tcpaMin = tH * 60.0;
    if (tH <= 0) { o.dcpaNm = o.distNm; return o; }   // moving apart: the closest approach was (or is) now
    const double cx = xE + ve * tH, cy = yN + vn * tH;
    o.dcpaNm = std::sqrt(cx * cx + cy * cy);
    return o;
}

bool AisGeofence::contains(double la, double lo) const {
    if (!polygon) return radiusNm > 0 && aisDistanceNm(lat, lon, la, lo) <= radiusNm;
    if (pts.size() < 3) return false;
    // ray casting on longitudes unwrapped around the point (polygons across the date line work)
    bool in = false;
    auto unwrap = [&](double x) { while (x - lo > 180) x -= 360; while (x - lo < -180) x += 360; return x; };
    for (size_t i = 0, j = pts.size() - 1; i < pts.size(); j = i++) {
        const double yi = pts[i].first, yj = pts[j].first, xi = unwrap(pts[i].second), xj = unwrap(pts[j].second);
        if (((yi > la) != (yj > la)) && lo < (xj - xi) * (la - yi) / (yj - yi) + xi) in = !in;
    }
    return in;
}

static bool parseLatLon(const std::string& s, double& la, double& lo) {
    const size_t c = s.find(',');
    if (c == std::string::npos) return false;
    char* e1 = nullptr; char* e2 = nullptr;
    la = strtod(s.substr(0, c).c_str(), &e1);
    const std::string r = s.substr(c + 1);
    lo = strtod(r.c_str(), &e2);
    return e1 != s.c_str() && e2 != r.c_str() && std::isfinite(la) && std::isfinite(lo) && std::fabs(la) <= 90 && std::fabs(lo) <= 180;
}

static std::string trim(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\r' || s.back() == '\t')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
    return s.substr(i);
}

std::vector<AisGeofence> aisParseGeofences(const std::string& text, std::vector<std::string>* errors) {
    std::vector<AisGeofence> out;
    std::istringstream in(text);
    std::string line;
    int n = 0;
    auto bad = [&](const char* why) { if (errors) errors->push_back("line " + std::to_string(n) + ": " + why); };
    while (std::getline(in, line)) {
        n++;
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        std::vector<std::string> f;
        size_t p = 0;
        for (;;) { const size_t e = line.find(';', p); f.push_back(trim(line.substr(p, e == std::string::npos ? std::string::npos : e - p))); if (e == std::string::npos) break; p = e + 1; }
        AisGeofence g;
        g.name = f[0];
        if (g.name.empty()) { bad("no name"); continue; }
        if (f.size() == 2) {
            const size_t c2 = f[1].rfind(',');
            if (c2 == std::string::npos || f[1].find(',') == c2) { bad("expected lat,lon,radius_nm"); continue; }
            double r = strtod(f[1].substr(c2 + 1).c_str(), nullptr);
            if (!parseLatLon(f[1].substr(0, c2), g.lat, g.lon)) { bad("bad latitude or longitude"); continue; }
            if (!(r > 0 && r < 5000)) { bad("radius must be above 0 and below 5000 nm"); continue; }
            g.radiusNm = r;
        } else if (f.size() == 3 && (f[1] == "poly" || f[1] == "polygon")) {
            g.polygon = true;
            std::istringstream ps(f[2]);
            std::string tok;
            bool ok = true;
            while (ps >> tok) { double la, lo; if (!parseLatLon(tok, la, lo)) { ok = false; break; } g.pts.push_back({la, lo}); }
            if (!ok) { bad("bad polygon point (use lat,lon separated by spaces)"); continue; }
            if (g.pts.size() < 3) { bad("a polygon needs at least 3 points"); continue; }
        } else { bad("expected name;lat,lon,radius_nm or name;poly;lat,lon lat,lon ..."); continue; }
        out.push_back(std::move(g));
        if (out.size() >= 32) break;
    }
    return out;
}

std::string aisGeofencesToText(const std::vector<AisGeofence>& v) {
    std::string o;
    char b[96];
    for (const auto& g : v) {
        o += g.name + ";";
        if (g.polygon) {
            o += "poly;";
            for (size_t i = 0; i < g.pts.size(); i++) { snprintf(b, sizeof b, "%s%.6f,%.6f", i ? " " : "", g.pts[i].first, g.pts[i].second); o += b; }
        } else { snprintf(b, sizeof b, "%.6f,%.6f,%.3f", g.lat, g.lon, g.radiusNm); o += b; }
        o += "\n";
    }
    return o;
}

// ---------------------------------------------------------------------------------------------------- alerts

void AisAlertEngine::configure(const AisAlertConfig& c) {
    cfg_ = c;
    if (cfg_.fences.size() > 32) cfg_.fences.resize(32);
}

std::vector<AisAlert> AisAlertEngine::update(const AisVessel& v, double nowUtc) {
    std::vector<AisAlert> out;
    if (!v.hasPos || !v.mmsi) return out;
    if (!cfg_.cpaOn && cfg_.fences.empty()) return out;
    if (!cfg_.filter.matches(v)) return out;
    if (cfg_.maxAgeSec > 0 && v.lastUtc > 0 && nowUtc - v.lastUtc > cfg_.maxAgeSec) return out;
    State& s = st_[v.mmsi];
    if (st_.size() > 4000) { for (auto it = st_.begin(); it != st_.end();) { if (it->first != v.mmsi && !it->second.inside && !it->second.cpaActive) it = st_.erase(it); else ++it; } }
    AisAlert base;
    base.utc = nowUtc; base.mmsi = v.mmsi; base.name = v.name; base.shipType = v.shipType; base.cls = v.cls;
    base.lat = v.lat; base.lon = v.lon; base.sogKn = v.sog; base.cogDeg = v.cog;
    if (cfg_.ownValid) base.distNm = aisDistanceNm(cfg_.ownLat, cfg_.ownLon, v.lat, v.lon);

    if (cfg_.cpaOn && cfg_.ownValid && v.cls != AIS_CLASS_BASE && v.cls != AIS_CLASS_ATON) {
        const AisCpa c = aisCpa(cfg_.ownLat, cfg_.ownLon, cfg_.ownSogKn, cfg_.ownCogDeg, v.lat, v.lon, v.sog, v.cog);
        // a vessel with no known course cannot be predicted: only an already very close one counts (its distance is its CPA)
        const bool predictable = v.sog >= 0 && v.sog < 102.3f && v.cog >= 0 && v.cog < 360;
        const bool danger = c.valid && c.dcpaNm < cfg_.cpaNm && ((predictable && c.tcpaMin >= 0 && c.tcpaMin <= cfg_.tcpaMin) || c.distNm < cfg_.cpaNm);
        if (danger) {
            if (!s.cpaActive && nowUtc - s.lastCpa >= cfg_.cooldownSec) {
                AisAlert a = base; a.kind = "cpa"; a.dcpaNm = c.dcpaNm; a.tcpaMin = std::max(0.0, c.tcpaMin); a.distNm = c.distNm;
                out.push_back(a);
                s.lastCpa = nowUtc;
            }
            s.cpaActive = true;
        } else if (c.valid && (c.dcpaNm > cfg_.cpaNm * 1.25 || c.tcpaMin < 0)) s.cpaActive = false;   // a margin so it does not flap
    }

    if (!cfg_.fences.empty()) {
        uint32_t now = 0;
        for (size_t i = 0; i < cfg_.fences.size() && i < 32; i++) if (cfg_.fences[i].contains(v.lat, v.lon)) now |= 1u << i;
        if (s.known) {
            for (size_t i = 0; i < cfg_.fences.size() && i < 32; i++) {
                const bool was = s.inside >> i & 1, is = now >> i & 1;
                if (was == is) continue;
                AisAlert a = base; a.kind = is ? "enter" : "exit"; a.fence = cfg_.fences[i].name;
                out.push_back(a);
            }
        }   // the first position of a vessel only records where it is: a vessel already inside does not "enter"
        s.inside = now;
        s.known = true;
    }
    return out;
}

} // namespace dect2
