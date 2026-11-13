#include "dect2/geo/mylocation.h"
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace dect2::myloc;

static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { failures++; std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)
static bool closeTo(double a, double b, double tol) { return std::fabs(a - b) <= tol; }
static double angDiff(double a, double b) { double d = std::fmod(std::fabs(a - b), 360.0); return d > 180 ? 360 - d : d; }

static std::string readFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static void maths() {
    CHECK(closeTo(distanceKm(0, 0, 0, 0), 0, 1e-9), "zero distance");
    CHECK(closeTo(distanceKm(0, 0, 0, 1), 111.195, 0.05), "one degree of equator %.3f", distanceKm(0, 0, 0, 1));
    CHECK(closeTo(distanceKm(51.5074, -0.1278, 48.8566, 2.3522), 343.5, 1.5), "London-Paris %.2f", distanceKm(51.5074, -0.1278, 48.8566, 2.3522));
    CHECK(closeTo(distanceKm(25.2048, 55.2708, 54.6872, 25.2797), 4095, 5), "Dubai-Vilnius %.1f", distanceKm(25.2048, 55.2708, 54.6872, 25.2797));
    CHECK(closeTo(distanceKm(40.7128, -74.0060, 34.0522, -118.2437), 3936, 15), "NY-LA %.1f", distanceKm(40.7128, -74.0060, 34.0522, -118.2437));
    CHECK(closeTo(distanceKm(10, 20, 30, 40), distanceKm(30, 40, 10, 20), 1e-9), "symmetric");
    CHECK(closeTo(distanceKm(0, 0, 0, 180), kPi * kEarthKm, 1e-6), "antipodal equator");
    CHECK(closeTo(distanceKm(90, 0, -90, 0), kPi * kEarthKm, 1e-6), "pole to pole");
    CHECK(closeTo(distanceKm(0, 0, 0, 180), distanceKm(0, 0, 0, -180), 1e-6), "half way either side");

    CHECK(closeTo(distanceKm(0, 179.5, 0, -179.5), 111.195, 0.05), "antimeridian short way %.3f", distanceKm(0, 179.5, 0, -179.5));
    CHECK(closeTo(distanceKm(10, 179.9, 10, -179.9), distanceKm(10, 179.9, 10, 180.1), 1e-6), "antimeridian with lon beyond 180");
    CHECK(angDiff(bearingDeg(0, 179.5, 0, -179.5), 90.0) < 1e-6, "antimeridian bearing east %.4f", bearingDeg(0, 179.5, 0, -179.5));
    CHECK(angDiff(bearingDeg(0, -179.5, 0, 179.5), 270.0) < 1e-6, "antimeridian bearing west");

    CHECK(angDiff(bearingDeg(0, 0, 1, 0), 0) < 1e-9, "north");
    CHECK(angDiff(bearingDeg(0, 0, 0, 1), 90) < 1e-9, "east");
    CHECK(angDiff(bearingDeg(0, 0, -1, 0), 180) < 1e-9, "south");
    CHECK(angDiff(bearingDeg(0, 0, 0, -1), 270) < 1e-9, "west");
    CHECK(angDiff(bearingDeg(51.5074, -0.1278, 48.8566, 2.3522), 148.1) < 0.5, "London-Paris bearing %.2f", bearingDeg(51.5074, -0.1278, 48.8566, 2.3522));
    for (int i = 0; i < 200; i++) {
        const double la = -89 + (i * 37 % 178), lo = -179 + (i * 91 % 358), la2 = -80 + (i * 53 % 160), lo2 = -170 + (i * 17 % 340);
        const double b = bearingDeg(la, lo, la2, lo2);
        CHECK(b >= 0 && b < 360, "bearing range %.6f", b);
        double ol, oo;
        destination(la, lo, b, distanceKm(la, lo, la2, lo2), ol, oo);
        CHECK(closeTo(ol, la2, 1e-6) && angDiff(oo, lo2) < 1e-6, "destination round trip %d: %.6f,%.6f vs %.6f,%.6f", i, ol, oo, la2, lo2);
    }

        CHECK(angDiff(bearingDeg(80, 30, 90, 0), 0) < 1e-9, "bearing to the north pole is north");
    CHECK(angDiff(bearingDeg(-80, 30, -90, 0), 180) < 1e-9, "bearing to the south pole is south");
    CHECK(std::isfinite(bearingDeg(90, 0, 90, 0)) && std::isfinite(bearingDeg(0, 0, 0, 0)), "identical points");
    CHECK(closeTo(distanceKm(89.99, 0, 89.99, 180), 2 * 0.01 * 111.195, 0.05), "over the pole %.4f", distanceKm(89.99, 0, 89.99, 180));
    CHECK(angDiff(bearingDeg(89.99, 0, 89.99, 180), 0) < 1e-6, "over the pole the way is north");
    CHECK(closeTo(distanceKm(90, 0, 90, 123), 0, 1e-9), "all longitudes meet at the pole");

    CHECK(std::string(compass(0)) == "N" && std::string(compass(90)) == "E" && std::string(compass(225)) == "SW" && std::string(compass(359.9)) == "N" && std::string(compass(-90)) == "W", "compass");
    CHECK(formatDistance(343.5, true) == "344 km" && formatDistance(5.04, true) == "5.0 km", "format km: %s", formatDistance(343.5, true).c_str());
    CHECK(formatDistance(185.2, false) == "100 nm" && formatDistance(1.852, false) == "1.0 nm", "format nm: %s", formatDistance(185.2, false).c_str());
    CHECK(formatBearing(5) == "005\xC2\xB0 N" && formatBearing(359.9) == "000\xC2\xB0 N", "format bearing %s", formatBearing(359.9).c_str());
    CHECK(describe(0, 0, 0, 1, true) == "111 km at 090\xC2\xB0 E", "describe %s", describe(0, 0, 0, 1, true).c_str());
    CHECK(describe(0, 0, 91, 0, true).empty() && describe(NAN, 0, 0, 0, true).empty(), "invalid positions give nothing");
}

