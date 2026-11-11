// Radiosonde ascent sounding and landing before burst (sonde_profile.h): a simulated flight through a model wind and temperature field,
// with GNSS noise, lost frames and the app's track thinning (one point per 6 s). Simulated only.
#include "dect2/sonde/sonde_profile.h"
#include <cmath>
#include <random>
#include "../common/test_util.h"
using namespace dect2;

static double windE(double z) { const double k = z / 1000; return k < 12 ? 5 + 2 * k : std::max(3.0, 29 - 2 * (k - 12)); }
static double windN(double z) { return 3 * std::sin(z / 4000); }
static double tempAt(double z) { const double k = z / 1000; return k < 11 ? 12 - 6.5 * k : k < 20 ? -59.5 : -59.5 + (k - 20); }

struct Flight { std::vector<SondeTrackPoint> track; double landLat = 0, landLon = 0; };
// 5 m/s up from 100 m to burstM, then down at v0 * sqrt(rho0 / rho); positions every second, kept one in `keep`
static Flight fly(double burstM, double posNoise, double loss, int keep, unsigned seed, double stopAtM = 1e9) {
    std::mt19937 r(seed); std::normal_distribution<double> g(0, 1); std::uniform_real_distribution<double> u(0, 1);
    Flight f; double E = 0, N = 0, z = 100; const double lat0 = 54.68, lon0 = 25.28; bool up = true; uint32_t t = 1700000000; int i = 0;
    while (z > 0) {
        const double vz = up ? 5 : -5.5 * std::exp(z / 14000.0);
        E += windE(z); N += windN(z); z += vz; t++;
        if (up && z >= burstM) up = false;
        if (up && z >= stopAtM) break;
        if (i++ % keep || u(r) < loss) continue;
        SondeTrackPoint p; p.unixT = t; p.altM = (float)z;
        p.lat = (float)(lat0 + (N + posNoise * g(r)) / 6371000.0 * 180 / M_PI);
        p.lon = (float)(lon0 + (E + posNoise * g(r)) / (6371000.0 * std::cos(lat0 * M_PI / 180)) * 180 / M_PI);
        p.tempC = (float)(tempAt(z) + 0.3 * g(r));
        f.track.push_back(p);
    }
    f.landLat = lat0 + N / 6371000.0 * 180 / M_PI; f.landLon = lon0 + E / (6371000.0 * std::cos(lat0 * M_PI / 180)) * 180 / M_PI;
    return f;
}
static double km(double a, double b, double c, double d) { const double x = (d - b) * M_PI / 180 * std::cos(a * M_PI / 180), y = (c - a) * M_PI / 180; return 6371 * std::hypot(x, y); }

int main() {
    for (double loss : {0.0, 0.3}) for (double noise : {3.0, 10.0}) {
        const Flight whole = fly(30000, noise, loss, 6, 1);
        SondeSounding s = sondeSounding(whole.track);
        CHECK(s.burst && std::fabs(s.burstM - 30000) < 60, "burst %.0f", s.burstM);
        CHECK(std::fabs(s.ascentMs - 5) < 0.2, "ascent %.2f", s.ascentMs);
        CHECK(s.hasFreezing && std::fabs(s.freezingM - 12 / 6.5 * 1000) < 250, "loss %.1f noise %.0f: freezing %.0f m", loss, noise, s.freezingM);
        CHECK(s.hasTropo && std::fabs(s.tropoM - 11000) < 700, "tropopause %.0f m", s.tropoM);
        int bad = 0, n = 0;
        for (const auto& ly : s.layers) {
            if (!ly.hasWind) continue;
            n++;
            const double z = ly.baseM + 250;
            if (std::hypot(ly.ve - windE(z), ly.vn - windN(z)) > 1.0) bad++;
        }
        CHECK(n >= 55 && bad <= 2, "loss %.1f noise %.0f: layer winds %d, off by > 1 m/s: %d", loss, noise, n, bad);
        // predictions during the climb, against where the full flight came down
        double prevErr = 1e9;
        for (double at : {12000.0, 20000.0, 28000.0}) {
            const Flight part = fly(30000, noise, loss, 6, 1, at);
            const auto& p = part.track.back();
            SondeEarlyLanding e = predictSondeLandingAscent(part.track, p.lat, p.lon, p.altM, 30000, 5.5, 0);
            const double err = e.ok ? km(e.lat, e.lon, whole.landLat, whole.landLon) : 1e9;
            const double lim = at < 15000 ? 70 : at < 25000 ? 26 : 3;   // early on the winds above the jet are not known yet (weaker here than extrapolated)
            CHECK(e.ok && err < lim, "loss %.1f noise %.0f: from %.0f km the landing is %.1f km off (limit %.0f)", loss, noise, at / 1000, err, lim);
            CHECK(err <= prevErr + 1, "the error shrinks as more is measured: %.1f after %.1f", err, prevErr);
            prevErr = err;
            if (at == 28000.0) CHECK(e.guessedFromM > 27000 && e.secondsToLanding > 2000 && e.secondsToLanding < 5000, "guess from %.0f, %.0f s", e.guessedFromM, e.secondsToLanding);
        }
    }
    {   // a wrong burst height moves the point; burst lower than now, after burst, no time and too short tracks give nothing
        const Flight part = fly(30000, 3, 0, 6, 2, 15000);
        const auto& p = part.track.back();
        CHECK(!predictSondeLandingAscent(part.track, p.lat, p.lon, p.altM, 14000).ok, "burst below the sonde");
        CHECK(!predictSondeLandingAscent(fly(30000, 3, 0, 6, 3).track, p.lat, p.lon, p.altM).ok, "already burst");
        std::vector<SondeTrackPoint> nt = part.track; for (auto& q : nt) q.unixT = 0;
        CHECK(!predictSondeLandingAscent(nt, p.lat, p.lon, p.altM).ok, "no time");
        CHECK(!predictSondeLandingAscent({}, p.lat, p.lon, p.altM).ok && sondeSounding({}).layers.empty(), "empty");
        const auto a = predictSondeLandingAscent(part.track, p.lat, p.lon, p.altM, 25000), b = predictSondeLandingAscent(part.track, p.lat, p.lon, p.altM, 35000);
        CHECK(a.ok && b.ok && km(a.lat, a.lon, b.lat, b.lon) > 2, "burst height matters %.1f km", km(a.lat, a.lon, b.lat, b.lon));
    }
    return testSummary("sonde_profile");
}
