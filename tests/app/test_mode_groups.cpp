// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The 35 groups of the mode list (app/ui/mode_groups.h, app/ui/mode_table.cpp, MODES.md). Text checks on the sources, so it builds without the app.
//   groups_cover_every_mode_once  every row of the table sits in exactly one real group, no group is empty, the names and colours match the enum
//   old_group_names_migrate       a saved list of 4.5 group names (hidden, unfolded) becomes the new names
//   group_counts_listed           MODES.md lists every group with the number of modes the table puts in it
#include "ui/mode_groups.h"
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <vector>
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; std::printf("FAIL: " __VA_ARGS__); std::printf("\n"); } } while (0)

static std::string readSource(const char* rel) {
    std::ifstream in(std::string(DECT2_SOURCE_DIR) + "/" + rel);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}
static std::string lower(std::string s) { for (char& c : s) c = (char)std::tolower((unsigned char)c); return s; }

struct Table { std::vector<std::string> groups; std::vector<std::pair<std::string, int>> rows; int cols = 0; };
static Table load() {
    Table t;
    const std::string src = readSource("app/ui/mode_table.cpp");
    const size_t gn = src.find("kGroupNames[kNumGroups] = {");
    if (gn != std::string::npos) {
        const size_t e = src.find("};", gn);
        const std::string blk = src.substr(gn, e - gn);
        std::regex q("\"((?:[^\"\\\\]|\\\\.)*)\"");
        for (std::sregex_iterator i(blk.begin(), blk.end(), q), end; i != end; ++i) t.groups.push_back((*i)[1]);
    }
    const size_t gc = src.find("kGroupCols[kNumGroups] = {");
    if (gc != std::string::npos) {
        const std::string blk = src.substr(gc, src.find("};", gc) - gc);
        for (size_t p = 0; (p = blk.find("rgb(", p)) != std::string::npos; p++) t.cols++;
    }
    std::istringstream in(src);
    std::string line;
    std::regex row("^\\s*\\{[^,\"]+,\\s*\"((?:[^\"\\\\]|\\\\.)*)\",\\s*(\\d+),");
    bool inTable = false;
    while (std::getline(in, line)) {
        if (line.find("kBuiltinModes[] = {") != std::string::npos) inTable = true;
        if (inTable && line.rfind("};", 0) == 0) inTable = false;
        std::smatch m;
        if (inTable && std::regex_search(line, m, row)) t.rows.push_back({m[1], std::stoi(m[2])});
    }
    return t;
}

static void coverEveryModeOnce() {
    const Table t = load();
    CHECK(t.groups.size() == (size_t)modegroups::kCount, "kGroupNames has %zu names, the enum %d", t.groups.size(), modegroups::kCount);
    CHECK(t.cols == modegroups::kCount, "kGroupCols has %d colours, the enum %d", t.cols, modegroups::kCount);
    CHECK(t.rows.size() > 350, "only %zu rows read from the table", t.rows.size());
    std::map<std::string, int> names;
    for (size_t i = 0; i < t.groups.size(); i++) { CHECK(!names.count(t.groups[i]), "group name \"%s\" twice", t.groups[i].c_str()); names[t.groups[i]] = (int)i; }
    CHECK(t.groups.size() > modegroups::Plugins && t.groups[modegroups::Plugins] == "PLUGINS" && t.groups[modegroups::CustomModes] == "CUSTOM MODES" && t.groups[modegroups::TV] == "TV" &&
          t.groups[modegroups::HfMonitoring] == "HF MONITORING" && t.groups[modegroups::Meters] == "METERS", "the enum and the names are out of step");
    std::vector<int> count(modegroups::kCount, 0);
    std::map<std::string, int> seen;
    for (const auto& r : t.rows) {
        CHECK(r.second >= 0 && r.second < modegroups::kCount, "\"%s\" is in group %d, outside 0..%d", r.first.c_str(), r.second, modegroups::kCount - 1);
        if (r.second >= 0 && r.second < modegroups::kCount) count[r.second]++;
        CHECK(!seen[r.first]++, "mode \"%s\" is listed twice", r.first.c_str());   // one row, one group: the search and the lists see it once
    }
    for (int g = 0; g < modegroups::kCount; g++)
        CHECK(count[g] > 0, "group %d (%s) has no mode", g, g < (int)t.groups.size() ? t.groups[g].c_str() : "?");
}

