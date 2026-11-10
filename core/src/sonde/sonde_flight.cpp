// Radiosonde flight track: see sonde_flight.h.
#include "dect2/sonde/sonde_flight.h"
#include <cstdio>

namespace dect2 {

SondeFlight sondeFlight(const SondeInfo& s, const SondeLanding& pred, double landedBelowM) {
    SondeFlight f;
    std::vector<SondeTrackPoint> tr = s.track;
    if (tr.empty() && s.hasPos) { SondeTrackPoint p; p.lat = (float)s.lat; p.lon = (float)s.lon; p.altM = (float)s.altM; p.unixT = (uint32_t)s.unixTime; tr.push_back(p); }
    if (tr.empty()) return f;
    size_t top = 0;
    for (size_t i = 0; i < tr.size(); i++) if (tr[i].altM > tr[top].altM) top = i;
    auto gp = [&](const SondeTrackPoint& p, const std::string& name, const std::string& desc) {
        xport::GeoPoint g; g.name = name; g.desc = desc; g.lat = p.lat; g.lon = p.lon; g.hasAlt = true; g.altM = p.altM; g.time = p.unixT; return g;
    };
    xport::GeoTrack up; up.name = s.serial + " ascent";
    xport::GeoTrack down; down.name = s.serial + " descent";
    for (size_t i = 0; i <= top; i++) up.pts.push_back(gp(tr[i], "", ""));
    for (size_t i = top; i < tr.size(); i++) down.pts.push_back(gp(tr[i], "", ""));
    char b[96];
    f.points.push_back(gp(tr.front(), s.serial + " first heard", s.type));
    // a burst: the highest point is well above the last one (more than 1000 m) and not the newest
    f.burst = top + 1 < tr.size() && tr[top].altM - tr.back().altM > 1000;
    if (f.burst) { std::snprintf(b, sizeof b, "%.0f m", tr[top].altM); f.points.push_back(gp(tr[top], s.serial + " burst", b)); }
    f.landed = f.burst && tr.back().altM < landedBelowM;
    std::snprintf(b, sizeof b, "%.0f m", tr.back().altM);
    f.points.push_back(gp(tr.back(), s.serial + (f.landed ? " landed (last heard)" : " last position"), b));
    if (pred.valid && !f.landed) {
        xport::GeoPoint g; g.name = s.serial + " predicted landing"; g.lat = pred.lat; g.lon = pred.lon; g.time = tr.back().unixT ? tr.back().unixT + pred.secondsLeft : 0;
        f.points.push_back(g);
    }
    if (up.pts.size() >= 2) f.tracks.push_back(up);
    if (down.pts.size() >= 2) f.tracks.push_back(down);
    return f;
}

}  // namespace dect2
