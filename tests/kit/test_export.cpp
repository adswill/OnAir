// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "../../app/ui/kit/export.h"
#include <cstdio>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

int main() {
    resexp::Table e;   // empty list
    e.cols = {"call", "lat", "lon"};
    CHECK(kit::clipboardCsv(e) == "call,lat,lon\n");
    CHECK(!kit::hasPositions(e));
    CHECK(resexp::toGeoJson(e).find("\"features\"") != std::string::npos);

    resexp::Table t;
    t.cols = {"call", "comment", "Lat", "LON"};
    t.add({"DL1ABC", "a,b \"q\"", "52.5", "13.4"});
    t.add({"Jūratė", "line1\nline2", "", ""});   // unicode, embedded newline, no position
    const std::string csv = kit::clipboardCsv(t);
    CHECK(csv.find("DL1ABC,\"a,b \"\"q\"\"\",52.5,13.4\n") != std::string::npos);
    CHECK(csv.find("Jūratė,\"line1\nline2\",,\n") != std::string::npos);
    CHECK(csv.find('\r') == std::string::npos || csv.find("\"line1\r") == std::string::npos);
    CHECK(kit::hasPositions(t));
    const std::string k = resexp::toKml(t), g = resexp::toGeoJson(t);
    CHECK(k.find("<coordinates>13.4,52.5") != std::string::npos);
    CHECK(g.find("[13.4,52.5") != std::string::npos);
    CHECK(k.find("Jūratė") == std::string::npos);   // the row without a position is not a placemark
    resexp::Table np; np.cols = {"call", "lat", "lon"}; np.add({"X", "", ""});
    CHECK(!kit::hasPositions(np));
    printf(fails ? "FAILED\n" : "all ok\n");
    return fails ? 1 : 0;
}
