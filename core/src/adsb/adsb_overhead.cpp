// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// ADS-B overhead log: see adsb_overhead.h.
#include "dect2/adsb/adsb_overhead.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <sstream>

namespace dect2 {

namespace {
std::string iso(int64_t t) {
    const time_t tt = (time_t)t;
    struct tm g;
#ifdef _WIN32
    gmtime_s(&g, &tt);
#else
    gmtime_r(&tt, &g);
#endif
    char b[32];
    std::strftime(b, sizeof b, "%Y-%m-%dT%H:%M:%SZ", &g);
    return b;
}
int64_t fromIso(const std::string& s) {
    int Y, M, D, h, m, sec;
    if (std::sscanf(s.c_str(), "%d-%d-%dT%d:%d:%dZ", &Y, &M, &D, &h, &m, &sec) != 6) return 0;
    // days from civil (Howard Hinnant's algorithm)
    Y -= M <= 2;
    const int64_t era = (Y >= 0 ? Y : Y - 399) / 400;
    const unsigned yoe = (unsigned)(Y - era * 400);
    const unsigned doy = (153 * (M + (M > 2 ? -3 : 9)) + 2) / 5 + D - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const int64_t days = era * 146097 + (int64_t)doe - 719468;
    return days * 86400 + h * 3600 + m * 60 + sec;
}
std::string csvField(const std::string& s) {
    if (s.find_first_of(",\"\n") == std::string::npos) return s;
    std::string o = "\"";
    for (char c : s) { if (c == '"') o += '"'; o += c == '\n' ? ' ' : c; }
    return o + "\"";
}
std::vector<std::string> csvSplit(const std::string& l) {
    std::vector<std::string> v;
    std::string cur;
    bool q = false;
    for (size_t i = 0; i < l.size(); i++) {
        const char c = l[i];
        if (q) { if (c == '"') { if (i + 1 < l.size() && l[i + 1] == '"') { cur += '"'; i++; } else q = false; } else cur += c; }
        else if (c == '"') q = true;
        else if (c == ',') { v.push_back(cur); cur.clear(); }
        else cur += c;
    }
    v.push_back(cur);
    return v;
}
}  // namespace

void AdsbOverheadLog::note(const AdsbAircraft& ac, double km, int64_t now, const AdsbOverheadInfo& info) {
    auto it = open_.find(ac.icao);
    if (it == open_.end()) {
        Open o;
        o.p.icao = ac.icao;
        o.p.firstUtc = now;
        it = open_.emplace(ac.icao, o).first;
    }
    Open& o = it->second;
    o.lastIn = now;
    o.p.lastUtc = now;
    if (!ac.callsign.empty()) o.p.callsign = ac.callsign;
    if (!info.type.empty()) o.p.type = info.type;
    if (!info.registration.empty()) o.p.registration = info.registration;
    if (km < o.p.closestKm) {
        o.p.closestKm = km;
        o.p.closestUtc = now;
        o.p.hasAlt = ac.hasAlt || ac.ground;
        o.p.altFt = ac.ground ? 0 : ac.altFt;
        o.p.ground = ac.ground;
    }
}

std::vector<AdsbOverheadPass> AdsbOverheadLog::closeOld(const std::vector<AdsbAircraft>& aircraft, int64_t now) {
    std::vector<AdsbOverheadPass> ended;
    for (auto it = open_.begin(); it != open_.end();) {
        bool inside = it->second.lastIn == now;
        bool gone = !inside && now - it->second.lastIn >= kEndSec;
        if (!inside && !gone) {   // heard outside the radius again: the pass is over
            for (const AdsbAircraft& ac : aircraft)
                if (ac.icao == it->first && ac.hasRange && ac.ageSec <= 30 && ac.distNm * 1.852 > radiusKm_ * 1.05) gone = true;
        }
        if (gone) { ended.push_back(it->second.p); add(it->second.p); it = open_.erase(it); }
        else ++it;
    }
    return ended;
}

std::vector<AdsbOverheadPass> AdsbOverheadLog::update(const std::vector<AdsbAircraft>& aircraft, int64_t nowUtc) {
    return update(aircraft, nowUtc, [](const AdsbAircraft&) { return AdsbOverheadInfo(); });
}

std::vector<AdsbOverheadPass> AdsbOverheadLog::now() const {
    std::vector<AdsbOverheadPass> v;
    for (const auto& kv : open_) v.push_back(kv.second.p);
    std::sort(v.begin(), v.end(), [](const AdsbOverheadPass& a, const AdsbOverheadPass& b) { return a.closestKm < b.closestKm; });
    return v;
}

void AdsbOverheadLog::add(const AdsbOverheadPass& p) {
    done_.push_back(p);
    if (done_.size() > kKeep) done_.erase(done_.begin(), done_.begin() + (long)(done_.size() - kKeep));
}

std::string adsbOverheadCsv(const std::vector<AdsbOverheadPass>& v) {
    std::string o = "utc_closest,icao,callsign,registration,type,closest_km,alt_ft,first_utc,last_utc,ground\n";
    for (const auto& p : v) {
        char hex[8], km[24];
        std::snprintf(hex, sizeof hex, "%06X", p.icao & 0xFFFFFF);
        std::snprintf(km, sizeof km, "%.2f", p.closestKm);
        o += iso(p.closestUtc) + "," + hex + "," + csvField(p.callsign) + "," + csvField(p.registration) + "," + csvField(p.type) + "," + km + "," +
             (p.hasAlt ? std::to_string(p.altFt) : std::string()) + "," + iso(p.firstUtc) + "," + iso(p.lastUtc) + "," + (p.ground ? "1" : "0") + "\n";
    }
    return o;
}

std::vector<AdsbOverheadPass> adsbOverheadFromCsv(const std::string& text) {
    std::vector<AdsbOverheadPass> v;
    std::stringstream ss(text);
    std::string l;
    while (std::getline(ss, l)) {
        if (!l.empty() && l.back() == '\r') l.pop_back();
        const std::vector<std::string> f = csvSplit(l);
        if (f.size() < 10) continue;
        AdsbOverheadPass p;
        p.closestUtc = fromIso(f[0]);
        if (!p.closestUtc) continue;
        char* e = nullptr;
        p.icao = (uint32_t)std::strtoul(f[1].c_str(), &e, 16);
        if (!e || *e || f[1].empty()) continue;
        p.callsign = f[2]; p.registration = f[3]; p.type = f[4];
        p.closestKm = std::atof(f[5].c_str());
        p.hasAlt = !f[6].empty(); p.altFt = std::atoi(f[6].c_str());
        p.firstUtc = fromIso(f[7]); p.lastUtc = fromIso(f[8]);
        p.ground = f[9] == "1";
        v.push_back(p);
    }
    return v;
}

std::string adsbOverheadDay(int64_t utc, int tzOffsetSec) { return iso(utc + tzOffsetSec).substr(0, 10); }

}  // namespace dect2
