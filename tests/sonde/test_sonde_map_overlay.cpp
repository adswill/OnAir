// Radiosonde map overlay (dect2/sonde/sonde_map_overlay.h, sonde_chase.h): the launch sites in view with their names, a learned site
// hiding the known one it sits on, the station and the chase car with the callsign, a view across the 180 degree line, the thinning when
// zoomed out; the Tawhiri request built from a climbing and a falling sonde, its URL, and the parse of an answer and of an error; the
// chase GPS lines (NMEA GGA with altitude, RMC, no fix, a bad checksum; gpsd TPV).
#include "dect2/sonde/sonde_map_overlay.h"
#include "dect2/sonde/sonde_chase.h"
#include "../common/test_util.h"
#include <cmath>
#include <string>
using namespace dect2;

static bool has(const std::string& s, const std::string& x) { return s.find(x) != std::string::npos; }
static const SondeMapMark* find(const std::vector<SondeMapMark>& v, SondeMapMark::Kind k, const std::string& label) {
    for (const auto& m : v) if (m.kind == k && m.label == label) return &m;
    return nullptr;
}

static void testMarks() {
    KnownSondeSites known;
    SondeMapBox de; de.south = 47; de.north = 55; de.west = 5.5; de.east = 15.5;   // Germany
    SondeMapStation none;
    auto v = sondeMapMarks(de, &known, {}, none);
    const char* expect[] = {"Bergen", "Lindenberg", "Idar-Oberstein", "Meppen", "Essen-Bredeney", "Stuttgart/Schnarrenberg", "Hohenpeissenberg", "Greifswald", "Norderney", "Schleswig"};
    for (const char* e : expect) CHECK(find(v, SondeMapMark::KnownSite, e), "known site %s in view", e);
    for (const auto& m : v) CHECK(m.lat >= 47 && m.lat <= 55 && m.lon >= 5.5 && m.lon <= 15.5, "only sites in the box (%s)", m.label.c_str());
    // a learned site at Bergen hides the known Bergen and shows with its own name and launches
    SondeSite l; l.id = 1; l.name = "Bergen"; l.lat = 52.83; l.lon = 9.95; l.launches = 7;
    SondeMapStation me; me.set = true; me.lat = 52.40; me.lon = 9.70; me.label = "DL1ABC";
    v = sondeMapMarks(de, &known, {l}, me);
    CHECK(find(v, SondeMapMark::LearnedSite, "Bergen") && find(v, SondeMapMark::LearnedSite, "Bergen")->launches == 7 && !find(v, SondeMapMark::KnownSite, "Bergen"), "learned Bergen replaces the known one");
    CHECK(find(v, SondeMapMark::Station, "DL1ABC"), "my station with the callsign");
    me.live = true; me.label = "";
    v = sondeMapMarks(de, &known, {l}, me);
    CHECK(find(v, SondeMapMark::ChaseCar, "My station") && !find(v, SondeMapMark::Station, "My station"), "a live GPS position is the chase car");
    me.lat = 40; v = sondeMapMarks(de, &known, {l}, me);
    for (const auto& m : v) CHECK(m.kind != SondeMapMark::ChaseCar, "station outside the view is not drawn");
    // across the 180 degree line: Shemya (174.1 E) and a user site at 170 W
    known.addUserText("Dateline test; 52.0; -170.0\n");
    SondeMapBox dl; dl.south = 45; dl.north = 60; dl.west = 170; dl.east = -165;
    v = sondeMapMarks(dl, &known, {}, none);
    CHECK(find(v, SondeMapMark::KnownSite, "Dateline test") && find(v, SondeMapMark::KnownSite, "Shemya AFB"), "both sides of the 180 degree line (%zu marks)", v.size());
    // zoomed out to the world: thinned evenly, user sites first
    SondeMapBox world;
    v = sondeMapMarks(world, &known, {}, none, 100);
    CHECK(v.size() == 100 && v[0].label == "Dateline test", "the world view is thinned to 100 (%zu)", v.size());
    v = sondeMapMarks(world, &known, {}, none, 5000);
    CHECK(v.size() == known.all().size(), "unthinned: every site (%zu of %zu)", v.size(), known.all().size());
    CHECK(sondeMapMarks(de, nullptr, {l}, none).size() == 1, "known sites off: only the learned one");
}

