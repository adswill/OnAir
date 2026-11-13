// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// A sensor's health, learned from what it sends: its reporting interval (a repeat within 3 s is the same transmission), silence against that interval, a reading far from its own
// recent values, two sensors sharing an id (readings that keep jumping between two levels), and a battery voltage that is falling.
#pragma once
#include "sensors_tel.h"
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace dect2 {

inline double sensorMedian(std::vector<double> v) { if (v.empty()) return 0; std::nth_element(v.begin(), v.begin() + (long)v.size() / 2, v.end()); return v[v.size() / 2]; }

// call with every message; a key that is really a level (temperature, humidity, voltage) can be judged
inline void sensorHealthMessage(SensorHealth& h, const SensorDevice& d, int64_t utc) {
    if (h.lastTx) {
        const double gap = (double)(utc - h.lastTx);
        if (gap >= 3) {
            h.intervalSec = h.gaps == 0 ? gap : h.intervalSec + (std::min(gap, 4 * h.intervalSec + 1) - h.intervalSec) * 0.2;
            h.gaps++;
            h.lastTx = utc;
        }
    } else h.lastTx = utc;
    h.outlier = h.collision = h.batteryFalling = false;
    for (const auto& s : d.history) {
        if (s.pts.size() < 12) continue;
        std::string k = s.key; for (auto& c : k) c = (char)std::tolower((unsigned char)c);
        const bool level = k.find("temperature") != std::string::npos || k.find("humidity") != std::string::npos || k.find("pressure") != std::string::npos;
        const bool batt = k.find("batt") != std::string::npos && k.find("ok") == std::string::npos;
        if (!level && !batt) continue;
        const size_t n = std::min<size_t>(s.pts.size(), 40);
        std::vector<double> v; for (size_t i = s.pts.size() - n; i < s.pts.size(); i++) v.push_back(s.pts[i].v);
        const double med = sensorMedian(v);
        std::vector<double> dev; for (double x : v) dev.push_back(std::fabs(x - med));
        const double mad = sensorMedian(dev);
        if (level) {
            const double lim = std::max(k.find("pressure") != std::string::npos ? 15.0 : 8.0, 6 * mad);
            if (std::fabs(v.back() - med) > lim) { h.outlier = true; h.detail = s.key; }
            // two levels taking turns: many sign changes with big swings
            int alt = 0;
            const double range = *std::max_element(v.begin(), v.end()) - *std::min_element(v.begin(), v.end());
            const double swing = std::max(3.0, 0.5 * range);
            for (size_t i = 1; i < v.size(); i++) if (std::fabs(v[i] - v[i - 1]) > swing) alt++;
            if (alt >= (int)v.size() / 3) { h.collision = true; h.detail = s.key; }
        } else {
            const size_t m = v.size() / 3;
            if (m >= 4) {
                double a = 0, b = 0;
                for (size_t i = 0; i < m; i++) { a += v[i]; b += v[v.size() - 1 - i]; }
                a /= m; b /= m;
                if (a > 0 && b < a * 0.97) { h.batteryFalling = true; h.detail = s.key; }
            }
        }
    }
}
// call whenever: silent when it has been quiet for 3.5 of its intervals (and at least 5 minutes)
inline void sensorHealthTick(SensorHealth& h, int64_t now) { h.silent = h.gaps >= 3 && h.intervalSec > 0 && (double)(now - h.lastTx) > std::max(300.0, 3.5 * h.intervalSec); }

} // namespace dect2
