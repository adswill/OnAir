// Sunrise and sunset marks per VLF path (see vlf_terminator.h).
#include "dect2/smart5/vlf_terminator.h"
#include "dect2/smart5/sun.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>

namespace dect2 {
namespace smart5 {

namespace {
const double kPi = 3.14159265358979323846, kD = kPi / 180;
const double kGround = -0.833;                                     // refraction and the Sun's radius
const double kDRegion = -std::acos(6371.0 / (6371.0 + 85.0)) / kD;   // -9.3 deg: the Sun's rays still reach 85 km
double sunEl(double t, double lat, double lon) { double a, e; sunAzEl(t, lat, lon, a, e); return e; }
}

void greatCirclePoint(double la1, double lo1, double la2, double lo2, double f, double& la, double& lo) {
    const double p1 = la1 * kD, l1 = lo1 * kD, p2 = la2 * kD, l2 = lo2 * kD;
    const double d = 2 * std::asin(std::sqrt(std::pow(std::sin((p2 - p1) / 2), 2) + std::cos(p1) * std::cos(p2) * std::pow(std::sin((l2 - l1) / 2), 2)));
    if (d < 1e-12) { la = la1; lo = lo1; return; }
    const double A = std::sin((1 - f) * d) / std::sin(d), B = std::sin(f * d) / std::sin(d);
    const double x = A * std::cos(p1) * std::cos(l1) + B * std::cos(p2) * std::cos(l2);
    const double y = A * std::cos(p1) * std::sin(l1) + B * std::cos(p2) * std::sin(l2);
    const double z = A * std::sin(p1) + B * std::sin(p2);
    la = std::atan2(z, std::sqrt(x * x + y * y)) / kD;
    lo = std::atan2(y, x) / kD;
}

std::vector<TermEvent> vlfTerminator(const VlfPath& p, double day) {
    std::vector<TermEvent> out;
    double ml, mo; greatCirclePoint(p.txLat, p.txLon, p.rxLat, p.rxLon, 0.5, ml, mo);
    const double pts[3][2] = {{p.txLat, p.txLon}, {ml, mo}, {p.rxLat, p.rxLon}};
    for (int k = 0; k < 3; k++)
        for (int dr = 0; dr < 2; dr++) {
            const double h = dr ? kDRegion : kGround;
            double prev = sunEl(day, pts[k][0], pts[k][1]) - h;
            for (double t = day + 60; t <= day + 86400; t += 60) {
                const double cur = sunEl(t, pts[k][0], pts[k][1]) - h;
                if ((prev < 0) != (cur < 0)) {
                    double a = t - 60, b = t; const bool up = cur > 0;
                    for (int i = 0; i < 25; i++) { const double m = 0.5 * (a + b); if ((sunEl(m, pts[k][0], pts[k][1]) - h > 0) == up) b = m; else a = m; }
                    out.push_back({0.5 * (a + b), (TermPoint)k, up, dr == 1});
                }
                prev = cur;
            }
        }
    std::sort(out.begin(), out.end(), [](const TermEvent& a, const TermEvent& b) { return a.t < b.t; });
    return out;
}

double vlfSunlitFraction(const VlfPath& p, double t) {
    int lit = 0; const int n = 21;
    for (int i = 0; i < n; i++) {
        double la, lo; greatCirclePoint(p.txLat, p.txLon, p.rxLat, p.rxLon, i / (n - 1.0), la, lo);
        if (sunEl(t, la, lo) > kDRegion) lit++;
    }
    return (double)lit / n;
}

std::string termText(const TermEvent& e) {
    static const char* at[] = {"transmitter", "path midpoint", "receiver"};
    char b[120], ts[16];
    const time_t x = (time_t)e.t;
    struct tm tm;
#ifdef _WIN32
    gmtime_s(&tm, &x);
#else
    gmtime_r(&x, &tm);
#endif
    strftime(ts, sizeof ts, "%H:%M", &tm);
    snprintf(b, sizeof b, "%s UTC %s at the %s%s", ts, e.sunrise ? "sunrise" : "sunset", at[(int)e.at], e.dRegion ? " (D region)" : "");
    return b;
}

} // namespace smart5
} // namespace dect2