static void oldNamesMigrate() {
    using modegroups::migrate;
    using modegroups::migrateList;
    const Table t = load();
    std::set<std::string> now(t.groups.begin(), t.groups.end());
    for (const auto& o : modegroups::oldGroups()) {
        CHECK(!now.count(o.name), "old name \"%s\" is still a group", o.name);
        const std::set<std::string> m = migrate({o.name});
        CHECK(m.size() == o.now.size(), "\"%s\" migrates to %zu names, expected %zu", o.name, m.size(), o.now.size());
        for (const std::string& g : m) CHECK(now.count(g), "\"%s\" migrates to \"%s\", which is not a group", o.name, g.c_str());
    }
    CHECK(migrate({"TV", "MARITIME", "PAGERS"}) == std::set<std::string>({"TV", "MARITIME", "PAGERS"}), "names that stay are kept");
    const std::set<std::string> h = migrate({"HF UTILITY", "TV"});
    CHECK(h.size() == 4 && h.count("HF DATA") && h.count("HF TELEGRAPH & FSK") && h.count("HF MONITORING") && h.count("TV") && !h.count("HF UTILITY"), "HF UTILITY splits in three");
    CHECK(migrate({"GENERAL & TOOLS"}).count("METERS") == 1, "General & tools becomes receivers, meters, interference");
    CHECK(migrate(migrate({"SATELLITE & NAVIGATION"})) == migrate({"SATELLITE & NAVIGATION"}), "migrating twice changes nothing");
    CHECK(migrateList("") == "" && migrateList("PAGERS") == "PAGERS", "list: empty and unchanged");
    CHECK(migrateList("AMATEUR - DIGITAL|TV").find("AMATEUR - WEAK SIGNAL") != std::string::npos && migrateList("AMATEUR - DIGITAL|TV").find("AMATEUR - DIGITAL") == std::string::npos, "list: Amateur digital splits");
    // no old name is left in the source, and a group name never holds a | (the saved lists are split on it)
    for (const std::string& g : t.groups) CHECK(g.find('|') == std::string::npos, "group \"%s\" holds a |", g.c_str());
}

static void countsListed() {
    const Table t = load();
    std::vector<int> count(t.groups.size(), 0);
    for (const auto& r : t.rows) if (r.second >= 0 && r.second < (int)count.size()) count[r.second]++;
    const std::string md = readSource("MODES.md");
    std::vector<std::pair<std::string, int>> heads;
    std::regex h("^## (.*) \\((\\d+)\\)$");
    std::istringstream in(md);
    std::string line;
    while (std::getline(in, line)) { std::smatch m; if (std::regex_match(line, m, h)) heads.push_back({m[1], std::stoi(m[2])}); }
    size_t k = 0;
    for (size_t g = 0; g < t.groups.size(); g++) {
        if (!count[g]) continue;
        CHECK(k < heads.size(), "MODES.md has no heading for \"%s\"", t.groups[g].c_str());
        if (k >= heads.size()) break;
        CHECK(lower(heads[k].first) == lower(t.groups[g]), "MODES.md heading %zu is \"%s\", expected \"%s\"", k, heads[k].first.c_str(), t.groups[g].c_str());
        CHECK(heads[k].second == count[g], "MODES.md says %d modes for \"%s\", the table has %d", heads[k].second, t.groups[g].c_str(), count[g]);
        k++;
    }
    CHECK(k == heads.size(), "MODES.md lists %zu groups, the table fills %zu", heads.size(), k);
}

int main(int argc, char** argv) {
    #ifdef MODE_GROUPS_CASE
    const std::string w = MODE_GROUPS_CASE;   // one program per case (CMakeLists.txt)
#else
    const std::string w = argc > 1 ? argv[1] : "all";
#endif
    if (w == "all" || w == "groups_cover_every_mode_once") coverEveryModeOnce();
    if (w == "all" || w == "old_group_names_migrate") oldNamesMigrate();
    if (w == "all" || w == "group_counts_listed") countsListed();
    if (fails) { std::printf("%d failed\n", fails); return 1; }
    std::printf("mode groups ok\n");
    return 0;
}
