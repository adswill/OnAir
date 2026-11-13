// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The exporters (core/src/export): KML / GPX / GeoJSON / CSV / JSON / text / ADIF writers, escaping, coordinate order, time formats, the sinks.
#include "dect2/common/xmlmini.h"
#include "dect2/export/exporters.h"
#include "../common/test_util.h"
#include <algorithm>
#include <clocale>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>

using namespace dect2::xport;

namespace {
constexpr double kT = 1792232100.0;   // 2026-10-17T10:15:00Z

GeoPoint pt(const char* n, double lat, double lon, bool alt = false, double a = 0, double t = 0) {
    GeoPoint p; p.name = n; p.lat = lat; p.lon = lon; p.hasAlt = alt; p.altM = a; p.time = t; return p;
}

std::string slurp(const std::string& p) { std::ifstream f(p, std::ios::binary); std::stringstream s; s << f.rdbuf(); return s.str(); }

void testHelpers() {
    CHECK(xmlEscape("a<b>&\"c'") == "a&lt;b&gt;&amp;&quot;c&apos;", "xml escape");
    CHECK(xmlEscape(std::string("x\x01y\tz")) == "xy\tz", "control characters dropped, tab kept");
    CHECK(jsonEscape("q\"\\\n\x02") == "q\\\"\\\\\\n\\u0002", "json escape");
    CHECK(jsonEscape("Z\xC3\xBCrich") == "Z\xC3\xBCrich", "UTF-8 passes through");
    CHECK(csvCell("plain") == "plain" && csvCell("a,b") == "\"a,b\"" && csvCell("say \"hi\"") == "\"say \"\"hi\"\"\"" && csvCell("l1\nl2") == "\"l1\nl2\"", "csv cell");
    CHECK(numText(1.5, 3) == "1.5" && numText(-0.0001, 2) == "0" && numText(100.0, 3) == "100" && numText(NAN) == "0", "number text");
    CHECK(isoUtc(kT) == "2026-10-17T10:15:00Z", "iso time %s", isoUtc(kT).c_str());
    CHECK(isoUtc(0).empty(), "no time -> empty");
    CHECK(validLatLon(54.7, 25.3) && !validLatLon(91, 0) && !validLatLon(0, 181) && !validLatLon(NAN, 0), "lat/lon range");
}

void testKml() {
    std::vector<GeoPoint> pts = {pt("A6-EDA <1>", 54.6872, 25.2797, true, 10668, kT), pt("bad", 95, 10), pt("nan", NAN, 10)};
    pts[0].desc = "FL350 & climbing";
    GeoTrack tr; tr.name = "track";
    tr.pts = {pt("", 54.0, 25.0), pt("", 54.5, 25.5, true, 3000), pt("", 99, 0)};
    const std::string k = toKml(pts, {tr}, "Flights & more");
    auto root = dect2::parseXml(k);
    CHECK(root && root->local() == "kml", "KML is well-formed XML");
    CHECK(k.find("<coordinates>25.2797,54.6872,10668</coordinates>") != std::string::npos, "KML point is lon,lat,alt: %s", k.c_str());
    CHECK(k.find("<name>A6-EDA &lt;1&gt;</name>") != std::string::npos && k.find("FL350 &amp; climbing") != std::string::npos, "KML escaping");
    CHECK(k.find("<name>Flights &amp; more</name>") != std::string::npos, "document name escaped");
    CHECK(k.find("<when>2026-10-17T10:15:00Z</when>") != std::string::npos, "KML time");
    CHECK(k.find("bad") == std::string::npos && k.find("nan") == std::string::npos, "invalid points left out");
    CHECK(k.find("<LineString>") != std::string::npos && k.find("25,54,0 25.5,54.5,3000") != std::string::npos, "KML line: lon,lat per vertex, altitude filled in");
    const std::string e = toKml({}, {});
    CHECK(dect2::parseXml(e) != nullptr && e.find("Placemark") == std::string::npos, "empty KML is still valid");
}

void testGpx() {
    GeoTrack tr; tr.name = "p1";
    tr.pts = {pt("", 10.5, 20.25, true, 100, kT), pt("", 10.6, 20.35, true, 120, kT + 5)};
    const std::string g = toGpx({pt("wp & co", -33.5, -70.25)}, {tr});
    auto root = dect2::parseXml(g);
    CHECK(root && root->local() == "gpx", "GPX is well-formed XML");
    CHECK(g.find("<wpt lat=\"-33.5\" lon=\"-70.25\">") != std::string::npos, "GPX attributes lat then lon: %s", g.c_str());
    CHECK(g.find("<name>wp &amp; co</name>") != std::string::npos, "GPX escaping");
    CHECK(g.find("<trkpt lat=\"10.5\" lon=\"20.25\"><ele>100</ele><time>2026-10-17T10:15:00Z</time></trkpt>") != std::string::npos, "GPX track point order ele, time");
    CHECK(g.find("<time>2026-10-17T10:15:05Z</time>") != std::string::npos, "second point time");
    // ele must come before time and time before name inside a wpt
    GeoPoint w = pt("n", 1, 2, true, 5, kT);
    const std::string g2 = toGpx({w}, {});
    CHECK(g2.find("<ele>5</ele><time>2026-10-17T10:15:00Z</time><name>n</name>") != std::string::npos, "GPX element order");
}

void testGeoJson() {
    GeoTrack tr; tr.name = "t \"q\"";
    tr.pts = {pt("", 54, 25), pt("", 55, 26)};
    const std::string j = toGeoJson({pt("A \"1\"", 54.5, 25.5, true, 100, kT), pt("x", 200, 0)}, {tr});
    CHECK(j.find("\"coordinates\":[25.5,54.5,100]") != std::string::npos, "GeoJSON point is [lon,lat,alt]: %s", j.c_str());
    CHECK(j.find("\"LineString\",\"coordinates\":[[25,54],[26,55]]") != std::string::npos, "GeoJSON line");
    CHECK(j.find("\"name\":\"A \\\"1\\\"\"") != std::string::npos && j.find("\"time\":\"2026-10-17T10:15:00Z\"") != std::string::npos, "GeoJSON properties");
    CHECK(j.find("\"x\"") == std::string::npos, "invalid latitude left out");
    // balanced braces and brackets outside strings
    int br = 0, sq = 0; bool in = false;
    for (size_t i = 0; i < j.size(); i++) {
        const char c = j[i];
        if (in) { if (c == '\\') i++; else if (c == '"') in = false; continue; }
        if (c == '"') in = true; else if (c == '{') br++; else if (c == '}') br--; else if (c == '[') sq++; else if (c == ']') sq--;
        CHECK(br >= 0 && sq >= 0, "balanced at %zu", i);
    }
    CHECK(br == 0 && sq == 0 && !in, "GeoJSON is balanced");
    CHECK(toGeoJson({}, {}).find("\"features\":[") != std::string::npos, "empty collection");
}

void testTables() {
    Table t;
    t.cols = {"name", "freq_hz", "note"};
    t.add({"plain", "522000000", "ok"}, kT);
    t.add({"a,b", "88.5", "say \"hi\""}, kT + 60);
    t.add({"007", "-3.5", "Z\xC3\xBCrich\nline2"}, 0);
    const std::string c = toCsv(t);
    CHECK(c.rfind("name,freq_hz,note\r\n", 0) == 0 && c.find("\"a,b\",88.5,\"say \"\"hi\"\"\"\r\n") != std::string::npos && c.find("\"Z\xC3\xBCrich\nline2\"") != std::string::npos, "csv");
    const std::string j = toJson(t);
    CHECK(j.find("\"freq_hz\": 522000000") != std::string::npos && j.find("\"name\": \"007\"") != std::string::npos && j.find("\\nline2") != std::string::npos, "json: numbers stay numbers, 007 stays text");
    const std::string x = toText(t);
    CHECK(x.rfind("UTC", 0) == 0 && x.find("2026-10-17T10:15:00Z") != std::string::npos, "text has a time column");
    CHECK(x.find('\t') == std::string::npos && std::count(x.begin(), x.end(), '\n') == 4, "text: one line per row, no stray newlines");
    Table u;
    u.cols = {"a", "b"};
    u.add({"x", "y"});
    u.epoch.clear();
    CHECK(toText(u).rfind("a  b\n", 0) == 0, "text without times: %s", toText(u).c_str());
}

void testAdif() {
    Qso q; q.call = "LY1ABC"; q.mode = "ft8"; q.band = "20m"; q.freqMHz = 14.074; q.timeOn = kT; q.grid = "KO24"; q.rstRcvd = "-12"; q.comment = "tnx";
    Qso nocall; nocall.timeOn = kT;
    const std::string a = toAdif({q, nocall});
    CHECK(a.find("<EOH>") != std::string::npos && a.find("<ADIF_VER:5>3.1.4") != std::string::npos, "ADIF header");
    CHECK(a.find("<CALL:6>LY1ABC") != std::string::npos && a.find("<QSO_DATE:8>20261017") != std::string::npos && a.find("<TIME_ON:6>101500") != std::string::npos, "ADIF date and time: %s", a.c_str());
    CHECK(a.find("<MODE:3>FT8") != std::string::npos && a.find("<FREQ:6>14.074") != std::string::npos && a.find("<RST_RCVD:3>-12") != std::string::npos, "ADIF fields");
    CHECK(a.find("<EOR>") != std::string::npos && a.find("<EOR>") == a.rfind("<EOR>"), "one record: a row without a call is skipped");
}

void testSinks() {
    MemorySink m;
    std::string why;
    CHECK(exportGeo(m, "ADS-B tracks", "20261017-1015", Format::Kml, {pt("a", 1, 2)}, {}, &why) && m.items.size() == 1, "exportGeo");
    CHECK(m.items[0].name == "ADS-B-tracks-20261017-1015.kml" && m.items[0].mime.find("google-earth") != std::string::npos, "file name %s", m.items[0].name.c_str());
    CHECK(!exportGeo(m, "x", "", Format::Kml, {pt("bad", 99, 0)}, {}, &why) && !why.empty() && m.items.size() == 1, "nothing with a position: nothing delivered");
    CHECK(!exportGeo(m, "x", "", Format::Csv, {pt("a", 1, 2)}, {}, &why), "geo export refuses a table format");
    Table t; t.cols = {"a"}; t.add({"1"});
    CHECK(exportTable(m, "log", "", Format::Json, t) && m.items.back().name == "log.json", "exportTable");
    CHECK(!exportQsos(m, "log", "", {}, &why), "no contacts, no file");
    Qso q; q.call = "X1X"; q.timeOn = kT;
    CHECK(exportQsos(m, "qso", "s", {q}) && m.items.back().name == "qso-s.adi", "exportQsos");
    CHECK(formatFromPath("a/b/track.GeoJSON") == Format::GeoJson && formatFromPath("x.kml") == Format::Kml && formatFromPath("noext", Format::Txt) == Format::Txt, "format from path");
    CHECK(safeFileName("a b/c:d", "2026 10", Format::Gpx) == "a-b-c-d-2026-10.gpx", "safe file name %s", safeFileName("a b/c:d", "2026 10", Format::Gpx).c_str());

    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "onair-export-test";
    fs::remove_all(dir); fs::create_directories(dir);
    FileSink fsink(dir.string());
    ExportItem it; it.name = "../../evil/out.csv"; it.data = "x,y\r\n";
    CHECK(fsink.put(it) && slurp((dir / "out.csv").string()) == "x,y\r\n", "FileSink writes, and keeps a path in the name inside the folder");
    CHECK(!fs::exists(dir / "out.csv.tmp"), "no temp file left");
    FileSink bad((dir / "missing" / "deeper").string());
    CHECK(!bad.put(it) && !bad.error().empty(), "FileSink reports a folder that is not there");
    fs::remove_all(dir);
}

void testLocale() {
    // a decimal-comma locale must not leak into the files
    const char* names[] = {"de_DE.UTF-8", "de_DE", "lt_LT.UTF-8", "fr_FR.UTF-8"};
    for (const char* n : names) {
        if (!setlocale(LC_ALL, n)) continue;
        const std::string k = toKml({pt("a", 54.5, 25.25)}, {});
        CHECK(k.find("25.25,54.5") != std::string::npos, "locale %s: %s", n, k.c_str());
        break;
    }
    setlocale(LC_ALL, "C");
}
}  // namespace

int main() {
    testHelpers();
    testKml();
    testGpx();
    testGeoJson();
    testTables();
    testAdif();
    testSinks();
    testLocale();
    return testSummary("exporters");
}
