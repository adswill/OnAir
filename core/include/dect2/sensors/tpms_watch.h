// My tyres (3.9, docs/SMART5-iot-3.9.md): TPMS sensors the user has marked as their own vehicle's. Each reading's pressure is corrected
// to 20 C (the gas law on absolute pressure), so a cold morning is not a leak; from the corrected values over the last days: a slow leak
// (the least squares slope, kPa per day, over at least 2 days and 8 readings), a pressure under the user's minimum, and one tyre far below
// the others of the same vehicle. Sensors that are not marked are never followed (passing cars send TPMS too).
// Pure logic, no clock: times are Unix seconds.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <map>
#include <string>
#include <vector>

namespace dect2 {

inline double tpmsCorrectTo20C(double gaugeKpa, double tempC) {
    if (!std::isfinite(tempC) || tempC < -60 || tempC > 150) return gaugeKpa;
    return (gaugeKpa + 101.325) * (293.15 / (tempC + 273.15)) - 101.325;
}

struct TpmsTyre {
    std::string id, vehicle, position;   // position as the user named it ("front left"), may be empty
    double lastKpa = 0, lastCorrKpa = 0, lastTempC = NAN;
    int64_t lastUtc = 0;
    int readings = 0;
    bool hasTrend = false; double kpaPerDay = 0; double trendDays = 0;
    bool leak = false, low = false, imbalance = false;
    std::string text;
};

class TpmsWatch {
public:
    double leakKpaPerDay = -2.0;  // a healthy tyre loses well under 0.3 kPa a day
    double lowKpa = 180;          // corrected to 20 C
    double imbalanceKpa = 25;     // below the median of the vehicle's other tyres
    double historyDays = 14;

    void mark(const std::string& id, const std::string& vehicle, const std::string& position = "") { TpmsTyre& t = tyres_[id]; t.id = id; t.vehicle = vehicle; t.position = position; }
    void unmark(const std::string& id) { tyres_.erase(id); hist_.erase(id); }
    bool isMine(const std::string& id) const { return tyres_.count(id) != 0; }

    // a reading of a sensor; ignored unless it is marked. tempC NAN when the sensor sends none.
    void add(const std::string& id, int64_t utc, double gaugeKpa, double tempC) {
        auto it = tyres_.find(id);
        if (it == tyres_.end() || !std::isfinite(gaugeKpa) || gaugeKpa < 0 || gaugeKpa > 1500) return;
        TpmsTyre& t = it->second;
        auto& h = hist_[id];
        if (!h.empty() && utc - h.back().first < 60) return;   // one per minute (a sensor repeats each frame)
        const double corr = tpmsCorrectTo20C(gaugeKpa, tempC);
        h.push_back({utc, corr});
        while (!h.empty() && utc - h.front().first > historyDays * 86400) h.pop_front();
        t.lastKpa = gaugeKpa; t.lastCorrKpa = corr; t.lastTempC = tempC; t.lastUtc = utc; t.readings++;
        // trend
        t.hasTrend = false;
        const double span = h.empty() ? 0 : (double)(h.back().first - h.front().first) / 86400;
        if (h.size() >= 8 && span >= 2) {
            double sx = 0, sy = 0, sxx = 0, sxy = 0; const double n = (double)h.size(), t0 = (double)h.front().first;
            for (auto& [u, p] : h) { const double x = ((double)u - t0) / 86400; sx += x; sy += p; sxx += x * x; sxy += x * p; }
            const double den = n * sxx - sx * sx;
            if (den > 0) { t.kpaPerDay = (n * sxy - sx * sy) / den; t.hasTrend = true; t.trendDays = span; }
        }
        evaluate(t.vehicle);
    }
    const std::map<std::string, TpmsTyre>& tyres() const { return tyres_; }

private:
    std::map<std::string, TpmsTyre> tyres_;
    std::map<std::string, std::deque<std::pair<int64_t, double>>> hist_;
    void evaluate(const std::string& vehicle) {
        std::vector<TpmsTyre*> v; for (auto& [k, t] : tyres_) if (t.vehicle == vehicle && t.readings) v.push_back(&t);
        for (TpmsTyre* t : v) {
            t->leak = t->hasTrend && t->kpaPerDay < leakKpaPerDay;
            t->low = t->lastCorrKpa < lowKpa;
            std::vector<double> o; for (TpmsTyre* u : v) if (u != t) o.push_back(u->lastCorrKpa);
            t->imbalance = false;
            if (o.size() >= 2) { std::sort(o.begin(), o.end()); const double med = o[o.size() / 2]; t->imbalance = med - t->lastCorrKpa > imbalanceKpa; }
            const std::string name = t->position.empty() ? t->id : t->position;
            char b[200]; b[0] = 0;
            if (t->leak) snprintf(b, sizeof b, "%s: slow leak, %.1f kPa a day over %.1f days (at 20 C: %.0f kPa)", name.c_str(), t->kpaPerDay, t->trendDays, t->lastCorrKpa);
            else if (t->low) snprintf(b, sizeof b, "%s: low, %.0f kPa at 20 C", name.c_str(), t->lastCorrKpa);
            else if (t->imbalance) snprintf(b, sizeof b, "%s: %.0f kPa below the other tyres", name.c_str(), (o.empty() ? 0 : o[o.size() / 2]) - t->lastCorrKpa);
            t->text = b;
        }
    }
};

} // namespace dect2
