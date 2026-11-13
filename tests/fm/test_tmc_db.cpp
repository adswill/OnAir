// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// RDS-TMC in plain words: importing user event lists and location tables (German and English, ; , and tab, Latin-1 / UTF-8 / BOM / UTF-16,
// quoted fields, no header, the LCL list and the TISA exchange files), describing messages with and without them, the CSV / text log, and
// the whole path from generated RDS groups through the FM receiver. Every file here is synthetic (no national table ships with OnAir).
#include "dect2/fm/tmc_db.h"
#ifndef ONAIR_TEST_STANDALONE
#include "dect2/fm/fm_gen.h"
#include "dect2/fm/fm_rx.h"
#endif
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using namespace dect2;
using namespace dect2::tmcdb;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static std::string latin1(const std::string& utf8) {   // the test's UTF-8 text in ISO 8859-1, as older national lists come
    std::string o;
    for (size_t i = 0; i < utf8.size(); i++) {
        const unsigned char c = (unsigned char)utf8[i];
        if (c == 0xC3 && i + 1 < utf8.size()) { o += (char)(((unsigned char)utf8[i + 1] & 0x3F) | 0xC0); i++; }
        else if (c == 0xC2 && i + 1 < utf8.size()) { o += utf8[i + 1]; i++; }
        else o += (char)c;
    }
    return o;
}
static std::string utf16le(const std::string& ascii) {
    std::string o = "\xFF\xFE";
    for (char c : ascii) { o += c; o += '\0'; }
    return o;
}

static const char* kDeEvents =
    "CODE;TEXT;TEXT_Q;N;Q;T;D;U;C\n"
    "101;Stau;(Q) km Stau;F;1;D;1;;1\n"
    "701;Baustelle;(Q) Baustellen;O;0;L;1;;11\n"
    "1003;Straßenglätte;;W;;D;2;;9\n"
    "401;Straße gesperrt;;O;;L;1;;5\n";
static const char* kEnEvents =
    "\xEF\xBB\xBF\"Code\",\"Description\",\"Description (Q)\",\"Duration type\"\n"
    "101,\"stationary traffic\",\"(Q) km of stationary traffic\",D\n"
    "701,\"roadworks\",\"(Q) sets of roadworks\",L\n"
    "1003,\"slippery road\",,D\n"
    "1500,\"queue, then slow traffic\",,D\n";   // a comma inside quotes
static const char* kBothTab = "Nr\tEnglish\tDeutsch\n115\tqueuing traffic\tStockender Verkehr\n116\tslow traffic\tzähflüssiger Verkehr\n";
static const char* kNoHeader = "102;Stau auf 1 km\n103;Stau auf 2 km\n104;Stau auf 3 km\n";
static const char* kLcl =
    "LOCATION CODE;TYPE;SUBTYPE;ROAD_NUMBER;ROADNAME;FIRST_NAME;SECOND_NAME;AREA_REFERENCE;LINEAR_REFERENCE;NEGATIVE_OFFSET;POSITIVE_OFFSET;EXIT_NUMBER;CID;TABCD\n"
    "100;L1;1;A5;;Hattenbacher Dreieck;Basel;;;;;;58;1\n"
    "200;L3;0;A5;;Frankfurt;Karlsruhe;;100;;;;58;1\n"
    "4319;P1;3;;;Frankfurt-Süd;;;200;;4320;20;58;1\n"
    "4320;P1;3;;;Frankfurt-Niederrad;;;200;4319;4321;21;58;1\n"
    "4321;P1;1;;;Frankfurter Kreuz;;;200;4320;4322;22;58;1\n"
    "4322;P1;3;;;Zeppelinheim;;;200;4321;;23;58;1\n";
