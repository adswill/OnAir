// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Golden-vector test of the Rust SAME layer (rust/onair_same, dect2::same): built and known messages, the tables over every code, the time helpers over their
// whole range, county tables, and ONAIR_DIFF_ITERS (default 200000) seeded random / mutated / truncated inputs per family. Every result goes into a digest
// (tests/common/golden.h) compared with the one recorded from the C++ SAME layer the Rust code replaced.
#include "dect2/same/same_proto.h"
#include "../common/golden.h"
#include "../common/ordered.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
namespace A = dect2::same;

static golden::Digest dig;

static void addLoc(const A::Location& a) {
    dig.add(a.code, a.pca, a.state, a.county, a.pcaName, a.stateName);
    dig.add(a.countyName, a.text, a.hasPos, a.lat, a.lon);
}
static void addMsg(const A::Message& a) {
    dig.add(a.valid, a.eom, a.raw, a.err, a.org, a.orgName, a.event, a.eventName, a.eventKind, a.purgeMin, a.issueDay, a.issueHour, a.issueMin, a.callsign);
    dig.add((uint64_t)a.locs.size());
    for (const auto& l : a.locs) addLoc(l);
}

static void compareMessage(const std::string& s) {
    for (int c = 0; c < 2; c++) addMsg(A::parse(s, (A::Country)c));
    dig.add(A::trimMessage(s));
}

static std::mt19937 g(12345);
static std::string rnd(const char* alphabet, size_t n) { std::string s; for (size_t i = 0; i < n; i++) s += alphabet[g() % strlen(alphabet)]; return s; }

static std::string validMessage() {
    static const char* orgs[] = {"WXR", "EAS", "PEP", "CIV", "ABC"};
    static const char* events[] = {"TOR", "SVR", "RWT", "EAN", "FFW", "ZZW", "QQA", "BLU", "HLS", "XYZ"};
    std::string s = ORD_CAT("ZCZC-", orgs[g() % 5], "-", events[g() % 10]);   // ORD_*: draws in a fixed order (operands and arguments are not evaluated in one)
    const int n = 1 + (int)(g() % 5);
    for (int i = 0; i < n; i++) {
        char b[8];
        const int d1 = (int)(g() % 10), d2 = (int)(g() % 100), d3 = (int)(g() % 1000);
        snprintf(b, sizeof b, "%d%02d%03d", d1, d2, d3);
        s += std::string("-") + b;
    }
    char t[48];
    const int t1 = (int)(g() % 12), t2 = (int)(g() % 60), t3 = 1 + (int)(g() % 366), t4 = (int)(g() % 24), t5 = (int)(g() % 60);
    snprintf(t, sizeof t, "+%02d%02d-%03d%02d%02d-", t1, t2, t3, t4, t5);
    s += t;
    s += rnd("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789/ ", 1 + g() % 8) + "-";
    return s;
}

static std::string mutate(std::string s) {
    const int k = 1 + (int)(g() % 4);
    for (int i = 0; i < k && !s.empty(); i++) {
        switch (g() % 6) {
        case 0: s[g() % s.size()] = (char)(g() & 0xFF); break;
        case 1: s[g() % s.size()] = "0123456789+-/ ZCNAW"[g() % 19]; break;
        case 2: ORD_CALL(s.erase, g() % s.size(), 1 + g() % 3); break;
        case 3: ORD_CALL(s.insert, g() % (s.size() + 1), rnd("0123456789-+ ZN", 1 + g() % 3)); break;
        case 4: s.resize(g() % (s.size() + 1)); break;
        default: s = rnd("-+0123456789ZCNAWXYR/ ", g() % 80); break;
        }
    }
    if (g() % 4 == 0) s = ORD_CAT(rnd("\x01\x7f\xff-ab", g() % 5), s, rnd("\x01\xff-ab+", g() % 5));
    return s;
}

static void compareCounties(const std::string& csv) {
    std::string e1;
    const size_t n1 = A::loadCountyCsv(csv, &e1);
    dig.add(n1, e1);
    dig.add(A::countyCount());
    for (int i = 0; i < 40; i++) {
        const int st = (int)(g() % 100), co = (int)(g() % 1000);
        dig.add(A::countyNameOf(st, co));
    }
}

