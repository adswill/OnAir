// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// SAME message parsing and tables: fields, the Partial County Alerting digit, state names, validation, the optional county table, the bit vote.
#include "dect2/same/same_proto.h"
#include "dect2/same/same_rx.h"
#include <cstdio>
#include <string>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    // a whole message with PCA digits, several locations
    {
        const same::Message m = same::parse("ZCZC-WXR-TOR-048201-348113-029047+0030-1301530-KHGX/NWS-");
        CHECK(m.valid, "valid: %s", m.err.c_str());
        CHECK(m.org == "WXR" && m.orgName == "National Weather Service", "org %s", m.orgName.c_str());
        CHECK(m.event == "TOR" && m.eventName == "Tornado Warning" && m.eventKind == "warning", "event %s", m.eventName.c_str());
        CHECK(m.locs.size() == 3, "%zu locations", m.locs.size());
        if (m.locs.size() == 3) {
            CHECK(m.locs[0].pca == 0 && m.locs[0].state == 48 && m.locs[0].county == 201 && m.locs[0].stateName == "Texas", "loc 0");
            CHECK(m.locs[1].pca == 3 && m.locs[1].pcaName == "northeast" && m.locs[1].state == 48 && m.locs[1].county == 113, "loc 1: pca %d %s", m.locs[1].pca, m.locs[1].pcaName.c_str());
            CHECK(m.locs[1].text.find("Northeast part of county FIPS 113, Texas") == 0, "text '%s'", m.locs[1].text.c_str());
            CHECK(m.locs[2].stateName == "Missouri" && m.locs[2].county == 47, "loc 2");
            CHECK(m.locs[0].hasPos && m.locs[0].lat > 25 && m.locs[0].lat < 37, "centre");
        }
        CHECK(m.purgeMin == 30 && m.issueDay == 130 && m.issueHour == 15 && m.issueMin == 30, "times %d %d %d %d", m.purgeMin, m.issueDay, m.issueHour, m.issueMin);
        CHECK(m.callsign == "KHGX/NWS", "callsign");
    }
    // PCA digits 1 - 9 and the state-wide code
    {
        const char* want[10] = {"", "northwest", "north", "northeast", "west", "central", "east", "southwest", "south", "southeast"};
        for (int d = 0; d <= 9; d++) {
            char b[64]; snprintf(b, sizeof b, "ZCZC-CIV-CEM-%d06037+0100-0011200-KABC/NWS-", d);
            const same::Message m = same::parse(b);
            CHECK(m.valid && m.locs.size() == 1 && m.locs[0].pca == d && m.locs[0].pcaName == want[d], "pca %d: %s", d, m.err.c_str());
            CHECK(m.purgeMin == 60, "purge");
        }
        const same::Message m = same::parse("ZCZC-WXR-SVR-048000+0045-0011200-KABC/NWS-");
        CHECK(m.valid && m.locs[0].county == 0 && m.locs[0].text == "All of Texas", "state wide '%s'", m.valid ? m.locs[0].text.c_str() : m.err.c_str());
        CHECK(m.purgeMin == 45, "purge 45");
        CHECK(same::purgeText(90) == "1 h 30 min" && same::purgeText(45) == "45 min" && same::purgeText(120) == "2 h", "purge text");
    }
    // 31 locations is the most; 32 is refused
    {
        std::string t = "ZCZC-EAS-RWT";
        for (int i = 0; i < 31; i++) t += "-012001";
        CHECK(same::parse(t + "+0015-0011200-KABC/NWS-").valid, "31 locations");
        CHECK(!same::parse(t + "-012001+0015-0011200-KABC/NWS-").valid, "32 locations");
    }
    // tables
    {
        CHECK(std::string(same::stateName(6)) == "California" && std::string(same::stateName(72)) == "Puerto Rico" && std::string(same::stateName(66)) == "Guam", "states");
        CHECK(std::string(same::stateName(3)) == "" && std::string(same::stateName(35, same::CountryCanada)) == "Ontario" && std::string(same::stateName(35)) == "New Mexico", "other tables");
        CHECK(std::string(same::eventName("EAN")) != "" && std::string(same::eventName("CAE")) == "Child Abduction Emergency" && std::string(same::eventName("RMT")) == "Required Monthly Test", "events");
        CHECK(std::string(same::eventName("ZZZ")) == "", "unknown event");
        const char* all[] = {"TOR", "SVR", "FFW", "EAN", "RWT", "RMT", "CAE", "SVS", "SPS", "TOA", "SVA", "WSW", "BZW", "HUW", "TSW", "EQW", "NPT", "NIC", "DMO", "SPW", "LEW", "EVI", "SSW", "EWW", "BLU", "AVW", "VOW", "SMW"};
        for (const char* c : all) CHECK(std::string(same::eventName(c)) != "", "event %s", c);
        double la, lo;
        CHECK(same::stateCentre(48, same::CountryUs, la, lo) && la > 25 && la < 37 && lo < -93 && lo > -107, "Texas centre");
        CHECK(!same::stateCentre(57, same::CountryUs, la, lo), "a marine area has no centre");
        CHECK(same::issueText(2026, 130, 15, 30) == "May 10 15:30 UTC" && same::issueText(2024, 60, 0, 5) == "Feb 29 00:05 UTC" && same::issueText(2026, 366, 0, 0).empty(), "issue text '%s'", same::issueText(2026, 130, 15, 30).c_str());
        CHECK(same::issueEpoch(1970, 1, 0, 0) == 0 || same::issueEpoch(1970, 1, 0, 0) == 0, "epoch");
        CHECK(same::issueEpoch(2026, 1, 1, 0) - same::issueEpoch(2025, 365, 1, 0) == 86400, "year change");
    }
    // validation
    {
        const char* bad[] = {
            "ZCZC-WXR-TOR-048201+0030-1301530-KHGX/NWS",          // no closing dash
            "ZCZC-WXR-TOR-04820+0030-1301530-KHGX/NWS-",          // short location
            "ZCZC-WXR-TOR-048201+0090-1301530-KHGX/NWS-",         // minutes over 59
            "ZCZC-WXR-TOR-048201+0030-4001530-KHGX/NWS-",         // day 400
            "ZCZC-WXR-TOR-048201+0030-1302530-KHGX/NWS-",         // hour 25
            "ZCZC-WXR-TOR-048201+0030-1301570-KHGX/NWS-",         // minute 70
            "ZCZC-WXR-TOR-048201+0000-1301530-KHGX/NWS-",         // zero purge
            "ZCZC-WXR-TOR-048201+0030-1301530-khgx/nws-",         // lower case call sign
            "ZCZC-WX1-TOR-048201+0030-1301530-KHGX/NWS-",         // digit in the originator
            "ZCZC-WXR-TOR+0030-1301530-KHGX/NWS-",                // no location
            "ZCZC-WXR-TOR-048201+0030-1301530-KHGX/NWSXX-",       // call sign too long
            "ZCXC-WXR-TOR-048201+0030-1301530-KHGX/NWS-",         // header damaged
        };
        for (const char* b : bad) CHECK(!same::parse(b).valid, "accepted: %s", b);
        const same::Message e = same::parse("NNNN");
        CHECK(e.valid && e.eom, "end of message");
        CHECK(same::validChar('A') && same::validChar('-') && same::validChar('+') && !same::validChar(0xAB) && !same::validChar(0) && !same::validChar('a'), "chars");
        CHECK(same::trimMessage("\x01xZCZC-WXR-RWT-029047+0015-1301545-KEAX/NWS-\x12\x34zz") == "ZCZC-WXR-RWT-029047+0015-1301545-KEAX/NWS-", "trim");
        CHECK(same::trimMessage("NNNNxy") == "NNNN" && same::trimMessage("hello").empty(), "trim 2");
    }
    // the county table is optional and never invented
    {
        CHECK(same::countyCount() == 0, "no counties built in");
        const same::Location a = same::locate("048201", same::CountryUs);
        CHECK(a.countyName.empty() && a.text == "County FIPS 201, Texas", "no table: '%s'", a.text.c_str());
        std::string err;
        CHECK(same::loadCountyCsv("hello\nworld\n", &err) == 0 && !err.empty(), "junk refused");
        CHECK(same::countyCount() == 0, "still empty");
        // the three formats, with a header line and quoted names
        const size_t n = same::loadCountyCsv("fips,name\n99001,\"Testville, Co\"\n99,002,Sample County\nXX,99,003,Census County,H1\n99000,State line\n", &err);
        CHECK(n == 3, "%zu counties (%s)", n, err.c_str());
        CHECK(same::countyNameOf(99, 1) == "Testville, Co" && same::countyNameOf(99, 2) == "Sample County" && same::countyNameOf(99, 3) == "Census County", "names");
        const same::Location b = same::locate("599001", same::CountryUs);
        CHECK(b.countyName == "Testville, Co" && b.pca == 5 && b.text.find("Central part of Testville, Co") == 0, "with table: '%s'", b.text.c_str());
        same::clearCounties();
        CHECK(same::countyCount() == 0 && same::locate("599001", same::CountryUs).countyName.empty(), "cleared");
    }
    // the vote
    {
        const std::string good = "ZCZC-WXR-RWT-029047+0015-1301545-KEAX/NWS-";
        std::string b1 = good, b2 = good, b3 = good;
        b1[10] ^= 0x04; b1[20] ^= 0x01;          // two bit errors in one repeat
        b2[30] ^= 0x10;                          // one in another
        int fixed = 0, agree = 0;
        CHECK(same::vote({b1, b2, b3}, &fixed, &agree) == good && fixed == 3 && agree == 1, "3-way vote: fixed %d agree %d", fixed, agree);
        CHECK(same::vote({good, good, b1}) == good, "2 of 3");
        std::string shorter = good.substr(0, good.size() - 1);
        CHECK(same::vote({good, shorter, good}, &fixed, &agree) == good && agree == 2, "a repeat of another length is left out");
        CHECK(same::vote({good, shorter}).empty(), "two lengths: no vote");
        // two repeats that differ: the character the format allows wins
        std::string c = good; c[12] = (char)0xC3;
        CHECK(same::vote({c, good}) == good, "two repeats, one damaged");
        CHECK(same::vote({good}).empty(), "one repeat is not a vote");
    }
    printf(fails ? "same proto: %d FAILED\n" : "same proto: all passed\n", fails);
    return fails ? 1 : 0;
}