// TISA exchange format: several files, names by id
static const char* kTisaPoints = "CID;TABCD;LCD;CLASS;TCD;STCD;JUNCTIONNUMBER;RNID;N1ID;N2ID;POL_LCD;OTH_LCD;SEG_LCD;ROA_LCD;INPOS;INNEG;OUTPOS;OUTNEG;PRESENTPOS;PRESENTNEG;DIVERSIONPOS;DIVERSIONNEG;XCOORD;YCOORD;INTERRUPTSROAD;URBAN\n"
                                 "58;12;9001;P;1;3;7;;11;;;;9100;;1;1;1;1;1;1;;;+0;+0;0;0\n"
                                 "58;12;9002;P;1;3;8;;12;;;;9100;;1;1;1;1;1;1;;;+0;+0;0;0\n"
                                 "58;12;9003;P;1;3;9;;13;;;;9100;;1;1;1;1;1;1;;;+0;+0;0;0\n";
static const char* kTisaSegments = "CID;TABCD;LCD;CLASS;TCD;STCD;ROADNUMBER;RNID;N1ID;N2ID;ROA_LCD;SEG_LCD;POL_LCD\n58;12;9100;L;3;0;A7;;14;15;9200;;\n";
static const char* kTisaRoads = "CID;TABCD;LCD;CLASS;TCD;STCD;ROADNUMBER;RNID;N1ID;N2ID;POL_LCD;PES_LEV\n58;12;9200;L;1;1;A7;16;14;15;;\n";
static const char* kTisaOffsets = "CID;TABCD;LCD;NEG_OFF_LCD;POS_OFF_LCD\n58;12;9001;;9002\n58;12;9002;9001;9003\n58;12;9003;9002;\n";
static const char* kTisaNames = "CID;LID;NID;NAME;NCOMMENT;OFFICIALNAME\n58;1;11;Kassel-Nord;;\n58;1;12;Kassel-Mitte;;\n58;1;13;Kassel-Süd;;\n58;1;14;Flensburg;;\n58;1;15;Füssen;;\n58;1;16;Bundesautobahn 7;;\n";

static TmcMessage msg(int ev, int loc, int ext, bool neg, int dur, bool ll) {
    TmcMessage m;
    m.event = ev; m.location = loc; m.extent = ext; m.negative = neg; m.durationCode = dur;
    m.text = TmcDecoder::eventText(ev);
    m.durationText = TmcDecoder::durationText(dur, ll);
    m.received = 1.7e9;
    return m;
}

static std::string readAll(const std::filesystem::path& p) { std::ifstream f(p, std::ios::binary); std::stringstream s; s << f.rdbuf(); return s.str(); }