int main() {
    const char* it = getenv("ONAIR_DIFF_ITERS");
    const long iters = it ? atol(it) : 200000;

    // tables: every three letter code, every digit
    for (int a = 'A'; a <= 'Z'; a++) for (int b = 'A'; b <= 'Z'; b++) for (int c = 'A'; c <= 'Z'; c++) {
        const std::string s = {(char)a, (char)b, (char)c};
        dig.add(A::originatorName(s));
        dig.add(A::eventName(s));
    }
    for (const char* s : {"", "T", "TO", "TORR", "tor", "WXR ", "PEP", "EAN", "NIC"}) {
        dig.add(A::originatorName(s));
        dig.add(A::eventName(s));
    }
    for (int d = -3; d < 14; d++) dig.add(A::pcaName(d));
    for (int c = 0; c < 2; c++) for (int f = -3; f < 130; f++) {
        dig.add(A::stateName(f, (A::Country)c));
        dig.add(A::stateAbbr(f, (A::Country)c));
        double l1 = 1, o1 = 2;
        const bool r1 = A::stateCentre(f, (A::Country)c, l1, o1);
        dig.add(r1, l1, o1);
    }
    for (int c = 0; c < 256; c++) dig.add(A::validChar((uint8_t)c));
    for (int d = 0; d < 1000; d++) {   // the event kind of unknown codes comes through parse
        char b[64]; snprintf(b, sizeof b, "ZCZC-WXR-%c%c%c-029001+0030-1231415-KEAX/NWS-", 'A' + d % 26, 'A' + (d / 26) % 26, "WAESTX"[d % 6]);
        compareMessage(b);
    }

    // known messages
    for (const char* s : {"ZCZC-WXR-TOR-029001-029003+0030-1231415-KEAX/NWS-", "NNNN", "ZCZC-EAS-RWT-048201+0015-3661200-WABC/FM -", "ZCZC-PEP-EAN-099000+0100-0010000-PEP/1   -",
                          "ZCZC-WXR-SVR-129201+0045-2051620-KWNS/NWS-", "xxZCZC-WXR-TOR-029001+0030-1231415-KEAX/NWS-yy\xff", "garbage NNNN more", "ZCZC-WXR-TOR-029001+0030-1231415-KEAX/NWS", ""})
        compareMessage(s);

    // county tables (own copy per implementation), then locations with and without names
    compareCounties("SSCCC,Name\n48201,Harris County\n48113,\"Dallas, TX\"\n01,001,Autauga\nAL,01,003,Baldwin County,H1\n129201,Pca County\n0,0,Zero\n");
    for (long i = 0; i < iters / 20; i++) {
        std::string csv;
        const int lines = (int)(g() % 12);
        for (int k = 0; k < lines; k++) {
            switch (g() % 7) {
            case 0: csv += ORD_CAT(rnd("0123456789", 5), ",", rnd("abcXYZ \"", g() % 12)); break;
            case 1: csv += ORD_CAT(rnd("0123456789", 2), ",", rnd("0123456789", 3), ",", rnd("abc ", g() % 8)); break;
            case 2: csv += ORD_CAT(rnd("ABab", 2), ",", rnd("0123456789", 2), ",", rnd("0123456789", 3), ",\"", rnd("ab, ", g() % 9), "\",H1"); break;
            case 3: csv += ORD_CAT(rnd("0123456789", 6), ",", rnd("xyz", g() % 5)); break;
            case 4: csv += rnd("0123456789,\" \t\r", g() % 20); break;
            case 5: csv += mutate(rnd("0123456789,", 8)); break;
            default: csv += "State,County"; break;
            }
            csv += (g() % 5 ? "\n" : "\r\n");
        }
        compareCounties(csv);
    }
    compareCounties("48201,Harris County\n01001,Autauga County\n48000,All of Texas\n");   // the table the locations below use

    // locations: random codes, known and unknown states, both countries
    for (long i = 0; i < iters; i++) {
        std::string code;
        switch (g() % 5) {
        case 0: code = rnd("0123456789", 6); break;
        case 1: { char b[8]; const int d1 = (int)(g() % 10), d2 = (int)(g() % 100), d3 = (int)(g() % 3) ? 201 : (int)(g() % 1000); snprintf(b, sizeof b, "%d%02d%03d", d1, d2, d3); code = b; } break;
        case 2: { char b[8]; const int d1 = (int)(g() % 10), d2 = g() % 2 ? 48 : 1, d3 = g() % 2 ? 201 : 1; snprintf(b, sizeof b, "%d%02d%03d", d1, d2, d3); code = b; } break;
        case 3: code = rnd("0123456789", g() % 9); break;
        default: code = mutate(rnd("0123456789", 6)); break;
        }
        for (int c = 0; c < 2; c++) {
            const A::Location a = A::locate(code, (A::Country)c);
            addLoc(a);
        }
    }

    // messages: valid ones, mutations, random bytes; the county table above is loaded in both
    for (long i = 0; i < iters; i++) {
        std::string s;
        switch (g() % 8) {
        case 0: s = validMessage(); break;
        case 1: case 2: case 3: s = mutate(validMessage()); break;
        case 4: s = validMessage(); s.resize(g() % (s.size() + 1)); break;
        case 5: s = rnd("ZCNW-+0123456789/ ABCXYZ", g() % 90); break;
        case 6: s = rnd("\x01\x02\x7f\x80\xff-+ZCNA09", g() % 90); break;
        default: { std::string v = validMessage(); s = ORD_CAT(rnd("xx\xff -", g() % 4), v, rnd("-zZ\x01", g() % 4)); } break;
        }
        compareMessage(s);
    }

    // time helpers: the whole range of days and times, years around the leap rules and wild ones
    for (int year : {1969, 1970, 1971, 1999, 2000, 2023, 2024, 2025, 2100, 2400, 3000, 9999})
        for (int day = -1; day <= 368; day++) for (int hour : {-1, 0, 7, 23, 24}) for (int minute : {-1, 0, 30, 59, 60}) {
            dig.add(A::issueEpoch(year, day, hour, minute));
            dig.add(A::issueText(year, day, hour, minute));
        }
    for (long i = 0; i < iters; i++) {
        const int year = 1960 + (int)(g() % 8000), day = (int)(g() % 400) - 10, hour = (int)(g() % 30) - 3, minute = (int)(g() % 70) - 5;
        dig.add(A::issueEpoch(year, day, hour, minute));
        dig.add(A::issueText(year, day, hour, minute));
        const int mins = (int)(g() % 12000) - 100;
        dig.add(A::purgeText(mins));
    }
    for (int m : {0, 1, 59, 60, 61, 119, 120, -1, -60, -61, 1000000, 2147483647, -2147483647 - 1}) dig.add(A::purgeText(m));

    return golden::check("same_proto_rs", dig, 0x2a954da9a9eb6f6bull, !it);
}
