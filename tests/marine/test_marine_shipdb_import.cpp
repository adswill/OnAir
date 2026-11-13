// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The MMSI list import (YADDNET-style ship and coast station lists): delimiters, header spellings, columns found by content, degrees / minutes and decimal
// coordinates, spreadsheet-damaged numbers, the MID table and the merge rules.
#include "dect2/marine/marine_mmsi.h"
#include <cmath>
#include <cstdio>
using namespace dect2::marine;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)
static bool near(double a, double b) { return std::fabs(a - b) < 1e-3; }

int main() {
    // ---- coordinates
    {
        double v = 0; char h = 0;
        CHECK(parseCoordinate("51.5", v) && near(v, 51.5), "decimal");
        CHECK(parseCoordinate("-3.25", v) && near(v, -3.25), "negative decimal");
        CHECK(parseCoordinate("51,5", v) && near(v, 51.5), "decimal comma");
        CHECK(parseCoordinate("51 30.5 N", v, &h) && near(v, 51 + 30.5 / 60) && h == 'N', "deg min N");
        CHECK(parseCoordinate("003 12.3 W", v, &h) && near(v, -(3 + 12.3 / 60)) && h == 'W', "deg min W");
        CHECK(parseCoordinate("51\xC2\xB0" "30'30\"N", v) && near(v, 51 + 30.0 / 60 + 30.0 / 3600), "deg min sec with marks");
        CHECK(parseCoordinate("51-30.5N", v) && near(v, 51 + 30.5 / 60), "dashes");
        CHECK(parseCoordinate("N 51 30", v) && near(v, 51.5), "hemisphere first");
        CHECK(parseCoordinate("5130N", v) && near(v, 51.5), "ddmm");
        CHECK(parseCoordinate("00312W", v) && near(v, -(3 + 12.0 / 60)), "dddmm W");
        CHECK(!parseCoordinate("51 75 N", v), "minutes 75 rejected");
        CHECK(!parseCoordinate("hello", v) && !parseCoordinate("", v), "garbage");
        CHECK(!parseCoordinate("1234", v, nullptr, true) && parseCoordinate("12.5", v, nullptr, true), "strict wants a point or a letter");
        double la, lo;
        CHECK(parsePosition("51 30.5 N 003 12.3 W", la, lo) && near(la, 51 + 30.5 / 60) && near(lo, -(3 + 12.3 / 60)), "pair, suffix letters");
        CHECK(parsePosition("N 51 30 E 003 12", la, lo) && near(la, 51.5) && near(lo, 3.2), "pair, prefix letters");
        CHECK(parsePosition("003 12 W 51 30 N", la, lo) && near(la, 51.5) && near(lo, -3.2), "longitude first");
        CHECK(parsePosition("51.5/-3.2", la, lo) && near(la, 51.5) && near(lo, -3.2), "decimal slash");
        CHECK(parsePosition("51.5, -3.2", la, lo) && near(la, 51.5) && near(lo, -3.2), "decimal comma-space");
        CHECK(parsePosition("51,5 -3,2", la, lo) && near(la, 51.5) && near(lo, -3.2), "decimal comma, space");
        CHECK(!parsePosition("120.0/10.0", la, lo), "latitude out of range");
    }
    // ---- MMSI cells and the MID table
    {
        CHECK(normaliseMmsi("232 123 456") == "232123456" && normaliseMmsi("'002320064") == "002320064", "spaces, apostrophe");
        CHECK(normaliseMmsi("2320064") == "002320064", "coast station lost two zeros");
        CHECK(normaliseMmsi("23200645") == "023200645", "group lost a zero");
        CHECK(normaliseMmsi("5321234") == "" && normaliseMmsi("12345678901") == "" && normaliseMmsi("2321x3456") == "", "not numbers");
        CHECK(std::string(midCountry(232)) == "United Kingdom" && std::string(midCountry(244)) == "Netherlands" && std::string(midCountry(277)) == "Lithuania" &&
              std::string(midCountry(366)) == "United States" && std::string(midCountry(999)) == "", "MID table");
        CHECK(mmsiInfo("244123456").country == "Netherlands" && mmsiInfo("244123456").kind == MmsiKind::Ship, "ship MID");
        CHECK(mmsiInfo("002440001").kind == MmsiKind::CoastStation && mmsiInfo("002440001").country == "Netherlands", "coast MID");
        CHECK(mmsiInfo("024400001").kind == MmsiKind::Group && mmsiInfo("024400001").mid == 244, "group MID");
        CHECK(mmsiInfo("111232001").kind == MmsiKind::SarAircraft && mmsiInfo("111232001").country == "United Kingdom", "SAR MID");
    }
    // ---- layouts
    {   // 1: semicolon, header, DM position in two columns with hemisphere letters
        ShipDb db;
        const std::string t = "MMSI;Name;Callsign;Country;Latitude;Longitude\n"
                              "002440001;Scheveningen Radio;PCH;Netherlands;52 06.5 N;004 16.2 E\n"
                              "244123456;MV Voorbeeld;PABC;Netherlands;;\n";
        const ImportPreview pv = ShipDb::preview(t);
        CHECK(pv.ok && pv.delim == ';' && pv.header && pv.records == 2 && pv.columns.size() == 6 && pv.columns[4] == "lat" && pv.columns[5] == "lon", "layout 1 preview");
        const ImportStats s = db.importDetailed(t);
        ShipRec r;
        CHECK(s.read == 2 && s.added == 2 && s.withPos == 1 && s.coast == 1 && s.ships == 1, "layout 1 stats %d %d", s.read, s.added);
        CHECK(db.find("002440001", r) && r.hasPos && near(r.lat, 52 + 6.5 / 60) && near(r.lon, 4 + 16.2 / 60) && r.callsign == "PCH", "layout 1 DM position");
        CHECK(db.find("244123456", r) && !r.hasPos && r.name == "MV Voorbeeld", "layout 1 ship");
    }
    {   // 2: tab separated coast station list, one position column, decimal, odd header names
        ShipDb db;
        const std::string t = "MMSI number\tStation name\tLocation\tPosition\n"
                              "002320064\tSolent Coastguard\tLee-on-Solent\t50.80, -1.20\n"
                              "002320063\tThames Coastguard\tWalton\t51.85/1.27\n";
        ShipRec r;
        CHECK(db.importDetailed(t).read == 2, "layout 2 count");
        CHECK(db.find("002320064", r) && r.hasPos && near(r.lat, 50.8) && near(r.lon, -1.2) && r.station == "Solent Coastguard" && r.location == "Lee-on-Solent", "layout 2 fields");
        CHECK(db.name("002320063") == "Thames Coastguard, Walton" && db.positioned().size() == 2, "layout 2 names");
    }
    {   // 3: comma, quoted fields with commas, a position held in a "Location" column
        ShipDb db;
        const std::string t = "mmsi,\"ship name\",\"call sign\",flag,location\n"
                              "219000123,\"Nordic Star, The\",OXAB,Denmark,\"55 40.2 N 012 34.5 E\"\n";
        ShipRec r;
        CHECK(db.importDetailed(t).read == 1 && db.find("219000123", r) && r.name == "Nordic Star, The" && r.hasPos && near(r.lat, 55 + 40.2 / 60) && near(r.lon, 12 + 34.5 / 60), "layout 3");
    }
    {   // 4: no header, pipe separated, the coordinate columns are found by content
        ShipDb db;
        const std::string t = "232123456|MV Example|MABC1|Cargo|50.1000|-1.5000\n232123457|MV Other|MXYZ2|Tug|51.2000|-2.5000\n";
        const ImportPreview pv = ShipDb::preview(t);
        CHECK(pv.ok && pv.delim == '|' && !pv.header && pv.columns.size() == 6 && pv.columns[0] == "mmsi" && pv.columns[4] == "lat" && pv.columns[5] == "lon", "layout 4 preview");
        ShipRec r;
        CHECK(db.importDetailed(t).withPos == 2 && db.find("232123457", r) && near(r.lat, 51.2) && near(r.lon, -2.5) && r.type == "Tug", "layout 4");
    }
    {   // 5: fixed-width text (two spaces or more), a spreadsheet that dropped the leading zeros and a BOM, CRLF
        ShipDb db;
        const std::string t = "\xEF\xBB\xBF" "MMSI     Name                 Country\r\n2440001  Scheveningen Radio   Netherlands\r\n24400001  Dutch Group Call    Netherlands\r\n";
        ShipRec r;
        const ImportStats s = db.importDetailed(t);
        CHECK(s.read == 2, "layout 5 count %d", s.read);
        CHECK(db.find("002440001", r) && r.name == "Scheveningen Radio" && db.find("024400001", r), "layout 5 numbers repaired");
    }
    {   // 6: headerless, name column then a combined position column
        ShipDb db;
        const std::string t = "002190000,Lyngby Radio,OXZ,Denmark,\"56 02.0 N 008 07.0 E\"\n";
        ShipRec r;
        CHECK(db.importDetailed(t).read == 1 && db.find("002190000", r) && r.hasPos && near(r.lat, 56 + 2.0 / 60) && near(r.lon, 8 + 7.0 / 60) && r.name == "Lyngby Radio", "layout 6");
    }
    // ---- merge rules
    {
        ShipDb db;
        db.learn("232123456", "AIS NAME", "AISCALL", "Cargo");
        CHECK(db.importDetailed("mmsi,name,lat,lon\n232123456,Imported,50.0,-1.0\n").updated == 1, "update counts");
        ShipRec r;
        CHECK(db.find("232123456", r) && r.name == "Imported" && r.callsign == "AISCALL" && r.hasPos, "import wins, AIS call sign kept");
        // an empty cell does not erase, a position stays when the next file has none
        const ImportStats s = db.importDetailed("mmsi,name,callsign\n232123456,,NEWCALL\n232123456,\n999,x\nabc,def\n");
        CHECK(db.find("232123456", r) && r.name == "Imported" && r.callsign == "NEWCALL" && r.hasPos && near(r.lat, 50.0), "empty cells keep, position kept");
        CHECK(s.skipped >= 1, "rows without a number are skipped (%d)", s.skipped);
        // the same file twice changes nothing and makes no duplicates
        const std::string f = "mmsi;name;lat;lon\n002320064;Solent;50.8;-1.2\n232999999;Boat;;\n";
        const ImportStats a = db.importDetailed(f), b = db.importDetailed(f);
        CHECK(a.added == 2 && b.added == 0 && b.unchanged == 2 && b.updated == 0, "dedupe %d %d %d", b.added, b.unchanged, b.updated);
        CHECK(db.size() == 3, "three records: %zu", db.size());
        db.learn("232123456", "LATER AIS", "", "");
        CHECK(db.name("232123456") == "Imported", "AIS never replaces an import");
        // a new position replaces the old one
        db.importDetailed("mmsi;name;lat;lon\n002320064;Solent;50.9;-1.3\n");
        CHECK(db.find("002320064", r) && near(r.lat, 50.9), "position replaced");
        // round trip through the saved file
        ShipDb d2;
        CHECK(d2.importText(db.exportTsv()) == 3 && d2.positioned().size() == db.positioned().size(), "export round trip");
    }
    {   // the layouts the older tests rely on
        ShipDb db;
        CHECK(db.importText("MMSI,Name,Callsign,Type,Country,Station,Location,Lat,Lon\n232123456,\"Imported, Name\",MXYZ,Tanker,GB,,,,\n002320064,,,Coast station,GB,Solent Coastguard,Lee-on-Solent,50.80,-1.20\n") == 2, "csv with empty cells");
        CHECK(db.name("002320064") == "Solent Coastguard, Lee-on-Solent" && db.positioned().size() == 1, "coast station");
        ShipDb d3;
        CHECK(d3.importText("232123456 \n") == 0, "a number alone");
        CHECK(d3.importText("232123456;Ship;CALL;Tug;;;;;\n") == 1 && d3.name("232123456") == "Ship", "semicolons, no header");
        CHECK(d3.importText("219000123\tNordic Star\n") == 1, "two columns");
    }
    {
        ShipDb db;
        CHECK(db.importText("hello\nworld\n") == 0 && db.size() == 0, "no numbers, nothing stored");
        CHECK(!ShipDb::preview("just text").ok, "preview says no");
    }
    if (!fails) printf("shipdb import: all passed\n");
    return fails ? 1 : 0;
}