static void rings() {
    const auto r = ringPoints(25.0, 55.0, 100.0, 72);
    CHECK(r.size() == 73, "ring has segments+1 points: %zu", r.size());
    for (const auto& p : r) CHECK(closeTo(distanceKm(25.0, 55.0, p.first, p.second), 100.0, 1e-6), "every ring point at the radius: %.9f", distanceKm(25.0, 55.0, p.first, p.second));
    CHECK(closeTo(r.front().first, r.back().first, 1e-9) && angDiff(r.front().second, r.back().second) < 1e-9, "ring closes");
    CHECK(r.front().first > 25.0 && angDiff(r.front().second, 55.0) < 1e-9, "ring starts due north");
    CHECK(closeTo(r[18].first, 25.0, 0.01) && r[18].second > 55.0, "a quarter round is due east");
    const auto a = ringPoints(0.0, 179.9, 50.0, 36);
    for (const auto& p : a) CHECK(p.second >= -180 && p.second <= 180 && closeTo(distanceKm(0.0, 179.9, p.first, p.second), 50.0, 1e-6), "ring over the antimeridian stays on the globe and keeps its radius");
    const auto n = ringPoints(89.95, 10.0, 20.0, 36);
    for (const auto& p : n) CHECK(std::isfinite(p.first) && std::isfinite(p.second) && p.first <= 90.0 && closeTo(distanceKm(89.95, 10.0, p.first, p.second), 20.0, 1e-6), "ring round the pole is finite and keeps its radius");
    const auto big = ringPoints(0, 0, 1e6, 12);
    for (const auto& p : big) CHECK(closeTo(distanceKm(0, 0, p.first, p.second), kPi * kEarthKm, 1e-3), "radius over half the globe is capped at the antipode");
    CHECK(ringPoints(0, 0, 0, 36).empty() && ringPoints(0, 0, -5, 36).empty() && ringPoints(0, 0, 10, 2).empty() && ringPoints(100, 0, 10, 36).empty(), "no ring for bad input");
    CHECK(ringSegments(1) == 24 && ringSegments(400) == 100 && ringSegments(1e6) == 180, "segments follow the size: %d %d", ringSegments(400), ringSegments(1e6));

    const auto l = parseRingList("200, 50;100 100 x -5 0 1e9 25");
    CHECK(l.size() == 4 && l[0] == 25 && l[1] == 50 && l[2] == 100 && l[3] == 200, "ring list sorted, unique, bad entries dropped (%zu)", l.size());
    CHECK(parseRingList("").empty() && parseRingList("abc").empty(), "empty ring list");
    CHECK(parseRingList("1,2,3,4,5,6,7,8,9,10").size() == 8, "at most eight rings");
    CHECK(closeTo(toKm(100, true), 100, 1e-12) && closeTo(toKm(100, false), 185.2, 1e-9), "ring distances follow the unit");
    for (double nm : {10.0, 50.0, 250.0}) {
        const auto q = ringPoints(10, 10, toKm(nm, false), 36);
        CHECK(closeTo(distanceKm(10, 10, q[5].first, q[5].second) / kNmKm, nm, 1e-6), "ring of %g nm", nm);
    }
}

