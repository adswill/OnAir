// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The per-aircraft ACARS log (core/src/acars/acars_log.cpp): which messages are interesting, redaction of phone numbers and e-mail, the
// attachment of messages to aircraft (by ICAO address, registration, flight; an ambiguous callsign gets nothing), the bounds, and the
// JSON of an entry with awkward text.
#include "dect2/acars/acars_log.h"
#include <cstdio>
using namespace dect2;

#include "../common/test_util.h"

static AcarsLogEntry mk(int64_t wall, const char* label, const char* text, const char* flight = "") {
    AcarsLogEntry e;
    e.wall = wall; e.mode = "VDL2"; e.freqMHz = 136.975; e.label = label; e.text = text; e.flight = flight;
    e.cat = acarsClassify(label, text);
    return e;
}

int main() {
    // classification
    CHECK(acarsClassify("_d", "") == AcarsCat::Ignore, "ack ignored");
    CHECK(acarsClassify("Q0", "") == AcarsCat::Ignore, "link test ignored");
    CHECK(acarsClassify("SQ", "02XAOMS1234") == AcarsCat::Ignore, "squitter ignored");
    CHECK(acarsClassify("SA", "0EV123456VS") == AcarsCat::Ignore, "media advisory ignored");
    CHECK(acarsClassify("H1", "") == AcarsCat::Ignore, "empty frame ignored");
    CHECK(acarsClassify("H1", "ab") == AcarsCat::Ignore, "two characters ignored");
    CHECK(acarsClassify("H1", "0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF") == AcarsCat::Ignore, "hex blob ignored");
    CHECK(acarsClassify("H1", "..?..;..:.,.,.;..") == AcarsCat::Ignore, "binary ignored");
    CHECK(acarsClassify("80", "PLEASE ADVISE GATE FOR ARRIVAL") == AcarsCat::Free, "label 80 free text");
    CHECK(acarsClassify("ZZ", "REQUEST CATERING FOR TURNAROUND") == AcarsCat::Free, "unknown label with words is free text");
    CHECK(acarsClassify("ZZ", "XJ91KQ") == AcarsCat::Ignore, "unknown label, no words");
    CHECK(acarsClassify("Q1", "OMDBEGLL0540054505500600") == AcarsCat::Ops, "OOOI is ops");
    CHECK(acarsClassify("5Z", "B6 REQ 1234") == AcarsCat::Company, "5Z company");
    CHECK(acarsClassify("H1", "- #M1BPOS N4512.3E00812.4") == AcarsCat::Ops, "H1 ops");
    CHECK(acarsClassify("H1", "CFDS REPORT 32-41 FAULT L MLG") == AcarsCat::Fault, "fault keyword");
    CHECK(acarsClassify("ZZ", "MAINT MESSAGE: CFDS PRINT REQUESTED") == AcarsCat::Maint, "maintenance keyword");
    CHECK(acarsClassify("ZZ", "CAPT REQUESTS WHEELCHAIR AT GATE") == AcarsCat::Crew, "crew keyword");
    CHECK(std::string(acarsCatKey(AcarsCat::Maint)) == "maint", "key");

    // redaction
    CHECK(acarsRedact("CALL +971 4 123 4567 NOW") == "CALL [number] NOW", "intl phone: '%s'", acarsRedact("CALL +971 4 123 4567 NOW").c_str());
    CHECK(acarsRedact("TEL 0044 20 7946 0958") == "TEL [number]", "00 prefix");
    CHECK(acarsRedact("TEL 370-612-34567 OK") == "TEL [number] OK", "dashed: '%s'", acarsRedact("TEL 370-612-34567 OK").c_str());
    CHECK(acarsRedact("MAIL crew.ops@airline-x.example PLS") == "MAIL [email] PLS", "email: '%s'", acarsRedact("MAIL crew.ops@airline-x.example PLS").c_str());
    CHECK(acarsRedact("FL350 N4512.3 E00812.4 ETA 1234Z") == "FL350 N4512.3 E00812.4 ETA 1234Z", "coordinates untouched");
    CHECK(acarsRedact("EK0201 GATE B12 0540 0545 0550") == "EK0201 GATE B12 0540 0545 0550", "times untouched: '%s'", acarsRedact("EK0201 GATE B12 0540 0545 0550").c_str());
    CHECK(acarsRedact("A@B") == "A@B", "not an address");
    CHECK(acarsRedact("") == "", "empty");

    // keys
    CHECK(acarsNormReg("a6-eda") == "A6EDA" && acarsNormReg("..N123AB") == "N123AB", "reg key");
    CHECK(acarsNormFlight("EK0201") == "EK201" && acarsNormFlight("ek 201") == "EK201", "flight key");

    // attachment
    const int64_t T = 1800000000;
    AcarsLog log;
    AcarsIdent byIcao; byIcao.icao = 0x896123; byIcao.reg = "A6-EDA"; byIcao.flight = "EK0201";
    CHECK(log.add(byIcao, mk(T, "H1", "ARRIVING WITH TWO WHEELCHAIRS")), "added");
    AcarsIdent byReg; byReg.reg = "A6EDA";
    CHECK(log.add(byReg, mk(T + 60, "80", "GATE CHANGE B12 TO B14")), "added by reg (alias to the ICAO bucket)");
    AcarsIdent other; other.reg = "D-AIXA";
    CHECK(log.add(other, mk(T + 70, "80", "OTHER AIRCRAFT MESSAGE TEXT")), "other aircraft");
    AcarsIdent byFlight; byFlight.flight = "LH0400";
    CHECK(log.add(byFlight, mk(T + 80, "80", "FLIGHT ONLY MESSAGE TEXT HERE")), "flight only");
    CHECK(!log.add(AcarsIdent{}, mk(T + 90, "80", "NOBODY OWNS THIS MESSAGE")), "no identity: not stored");
    CHECK(!log.add(byIcao, mk(T + 5, "Q0", "")), "ignored class not stored");

    AcarsAircraftKey a; a.icao = 0x896123;
    auto v = log.messagesFor(a, T + 100);
    CHECK(v.size() == 2 && v[0].text.rfind("GATE", 0) == 0, "by ICAO: %zu", v.size());
    AcarsAircraftKey b; b.icao = 0x3C6444; b.reg = "D-AIXA";
    CHECK(log.messagesFor(b, T + 100).size() == 1, "by registration only");
    AcarsAircraftKey c; c.icao = 0x111111; c.callsign = "LH400"; c.callsignHolders = 1;
    CHECK(log.messagesFor(c, T + 100).size() == 1, "by unique callsign");
    c.callsignHolders = 2;
    CHECK(log.messagesFor(c, T + 100).empty(), "ambiguous callsign gets nothing");
    AcarsAircraftKey d; d.icao = 0x222222; d.reg = "N999ZZ";
    CHECK(log.messagesFor(d, T + 100).empty(), "different aircraft gets nothing");

    // repeats, bounds, age
    CHECK(!log.add(byIcao, mk(T + 70, "80", "GATE CHANGE B12 TO B14")), "repeat within 30 s dropped");
    AcarsLog big;
    for (int i = 0; i < 200; i++) big.add(byIcao, mk(T + i * 40, "80", ("MESSAGE NUMBER " + std::to_string(i)).c_str()));
    CHECK(big.messagesFor(a, T + 8000).size() == kAcarsLogPerAircraft, "per aircraft cap");
    CHECK(big.messagesFor(a, T + 8000)[0].text == "MESSAGE NUMBER 199", "newest first");
    CHECK(big.messagesFor(a, T + 8000 + kAcarsLogMaxAgeSec + 10).empty(), "older than 48 h hidden");
    big.prune(T + 8000 + kAcarsLogMaxAgeSec + 10);
    CHECK(big.aircraft() == 0, "pruned");
    AcarsLog many;
    for (uint32_t i = 0; i < 1000; i++) { AcarsIdent id; id.icao = 0x100000 + i; many.add(id, mk(T + i, "80", "SOME READABLE TEXT HERE")); }
    CHECK(many.aircraft() <= kAcarsLogMaxAircraft, "aircraft cap: %zu", many.aircraft());
    AcarsLogEntry longE = mk(T, "80", "X");
    longE.text = std::string(5000, 'A'); longE.cat = AcarsCat::Free;
    AcarsLog lg; lg.add(byIcao, longE);
    CHECK(lg.messagesFor(a, T + 1)[0].text.size() == kAcarsLogTextMax, "text cap");

    // JSON
    const std::string weird = "quote\" back\\ tab\t nl\n ctl\x01 del\x7f uni \xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80 bad\xFF\xC3 end";
    const std::string esc = acarsJsonEscape(weird);
    CHECK(esc.find('\n') == std::string::npos && esc.find('\t') == std::string::npos && esc.find('\x01') == std::string::npos, "no raw controls");
    CHECK(esc.find("\\\"") != std::string::npos && esc.find("\\\\") != std::string::npos && esc.find("\\u000a") != std::string::npos, "escapes: %s", esc.c_str());
    CHECK(esc.find("\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80") != std::string::npos, "valid UTF-8 kept");
    CHECK(esc.find("bad?? end") != std::string::npos || esc.find("bad??") != std::string::npos, "invalid bytes replaced: %s", esc.c_str());
    AcarsLogEntry e = mk(T, "H1", "x"); e.text = weird; e.cat = AcarsCat::Crew;
    const std::string j = acarsEntryJson(e);
    CHECK(j.front() == '{' && j.back() == '}' && j.find("\"c\":\"crew\"") != std::string::npos && j.find("\"f\":136.975") != std::string::npos, "entry json: %s", j.c_str());
    // every quote in the JSON is either structural or escaped: count unescaped quotes (10 for five string/number-key pairs... keys t m f l c x)
    int q = 0;
    for (size_t i = 0; i < j.size(); i++) { if (j[i] == '\\') { i++; continue; } if (j[i] == '"') q++; }
    CHECK(q == 2 * 6 + 2 * 4, "balanced quotes: %d", q);   // 6 keys, 4 string values (m, l, c, x)
    return testSummary("acars_log");
}
