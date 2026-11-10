// Radiosonde launch sites: see sonde_sites.h.
#include "dect2/sonde/sonde_sites.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <sstream>

namespace dect2 {

namespace {
double km(double la1, double lo1, double la2, double lo2) {
    const double r = 3.14159265358979 / 180, a = std::sin((la2 - la1) * r / 2), b = std::sin((lo2 - lo1) * r / 2);
    const double h = a * a + std::cos(la1 * r) * std::cos(la2 * r) * b * b;
    return 6371.0 * 2 * std::atan2(std::sqrt(h), std::sqrt(1 - h));
}
}  // namespace

void SondeSites::update(const std::vector<SondeInfo>& v, int64_t utc) {
    for (const SondeInfo& s : v) {
        if (s.serial.empty() || !s.trackIncluded || s.track.size() < 2) continue;
        Seen& w = seen_[s.serial];
        if (!w.launch) {
            w.launch = true;
            const SondeTrackPoint& f = s.track.front();
            // a launch: the first fix low, and the next points higher (climbing)
            if (f.altM < kLaunchMaxM && s.track[std::min<size_t>(s.track.size() - 1, 5)].altM > f.altM + 20) {
                int best = -1; double bd = 1e9;
                for (size_t i = 0; i < sites_.size(); i++) { const double d = km(sites_[i].lat, sites_[i].lon, f.lat, f.lon); if (d < bd) { bd = d; best = (int)i; } }
                if (best < 0 || bd > kSiteKm) {
                    SondeSite n; n.id = nextId_++; n.name = "Site " + std::to_string(n.id); n.lat = f.lat; n.lon = f.lon;
                    sites_.push_back(n); best = (int)sites_.size() - 1;
                }
                SondeSite& st = sites_[(size_t)best];
                st.lat = (st.lat * st.launches + f.lat) / (st.launches + 1);
                st.lon = (st.lon * st.launches + f.lon) / (st.launches + 1);
                st.launches++; st.lastUtc = utc;
                w.site = st.id;
                dirty_ = true;
            }
        }
        // a landing: came down after a burst (highest point more than 1000 m above the last), heard below 1500 m, not heard for 2 minutes
        if (w.site > 0 && !w.landed && !s.active && s.lastHeardS > 120) {
            float top = 0;
            for (const auto& p : s.track) top = std::max(top, p.altM);
            const SondeTrackPoint& l = s.track.back();
            if (top - l.altM > 1000 && l.altM < 1500) {
                for (auto& st : sites_) if (st.id == w.site) { st.landings.push_back({l.lat, l.lon}); if (st.landings.size() > 2000) st.landings.erase(st.landings.begin()); }
                w.landed = true;
                dirty_ = true;
            }
        }
    }
    if (seen_.size() > 5000) seen_.clear();
}

void SondeSites::rename(int id, const std::string& name) { for (auto& s : sites_) if (s.id == id && !name.empty()) { s.name = name; dirty_ = true; } }

std::string SondeSites::serialize() const {
    std::ostringstream o;
    o.precision(7);
    for (const auto& s : sites_) {
        std::string n = s.name; for (char& c : n) if (c == '\t' || c == '\n') c = ' ';
        o << "S\t" << s.id << '\t' << n << '\t' << s.lat << '\t' << s.lon << '\t' << s.launches << '\t' << s.lastUtc << '\n';
        for (const auto& l : s.landings) o << "L\t" << s.id << '\t' << l.first << '\t' << l.second << '\n';
    }
    return o.str();
}

void SondeSites::parse(const std::string& text) {
    sites_.clear();
    std::stringstream ss(text);
    std::string l;
    while (std::getline(ss, l)) {
        std::vector<std::string> f;
        std::stringstream ls(l);
        std::string x;
        while (std::getline(ls, x, '\t')) f.push_back(x);
        if (f.size() >= 7 && f[0] == "S") {
            SondeSite s; s.id = std::atoi(f[1].c_str()); s.name = f[2]; s.lat = std::atof(f[3].c_str()); s.lon = std::atof(f[4].c_str()); s.launches = std::atoi(f[5].c_str()); s.lastUtc = std::atoll(f[6].c_str());
            if (s.id > 0) { sites_.push_back(s); nextId_ = std::max(nextId_, s.id + 1); }
        } else if (f.size() >= 4 && f[0] == "L") {
            const int id = std::atoi(f[1].c_str());
            for (auto& s : sites_) if (s.id == id) s.landings.push_back({(float)std::atof(f[2].c_str()), (float)std::atof(f[3].c_str())});
        }
    }
    dirty_ = false;
}

}  // namespace dect2
