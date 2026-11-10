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

static int hourOfDay(double t, double lon) {
    // local mean solar hour, where the sporadic curve lives
    double h = std::fmod(t / 3600.0 + lon / 15.0, 24.0);
    if (h < 0) h += 24;
    return (int)h % 24;
}

void MeteorRate::refit() {
    // alternate: the sporadic curve as the per-hour median of (count - k x shower), then k by least squares on the remainder
    for (int it = 0; it < 6; it++) {
        std::vector<double> byH[24];
        for (auto& m : h_) byH[hourOfDay(m.t, lon_)].push_back(m.count - k_ * m.showerTerm);
        for (int h = 0; h < 24; h++) {
            if (byH[h].empty()) continue;
            std::vector<double>& v = byH[h];
            std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
            sporadic_[h] = std::max(0.0, v[v.size() / 2]);
        }
        double num = 0, den = 0;
        for (auto& m : h_) if (!m.outburst) { num += (m.count - sporadic_[hourOfDay(m.t, lon_)]) * m.showerTerm; den += m.showerTerm * m.showerTerm; }
        k_ = den > 0 ? std::max(0.0, num / den) : 0;
    }
}

double MeteorRate::expected(double t) const { return sporadic_[hourOfDay(t, lon_)] + k_ * showerTerm(t); }

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
