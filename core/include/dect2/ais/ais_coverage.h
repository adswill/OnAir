// AIS station intelligence (4.0): the polar coverage plot, the ship type mix and the ship-name cache. Pure code, no radio.
//  - AisCoverage: for every bearing sector around the station the farthest position report heard (and how many), so the polar plot shows
//    where the antenna reaches and where it is blind. Reports are counted over a time window, the farthest value is kept as a p95-like
//    "typical" range as well so one ducting event does not draw the plot.
//  - aisTypeClass / AisTypeMix: counts ships by class (cargo, tanker, passenger, fishing, tug, sailing, pleasure, military, SAR, other).
//  - AisNameCache: a position report has no name; the name, call sign and type arrive in a static message minutes later (or never in a
//    short session). The cache remembers them per MMSI so every row shows a name once it has been heard at any time, and expires old ones.
#pragma once
#include "dect2/track/track_geo.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace dect2 {

class AisCoverage {
public:
    explicit AisCoverage(int sectors = 36) : n_(sectors < 4 ? 4 : sectors > 360 ? 360 : sectors), s_(n_) {}
    void setStation(double lat, double lon) { lat_ = lat; lon_ = lon; has_ = true; }
    bool hasStation() const { return has_; }
    // one position report; ignored without a station or with a position of 0/0 or out of range
    void add(double lat, double lon, double nowS) {
        if (!has_ || (lat == 0 && lon == 0) || std::fabs(lat) > 90 || std::fabs(lon) > 180) return;
        const double d = track::gcDistM(lat_, lon_, lat, lon) / 1000.0;
        if (d < 0.05) return;
        const double b = track::gcBearingDeg(lat_, lon_, lat, lon);
        Sector& s = s_[sectorOf(b)];
        s.ranges.push_back({nowS, (float)d});
        if (s.ranges.size() > 400) s.ranges.erase(s.ranges.begin(), s.ranges.begin() + 100);
    }
    struct Sec { double fromDeg = 0, toDeg = 0; int reports = 0; double maxKm = 0, typicalKm = 0; };
    // the sectors over the last windowS seconds; typicalKm is the 90th percentile of the ranges in the sector
    std::vector<Sec> sectors(double nowS, double windowS) const {
        std::vector<Sec> out(n_);
        for (int i = 0; i < n_; i++) {
            out[i].fromDeg = 360.0 * i / n_; out[i].toDeg = 360.0 * (i + 1) / n_;
            std::vector<float> r;
            for (const auto& e : s_[i].ranges) if (e.t >= nowS - windowS && e.t <= nowS) r.push_back(e.km);
            if (r.empty()) continue;
            std::sort(r.begin(), r.end());
            out[i].reports = (int)r.size(); out[i].maxKm = r.back();
            out[i].typicalKm = r[(size_t)std::min<double>(r.size() - 1, std::floor(0.9 * (r.size() - 1) + 0.5))];
        }
        return out;
    }
    // sectors with no report at all in the window while both neighbours have some: a blind spot (a hill, a mast) rather than an empty sea
    std::vector<int> blindSectors(double nowS, double windowS) const {
        const auto v = sectors(nowS, windowS); std::vector<int> b;
        for (int i = 0; i < n_; i++)
            if (v[i].reports == 0 && v[(i + n_ - 1) % n_].reports > 0 && v[(i + 1) % n_].reports > 0) b.push_back(i);
        return b;
    }
    int sectorCount() const { return n_; }
private:
    struct Rng { double t; float km; };
    struct Sector { std::vector<Rng> ranges; };
    int sectorOf(double bDeg) const { int i = (int)(bDeg / 360.0 * n_); return i < 0 ? 0 : i >= n_ ? n_ - 1 : i; }
    int n_; std::vector<Sector> s_; double lat_ = 0, lon_ = 0; bool has_ = false;
};

enum class AisShipClass : uint8_t { Unknown, Cargo, Tanker, Passenger, Fishing, Tug, Sailing, Pleasure, Military, SarLaw, HighSpeed, Other };
inline const char* aisShipClassName(AisShipClass c) {
    static const char* n[] = {"Unknown", "Cargo", "Tanker", "Passenger", "Fishing", "Tug / towing", "Sailing", "Pleasure craft", "Military", "SAR / law", "High speed", "Other"};
    return n[(int)c];
}
// ITU-R M.1371 ship and cargo type (message 5 / 24) to a class
inline AisShipClass aisTypeClass(int type) {
    if (type <= 0) return AisShipClass::Unknown;
    if (type >= 70 && type <= 79) return AisShipClass::Cargo;
    if (type >= 80 && type <= 89) return AisShipClass::Tanker;
    if (type >= 60 && type <= 69) return AisShipClass::Passenger;
    if (type == 30) return AisShipClass::Fishing;
    if (type == 31 || type == 32 || type == 52) return AisShipClass::Tug;
    if (type == 36) return AisShipClass::Sailing;
    if (type == 37) return AisShipClass::Pleasure;
    if (type == 35) return AisShipClass::Military;
    if (type == 51 || type == 55 || type == 50 || type == 58) return AisShipClass::SarLaw;
    if (type >= 40 && type <= 49) return AisShipClass::HighSpeed;
    return AisShipClass::Other;
}
struct AisTypeMix {
    int count[12] = {0};
    void add(int type) { count[(int)aisTypeClass(type)]++; }
    int total() const { int t = 0; for (int c : count) t += c; return t; }
    // classes with at least one ship, most ships first
    std::vector<std::pair<AisShipClass, int>> ranked() const {
        std::vector<std::pair<AisShipClass, int>> v;
        for (int i = 0; i < 12; i++) if (count[i]) v.push_back({(AisShipClass)i, count[i]});
        std::stable_sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        return v;
    }
};

class AisNameCache {
public:
    struct Info { std::string name, callsign; int shipType = 0; int lengthM = 0; double lastS = 0; };
    // merges: an empty field never erases a known one (message 24 part A has the name, part B the call sign and type)
    void learn(uint32_t mmsi, const std::string& name, const std::string& call, int type, int lengthM, double nowS) {
        if (!mmsi) return;
        Info& i = m_[mmsi];
        const std::string nn = clean(name), cc = clean(call);
        if (!nn.empty()) i.name = nn;
        if (!cc.empty()) i.callsign = cc;
        if (type > 0) i.shipType = type;
        if (lengthM > 0) i.lengthM = lengthM;
        i.lastS = nowS;
        if (m_.size() > maxN_) prune(nowS - maxAgeS_);
    }
    const Info* find(uint32_t mmsi) const { auto it = m_.find(mmsi); return it == m_.end() ? nullptr : &it->second; }
    // "NAME" when known, else the MMSI as text
    std::string label(uint32_t mmsi) const {
        const Info* i = find(mmsi);
        return i && !i->name.empty() ? i->name : std::to_string(mmsi);
    }
    void prune(double olderThanS) { for (auto it = m_.begin(); it != m_.end();) it = it->second.lastS < olderThanS ? m_.erase(it) : std::next(it); }
    size_t size() const { return m_.size(); }
    size_t maxN_ = 20000; double maxAgeS_ = 30 * 86400.0;
private:
    // AIS 6-bit text pads with '@' and spaces
    static std::string clean(std::string s) {
        for (auto& c : s) if (c == '@') c = ' ';
        while (!s.empty() && s.back() == ' ') s.pop_back();
        size_t a = 0; while (a < s.size() && s[a] == ' ') a++;
        return s.substr(a);
    }
    std::map<uint32_t, Info> m_;
};

} // namespace dect2
