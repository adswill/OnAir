// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// DAB transmitter lists (app/dab/dab_txlist.h): reading a line, writing one, importing a list into the own one (#249)
#include "../../app/dab/dab_txlist.h"
#include <cstdio>
static int fails = 0;
static void check(bool ok, const char* w) { if (!ok) { printf("FAIL: %s\n", w); fails++; } }
int main() {
    using namespace dabtx;
    Row r;
    check(parse("", r) == Line::Skip && parse("# a comment", r) == Line::Skip && parse("eid,main,sub,lat,lon,site,power_kw,channel_mhz", r) == Line::Skip, "comments and the header");
    check(parse("\xEF\xBB\xBF" "eid,main,sub,lat,lon", r) == Line::Skip, "a header behind a byte order mark");
    check(parse("10BC,1,3,47.70361,12.01194,Wendelstein,10,178.352", r) == Line::Ok, "a full line");
    check(r.eid == 0x10BC && r.mainId == 1 && r.subId == 3 && r.site == "Wendelstein" && r.powerKw == 10 && r.channelMhz == 178.352, "its fields");
    check(parse("10bc\t1\t3\t47.7\t12.0\tWendelstein", r) == Line::Ok && r.eid == 0x10BC, "a TSV, lower-case hex");
    check(parse("10BC,1,3,\"48.1,\",11.5", r) == Line::Bad, "a latitude with junk");
    check(parse("10BC,1,3,,,,,178.352,DR Deutschland,12.5", r) == Line::NoPosition && r.channelMhz == 178.352, "an exported row without a position");
    check(parse("10BC,1,3,48.1,,x", r) == Line::NoPosition, "only a latitude: no position (not placed at longitude 0)");
    check(parse("10BC,70,3,48,11", r) == Line::Bad && parse("10BC,1,24,48,11", r) == Line::Bad, "TII ids out of range");
    check(parse("XYZ,1,3,48,11", r) == Line::Bad && parse("1FFFF,1,3,48,11", r) == Line::Bad, "a bad ensemble id");
    check(parse("10BC,1,3,91,11", r) == Line::Bad, "a latitude beyond the pole");
    check(parse("10BC,1,3", r) == Line::Bad, "too few columns");

    Row w; w.eid = 0x10BC; w.mainId = 1; w.subId = 3; w.lat = 47.70361; w.lon = 12.01194; w.site = "Wendelstein, \"Sender\""; w.channelMhz = 178.352;
    const std::string line = format(w);
    Row back;
    check(parse(line, back) == Line::Ok && back.site == "Wendelstein, Sender" && back.eid == 0x10BC && back.subId == 3 && std::fabs(back.lat - 47.70361) < 1e-6, "a written line reads back");

    const std::string own = "# my list\neid,main,sub,lat,lon,site,power_kw,channel_mhz\n10BC,1,3,47.0,12.0,old place,,\n1001,5,5,50.0,8.0,Feldberg,,\n";
    const std::string imp = "eid,main,sub,lat,lon,site,power_kw,channel_mhz,ensemble,level_db\n"
                            "10BC,1,3,47.70361,12.01194,Wendelstein,10,178.352,\"DR Deutschland\",12.5\n"
                            "10BC,2,7,,,\"\",,178.352,\"DR Deutschland\",4.0\n"
                            "D210,4,1,48.1,11.5,Olympiaturm,,202.928,\"BR Bayern\",20\n"
                            "garbage\n";
    MergeStats st;
    const std::string m = merge(own, imp, st);
    check(st.added == 1 && st.replaced == 1 && st.noPosition == 1 && st.bad == 1, "import counts");
    check(m.find("# my list") == 0, "the own comments stay");
    check(m.find("old place") == std::string::npos && m.find("Wendelstein") != std::string::npos, "a transmitter of both is updated");
    check(m.find("Feldberg") != std::string::npos && m.find("Olympiaturm") != std::string::npos, "the own others stay, the new one is added");
    check(m.find("10BC,2,7") == std::string::npos, "no row without a position is written");
    int ok = 0;
    {
        size_t p = 0;
        while (p < m.size()) { const size_t e = m.find('\n', p); Row x; if (parse(m.substr(p, e - p), x) == Line::Ok) ok++; p = e == std::string::npos ? m.size() : e + 1; }
    }
    check(ok == 3, "the merged list reads back with three transmitters");
    const std::string fresh = merge("", imp, st);
    check(fresh.find("eid,main,sub") == 0 && st.added == 2, "into no own file yet: a header and the transmitters");

    // ---- qt-dab's list (made-up rows: the ids and places are test values, not real transmitters)
    {
        const std::string text =
            "1;DE;5C;Test Mux;10BC;1903;Testberg;52.50000;13.40000;100;150;H;178.352;10;ND;\n"   // 19/03
            "2;DE;5C;Test Mux;10bc;2007;Testhausen;51.00000;10.00000;0;0;V;178352;5;ND;\n"     // 20/07, a frequency in kHz: not taken as MHz
            "3;DE;5C;Test Mux;10BC;2000;No sub;51.0;10.0;0;0;H;178.352;1;ND;\n"               // SubId 0: skipped (as qt-dab does)
            "4;DE;5C;;10BC;2006;No ensemble;51.0;10.0;0;0;H;178.352;1;ND;\n"                  // no ensemble: skipped
            "5;DE;5C;Test Mux;10BC;2015;No place;;;0;0;H;178.352;1;ND;\n"                     // no position: skipped
            "6;DE;5C;Test Mux;10BC;7003;Bad main;51.0;10.0;0;0;H;178.352;1;ND;\n"             // MainId 70: skipped
            "7;DE;5C;Test Mux;10BC;2014\n";                                                   // too few fields
        std::vector<Row> rows;
        QtDabStats qs;
        check(readQtDab(text, rows, qs) && !qs.scrambled && qs.rows == 2 && qs.skipped == 5, "qt-dab plain text: 2 rows, 5 skipped");
        check(rows.size() == 2 && rows[0].eid == 0x10BC && rows[0].mainId == 19 && rows[0].subId == 3 && rows[0].site == "Testberg" &&
                  std::fabs(rows[0].lat - 52.5) < 1e-9 && std::fabs(rows[0].lon - 13.4) < 1e-9 && std::fabs(rows[0].channelMhz - 178.352) < 1e-9, "qt-dab row fields");
        check(rows.size() == 2 && rows[1].mainId == 20 && rows[1].subId == 7 && rows[1].channelMhz == 0, "lower-case EId; a kHz frequency is not a MHz one");
        for (const unsigned char k : {(unsigned char)0x07, (unsigned char)0xAA, (unsigned char)0x30}) {   // the scrambled file: a key byte, then text minus the key (XOR 0xAA for 0xAA)
            std::string sc(1, (char)k);
            for (const char c : text) sc += c == '\n' ? '\n' : (char)(k == 0xAA ? ((unsigned char)c ^ 0xAA) : (unsigned char)((unsigned char)c + k));
            std::vector<Row> r2;
            QtDabStats s2;
            const bool ok2 = readQtDab(sc, r2, s2);
            // (keys for which no character of the text scrambles to a newline, as qt-dab's own files)
            check(ok2 && s2.scrambled && r2.size() == 2 && r2[0].mainId == 19 && r2[0].subId == 3 && r2[1].site == "Testhausen", "qt-dab scrambled list");
        }
        std::vector<Row> none;
        check(!readQtDab("eid,main,sub,lat,lon\n10BC,1,3,47.7,12.0\n", none, qs) && none.empty(), "an OnAir CSV is not a qt-dab list");
        check(!readQtDab("", none, qs) && !readQtDab("x", none, qs), "empty and one-byte files");
    }

    // ---- matching: (EId, MainId, SubId); the lists before the ensemble's own FIG 0/22 data; a main identifier's point only as the last resort
    {
        std::map<uint64_t, Site> lists, air;
        Site a; a.lat = 1; a.src = Src::Shared; lists[key(0x10BC, 19, 3)] = a;
        Site b; b.lat = 2; b.src = Src::Air; air[key(0x10BC, 19, 3)] = b;
        Site c; c.lat = 3; c.src = Src::Air; air[key(0x10BC, 20, 7)] = c;
        Site d; d.lat = 4; d.src = Src::AirMain; d.mainRef = 20; air[key(0x10BC, 20, 0)] = d;
        const Site* p = resolve(lists, air, 0x10BC, 19, 3);
        check(p && p->lat == 1 && p->src == Src::Shared, "a list beats the broadcast position");
        p = resolve(lists, air, 0x10BC, 20, 7);
        check(p && p->lat == 3 && p->src == Src::Air, "the sub identifier's broadcast position");
        p = resolve(lists, air, 0x10BC, 20, 15);
        check(p && p->src == Src::AirMain && p->mainRef == 20, "only the main identifier's reference: approximate");
        check(!resolve(lists, air, 0x10BD, 19, 3), "another ensemble with the same codes: not matched");
        check(!resolve(lists, air, 0x10BC, 21, 3), "an unknown MainId: nothing");
        check(key(0x10BC, 19, 3) != key(0x10BC, 3, 19) && key(0xFFFF, 69, 23) != key(0xFFFE, 69, 23), "keys keep the three ids apart");
    }
    if (!fails) printf("dab transmitter lists: ok\n");
    return fails ? 1 : 0;
}
