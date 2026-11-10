// Sun outage predictor (see sun_outage.h).
#include "dect2/smart5/sun_outage.h"
#include "dect2/smart5/sun.h"
#include <cmath>
#include <cstdio>
#include <ctime>

namespace dect2 {
namespace smart5 {

namespace {
const double kPi = 3.14159265358979323846, kD = kPi / 180;
const double kGeoKm = 42164.17, kSunRadiusDeg = 0.267;
}

double dishBeamwidthDeg(double d, double f) { return d > 0 && f > 0 ? 70.0 * (299792458.0 / f) / d : 0; }

double sunSatSeparation(double t, double lat, double lon, double satLon) {
    double site[3]; siteEcef(lat, lon, 0, site);
    const double sat[3] = {kGeoKm * std::cos(satLon * kD), kGeoKm * std::sin(satLon * kD), 0};
    double v[3] = {sat[0] - site[0], sat[1] - site[1], sat[2] - site[2]};
    const double r = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    const SunPos s = sunAt(t);
    double c = (v[0] * s.ecef[0] + v[1] * s.ecef[1] + v[2] * s.ecef[2]) / r;
    c = std::max(-1.0, std::min(1.0, c));
    return std::acos(c) / kD;
}

bool inSunOutage(double t, double lat, double lon, double satLon, double bw) {
    return sunSatSeparation(t, lat, lon, satLon) <= bw / 2 + kSunRadiusDeg;
}

std::vector<SunOutage> sunOutages(double t0, double t1, double lat, double lon, double satLon, double bw) {
    std::vector<SunOutage> out;
    const double lim = bw / 2 + kSunRadiusDeg;
    // the Sun moves 0.25 deg a minute across the sky: a 20 s step cannot miss a beam of 0.2 deg or more; the edges are bisected
    const double step = 20;
    auto inside = [&](double t) { return sunSatSeparation(t, lat, lon, satLon) <= lim; };
    auto edge = [&](double a, double b, bool aIn) {   // a and b straddle the edge
        for (int i = 0; i < 30; i++) { const double m = 0.5 * (a + b); if (inside(m) == aIn) a = m; else b = m; }
        return 0.5 * (a + b);
    };
    double t = t0;
    while (t < t1) {
        const double sep = sunSatSeparation(t, lat, lon, satLon);
        // far away: jump ahead (the separation shrinks at most ~0.26 deg per minute)
        if (sep > lim + 2) { t += std::max(step, (sep - lim - 1) / 0.26 * 60); continue; }
        if (sep <= lim) {
            SunOutage o;
            o.start = t > t0 ? edge(t - step, t, false) : t0;
            double u = t, best = sep, bt = t;
            while (u < t1 && inside(u)) {
                const double s2 = sunSatSeparation(u, lat, lon, satLon);
                if (s2 < best) { best = s2; bt = u; }
                u += step;
            }
            o.end = u < t1 ? edge(u - step, u, true) : t1;
            o.peak = bt; o.minSepDeg = best;
            out.push_back(o);
            t = u;
            continue;
        }
        t += step;
    }
    return out;
}

std::string sunOutageText(const SunOutage& o) {
    char b[160], d0[32], d1[32];
    const time_t a = (time_t)o.start, e = (time_t)o.end;
    struct tm ta, te;
#ifdef _WIN32
    gmtime_s(&ta, &a); gmtime_s(&te, &e);
#else
    gmtime_r(&a, &ta); gmtime_r(&e, &te);
#endif
    strftime(d0, sizeof d0, "%Y-%m-%d %H:%M:%S", &ta);
    strftime(d1, sizeof d1, "%H:%M:%S", &te);
    snprintf(b, sizeof b, "sun outage %s - %s UTC (%.0f s, closest %.2f deg)", d0, d1, o.end - o.start, o.minSepDeg);
    return b;
}

} // namespace smart5
} // namespace dect2
