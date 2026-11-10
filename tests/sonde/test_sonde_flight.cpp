// Radiosonde flight track (round 4 #37): a simulated flight up to a 31 km burst and down to 400 m gives an ascent and a descent line, the burst,
// the landing; a sonde still going up gives one line and its prediction; the KML and GPX carry the lines with altitude.
#include "dect2/sonde/sonde_flight.h"
#include "../common/test_util.h"
#include <cmath>
using namespace dect2;

int main() {
    SondeInfo s;
    s.serial = "S1234567"; s.type = "RS41";
    for (int t = 0; t <= 7200; t += 30) {
        SondeTrackPoint p;
        const double alt = t <= 6000 ? 100 + 5.0 * t : 30100 - (t - 6000) * 25.0;
        p.altM = (float)std::max(400.0, alt); p.lat = (float)(54.6 + t * 1e-5); p.lon = (float)(25.3 + t * 3e-5); p.unixT = 1'800'000'000 + (uint32_t)t;
        s.track.push_back(p);
    }
    s.trackIncluded = true; s.hasPos = true;
    SondeLanding none;
    const SondeFlight f = sondeFlight(s, none);
    CHECK(f.burst && f.landed && f.tracks.size() == 2 && f.tracks[0].name == "S1234567 ascent" && f.tracks[1].name == "S1234567 descent", "two lines, burst, landed");
    CHECK(f.points.size() == 3 && f.points[1].name == "S1234567 burst" && std::fabs(f.points[1].altM - 30100) < 1 && f.points[2].name == "S1234567 landed (last heard)", "points");
    CHECK(f.tracks[0].pts.back().altM == f.tracks[1].pts.front().altM && f.tracks[0].pts.size() == 201, "the lines meet at the burst (%zu)", f.tracks[0].pts.size());
    const std::string kml = xport::toKml(f.points, f.tracks, "S1234567");
    CHECK(kml.find("<LineString>") != std::string::npos && kml.find("S1234567 descent") != std::string::npos && kml.find(",30100") != std::string::npos, "KML has the lines with altitude");
    const std::string gpx = xport::toGpx(f.points, f.tracks, "S1234567");
    CHECK(gpx.find("<trk>") != std::string::npos && gpx.find("<ele>30100") != std::string::npos && gpx.find("S1234567 burst") != std::string::npos, "GPX has the tracks and the burst");
    // still rising, with a (made-up) prediction: no burst, one line, the prediction is a point
    SondeInfo up = s;
    up.track.resize(100);
    SondeLanding pred; pred.valid = true; pred.lat = 55.0; pred.lon = 26.0; pred.secondsLeft = 3600;
    const SondeFlight g = sondeFlight(up, pred);
    CHECK(!g.burst && !g.landed && g.tracks.size() == 1 && g.points.size() == 3 && g.points[2].name == "S1234567 predicted landing" && g.points[1].name == "S1234567 last position", "rising");
    SondeInfo one; one.serial = "X"; one.hasPos = true; one.lat = 1; one.lon = 2; one.altM = 300;
    const SondeFlight h = sondeFlight(one, none);
    CHECK(h.tracks.empty() && h.points.size() == 2, "one position: points only");
    return testSummary("sonde_flight");
}
