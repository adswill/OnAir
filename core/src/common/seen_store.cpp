// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Heard over days and years: see seen_store.h.
#include "dect2/common/seen_store.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <sstream>

namespace dect2 {

namespace {
std::string clean(std::string s) { for (char& c : s) if (c == '\t' || c == '\n' || c == '\r') c = ' '; return s; }
std::vector<std::string> tabs(const std::string& l) {
    std::vector<std::string> v;
    size_t a = 0;
    for (;;) { const size_t b = l.find('\t', a); v.push_back(l.substr(a, b == std::string::npos ? std::string::npos : b - a)); if (b == std::string::npos) break; a = b + 1; }
    return v;
}
}  // namespace

std::string SeenStore::dayOf(int64_t utc) const {
    const time_t t = (time_t)(utc + tz_);
    struct tm g;
#ifdef _WIN32
    gmtime_s(&g, &t);
#else
    gmtime_r(&t, &g);
#endif
    char b[16];
    std::strftime(b, sizeof b, "%Y-%m-%d", &g);
    return b;
}

SeenDay& SeenStore::day(int64_t utc) {
    const std::string d = dayOf(utc);
    if (!days_.empty() && days_.back().day == d) return days_.back();
    for (auto& x : days_) if (x.day == d) return x;   // a late report of an earlier day
    SeenDay n; n.day = d;
    days_.push_back(n);
    std::sort(days_.begin(), days_.end(), [](const SeenDay& a, const SeenDay& b) { return a.day < b.day; });
    if (days_.size() > kMaxDays) days_.erase(days_.begin(), days_.begin() + (long)(days_.size() - kMaxDays));
    for (auto& x : days_) if (x.day == d) return x;
    return days_.back();
}

bool SeenStore::seen(const std::string& id, int64_t utc, const std::string& name, const std::string& kind) {
    if (id.empty()) return false;
    const std::string d = dayOf(utc);
    auto it = ids_.find(id);
    const bool fresh = it == ids_.end();
    if (fresh) {
        if (ids_.size() >= kMaxIds) return false;
        SeenId s; s.firstUtc = utc; s.firstDay = d;
        it = ids_.emplace(id, s).first;
    }
    SeenId& s = it->second;
    if (!name.empty() && name != s.name) { s.name = clean(name); dirty_ = true; }
    if (!kind.empty() && kind != s.kind) { s.kind = clean(kind); dirty_ = true; }
    const int64_t hk = (utc + tz_) / 3600;
    if (s.lastHour != hk) {
        s.lastHour = hk;
        day(utc).hours[(size_t)(((hk % 24) + 24) % 24)]++;
    }
    if (s.lastDay != d) {
        SeenDay& dd = day(utc);
        dd.unique++;
        if (fresh) dd.fresh++;
        s.days++;
        s.lastDay = d;
        dirty_ = true;
    }
    if (utc > s.lastUtc) s.lastUtc = utc;
    if (utc < s.firstUtc) s.firstUtc = utc;
    return fresh;
}

void SeenStore::position(const std::string& id, int64_t utc, double km, double bearingDeg, int band) {
    if (!std::isfinite(km) || km < 0 || km > 5000) return;
    SeenDay& d = day(utc);
    d.positions++;
    if (km > d.maxKm) { d.maxKm = km; d.maxId = id; }
    if (km > farKm_) { farKm_ = km; farId_ = id; farUtc_ = utc; }
    if (band < 0 || band >= kBands) band = 0;
    if (km > bandFarKm_[(size_t)band]) { bandFarKm_[(size_t)band] = km; bandFarId_[(size_t)band] = id; }
    double b = std::fmod(bearingDeg, 360.0); if (b < 0) b += 360;
    float& o = outline_[(size_t)band][(size_t)std::min(kSectors - 1, (int)(b / (360.0 / kSectors)))];
    if (km > o) o = (float)km;
    dirty_ = true;
}

void SeenStore::messages(int64_t utc, uint64_t n) { if (n) { day(utc).messages += n; dirty_ = true; } }

const SeenId* SeenStore::get(const std::string& id) const { auto it = ids_.find(id); return it == ids_.end() ? nullptr : &it->second; }
bool SeenStore::newToday(const std::string& id, int64_t utc) const { const SeenId* s = get(id); return s && s->firstDay == dayOf(utc); }
const SeenDay* SeenStore::today(int64_t utc) const { const std::string d = dayOf(utc); for (const auto& x : days_) if (x.day == d) return &x; return nullptr; }

std::string SeenStore::serialize() const {
    std::ostringstream o;
    o << "onair-seen\t1\n";
    char b[64];
    std::snprintf(b, sizeof b, "%.3f", farKm_);
    o << "F\t" << b << "\t" << clean(farId_) << "\t" << farUtc_ << "\n";
    for (int k = 0; k < kBands; k++) {
        o << "O\t" << k;
        for (float v : outline_[(size_t)k]) { std::snprintf(b, sizeof b, "\t%.2f", v); o << b; }
        o << "\n";
    }
    for (int k = 0; k < kBands; k++) { std::snprintf(b, sizeof b, "%.3f", bandFarKm_[(size_t)k]); o << "B\t" << k << "\t" << b << "\t" << clean(bandFarId_[(size_t)k]) << "\n"; }
    for (const auto& d : days_) {
        std::snprintf(b, sizeof b, "%.3f", d.maxKm);
        o << "D\t" << d.day << "\t" << d.unique << "\t" << d.fresh << "\t" << d.positions << "\t" << d.messages << "\t" << b << "\t" << clean(d.maxId) << "\t";
        for (int h = 0; h < 24; h++) o << (h ? "," : "") << d.hours[(size_t)h];
        o << "\n";
    }
    std::vector<const std::pair<const std::string, SeenId>*> v;
    for (const auto& kv : ids_) v.push_back(&kv);
    std::sort(v.begin(), v.end(), [](auto* a, auto* b) { return a->first < b->first; });
    for (auto* kv : v) o << "I\t" << clean(kv->first) << "\t" << kv->second.firstUtc << "\t" << kv->second.lastUtc << "\t" << kv->second.firstDay << "\t" << kv->second.lastDay << "\t" << kv->second.days << "\t" << kv->second.name << "\t" << kv->second.kind << "\n";
    return o.str();
}

bool SeenStore::parse(const std::string& text) {
    std::stringstream ss(text);
    std::string l;
    if (!std::getline(ss, l) || l.rfind("onair-seen\t", 0) != 0) return false;
    SeenStore n;
    n.tz_ = tz_;
    while (std::getline(ss, l)) {
        if (!l.empty() && l.back() == '\r') l.pop_back();
        const auto f = tabs(l);
        if (f.empty()) continue;
        if (f[0] == "B" && f.size() >= 4) { const int k = std::atoi(f[1].c_str()); if (k >= 0 && k < kBands) { n.bandFarKm_[(size_t)k] = std::atof(f[2].c_str()); n.bandFarId_[(size_t)k] = f[3]; } }
        else if (f[0] == "F" && f.size() >= 4) { n.farKm_ = std::atof(f[1].c_str()); n.farId_ = f[2]; n.farUtc_ = std::atoll(f[3].c_str()); }
        else if (f[0] == "O" && f.size() == 2 + kSectors) {
            const int k = std::atoi(f[1].c_str());
            if (k >= 0 && k < kBands) for (int i = 0; i < kSectors; i++) n.outline_[(size_t)k][(size_t)i] = (float)std::atof(f[(size_t)i + 2].c_str());
        } else if (f[0] == "D" && f.size() >= 8 && f[1].size() == 10) {
            SeenDay d; d.day = f[1]; d.unique = (uint32_t)std::atol(f[2].c_str()); d.fresh = (uint32_t)std::atol(f[3].c_str());
            d.positions = std::strtoull(f[4].c_str(), nullptr, 10); d.messages = std::strtoull(f[5].c_str(), nullptr, 10); d.maxKm = std::atof(f[6].c_str()); d.maxId = f[7];
            if (f.size() >= 9) { std::stringstream hs(f[8]); std::string x; int h = 0; while (h < 24 && std::getline(hs, x, ',')) d.hours[(size_t)h++] = (uint32_t)std::atol(x.c_str()); }
            n.days_.push_back(d);
        } else if (f[0] == "I" && f.size() >= 8 && !f[1].empty()) {
            SeenId s; s.firstUtc = std::atoll(f[2].c_str()); s.lastUtc = std::atoll(f[3].c_str()); s.firstDay = f[4]; s.lastDay = f[5]; s.days = (uint32_t)std::atol(f[6].c_str()); s.name = f[7];
            if (f.size() >= 9) s.kind = f[8];
            n.ids_[f[1]] = s;
        }
    }
    std::sort(n.days_.begin(), n.days_.end(), [](const SeenDay& a, const SeenDay& b) { return a.day < b.day; });
    *this = std::move(n);
    return true;
}

std::string seenDaysCsv(const SeenStore& s) {
    std::string o = "day,unique,new,positions,messages,max_km,max_id\n";
    char b[64];
    for (const auto& d : s.days()) { std::snprintf(b, sizeof b, "%.1f", d.maxKm); o += d.day + "," + std::to_string(d.unique) + "," + std::to_string(d.fresh) + "," + std::to_string(d.positions) + "," + std::to_string(d.messages) + "," + b + "," + d.maxId + "\n"; }
    return o;
}

}  // namespace dect2
