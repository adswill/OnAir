#include "dect2/ease/navtex.h"
#include "dect2/geo/maidenhead.h"
#include <algorithm>

namespace dect2::ease {

int navtexSlotMinute(const NavtexStation& s) {
    if (s.slotMin >= 0) return s.slotMin;
    return (s.letter - 'A') * 10;
}

int navtexSecsToNext(const NavtexStation& s, int64_t u) {
    const int64_t cycle = 4 * 3600;
    int64_t into = (u - navtexSlotMinute(s) * 60LL) % cycle;
    if (into < 0) into += cycle;
    return into == 0 ? 0 : (int)(cycle - into);
}

std::vector<NavtexNear> navtexNearest(double lat, double lon, int khz, int64_t u, int maxCount, double maxKm) {
    std::vector<NavtexNear> all;
    for (int i = 0; i < kNavtexStationCount; i++) {
        const NavtexStation& s = kNavtexStations[i];
        if (s.khz != khz) continue;
        NavtexNear n; n.st = &s;
        greatCircle(lat, lon, s.lat, s.lon, n.km, n.bearing);
        n.secsToNext = navtexSecsToNext(s, u);
        all.push_back(n);
    }
    std::sort(all.begin(), all.end(), [](const NavtexNear& a, const NavtexNear& b) { return a.km < b.km; });
    std::vector<NavtexNear> out;
    for (const auto& n : all) {
        if ((int)out.size() >= maxCount) break;
        if (n.km > maxKm && !out.empty()) break;
        out.push_back(n);
    }
    return out;
}

const NavtexStation* navtexByLetter(char letter, int khz, bool havePos, double lat, double lon) {
    const NavtexStation* best = nullptr;
    double bestKm = 1e12;
    int found = 0;
    for (int i = 0; i < kNavtexStationCount; i++) {
        const NavtexStation& s = kNavtexStations[i];
        if (s.khz != khz || s.letter != letter) continue;
        found++;
        double km = 0, br = 0;
        if (havePos) greatCircle(lat, lon, s.lat, s.lon, km, br);
        if (km < bestKm) { bestKm = km; best = &s; }
    }
    return (havePos || found == 1) ? best : nullptr;
}

} // namespace dect2::ease
