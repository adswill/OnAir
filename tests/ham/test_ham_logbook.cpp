// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The Ham logbook: add / edit / delete with undo and redo, dupe check, search, ADIF round trip (every field, unknown fields kept), import
// of another program's file (header, lower case tags, odd lengths), save and load.
#include "dect2/ham/ham.h"
#include "dect2/ham/ham_log.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
using namespace dect2::ham;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static LogQso mk(const char* call, double mhz, const char* mode, double utc, const char* grid = "") {
    LogQso q; q.call = call; q.freqHz = mhz * 1e6; q.mode = mode; q.startUtc = utc; q.grid = grid; q.myCall = "LY1ABC"; q.myGrid = "KO24";
    return q;
}

int main() {
    // dates
    CHECK(adifDate(0) == "19700101" && adifTime(0) == "000000");
    CHECK(adifUtc("20261022", "123456") == 1792672496.0);
    CHECK(adifDate(1792672496.0) == "20261022" && adifTime(1792672496.0) == "123456");
    CHECK(adifUtc("2026", "") == 0 && adifUtc("20261322", "0000") == 0);
    CHECK(adifUtc("20000229", "2359") == 951868740.0);   // leap day
    CHECK(baseCall("DL/LY1ABC/P") == "LY1ABC" && baseCall("k1abc") == "K1ABC" && baseCall("LY1ABC/MM") == "LY1ABC");
    CHECK(modeClass("FT8") == "DIGI" && modeClass("USB") == "PHONE" && modeClass("CW") == "CW");

    // add, edit, delete, undo, redo
    Logbook b;
    const double t0 = 1792672496.0;
    const long long a = b.add(mk("k1abc", 14.074, "ft8", t0, "fn42"));
    const long long c = b.add(mk("JA1XYZ", 7.074, "FT8", t0 + 600, "PM95"));
    CHECK(b.size() == 2 && b.find(a)->call == "K1ABC" && b.find(a)->band == "20m" && b.find(a)->grid == "FN42" && b.find(a)->mode == "FT8");
    CHECK(b.find(c)->band == "40m" && a != c);
    LogQso e = *b.find(a);
    e.rstSent = "-10"; e.name = "John";
    CHECK(b.update(e) && b.find(a)->name == "John");
    CHECK(b.undoLabel() == "Undo edit K1ABC");
    CHECK(b.undo() && b.find(a)->name.empty());
    CHECK(b.redo() && b.find(a)->name == "John");
    CHECK(b.remove(a) && b.size() == 1 && !b.find(a));
    CHECK(b.undo() && b.size() == 2 && b.find(a) && b.indexOf(a) == 0);   // back at its place
    CHECK(b.undo() && b.find(a)->name.empty());                           // the edit
    CHECK(b.undo() && b.size() == 1);                                     // the second add
    CHECK(b.redo() && b.size() == 2 && b.redo() && b.find(a)->name == "John" && b.redo() && b.size() == 1);
    CHECK(!b.redo());
    CHECK(b.undo() && b.size() == 2);
    LogQso bad; bad.id = 999;
    CHECK(!b.update(bad) && !b.remove(999));
    // a new action after undo drops the redo history
    b.add(mk("G4ABC", 21.074, "FT8", t0 + 1200));
    CHECK(!b.canRedo());

    // dupes: call (portable suffix ignored), band, mode; time window
    CHECK(b.findDupe("K1ABC/P", "20m", "FT8") >= 0);
    CHECK(b.findDupe("K1ABC", "40m", "FT8") < 0);
    CHECK(b.findDupe("K1ABC", "40m", "FT8", false) >= 0);                 // not per band
    CHECK(b.findDupe("K1ABC", "20m", "FT4") < 0);                         // another digital mode is another mode
    CHECK(b.findDupe("K1ABC", "20m", "FT4", true, false) >= 0);
    CHECK(b.findDupe("K1ABC", "20m", "FT8", true, true, t0 + 30, 60) >= 0 && b.findDupe("K1ABC", "20m", "FT8", true, true, t0 + 3600, 60) < 0);
    CHECK(b.findDupe("K1ABC", "20m", "FT8", true, true, 0, 0, a) < 0);    // the QSO being edited is not its own dupe
    b.add(mk("W1AW", 14.2, "USB", t0 + 2000));
    CHECK(b.findDupe("W1AW", "20m", "SSB") >= 0);                         // SSB = USB

    // search
    CHECK(b.search("k1abc").size() == 1 && b.search("JOHN").size() == 1);
    CHECK(b.search("band:40m").size() == 1 && b.search("mode:FT8").size() == 3 && b.search("ft8 20m").size() == 1);
    CHECK(b.search("20261022").size() == b.size() && b.search("").size() == b.size());
    CHECK(b.search("nobody").empty() && b.search("band:4").empty());
    CHECK(b.search("").front() == b.size() - 1);                          // newest first

    // ADIF round trip of every field, unknown fields kept
    LogQso f = mk("VK2ABC", 14.0745, "FT8", t0, "QF56OD");
    f.submode = ""; f.endUtc = t0 + 90; f.rstSent = "-12"; f.rstRcvd = "+03"; f.name = "Bob"; f.qth = "Sydney"; f.comment = "first VK, 73";
    f.state = "NSW"; f.country = "Australia"; f.cont = "OC"; f.txPwr = "50"; f.dxcc = 150; f.cqz = 30; f.ituz = 59;
    f.qslSent = true; f.qslRcvd = false; f.lotwRcvd = true; f.stx = "001"; f.srx = "123"; f.contestId = "CQ-WPX-RTTY";
    f.extra["APP_OTHER_X"] = "<odd:3> text"; f.extra["SIG"] = "POTA";
    const std::string rec = qsoToAdif(f);
    const auto recs = adifReadRecords(adifHeader() + rec);
    CHECK(recs.size() == 1);
    if (recs.size() == 1) {
        const LogQso g = adifToQso(recs[0]);
        CHECK(g.call == f.call && g.grid == "QF56OD" && g.mode == "FT8" && g.band == "20m" && std::abs(g.freqHz - f.freqHz) < 1);
        CHECK(g.startUtc == f.startUtc && g.endUtc == f.endUtc && g.rstSent == "-12" && g.rstRcvd == "+03");
        CHECK(g.name == "Bob" && g.qth == "Sydney" && g.comment == "first VK, 73" && g.state == "NSW" && g.country == "Australia" && g.cont == "OC");
        CHECK(g.myCall == "LY1ABC" && g.myGrid == "KO24" && g.txPwr == "50" && g.dxcc == 150 && g.cqz == 30 && g.ituz == 59);
        CHECK(g.qslSent && !g.qslRcvd && g.lotwRcvd && g.confirmed() && g.stx == "001" && g.srx == "123" && g.contestId == "CQ-WPX-RTTY");
        CHECK(g.extra.size() == 2 && g.extra.at("APP_OTHER_X") == "<odd:3> text" && g.extra.at("SIG") == "POTA");
        CHECK(qsoToAdif(g) == rec);                                       // byte for byte the second time
    }

    // another program's file: a header with text, lower case tags, a type letter, a missing <EOR> at the end, a midnight crossing
    const std::string other =
        "Exported by SomeLogger\n<ADIF_VER:5>3.1.4 <PROGRAMID:4>Test\n<EOH>\n"
        "<call:5>DL1AA <qso_date:8:d>20261021 <time_on:4>2358 <time_off:4>0002 <band:3>40M <mode:3>SSB <freq:5>7.150 <qsl_rcvd:1>Y\n<eor>\n"
        "<CALL:5>K1ABC <QSO_DATE:8>20261022 <TIME_ON:6>123456 <BAND:3>20m <MODE:3>FT8 <EOR>\n"   // the dupe of the first QSO
        "<CALL:2>XX <QSO_DATE:8>20261022 <EOR>\n"                                                 // too short a call
        "<CALL:6>OH2ABC <QSO_DATE:8>20261022 <TIME_ON:4>1300 <FREQ:6>50.313 <MODE:3>FT8";
    const size_t before = b.size();
    const Logbook::ImportResult r = b.importAdif(other);
    CHECK(r.added == 2 && r.dupes == 1 && r.bad == 1 && b.size() == before + 2);
    if (r.added != 2 || r.dupes != 1 || r.bad != 1) printf("  import: added %d dupes %d bad %d\n", r.added, r.dupes, r.bad);
    const auto dl = b.search("call:DL1AA");
    CHECK(dl.size() == 1);
    if (dl.size() == 1) {
        const LogQso& q = b.qsos()[dl[0]];
        CHECK(q.band == "40m" && q.mode == "SSB" && q.qslRcvd && q.endUtc - q.startUtc == 240);
    }
    CHECK(b.search("call:OH2ABC").size() == 1 && b.qsos()[b.search("call:OH2ABC")[0]].band == "6m");
    CHECK(b.undo() && b.size() == before);                                // an import is one undo step
    CHECK(b.redo() && b.size() == before + 2);

    // enrich fills what the prefix database knows
    Logbook en;
    en.enrich = [](LogQso& q) { if (q.dxcc == 0 && q.call.rfind("LY", 0) == 0) { q.dxcc = 146; q.country = "Lithuania"; q.cqz = 15; } };
    en.add(mk("LY2XX", 3.573, "FT8", t0));
    CHECK(en.qsos()[0].dxcc == 146 && en.qsos()[0].cqz == 15);

    // save and load
    const std::string path = "test_ham_logbook.adi";
    std::string err;
    CHECK(b.save(path, err));
    Logbook l2;
    CHECK(l2.load(path, err) && l2.size() == b.size());
    for (size_t i = 0; i < b.size() && i < l2.size(); i++) {
        LogQso x = b.qsos()[i], y = l2.qsos()[i];
        x.id = y.id = 0;
        CHECK(qsoToAdif(x) == qsoToAdif(y));
    }
    CHECK(l2.exportAdif() == b.exportAdif());
    std::remove(path.c_str());
    Logbook none;
    CHECK(none.load("no_such_dir_xyz/none.adi", err) && none.size() == 0);
    CHECK(!b.save("no_such_dir_xyz/x.adi", err) && !err.empty());

    // a selection export
    const std::string sel = b.exportAdif(b.search("band:40m"));
    CHECK(adifReadRecords(sel).size() == 2);

    if (fails) { printf("%d failures\n", fails); return 1; }
    printf("ham logbook: all passed\n");
    return 0;
}
