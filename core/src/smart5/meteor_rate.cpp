// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Meteor scatter hourly rate against the shower calendar (see meteor_rate.h).
#include "dect2/smart5/meteor_rate.h"
#include "dect2/smart5/sun.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>

namespace dect2 {
namespace smart5 {

namespace {
const double kPi = 3.14159265358979323846, kD = kPi / 180;
// name, peak month, day, ZHR, radiant RA, dec (deg), B (fall-off per day)
struct Shower { const char* name; int m, d, zhr; double ra, dec, b; };
const Shower kShowers[] = {
    {"Quadrantids", 1, 4, 110, 230, 49, 0.8},
    {"Lyrids", 4, 22, 18, 271, 34, 0.3},
    {"Eta Aquariids", 5, 6, 50, 338, -1, 0.1},
    {"Arietids", 6, 7, 30, 44, 24, 0.1},
    {"Zeta Perseids", 6, 9, 20, 62, 23, 0.1},
    {"Southern Delta Aquariids", 7, 30, 25, 340, -16, 0.1},
    {"Perseids", 8, 12, 100, 48, 58, 0.2},
    {"Orionids", 10, 21, 20, 95, 16, 0.15},
    {"Leonids", 11, 17, 15, 152, 22, 0.3},
    {"Geminids", 12, 14, 150, 112, 33, 0.35},
    {"Ursids", 12, 22, 10, 217, 76, 0.5},
};
// day of year (0 - 364.x) of a time, and of a month/day
double doy(double t) {
    const time_t x = (time_t)t; struct tm tm;
#ifdef _WIN32
    gmtime_s(&tm, &x);
#else
    gmtime_r(&x, &tm);
#endif
    return tm.tm_yday + (tm.tm_hour + tm.tm_min / 60.0) / 24.0;
}
double doyOf(int m, int d) { static const int c[] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334}; return c[m - 1] + d - 1; }
}

std::vector<ShowerNow> showersAt(double t, double lat, double lon) {
    std::vector<ShowerNow> v;
    const double today = doy(t);
    for (const Shower& s : kShowers) {
        double dd = std::fabs(today - (doyOf(s.m, s.d) + 0.5));
        if (dd > 182.5) dd = 365.25 - dd;
        const double zhr = s.zhr * std::pow(10.0, -s.b * dd);
        if (zhr < 2) continue;
        double az, el; radecAzEl(t, s.ra, s.dec, lat, lon, az, el);
        v.push_back({s.name, zhr, el});
    }
    return v;
}

double MeteorRate::showerTerm(double t) const {
    double x = 0;
    for (auto& s : showersAt(t + 1800, lat_, lon_)) if (s.radiantElDeg > 0) x += s.zhrNow * std::sin(s.radiantElDeg * kD);
    return x;
}

// local mean solar time in hours, 0 .. 24, where the sporadic curve lives
static double solarHour(double t, double lon) {
    double h = std::fmod(t / 3600.0 + lon / 15.0, 24.0);
    if (h < 0) h += 24;
    return h;
}

// the regressors of the hourly model: the sporadic daily curve as a mean and two harmonics (the morning maximum and its shape), then the shower term
static void regressors(double t, double lon, double showerTerm, double x[6]) {
    const double w = 2 * 3.14159265358979323846 * solarHour(t, lon) / 24.0;
    x[0] = 1; x[1] = std::cos(w); x[2] = std::sin(w); x[3] = std::cos(2 * w); x[4] = std::sin(2 * w); x[5] = showerTerm;
}

void MeteorRate::refit() {
    // one least-squares fit over every hour not flagged as an outburst: the smooth sporadic curve and the shower sensitivity together. A curve made
    // of per-hour medians had only 3 to 8 counts behind each hour in the first days, so its Poisson noise read as outbursts (and its result
    // depended on the exact random draws of a test, which differ between the C++ libraries).
    double A[6][7] = {};
    int n = 0;
    for (const auto& m : h_) {
        if (m.outburst) continue;
        double x[6];
        regressors(m.t, lon_, m.showerTerm, x);
        for (int r = 0; r < 6; r++) { for (int c = 0; c < 6; c++) A[r][c] += x[r] * x[c]; A[r][6] += x[r] * m.count; }
        n++;
    }
    if (n < 12) return;
    for (int r = 0; r < 6; r++) A[r][r] += 1e-6 * (A[r][r] + 1);   // a shower term that stays 0 (no shower active) leaves the system solvable
    for (int c = 0; c < 6; c++) {   // Gauss-Jordan with partial pivoting
        int p = c;
        for (int r = c + 1; r < 6; r++) if (std::fabs(A[r][c]) > std::fabs(A[p][c])) p = r;
        if (std::fabs(A[p][c]) < 1e-12) return;
        if (p != c) for (int k = 0; k < 7; k++) std::swap(A[p][k], A[c][k]);
        for (int r = 0; r < 6; r++) {
            if (r == c) continue;
            const double f = A[r][c] / A[c][c];
            for (int k = c; k < 7; k++) A[r][k] -= f * A[c][k];
        }
    }
    for (int r = 0; r < 6; r++) coef_[r] = A[r][6] / A[r][r];
    k_ = std::max(0.0, coef_[5]);
    for (int h = 0; h < 24; h++) {
        const double w = 2 * 3.14159265358979323846 * (h + 0.5) / 24.0;
        sporadic_[h] = std::max(0.0, coef_[0] + coef_[1] * std::cos(w) + coef_[2] * std::sin(w) + coef_[3] * std::cos(2 * w) + coef_[4] * std::sin(2 * w));
    }
}

double MeteorRate::expected(double t) const {
    double x[6];
    regressors(t, lon_, showerTerm(t), x);
    double spor = 0;
    for (int r = 0; r < 5; r++) spor += coef_[r] * x[r];
    return std::max(0.0, spor) + k_ * x[5];
}

void MeteorRate::hour(double t, int count) {
    MeteorHour m; m.t = t; m.count = count; m.showerTerm = showerTerm(t);
    if (ready()) {
        m.expected = expected(t);
        m.outburst = count > m.expected + 3 * std::sqrt(std::max(1.0, m.expected)) + 2 && count > 1.5 * m.expected;
    }
    h_.push_back(m);
    if (h_.size() > 24 * 60) h_.erase(h_.begin());
    refit();
}

std::string MeteorRate::text() const {
    if (h_.empty()) return "";
    const MeteorHour& m = h_.back();
    char b[200];
    if (!ready()) snprintf(b, sizeof b, "%d meteors this hour; learning the daily curve (%zu of 72 hours)", m.count, h_.size());
    else snprintf(b, sizeof b, "%d meteors this hour, %.0f expected%s", m.count, m.expected, m.outburst ? " - possible outburst" : "");
    return b;
}

} // namespace smart5
} // namespace dect2