static void testTawhiri() {
    const double now = 1763000000;
    SondeInfo s; s.serial = "S1"; s.hasPos = true; s.lat = 52.8; s.lon = -1.5; s.altM = 8000; s.hasVel = true; s.vSpeed = 5.3; s.hSpeed = 10;
    s.maxAltM = 8000; s.hasTime = true; s.unixTime = now - 2;
    tawhiri::Request r; std::string why;
    CHECK(tawhiri::requestFor(s, 30000, 5, now, r) && !r.descending && std::fabs(r.ascentMs - 5.3) < 1e-9 && r.burstM == 30000 && r.descentMs == 5 && r.unixT == now - 2, "climbing: the measured climb up to the assumed burst");
    s.altM = 31000; s.maxAltM = 31000;
    CHECK(tawhiri::requestFor(s, 30000, 5, now, r) && r.burstM == 31100, "above the assumed burst: 100 m higher");
    s.vSpeed = 0.01; s.altM = 500; s.maxAltM = 500;
    CHECK(tawhiri::requestFor(s, 30000, 5, now, r) && r.ascentMs == 0.1, "climb at least 0.1 m/s");
    s.vSpeed = -20; s.altM = 14000; s.maxAltM = 33000;
    CHECK(tawhiri::requestFor(s, 30000, 5, now, r) && r.descending && r.burstM == 14001 && std::fabs(r.descentMs - 20 * std::exp(-1.0)) < 1e-6, "falling: burst 1 m above, sea-level rate (%g)", r.descentMs);
    CHECK(std::fabs(tawhiri::seaLevelDescent(-5, 0) - 5) < 1e-12, "at sea level the rate is the rate");
    s.hasVel = false; CHECK(!tawhiri::requestFor(s, 30000, 5, now, r, &why) && has(why, "climb"), "no speed: no request");
    s.hasVel = true; s.hasPos = false; CHECK(!tawhiri::requestFor(s, 30000, 5, now, r, &why), "no position: no request");
    s.hasPos = true; s.hasTime = false;
    CHECK(tawhiri::requestFor(s, 30000, 5, now, r) && r.unixT == now, "no sonde time: the clock");
    tawhiri::Request q; q.lat = 52.8; q.lon = -1.5; q.altM = 8000; q.unixT = 1700000000; q.ascentMs = 5; q.burstM = 30000; q.descentMs = 5;
    const std::string u = tawhiri::url("https://api.v2.sondehub.org/", q);
    CHECK(u == "https://api.v2.sondehub.org/tawhiri?profile=standard_profile&launch_latitude=52.80000&launch_longitude=358.50000&launch_altitude=8000.0&launch_datetime=2023-11-14T22:13:20Z&ascent_rate=5.00&burst_altitude=30000.0&descent_rate=5.00", "url %s", u.c_str());
    const std::string ans = "{\"metadata\":{\"complete_datetime\":\"2023-11-14T22:13:21.5Z\",\"start_datetime\":\"2023-11-14T22:13:20Z\"},"
        "\"prediction\":[{\"stage\":\"ascent\",\"trajectory\":[{\"altitude\":8000.0,\"datetime\":\"2023-11-14T22:13:20Z\",\"latitude\":52.8,\"longitude\":358.5},"
        "{\"altitude\":30000.0,\"datetime\":\"2023-11-14T23:26:40Z\",\"latitude\":52.9,\"longitude\":359.9}]},"
        "{\"stage\":\"descent\",\"trajectory\":[{\"altitude\":30000.0,\"datetime\":\"2023-11-14T23:26:40Z\",\"latitude\":52.9,\"longitude\":359.9},"
        "{\"altitude\":60.5,\"datetime\":\"2023-11-14T23:58:00.25Z\",\"latitude\":53.0,\"longitude\":0.6}]}],"
        "\"request\":{\"version\":1}}";
    const tawhiri::Result res = tawhiri::parse(ans);
    CHECK(res.ok && res.path.size() == 4 && res.hasBurst && res.burst.altM == 30000 && std::fabs(res.burst.lon - (-0.1)) < 1e-9, "parsed: 4 points, burst at -0.1 E (%zu)", res.path.size());
    CHECK(std::fabs(res.landing.lat - 53.0) < 1e-12 && std::fabs(res.landing.lon - 0.6) < 1e-12 && std::fabs(res.landing.unixT - (1700000000 + 6280.25)) < 1e-6, "landing point and time (%.2f)", res.landing.unixT);
    const tawhiri::Result er = tawhiri::parse("{\"error\":{\"type\":\"RequestException\",\"description\":\"Parameter 'launch_datetime' not provided in request.\"},\"metadata\":{}}");
    CHECK(!er.ok && has(er.error, "launch_datetime"), "error text: %s", er.error.c_str());
    CHECK(!tawhiri::parse("<html>").ok && !tawhiri::parse("{\"prediction\":[]}").ok, "junk and empty predictions are no prediction");
}

static void testChase() {
    ChaseFix f;
    CHECK(chase::nmeaLine("$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47\r\n", f, 10) && f.valid && std::fabs(f.lat - 48.1173) < 1e-4 && std::fabs(f.lon - 11.5166667) < 1e-6 && f.hasAlt && f.altM == 545.4 && f.sats == 8 && f.steadyT == 10, "GGA with altitude");
    ChaseFix r;
    CHECK(chase::nmeaLine("$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*6A", r, 11) && r.valid && !r.hasAlt && std::fabs(r.lat - 48.1173) < 1e-4, "RMC");
    ChaseFix n;
    CHECK(chase::nmeaLine("$GPRMC,123519,V,,,,,,,230394,,*33", n, 12) && !n.valid, "RMC without a fix");
    CHECK(!chase::nmeaLine("$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*48", n, 13) && !n.valid, "a bad checksum is ignored");
    ChaseFix g;
    CHECK(chase::gpsdLine("{\"class\":\"TPV\",\"device\":\"/dev/ttyACM0\",\"mode\":3,\"lat\":52.81,\"lon\":9.92,\"altMSL\":71.5,\"alt\":118.0}", g, 20) && g.valid && g.altM == 71.5 && g.hasAlt, "gpsd TPV, 3D: altMSL");
    ChaseFix g2;
    CHECK(chase::gpsdLine("{\"class\":\"TPV\",\"mode\":2,\"lat\":52.81,\"lon\":9.92,\"alt\":118.0}", g2, 21) && g2.valid && !g2.hasAlt, "gpsd 2D fix: no altitude");
    ChaseFix g3;
    CHECK(chase::gpsdLine("{\"class\":\"TPV\",\"mode\":1}", g3, 22) && !g3.valid, "gpsd mode 1: no fix");
    CHECK(!chase::gpsdLine("{\"class\":\"SKY\"}", g3, 23) && !chase::gpsdLine("not json", g3, 23), "other gpsd reports are not positions");
}

int main() {
    testMarks();
    testTawhiri();
    testChase();
    return testSummary("sonde_map_overlay");
}
