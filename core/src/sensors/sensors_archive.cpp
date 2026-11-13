// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Long sensor history: see sensors_archive.h.
#include "dect2/sensors/sensors_archive.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <sstream>

namespace dect2 {

std::string SensorArchive::dayOf(int64_t utc) const {
    const time_t t = (time_t)(utc + tz_);
    struct tm g;
#ifdef _WIN32
    gmtime_s(&g, &t);
#else
    gmtime_r(&t, &g);
#endif
    char b[16]; std::strftime(b, sizeof b, "%Y-%m-%d", &g); return b;
}

void SensorArchive::add(const std::string& key, int64_t utc, const std::vector<std::pair<std::string, double>>& vals) {
    if (key.empty()) return;
    Sens& s = s_[key];
    const std::string d = dayOf(utc);
    if (s.days.empty() || s.days.back().day < d) { SensorDay n; n.day = d; s.days.push_back(n); if (s.days.size() > kMaxDays) s.days.erase(s.days.begin()); }
    SensorDay* day = &s.days.back();
    if (day->day != d) { for (auto& x : s.days) if (x.day == d) day = &x; if (day->day != d) return; }   // a reading older than the oldest day kept
    for (const auto& kv : vals) {
        if (!std::isfinite(kv.second)) continue;
        if (kv.first == "rain_mm" || kv.first == "rain_in") {
            const double mm = kv.second * (kv.first == "rain_in" ? 25.4 : 1);
            if (s.lastRain >= 0 && mm >= s.lastRain) { day->rainMm += mm - s.lastRain; }
            day->hasRain = true;
            s.lastRain = mm;
            continue;
        }
        if (kv.first == "id" || kv.first == "channel" || kv.first == "mic" || kv.first == "subtype") continue;
        SensorDayStat& st = day->values[kv.first];
        if (!st.n) { st.min = st.max = kv.second; }
        st.min = std::min(st.min, kv.second); st.max = std::max(st.max, kv.second); st.sum += kv.second; st.n++;
    }
    dirty_ = true;
}

const std::vector<SensorDay>* SensorArchive::days(const std::string& key) const { auto it = s_.find(key); return it == s_.end() ? nullptr : &it->second.days; }
std::vector<std::string> SensorArchive::keys() const { std::vector<std::string> v; for (const auto& kv : s_) v.push_back(kv.first); return v; }
double SensorArchive::rainTotal(const std::string& key, const std::string& prefix) const {
    const auto* d = days(key);
    double t = 0;
    if (d) for (const auto& x : *d) if (x.hasRain && x.day.compare(0, prefix.size(), prefix) == 0) t += x.rainMm;
    return t;
}

std::string SensorArchive::serialize() const {
    std::ostringstream o;
    char b[64];
    for (const auto& kv : s_) {
        std::snprintf(b, sizeof b, "%.3f", kv.second.lastRain);
        o << "S\t" << kv.first << "\t" << b << "\n";
        for (const auto& d : kv.second.days) {
            std::snprintf(b, sizeof b, "%.3f", d.rainMm);
            o << "D\t" << d.day << "\t" << (d.hasRain ? b : "-");
            for (const auto& v : d.values) { char c[128]; std::snprintf(c, sizeof c, "\t%s=%.3f,%.3f,%.3f,%u", v.first.c_str(), v.second.min, v.second.max, v.second.sum, v.second.n); o << c; }
            o << "\n";
        }
    }
    return o.str();
}

void SensorArchive::parse(const std::string& text) {
    s_.clear();
    std::stringstream ss(text);
    std::string l;
    Sens* cur = nullptr;
    while (std::getline(ss, l)) {
        std::vector<std::string> f;
        size_t a = 0;
        for (;;) { const size_t b = l.find('\t', a); f.push_back(l.substr(a, b == std::string::npos ? std::string::npos : b - a)); if (b == std::string::npos) break; a = b + 1; }
        if (f[0] == "S" && f.size() >= 3 && !f[1].empty()) { cur = &s_[f[1]]; cur->lastRain = std::atof(f[2].c_str()); }
        else if (f[0] == "D" && f.size() >= 3 && cur && f[1].size() == 10) {
            SensorDay d; d.day = f[1];
            if (f[2] != "-") { d.hasRain = true; d.rainMm = std::atof(f[2].c_str()); }
            for (size_t i = 3; i < f.size(); i++) {
                const size_t e = f[i].find('=');
                if (e == std::string::npos) continue;
                SensorDayStat st; unsigned n = 0;
                if (std::sscanf(f[i].c_str() + e + 1, "%lf,%lf,%lf,%u", &st.min, &st.max, &st.sum, &n) == 4) { st.n = n; d.values[f[i].substr(0, e)] = st; }
            }
            cur->days.push_back(d);
        }
    }
    dirty_ = false;
}

}  // namespace dect2
