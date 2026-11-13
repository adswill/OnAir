// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Long sensor history (WeeWX, Cumulus): per sensor and local day the minimum, maximum and mean of each value, and rain per day from the
// rain counter (resets skipped), with month and year totals. Kept on disk between sessions. docs/SENSORS-HISTORY-3.9.md.
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace dect2 {

struct SensorDayStat { double min = 0, max = 0, sum = 0; uint32_t n = 0; double mean() const { return n ? sum / n : 0; } };
struct SensorDay {
    std::string day;                                  // "2026-11-10" local
    std::map<std::string, SensorDayStat> values;      // by field (temperature_C, humidity ...), counters excluded
    double rainMm = 0; bool hasRain = false;          // the rise of the rain counter that day
};

class SensorArchive {
public:
    void setTzOffset(int s) { tz_ = s; }
    // one reading: numeric fields of the message; rain_mm / rain_in counters go to the rain total
    void add(const std::string& key, int64_t utc, const std::vector<std::pair<std::string, double>>& values);
    const std::vector<SensorDay>* days(const std::string& key) const;     // oldest first
    std::vector<std::string> keys() const;
    double rainTotal(const std::string& key, const std::string& prefix) const;   // "2026-11" a month, "2026" a year
    std::string serialize() const;
    void parse(const std::string& text);
    bool dirty() const { return dirty_; }
    void clearDirty() { dirty_ = false; }
    static constexpr size_t kMaxDays = 3660;   // ten years per sensor
private:
    std::string dayOf(int64_t utc) const;
    struct Sens { std::vector<SensorDay> days; double lastRain = -1; };
    std::map<std::string, Sens> s_;
    int tz_ = 0;
    bool dirty_ = false;
};

}  // namespace dect2