int main() {
    {   // CSV basics
        const Csv a = parseCsv(toUtf8(kEnEvents));
        CHECK(a.delim == ',' && a.header && a.cols.size() == 4 && a.cols[0] == "Code", "en csv: delim '%c' header %d cols %zu", a.delim, a.header, a.cols.size());
        CHECK(a.rows.size() == 4 && a.rows[3][1] == "queue, then slow traffic", "quoted comma lost");
        const Csv b = parseCsv(kBothTab);
        CHECK(b.delim == '\t' && b.header && b.rows.size() == 2, "tab csv: delim %d rows %zu", b.delim, b.rows.size());
        const Csv c = parseCsv(kNoHeader);
        CHECK(c.delim == ';' && !c.header && c.rows.size() == 3, "no header: header %d rows %zu", c.header, c.rows.size());
        CHECK(toUtf8(latin1("Straßenglätte")) == "Straßenglätte", "Latin-1 to UTF-8: '%s'", toUtf8(latin1("Straßenglätte")).c_str());
        CHECK(toUtf8("\x80 \x93x\x94") == "\xE2\x82\xAC \xE2\x80\x9Cx\xE2\x80\x9D", "Windows-1252 euro / quotes");
        CHECK(toUtf8(utf16le("101;Stau")) == "101;Stau", "UTF-16 LE");
        CHECK(normCol(" Location-Code ") == "locationcode", "normCol");
    }
    Database db;
    {   // event lists
        ImportReport r = db.importText(latin1(kDeEvents), "EL_deu.csv");
        CHECK(r.ok && r.kind == "event list" && r.rows == 4 && r.languages.size() == 1 && r.languages[0] == "de", "de events: ok %d kind '%s' rows %d (%s)", r.ok, r.kind.c_str(), r.rows, r.message.c_str());
        r = db.importText(kEnEvents, "events.csv");
        CHECK(r.ok && r.rows == 4 && r.languages.size() == 1 && r.languages[0] == "en", "en events: %s (%s)", r.message.c_str(), r.languages.empty() ? "" : r.languages[0].c_str());
        r = db.importText(kBothTab, "list.txt");
        CHECK(r.ok && r.rows == 2 && r.languages.size() == 2, "two-language list: %s", r.message.c_str());
        r = db.importText(kNoHeader, "x.csv");
        CHECK(r.ok && r.rows == 3 && !r.languages.empty() && r.languages[0] == "de", "headerless list: %s", r.message.c_str());
        CHECK(db.eventText(101, "de") == "Stau" && db.eventText(101, "en") == "stationary traffic" && db.eventText(101, "auto") == "stationary traffic", "event 101 texts");
        CHECK(db.eventText(1003, "de") == "Straßenglätte", "umlaut lost: '%s'", db.eventText(1003, "de").c_str());
        CHECK(db.eventText(116, "de") == "zähflüssiger Verkehr" && db.eventText(116, "en") == "slow traffic", "tab list texts");
        CHECK(db.eventText(102, "fr") == "Stau auf 1 km", "fallback language");
        CHECK(db.event(701) && db.event(701)->durType == 'L' && db.event(101)->durType == 'D', "duration types");
        CHECK(db.eventText(999, "de").empty(), "unknown event has text");
        const ImportReport bad = db.importText("hello world\nfoo bar\n", "notes.txt");
        CHECK(!bad.ok, "a text file taken as a list");
    }
    {   // location table, one list
        const ImportReport r = db.importText(latin1(kLcl), "LCL21.0.D.csv");
        CHECK(r.ok && r.kind == "location table" && r.rows == 6 && r.tabcd == 1 && r.cid == 58, "LCL: %s (tabcd %d)", r.message.c_str(), r.tabcd);
        const Location* l = db.location(4321);
        CHECK(l && l->firstName == "Frankfurter Kreuz" && l->type == "P1.1" && l->linear == 200 && l->negOff == 4320 && l->posOff == 4322 && l->exitNumber == "22", "LCL row 4321");
        CHECK(db.follow(4321, 2, false) == 4319 && db.follow(4321, 1, true) == 4322 && db.follow(4322, 1, true) == 0, "offset chain");
    }
    {   // TISA files
        Database t;
        // names first: the ids are resolved when the points come
        CHECK(t.importText(latin1(kTisaNames), "NAMES.DAT").ok, "TISA names");
        const ImportReport p = t.importText(kTisaPoints, "POINTS.DAT");
        CHECK(p.ok && p.kind == "TISA points" && p.rows == 3 && p.tabcd == 12, "TISA points: %s", p.message.c_str());
        CHECK(t.importText(kTisaSegments, "SEGMENTS.DAT").kind == "TISA segments", "TISA segments");
        CHECK(t.importText(kTisaRoads, "ROADS.DAT").kind == "TISA roads", "TISA roads");
        CHECK(t.importText(kTisaOffsets, "POFFSETS.DAT").kind == "TISA offsets", "TISA offsets");
        const Location* l = t.location(9002);
        CHECK(l && l->firstName == "Kassel-Mitte" && l->exitNumber == "8" && l->negOff == 9001 && l->posOff == 9003, "TISA point 9002");
        // also without file names: the columns tell the kind
        Database u;
        CHECK(u.importText(kTisaPoints, "a.csv").kind == "TISA points" && u.importText(kTisaOffsets, "b.csv").kind == "TISA offsets", "TISA by columns");
        const Plain pl = describe(msg(101, 9001, 2, true, 0, false), &t, "en");
        CHECK(pl.road == "A7 Bundesautobahn 7" && pl.location == "Kassel-Nord (7)" && pl.direction == "towards Flensburg", "TISA describe: road '%s' loc '%s' dir '%s'", pl.road.c_str(), pl.location.c_str(), pl.direction.c_str());
        CHECK(pl.extent == "from Kassel-Süd (9) (2 locations)", "TISA extent '%s'", pl.extent.c_str());
    }
    {   // in words
        TmcMessage m = msg(101, 4321, 2, false, 3, false);
        m.details = {{3, 12}, {9, 1003}, {10, 4320}};
        Plain p = describe(m, &db, "de");
        CHECK(p.event == "Stau" && p.road == "A5" && p.location == "Frankfurter Kreuz (22)" && p.direction == "towards Karlsruhe", "de: '%s' / '%s' / '%s' / '%s'", p.event.c_str(), p.road.c_str(), p.location.c_str(), p.direction.c_str());
        CHECK(p.extent == "from Frankfurt-Süd (20) (2 locations)", "extent '%s'", p.extent.c_str());
        CHECK(p.duration == "1 hour" && p.details.size() == 3 && p.details[0] == "speed limit 60 km/h" && p.details[1] == "Straßenglätte" && p.details[2] == "diversion via Frankfurt-Niederrad (21)",
              "details (%zu) '%s'", p.details.size(), p.details.empty() ? "" : p.details[1].c_str());
        CHECK(p.eventKnown && p.locationKnown, "known flags");
        const std::string line = p.line();
        CHECK(line == "A5 Frankfurter Kreuz (22), towards Karlsruhe: Stau; speed limit 60 km/h; Straßenglätte; diversion via Frankfurt-Niederrad (21); from Frankfurt-Süd (20) (2 locations); 1 hour",
              "line '%s'", line.c_str());
        // negative direction: towards the first name, extent along the positive offsets
        p = describe(msg(701, 4320, 1, true, 2, false), &db, "en");
        CHECK(p.event == "roadworks" && p.direction == "towards Frankfurt" && p.extent == "from Frankfurter Kreuz (22) (1 location)", "neg: '%s' '%s'", p.direction.c_str(), p.extent.c_str());
        CHECK(p.duration == "until tonight", "the list's duration type (L) gives '%s'", p.duration.c_str());
        // no files: the codes stay, with the hint mark
        p = describe(msg(101, 4321, 2, false, 0, false), nullptr, "");
        CHECK(p.event == "stationary traffic" && p.location == "location 4321 (?)" && p.direction == "positive direction" && p.extent == "+2 locations" && !p.locationKnown,
              "no db: '%s' '%s' '%s'", p.event.c_str(), p.location.c_str(), p.extent.c_str());
        p = describe(msg(1999, 1, 0, false, 0, false), nullptr, "");
        CHECK(p.event == "event 1999 (?)" && !p.eventKnown, "unknown event '%s'", p.event.c_str());
        // an extent that runs off the table
        p = describe(msg(101, 4321, 5, false, 0, false), &db, "en");
        CHECK(p.extent == "+5 locations", "broken chain '%s'", p.extent.c_str());
    }
    {   // the log
        const std::filesystem::path dir = std::filesystem::temp_directory_path() / ("onair_tmclog_" + std::to_string((long long)std::chrono::steady_clock::now().time_since_epoch().count()));
        Log log;
        CHECK(!log.enabled(), "log on without a path");
        log.setPath((dir / "tmc").string());
        CHECK(log.enabled(), "log off with a path (default on)");
        Station st; st.freqMhz = 93.9; st.pi = 0xD3C2; st.ps = "HR3     "; st.ltn = 1;
        std::vector<TmcMessage> ms = {msg(101, 4321, 2, false, 3, false)};
        CHECK(log.update(ms, st, &db, "de") == 1, "first message not logged");
        CHECK(log.update(ms, st, &db, "de") == 0, "a repeat logged again");
        ms[0].count = 5; ms[0].lastSeen += 60;
        CHECK(log.update(ms, st, &db, "de") == 0, "a repeat with a new count logged");
        TmcMessage m2 = msg(1500, 4320, 0, true, 0, false);
        m2.received += 30;
        ms.insert(ms.begin(), m2);
        CHECK(log.update(ms, st, &db, "en") == 1, "second message");
        log.setPath("");
        const std::string csv = readAll(dir / "tmc.csv"), txt = readAll(dir / "tmc.txt");
        int rows = 0;
        for (char ch : csv) rows += ch == '\n';
        CHECK(rows == 3 && csv.rfind(Log::csvHeader(), 0) == 0, "csv rows %d", rows);
        CHECK(csv.find(",93.90,D3C2,HR3     ,1,,101,Stau,4321,Frankfurter Kreuz (22),A5,towards Karlsruhe,2,from Frankfurt-Süd (20) (2 locations),1 hour,no,") != std::string::npos, "csv row:\n%s", csv.c_str());
        CHECK(csv.find("\"queue, then slow traffic\"") != std::string::npos, "csv quoting");
        CHECK(txt.find("93.90 MHz HR3 (D3C2): A5 Frankfurter Kreuz (22), towards Karlsruhe: Stau;") != std::string::npos, "txt:\n%s", txt.c_str());
        // appending: a new log on the same path keeps the header once
        Log again; again.setPath((dir / "tmc").string());
        TmcMessage m3 = msg(701, 4322, 0, false, 0, false);
        again.update({m3}, st, nullptr, "");
        again.setPath("");
        const std::string csv2 = readAll(dir / "tmc.csv");
        CHECK(csv2.find("time_utc", 10) == std::string::npos && csv2.find("roadworks") != std::string::npos, "append");
        // the folder loader
        Database f;
        std::filesystem::create_directories(dir / "tables");
        { std::ofstream o(dir / "tables" / "events_en.csv", std::ios::binary); o << kEnEvents; }
        { std::ofstream o(dir / "tables" / "LCL.csv", std::ios::binary); o << latin1(kLcl); }
        const ImportReport r = loadFolder(f, (dir / "tables").string());
        CHECK(r.ok && f.eventCount() == 4 && f.locationCount() == 6, "folder: %s", r.message.c_str());
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
#ifndef ONAIR_TEST_STANDALONE
    {   // generated RDS: 3A announcement, a single-group message (roadworks at 4320, 1 more location, negative), through the receiver, in words
        auto hdr = [](int low) { return (uint16_t)(0x8000 | (1 << 10) | (10 << 5) | low); };
        FmGenConfig c; c.rate = 2e6; c.cnrDb = 40;
        c.extraGroups.push_back({(uint16_t)(0x3000 | (1 << 10) | (10 << 5) | 16), (uint16_t)((1 << 6) | (1 << 4)), 0xCD46});
        c.extraGroups.push_back({hdr(8 | 2), (uint16_t)((1 << 14) | (1 << 11) | 701), 4320});
        FmGenerator gen(c);
        FmReceiver rx; rx.setSilent(true); rx.configure(c.rate);
        std::vector<cf32> buf;
        FmTelemetry t; uint64_t seq = 0;
        for (int i = 0; i < 200; i++) {
            buf.clear(); gen.generate(65536, buf); rx.feed(buf.data(), buf.size());
            FmTelemetry x; if (rx.telemetry(x, seq)) { seq = x.seq; t = x; }
        }
        CHECK(t.tmc.seen && t.tmcMessages.size() == 1, "receiver: TMC %d, %zu messages", t.tmc.seen, t.tmcMessages.size());
        if (!t.tmcMessages.empty()) {
            const Plain p = describe(t.tmcMessages[0], &db, "de");
            CHECK(p.line() == "A5 Frankfurt-Niederrad (21), towards Frankfurt: Baustelle; from Frankfurter Kreuz (22) (1 location); until tonight", "receiver line '%s'", p.line().c_str());
        }
    }
#endif
    printf(fails ? "tmc_db: %d FAILED\n" : "tmc_db: all passed\n", fails);
    return fails ? 1 : 0;
}
