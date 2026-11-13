// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Maidenhead locators (known pairs, every length, the edges and the poles, round trips), the coordinate parser and the great circle
#include "dect2/geo/maidenhead.h"
#include <cmath>
#include <cstdio>
#include <string>

using namespace dect2;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main() {
    // ---- known pairs
    CHECK(toMaidenhead(54.687, 25.280, 4) == "KO24", "Vilnius 4: %s", toMaidenhead(54.687, 25.280, 4).c_str());
    CHECK(toMaidenhead(54.687, 25.280) == "KO24pq", "Vilnius 6: %s", toMaidenhead(54.687, 25.280).c_str());
    CHECK(toMaidenhead(54.687, 25.280, 8) == "KO24pq34", "Vilnius 8: %s", toMaidenhead(54.687, 25.280, 8).c_str());
    CHECK(toMaidenhead(48.146, 11.608) == "JN58td", "Munich: %s", toMaidenhead(48.146, 11.608).c_str());
    CHECK(toMaidenhead(48.146, 11.608, 2) == "JN", "Munich 2: %s", toMaidenhead(48.146, 11.608, 2).c_str());
    CHECK(toMaidenhead(-34.91, -56.21) == "GF15vc", "Montevideo: %s", toMaidenhead(-34.91, -56.21).c_str());
    CHECK(toMaidenhead(38.92, -77.07) == "FM18lw", "Washington: %s", toMaidenhead(38.92, -77.07).c_str());
    CHECK(toMaidenhead(-41.28, 174.77) == "RE78jr", "Wellington: %s", toMaidenhead(-41.28, 174.77).c_str());
    // ---- edges and poles
    CHECK(toMaidenhead(-90, -180, 8) == "AA00aa00", "south-west corner: %s", toMaidenhead(-90, -180, 8).c_str());
    CHECK(toMaidenhead(90, 180, 8) == "RR99xx99", "north pole, 180 E: %s", toMaidenhead(90, 180, 8).c_str());
    CHECK(toMaidenhead(90, 0) == "JR09ax", "north pole, 0: %s", toMaidenhead(90, 0).c_str());
    CHECK(toMaidenhead(-90, 0) == "JA00aa", "south pole, 0: %s", toMaidenhead(-90, 0).c_str());
    CHECK(toMaidenhead(0, 0) == "JJ00aa", "null island: %s", toMaidenhead(0, 0).c_str());
    CHECK(toMaidenhead(-1e-9, -1e-9) == "II99xx", "just south-west of 0,0: %s", toMaidenhead(-1e-9, -1e-9).c_str());
    CHECK(toMaidenhead(10, 190) == toMaidenhead(10, -170), "longitude wraps");
    CHECK(toMaidenhead(NAN, 0).empty(), "NaN gives nothing");
    // ---- decoding: the centre of the square, sizes, case, bad input
    double la = 0, lo = 0, h = 0, w = 0;
    CHECK(fromMaidenhead("KO24", la, lo, &h, &w) && std::fabs(la - 54.5) < 1e-9 && std::fabs(lo - 25) < 1e-9 && h == 1 && w == 2, "KO24 centre %.4f %.4f", la, lo);
    CHECK(fromMaidenhead("ko24PQ", la, lo, &h, &w) && std::fabs(la - (54 + 16.5 / 24)) < 1e-9 && std::fabs(lo - (24 + 15.5 / 12)) < 1e-9, "KO24pq centre %.5f %.5f", la, lo);
    CHECK(std::fabs(h - 1.0 / 24) < 1e-12 && std::fabs(w - 2.0 / 24) < 1e-12, "subsquare size");
    CHECK(fromMaidenhead("KO24pq34", la, lo, &h, &w) && std::fabs(h - 1.0 / 240) < 1e-12 && std::fabs(w - 1.0 / 120) < 1e-12, "extended size");
    CHECK(toMaidenhead(la, lo, 8) == "KO24pq34", "KO24pq34 round trip %s", toMaidenhead(la, lo, 8).c_str());
    CHECK(fromMaidenhead("RR", la, lo) && la == 85 && lo == 170, "RR centre");
    CHECK(fromMaidenhead(" JN58td ", la, lo) && toMaidenhead(la, lo) == "JN58td", "spaces ignored");
    CHECK(!fromMaidenhead("SA", la, lo), "field S refused");
    CHECK(!fromMaidenhead("KO2", la, lo), "3 characters refused");
    CHECK(!fromMaidenhead("KO24yz", la, lo), "subsquare y refused");
    CHECK(!fromMaidenhead("KOAB", la, lo), "letters as square refused");
    CHECK(!fromMaidenhead("KO24pqab", la, lo), "letters as extended square refused");
    CHECK(!fromMaidenhead("", la, lo), "empty refused");
    // every 6-character square round-trips over a grid of the globe
    int bad = 0;
    for (double y = -89.9; y < 90; y += 1.37)
        for (double x = -179.9; x < 180; x += 2.71) {
            const std::string s = toMaidenhead(y, x, 8);
            double a, b, hh, ww;
            if (!fromMaidenhead(s, a, b, &hh, &ww) || std::fabs(a - y) > hh / 2 + 1e-9 || std::fabs(b - x) > ww / 2 + 1e-9 || toMaidenhead(a, b, 8) != s) bad++;
        }
    CHECK(bad == 0, "%d round trips failed", bad);
    // ---- coordinate text
    auto nearBy = [](double a, double b) { return std::fabs(a - b) < 1e-6; };
    CHECK(parseCoordinates("54.687, 25.280", la, lo) && nearBy(la, 54.687) && nearBy(lo, 25.28), "decimal with comma");
    CHECK(parseCoordinates("54.687 25.280", la, lo) && nearBy(la, 54.687) && nearBy(lo, 25.28), "decimal with space");
    CHECK(parseCoordinates("-33.9 151.2", la, lo) && nearBy(la, -33.9) && nearBy(lo, 151.2), "negative");
    CHECK(parseCoordinates("33.9S 70.5W", la, lo) && nearBy(la, -33.9) && nearBy(lo, -70.5), "hemisphere letters after");
    CHECK(parseCoordinates("N54.687 E25.28", la, lo) && nearBy(la, 54.687) && nearBy(lo, 25.28), "hemisphere letters before");
    CHECK(parseCoordinates("25.28E 54.687N", la, lo) && nearBy(la, 54.687) && nearBy(lo, 25.28), "longitude first with letters");
    CHECK(parseCoordinates("54\xC2\xB0" "41'13\"N 25\xC2\xB0" "16'48\"E", la, lo) && nearBy(la, 54 + 41 / 60.0 + 13 / 3600.0) && nearBy(lo, 25 + 16 / 60.0 + 48 / 3600.0), "DMS with signs");
    CHECK(parseCoordinates("54 41 13 N 25 16 48 E", la, lo) && nearBy(la, 54 + 41 / 60.0 + 13 / 3600.0) && nearBy(lo, 25.28), "DMS with spaces");
    CHECK(parseCoordinates("54 41 13 25 16 48", la, lo) && nearBy(la, 54 + 41 / 60.0 + 13 / 3600.0) && nearBy(lo, 25.28), "DMS without letters");
    CHECK(parseCoordinates("N54 41.2 E25 16.8", la, lo) && nearBy(la, 54 + 41.2 / 60) && nearBy(lo, 25.28), "degrees and decimal minutes");
    CHECK(parseCoordinates("48\xC2\xB0" "08\xE2\x80\xB2" "45.6\xE2\x80\xB3" "S 11\xC2\xB0" "36\xE2\x80\xB2" "28.8\xE2\x80\xB3" "W", la, lo) && nearBy(la, -(48 + 8 / 60.0 + 45.6 / 3600)) && nearBy(lo, -(11 + 36 / 60.0 + 28.8 / 3600)), "prime signs");
    CHECK(!parseCoordinates("91 10", la, lo), "latitude over 90 refused");
    CHECK(!parseCoordinates("10 181", la, lo), "longitude over 180 refused");
    CHECK(!parseCoordinates("54 61 0 25 0 0", la, lo), "61 minutes refused");
    CHECK(!parseCoordinates("54.5", la, lo), "one number refused");
    CHECK(!parseCoordinates("54N 25N", la, lo), "two latitudes refused");
    CHECK(!parseCoordinates("KO24", la, lo), "a locator is not coordinates");
    CHECK(!parseCoordinates("", la, lo), "empty refused");
    CHECK(toDms(54.687, true) == "54\xC2\xB0" "41'13.2\"N", "DMS text %s", toDms(54.687, true).c_str());
    CHECK(toDms(-0.5, false) == "0\xC2\xB0" "30'00.0\"W", "DMS west %s", toDms(-0.5, false).c_str());
    // ---- great circle: Vilnius to Munich is about 1180 km to the south-west
    double km = 0, brg = 0;
    greatCircle(54.687, 25.280, 48.146, 11.608, km, brg);
    CHECK(km > 1150 && km < 1210 && brg > 230 && brg < 250, "Vilnius-Munich %.1f km %.1f deg", km, brg);
    greatCircle(0, 0, 0, 90, km, brg);
    CHECK(std::fabs(km - 6371.0088 * M_PI / 2) < 1e-6 && std::fabs(brg - 90) < 1e-9, "a quarter of the equator %.3f %.3f", km, brg);
    greatCircle(10, 20, 10, 20, km, brg);
    CHECK(km == 0, "same point");
    if (fails) { printf("%d failures\n", fails); return 1; }
    printf("maidenhead: all tests passed\n");
    return 0;
}
