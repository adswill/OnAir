// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// When is the band open: see band_open.h.
#include "dect2/ham/band_open.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <sstream>

namespace dect2::ham {


void BandOpenStore::add(const std::string& band, int64_t utc, double km) {
    if (band.empty()) return;
    const int64_t day = utc >= 0 ? utc / 86400 : -((-utc + 86399) / 86400);
    const int hour = (int)((utc - day * 86400) / 3600);
    V& v = m_[{day, band, hour}];
    v.spots++;
    if (km > v.maxKm && km < 20100) v.maxKm = km;
    dirty_ = true;
    while (!m_.empty() && m_.begin()->first.day < day - kKeepDays) m_.erase(m_.begin());
}

std::map<std::string, std::vector<BandHourCell>> BandOpenStore::matrix(int64_t utc, int days) const {
    std::map<std::string, std::vector<BandHourCell>> out;
    const int64_t today = utc >= 0 ? utc / 86400 : -((-utc + 86399) / 86400);
    for (const auto& kv : m_) {
        if (kv.first.day <= today - days || kv.first.day > today) continue;
        auto& row = out[kv.first.band];
        if (row.empty()) row.resize(24);
        BandHourCell& c = row[(size_t)kv.first.hour];
        c.days++; c.spots += kv.second.spots; c.maxKm = std::max(c.maxKm, kv.second.maxKm);
    }
    return out;
}

std::string BandOpenStore::serialize() const {
    std::ostringstream o;
    char b[32];
    for (const auto& kv : m_) { std::snprintf(b, sizeof b, "%.0f", kv.second.maxKm); o << kv.first.day << '\t' << kv.first.band << '\t' << kv.first.hour << '\t' << kv.second.spots << '\t' << b << '\n'; }
    return o.str();
}

void BandOpenStore::parse(const std::string& text) {
    m_.clear();
    std::stringstream ss(text);
    std::string l;
    while (std::getline(ss, l)) {
        std::vector<std::string> f;
        std::stringstream ls(l);
        std::string x;
        while (std::getline(ls, x, '\t')) f.push_back(x);
        if (f.size() < 5 || f[1].empty()) continue;
        const int h = std::atoi(f[2].c_str());
        if (h < 0 || h > 23) continue;
        V v; v.spots = (uint32_t)std::atol(f[3].c_str()); v.maxKm = std::atof(f[4].c_str());
        m_[{std::atoll(f[0].c_str()), f[1], h}] = v;
    }
    dirty_ = false;
}

}  // namespace dect2::ham