static std::string between(const std::string& s, const std::string& from, const std::string& to) {
    const size_t a = s.find(from);
    if (a == std::string::npos) return "";
    const size_t b = s.find(to, a);
    return s.substr(a, b == std::string::npos ? std::string::npos : b - a);
}

static void sources(const fs::path& root) {
    const fs::path app = root / "app";
    int callers = 0;
    std::vector<std::string> own;
    for (const auto& e : fs::recursive_directory_iterator(app)) {
        if (!e.is_regular_file()) continue;
        const std::string ext = e.path().extension().string();
        if (ext != ".cpp" && ext != ".h" && ext != ".mm") continue;
        const std::string rel = fs::relative(e.path(), app).generic_string();
        const std::string s = readFile(e.path());
        if (rel != "adsb/adsb_map.cpp" && rel != "adsb/adsb_map.h" && rel != "map/my_location.cpp" && s.find("adsbmap::draw(") != std::string::npos) callers++;
        const bool ownProjection = s.find("mercX(") != std::string::npos || s.find("worldSize(") != std::string::npos;
        if (rel != "map/my_location.cpp" && (s.find("receiverLocationSet() ? \"me\"") != std::string::npos || s.find("IM_COL32(110, 210, 230, 255), \"me\"") != std::string::npos || s.find("tr(\"home\")") != std::string::npos))
            own.push_back(rel + " (its own home marker)");
        if (ownProjection && rel != "adsb/adsb_map.cpp" && rel != "map/wx_overlay.cpp") own.push_back(rel);
    }
    CHECK(callers >= 38, "only %d map screens found that draw through adsbmap::draw", callers);
    for (const auto& r : own) CHECK(false, "%s projects the map itself instead of drawing through adsbmap::draw, so it would lack my location", r.c_str());

    const std::string map = readFile(app / "adsb/adsb_map.cpp");
    const std::string body = between(map, "bool draw(View& v, ImVec2 size) {", "void legend(");
    CHECK(!body.empty(), "draw() not found");
    CHECK(body.find("mylocmap::overlay(dl, p0, size, hov)") != std::string::npos, "adsbmap::draw() must call mylocmap::overlay");
    const size_t ov = body.find("mylocmap::overlay"), ret = body.rfind("return hov;");
    CHECK(ov != std::string::npos && ret != std::string::npos && ov < ret, "the overlay is drawn before draw() returns");
    size_t early = body.find("return", 0);
    CHECK(early == ret, "draw() has no early return that would skip my location");

    const std::string h = readFile(app / "map/my_location.cpp");
    CHECK(h.find("receiverLocationSet()") != std::string::npos && h.find("receiverLocation(") != std::string::npos, "the helper uses the one receiver location");
    CHECK(h.find("settingsOpenPage(\"Location\")") != std::string::npos, "the no-location hint opens the Location settings");
    CHECK(h.find("adsbDistUnit()") != std::string::npos, "distances follow the app unit setting");
    CHECK(h.find("mapMyLoc\"") != std::string::npos && h.find("mapMyLocRings\"") != std::string::npos && h.find("mapMyLocRingList\"") != std::string::npos, "the three settings are read");

    const std::string tbl = readFile(app / "ui/settings_table.h");
    CHECK(tbl.find("\"mapMyLoc\"") != std::string::npos && tbl.find("\"mapMyLocRings\"") != std::string::npos && tbl.find("\"mapMyLocRingList\"") != std::string::npos, "the settings are in the Settings table");

    for (const char* f : {"ais/ais_ui.cpp", "mesh/mesh_views.cpp", "dmr/dmr_map.cpp", "rid/rid_ui.cpp"})
        CHECK(readFile(app / f).find("mylocmap::") != std::string::npos, "%s shows distance and bearing from me", f);
    CHECK(readFile(root / "CMakeLists.txt").find("app/map/my_location.cpp") != std::string::npos, "my_location.cpp is built");
}

int main() {
    maths();
    rings();
    sources(fs::path(DECT2_SOURCE_DIR));
    if (!failures) std::printf("ok\n");
    return failures ? 1 : 0;
}
