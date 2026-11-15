// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/geo/station_location.h"
#include <cmath>
#include <cstdio>

using namespace dect2::station;

static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { failures++; std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)
static bool near(double a, double b, double t) { return std::fabs(a - b) <= t; }

int main() {
    double la, lo;
    // text to position
    CHECK(resolveText("KO24", la, lo) && near(la, 54.5, 0.01) && near(lo, 25.0, 0.01), "locator KO24 %.3f %.3f", la, lo);
    CHECK(resolveText("  ko24pq ", la, lo) && near(la, 54.6, 0.1) && near(lo, 25.2, 0.1), "locator lower case with spaces");
    CHECK(resolveText("54.687, 25.280", la, lo) && near(la, 54.687, 1e-6) && near(lo, 25.28, 1e-6), "decimal pair");
    CHECK(resolveText("-33.9 151.2", la, lo) && near(la, -33.9, 1e-6) && near(lo, 151.2, 1e-6), "southern / eastern pair");
    CHECK(!resolveText("", la, lo) && !resolveText("   ", la, lo), "empty");
    CHECK(!resolveText("hello", la, lo), "not a position");
    CHECK(!resolveText("95, 10", la, lo), "latitude out of range");

    // placeholders
    CHECK(!isSetLatLon(0, 0) && isSetLatLon(0, 1) && isSetLatLon(-1, 0), "zero placeholder");
    CHECK(!isSetLatLon(91, 0) && !isSetLatLon(0, 181) && !isSetLatLon(NAN, 0), "out of range");

    // migration: first non-empty wins, empty and bad ones are skipped
    {
        std::vector<Candidate> v;
        v.push_back({"uat", 0, 0, ""});
        v.push_back({"rfi", 0, 0, "garbage"});
        v.push_back({"setup", 0, 0, "54.687, 25.28"});
        v.push_back({"ham.grid", 0, 0, "JN58"});
        const Pick p = firstNonEmpty(v);
        CHECK(p.found && p.source == "setup" && near(p.lat, 54.687, 1e-6), "first non-empty is the setup text (%s)", p.source.c_str());
        v[0] = {"uat", 48.1, 11.5, ""};
        const Pick q = firstNonEmpty(v);
        CHECK(q.found && q.source == "uat" && near(q.lat, 48.1, 1e-9) && near(q.lon, 11.5, 1e-9), "lat/lon candidate wins by order");
        const Pick none = firstNonEmpty({{"a", 0, 0, ""}, {"b", 0, 0, " "}});
        CHECK(!none.found, "nothing set");
        CHECK(!firstNonEmpty({}).found, "empty list");
        const Pick loc = firstNonEmpty({{"g", 0, 0, "KO24pq"}});
        CHECK(loc.found && near(loc.lat, 54.6, 0.1), "locator candidate");
    }

    // locator of a position and the override rule
    const double hlat = 54.687, hlon = 25.28;
    const std::string g6 = locatorOf(hlat, hlon, 6), g4 = locatorOf(hlat, hlon, 4);
    CHECK(g6.size() == 6 && g4.size() == 4 && g6.compare(0, 4, g4) == 0, "locator lengths %s %s", g6.c_str(), g4.c_str());
    CHECK(locatorOf(95, 0).empty(), "bad position has no locator");
    CHECK(!isOwnLocator("", hlat, hlon), "empty is not own");
    CHECK(!isOwnLocator(g6, hlat, hlon), "same square at 6 chars is the global one");
    CHECK(!isOwnLocator(g4, hlat, hlon), "same square at 4 chars is the global one");
    CHECK(!isOwnLocator(upper(g6), hlat, hlon) && !isOwnLocator(std::string(g6).replace(0, 2, "ko"), hlat, hlon),"case does not matter");
    CHECK(isOwnLocator("JN58td", hlat, hlon), "another square is own");
    CHECK(isOwnLocator("KO24", 0, 0) && !isOwnLocator("nonsense", hlat, hlon), "no global: a valid locator is kept; junk is not");
    CHECK(effectiveLocator(false, "JN58", hlat, hlon) == g6, "no own: the global locator");
    CHECK(effectiveLocator(true, "JN58", hlat, hlon) == "JN58", "own wins");
    CHECK(effectiveLocator(true, "", hlat, hlon) == g6, "own with empty text falls back");
    CHECK(effectiveLocator(false, "", 0, 0) == locatorOf(0, 0, 6), "unset global at 0,0");

    // round trip: position to locator to position stays in the square
    for (double a = -80; a <= 80; a += 17.3)
        for (double b = -170; b <= 170; b += 23.7) {
            double x, y;
            CHECK(resolveText(locatorOf(a, b, 6), x, y) && near(x, a, 0.05) && near(y, b, 0.1), "round trip %.2f %.2f -> %.3f %.3f", a, b, x, y);
        }

    // height
    CHECK(clampHeightM(100) == 100 && clampHeightM(1e9) == 9000 && clampHeightM(-1e9) == -500 && clampHeightM(NAN) == 0, "height clamp");

    std::printf(failures ? "station_location: %d FAILED\n" : "station_location: ok\n", failures);
    return failures ? 1 : 0;
}
