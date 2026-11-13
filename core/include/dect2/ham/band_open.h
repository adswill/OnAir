// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// "When is the band open": from the user's own FT8 / WSPR / ... spots, per band and UTC hour of day over the last 7 to 30 days: how many days had
// spots in that hour, how many spots, and the farthest. Kept on disk (one record per day, band and hour). docs/BAND-OPEN-3.9.md.
#pragma once
#include "dect2/ham/ham_log.h"   // bandOrder
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace dect2::ham {

struct BandHourCell { int days = 0; uint32_t spots = 0; double maxKm = 0; };

class BandOpenStore {
public:
    // a station heard: band "20m", the distance (km, <= 0 unknown)
    void add(const std::string& band, int64_t utc, double km);
    // the matrix for the last `days` days ending at utc: rows are the bands heard (in band order), 24 hour columns
    std::map<std::string, std::vector<BandHourCell>> matrix(int64_t utc, int days) const;
    std::string serialize() const;
    void parse(const std::string& text);
    bool dirty() const { return dirty_; }
    void clearDirty() { dirty_ = false; }
    static constexpr int kKeepDays = 31;
private:
    struct K { int64_t day; std::string band; int hour; bool operator<(const K& o) const { return day != o.day ? day < o.day : band != o.band ? band < o.band : hour < o.hour; } };
    struct V { uint32_t spots = 0; double maxKm = 0; };
    std::map<K, V> m_;
    bool dirty_ = false;
};


}  // namespace dect2::ham
