// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The Credits table (app/panels/credits_data.h, generated from tools/credits_data.tsv): no empty field, every link is https, no duplicates.
#include "panels/credits_data.h"
#include "panels/contributors_data.h"
#include <fstream>
#include <regex>
#include <sstream>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>

int main() {
    int fails = 0;
    std::set<std::string> seen;
    if (kCreditCount < 30) { std::printf("FAIL: only %d entries\n", kCreditCount); fails++; }
    for (int i = 0; i < kCreditCount; i++) {
        const CreditEntry& c = kCredits[i];
        const char* f[7] = {c.name, c.author, c.licence, c.where, c.how, c.url, c.note};
        const char* n[7] = {"name", "author", "licence", "where", "how", "url", "note"};
        for (int k = 0; k < 7; k++)
            if (!f[k] || !f[k][0]) { std::printf("FAIL: entry %d (%s): empty %s\n", i, c.name ? c.name : "?", n[k]); fails++; }
        if (c.url && std::strncmp(c.url, "https://", 8) != 0) { std::printf("FAIL: entry %d: link is not https\n", i); fails++; }
        if (c.name && !seen.insert(c.name).second) { std::printf("FAIL: duplicate %s\n", c.name); fails++; }
    }
    // the people table: no empty field, valid references, modes that exist in the mode table, no duplicate rows
    std::set<std::string> modes;
    {
        std::ifstream in(std::string(DECT2_SOURCE_DIR) + "/app/ui/mode_table.cpp");
        std::stringstream ss; ss << in.rdbuf();
        const std::string src = ss.str();
        const std::regex re("\n\\s*\\{\\d+, \"([^\"]+)\"");
        for (std::sregex_iterator it(src.begin(), src.end(), re), e; it != e; ++it) modes.insert((*it)[1]);
        if (modes.size() < 100) { std::printf("FAIL: read only %zu modes from mode_table.cpp\n", modes.size()); fails++; }
    }
    const std::set<std::string> kinds = {"pull request", "bug report", "idea", "testing", "samples", "translation", "optimisation", "fix"};
    const std::regex refRe("(#[0-9]+|commit:[0-9a-f]{7,40}|discord|email)");
    std::set<std::string> rowsSeen;
    if (kContribCount < 50) { std::printf("FAIL: only %d contributions\n", kContribCount); fails++; }
    for (int i = 0; i < kContribCount; i++) {
        const ContribEntry& c = kContribs[i];
        const char* f[6] = {c.who, c.link, c.kind, c.what, c.modes, c.refs};
        for (int k = 0; k < 6; k++) if (!f[k] || !f[k][0]) { std::printf("FAIL: contribution %d (%s): empty field %d\n", i, c.who ? c.who : "?", k); fails++; }
        if (std::strcmp(c.link, "-") && std::strncmp(c.link, "https://", 8)) { std::printf("FAIL: %s: link not https\n", c.who); fails++; }
        if (!kinds.count(c.kind)) { std::printf("FAIL: %s: unknown kind %s\n", c.who, c.kind); fails++; }
        if (!rowsSeen.insert(std::string(c.who) + "|" + c.kind + "|" + c.what).second) { std::printf("FAIL: duplicate row %s: %s\n", c.who, c.what); fails++; }
        std::stringstream ms(c.modes); std::string m;
        while (std::getline(ms, m, ';')) {
            m.erase(0, m.find_first_not_of(' '));
            if (m.compare(0, 5, "area:") == 0 ? m.size() == 5 : !modes.count(m)) { std::printf("FAIL: %s: unknown mode '%s'\n", c.who, m.c_str()); fails++; }
        }
        std::stringstream rs(c.refs); std::string r;
        while (std::getline(rs, r, ',')) {
            r.erase(0, r.find_first_not_of(' '));
            if (!std::regex_match(r, refRe)) { std::printf("FAIL: %s: bad reference '%s'\n", c.who, r.c_str()); fails++; }
        }
    }
    if (!fails) std::printf("credits: %d entries, %d contributions ok\n", kCreditCount, kContribCount);
    return fails ? 1 : 0;
}
