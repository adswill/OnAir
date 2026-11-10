// My meter consumption (3.9, docs/SMART5-iot-3.9.md): for a meter the user has marked as their own (Sub-GHz sensors: ERT SCM / IDM and the
// other rtl_433 meters; wireless M-Bus), the cumulative register is turned into usage per hour and per day. A register that steps back to
// near zero from near a power of ten has rolled over (the counter wrapped); one that steps back otherwise was replaced or reset, and
// counting starts again from the new value. Today's use is compared with the average of the full days before it.
// Only meters the user marks are followed; nothing is decrypted (an encrypted wM-Bus meter needs the user's key in the wM-Bus mode).
// Pure logic, no clock: times are Unix seconds (UTC; the caller passes its time zone offset for the day boundaries).
#pragma once
#include "sensors_tel.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace dect2 {

class MeterUsage {
public:
    int tzOffsetSec = 0;          // local day boundaries
    struct Bucket { int64_t start = 0; double use = 0; };

    // a reading of the cumulative register
    void add(int64_t utc, double value) {
        if (!std::isfinite(value) || value < 0) return;
        if (have_ && utc < lastUtc_) return;   // out of order (a repeat of an old frame)
        if (have_) {
            double d = value - last_;
            if (d < 0) {
                const double p = std::pow(10.0, std::ceil(std::log10(std::max(last_, 1.0)) - 1e-9));   // the counter's range: the next power of ten
                if (last_ > 0.9 * p && value < 0.1 * p) d = p - last_ + value;   // wrapped
                else { resets_++; d = 0; }                                       // replaced or reset: start again from here
            }
            if (d > 0) spread(lastUtc_, utc, d);
        }
        last_ = value; lastUtc_ = utc; have_ = true; readings_++;
    }
    // use per hour (local), oldest first, the last `hours`
    std::vector<Bucket> hourly(int64_t nowUtc, int hours = 48) const { return range(hour_, 3600, nowUtc, hours); }
    std::vector<Bucket> daily(int64_t nowUtc, int days = 30) const { return range(day_, 86400, nowUtc, days); }
    double today(int64_t nowUtc) const { auto it = day_.find(dayOf(nowUtc)); return it == day_.end() ? 0 : it->second; }
    // the mean of the full days before today that have readings (0 when none)
    double dailyAverage(int64_t nowUtc, int days = 14) const {
        const int64_t d0 = dayOf(nowUtc); double s = 0; int n = 0;
        for (const auto& [d, u] : day_) if (d < d0 && d >= d0 - days && d > firstDay_) { s += u; n++; }
        return n ? s / n : 0;
    }
    int resets() const { return resets_; }
    uint64_t readings() const { return readings_; }
    double lastValue() const { return last_; }

private:
    std::map<int64_t, double> hour_, day_;   // local hour / day index -> use
    double last_ = 0; int64_t lastUtc_ = 0; bool have_ = false; int resets_ = 0; uint64_t readings_ = 0;
    int64_t firstDay_ = INT64_MAX;
    int64_t dayOf(int64_t utc) const { return (int64_t)std::floor((double)(utc + tzOffsetSec) / 86400.0); }
    int64_t hourOf(int64_t utc) const { return (int64_t)std::floor((double)(utc + tzOffsetSec) / 3600.0); }
    // the use between two readings, spread evenly over the hours between them (a long gap is not all put in one hour)
    void spread(int64_t a, int64_t b, double d) {
        firstDay_ = std::min(firstDay_, dayOf(a));
        if (b <= a) { hour_[hourOf(b)] += d; day_[dayOf(b)] += d; return; }
        const double rate = d / (double)(b - a);
        int64_t t = a;
        while (t < b) {
            const int64_t hEnd = (hourOf(t) + 1) * 3600 - tzOffsetSec, e = std::min(b, hEnd);
            const double part = rate * (double)(e - t);
            hour_[hourOf(t)] += part; day_[dayOf(t)] += part;
            t = e;
        }
    }
    std::vector<Bucket> range(const std::map<int64_t, double>& m, int64_t len, int64_t nowUtc, int n) const {
        std::vector<Bucket> v; const int64_t cur = (int64_t)std::floor((double)(nowUtc + tzOffsetSec) / (double)len);
        for (int64_t i = cur - n + 1; i <= cur; i++) { auto it = m.find(i); v.push_back({i * len - tzOffsetSec, it == m.end() ? 0 : it->second}); }
        return v;
    }
};

// The cumulative register of a Sub-GHz meter from its decoded fields (rtl_433 names), with its unit; false when it has none
inline bool sensorMeterRegister(const SensorDevice& d, double& value, std::string& unit) {
    static const char* const keys[][2] = {{"total_kWh", "kWh"}, {"energy_kWh", "kWh"}, {"cumulative_kWh", "kWh"}, {"reading_kWh", "kWh"}, {"consumption_data", ""},
                                         {"consumption", ""}, {"total_m3", "m3"}, {"volume_m3", "m3"}, {"total_L", "L"}};
    for (const auto& k : keys)
        for (const auto& s : d.history)
            if (s.key == k[0] && !s.pts.empty()) { value = s.pts.back().v; unit = k[1]; return true; }
    return false;
}

} // namespace dect2
