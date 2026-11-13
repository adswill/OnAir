// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "../../app/state/results_export.h"
#include <clocale>
#include <cstdio>
#include <cstdlib>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

int main() {
    setlocale(LC_ALL, "");
    resexp::Table t;
    t.cols = {"name", "freq_hz", "note"};
    t.add({"plain", resexp::num(522000000.0, 0), "ok"});
    t.add({"a,b", resexp::num(88.1234, 2), "say \"hi\""});
    t.add({"line1\nline2", "-3.5", "Zürich \xE2\x82\xAC"});
    const std::string csv = resexp::toCsv(t);
    CHECK(csv.rfind("name,freq_hz,note\r\n", 0) == 0);
    CHECK(csv.find("plain,522000000,ok\r\n") != std::string::npos);
    CHECK(csv.find("\"a,b\",88.12,\"say \"\"hi\"\"\"\r\n") != std::string::npos);
    CHECK(csv.find("\"line1\nline2\",-3.5,Zürich \xE2\x82\xAC") != std::string::npos);
    const std::string js = resexp::toJson(t);
    CHECK(js.find("\"freq_hz\": 522000000") != std::string::npos);
    CHECK(js.find("\"name\": \"a,b\"") != std::string::npos);
    CHECK(js.find("say \\\"hi\\\"") != std::string::npos);
    CHECK(js.find("line1\\nline2") != std::string::npos);
    CHECK(js.find("Zürich \xE2\x82\xAC") != std::string::npos);
    CHECK(resexp::num(1.5, 3) == "1.5");
    CHECK(resexp::num(-0.0001, 2) == "0");
    CHECK(resexp::num(100.0, 3) == "100");
    CHECK(resexp::stamp().size() == 13);
    {   // ADIF
        resexp::Table h;
        h.cols = {"call", "grid", "frequency_hz", "mode", "snr"};
        h.add({"K1ABC", "FN42", "14074000", "FT8", "-12"}, 1760000000);   // 2025-10-09 08:53:20 UTC
        h.add({"", "JO55", "14074000", "FT8", "-3"}, 1760000000);
        const std::string a = resexp::toAdif(h);
        CHECK(a.find("<EOH>") != std::string::npos);
        CHECK(a.find("<CALL:5>K1ABC") != std::string::npos);
        CHECK(a.find("<QSO_DATE:8>20251009") != std::string::npos);
        CHECK(a.find("<TIME_ON:6>085320") != std::string::npos);
        CHECK(a.find("<BAND:3>20m") != std::string::npos);
        CHECK(a.find("<FREQ:6>14.074") != std::string::npos);
        CHECK(a.find("<GRIDSQUARE:4>FN42") != std::string::npos);
        CHECK(a.find("<RST_RCVD:3>-12") != std::string::npos);
        size_t n = 0; for (size_t p = 0; (p = a.find("<EOR>", p)) != std::string::npos; p++) n++;
        CHECK(n == 1);   // the row without a call is skipped
        CHECK(resexp::fmtFromPath("x.ADI") == resexp::Fmt::Adif);
    }
    setlocale(LC_ALL, "C");   // the program runs in the "C" locale (it never calls setlocale): with a decimal comma strtod stops at the '.' of "53.5"
    {   // KML / GPX
        resexp::Table g;
        g.cols = {"icao", "mode", "alt_ft", "lat", "lon"};
        g.add({"4CA<1>&", "ADS-B", "35000", "53.5", "-6.25"}, 1760000000);
        g.add({"NOPOS", "ADS-B", "", "", ""}, 1760000000);
        const std::string k = resexp::toKml(g), x = resexp::toGpx(g);
        CHECK(k.find("<coordinates>-6.25,53.5,10668</coordinates>") != std::string::npos);
        CHECK(k.find("4CA&lt;1&gt;&amp;") != std::string::npos);
        CHECK(k.find("NOPOS") == std::string::npos);
        CHECK(k.find("</kml>") != std::string::npos);
        CHECK(x.find("<wpt lat=\"53.5\" lon=\"-6.25\">") != std::string::npos);
        CHECK(x.find("<time>2025-10-09T08:53:20Z</time>") != std::string::npos);
        CHECK(x.find("</gpx>") != std::string::npos);
        CHECK(resexp::since(g, 1770000000).rows.empty());
        CHECK(resexp::since(g, 1750000000).rows.size() == 2);
        const std::string j = resexp::toGeoJson(g);   // GeoJSON: [lon,lat,alt], the row without a position left out, times kept
        CHECK(j.find("\"coordinates\":[-6.25,53.5,10668]") != std::string::npos);
        CHECK(j.find("NOPOS") == std::string::npos);
        CHECK(j.find("\"time\":\"2025-10-09T08:53:20Z\"") != std::string::npos);
        CHECK(resexp::fmtFromPath("a.geojson") == resexp::Fmt::GeoJson);
    }
    printf(fails ? "FAILED\n" : "ok\n");
    return fails;
}
