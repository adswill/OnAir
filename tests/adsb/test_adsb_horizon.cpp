// Theoretical range outline (round 4 #16): flat sea gives the 4/3-earth radio horizon for antenna plus aircraft height at every bearing and
// altitude; a 600 m ridge 30 km north cuts the low rings there only; no elevation data counts as sea level; the upintheair.json reader.
#include "dect2/adsb/adsb_horizon.h"
#include "../common/test_util.h"
#include <cmath>
using namespace dect2;

static double distKm(double la1, double lo1, double la2, double lo2) {
    const double r = 3.14159265358979 / 180, a = std::sin((la2 - la1) * r / 2), b = std::sin((lo2 - lo1) * r / 2);
    const double h = a * a + std::cos(la1 * r) * std::cos(la2 * r) * b * b;
    return 6371.0 * 2 * std::atan2(std::sqrt(h), std::sqrt(1 - h));
}

int main() {
    const double lat = 55.0, lon = 24.0;
    prop::Terrain flat;
    {
        prop::ElevTile t;
        t.latTop = 60; t.lonLeft = 14; t.dLat = 0.02; t.dLon = 0.02; t.w = 1001; t.h = 501;
        t.z.assign((size_t)t.w * t.h, 0);
        flat.tiles.push_back(t);
    }
    const std::vector<double> alts = {300, 3000, 10000};
    const double kRm = 4.0 / 3.0 * 6371000;
    const auto rings = terrainHorizon(flat, lat, lon, 10, alts, 36, 600, 0.5);
    CHECK(rings.size() == 3 && rings[0].pts.size() == 36, "rings");
    for (size_t i = 0; i < rings.size(); i++) {
        const double want = (std::sqrt(2 * kRm * 10) + std::sqrt(2 * kRm * alts[i])) / 1000;
        double lo = 1e9, hi = 0;
        for (const auto& p : rings[i].pts) { const double d = distKm(lat, lon, p.first, p.second); lo = std::min(lo, d); hi = std::max(hi, d); }
        CHECK(std::fabs(lo - want) < 2 && std::fabs(hi - want) < 2, "flat sea, %.0f m: %.1f .. %.1f km, want %.1f", alts[i], lo, hi, want);
    }
    // a 600 m ridge, about 4 km wide, 30 km north, across the whole width
    prop::Terrain ridge = flat;
    {
        prop::ElevTile& t = ridge.tiles[0];
        for (int r = 0; r < t.h; r++) {
            const double la = t.latTop - r * t.dLat;
            const double dn = (la - lat) * 111.2;
            if (dn > 28 && dn < 32) for (int c = 0; c < t.w; c++) t.z[(size_t)r * t.w + c] = 600;
        }
    }
    const auto rr = terrainHorizon(ridge, lat, lon, 10, alts, 36, 600, 0.5);
    const double n300 = distKm(lat, lon, rr[0].pts[0].first, rr[0].pts[0].second), s300 = distKm(lat, lon, rr[0].pts[18].first, rr[0].pts[18].second);
    const double n10k = distKm(lat, lon, rr[2].pts[0].first, rr[2].pts[0].second);
    // behind a 600 m ridge at 30 km an aircraft at 300 m stays below the ridge line from about 30 km on
    CHECK(n300 < 32 && n300 > 25 && std::fabs(s300 - (std::sqrt(2 * kRm * 10) + std::sqrt(2 * kRm * 300)) / 1000) < 2, "300 m: north %.1f km (cut by the ridge), south %.1f km", n300, s300);
    // at 10 000 m the ridge's shadow: the line from the antenna over the ridge top climbs about 20 m per km
    CHECK(n10k < 400 && n10k > 200, "10 000 m north %.0f km (cut, but far)", n10k);
    // no data: sea level
    prop::Terrain none;
    const auto rn = terrainHorizon(none, lat, lon, 10, {3000}, 8, 600, 1);
    CHECK(std::fabs(distKm(lat, lon, rn[0].pts[3].first, rn[0].pts[3].second) - (std::sqrt(2 * kRm * 10) + std::sqrt(2 * kRm * 3000)) / 1000) < 2, "no elevation data");
    // upintheair.json
    std::vector<HorizonRing> u;
    std::string err;
    CHECK(parseUpintheair("{\"id\":\"ABCD1234\",\"lat\":55.0,\"lon\":24.0,\"refraction\":0.25,\"rings\":[{\"alt\":\"3048\",\"alt_type\":\"amsl\",\"points\":[[55.5,24.0],[55.0,24.8],[54.5,24.0],[55.0,23.2]]},"
                          "{\"alt\":12192,\"points\":[[56,24],[55,26],[54,24],[55,22]]},{\"alt\":\"1\",\"points\":[[1,2]]}]}", u, err) &&
          u.size() == 2 && u[0].altM == 3048 && u[1].altM == 12192 && u[0].pts.size() == 4 && u[0].pts[1].second == 24.8, "upintheair.json: %s", err.c_str());
    CHECK(!parseUpintheair("{\"x\":1}", u, err) && !parseUpintheair("not json", u, err) && !parseUpintheair("{\"rings\":[]}", u, err), "bad files");
    return testSummary("adsb_horizon");
}
